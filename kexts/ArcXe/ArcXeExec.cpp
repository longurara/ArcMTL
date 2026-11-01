/*
 * ArcXeExec.cpp — Hardware Execlist Submission (RCS0/CCS0/BCS0) & GuC v70 Firmware Loader for Xe-LPG
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#include "ArcXeService.h"
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <kern/thread.h>

/* ============================================================================
 * 1. Hardware Engine Setup (RCS0 / CCS0 / BCS0)
 * ============================================================================ */

uint32_t ArcXeService::engineMmioBase(uint16_t engineClass, uint16_t engineInstance, ArcBo **outHwsp) const
{
    (void)engineInstance;
    switch (engineClass) {
    case kArcXeEngineClassRender:
    case kArcXeEngineClassCompute:
        /*
         * On Meteor Lake Xe-LPG (12.70/12.71), standalone CCS0 (0x1A000) is clock/power-gated
         * off at reset unless RCU_MODE_CCS_ENABLE / CCS_MODE slice partitioning is enabled,
         * while RCS0 (0x02000) natively executes both 3D and GPGPU COMPUTE_WALKER pipelines
         * with full undivided access to all 8 Xe-cores (128 Vector Engines).
         */
        if (outHwsp) *outHwsp = fRcsHwspBo;
        return ARC_RENDER_RING_BASE;
    case kArcXeEngineClassCopy:
        if (fBcsHwspBo) {
            if (outHwsp) *outHwsp = fBcsHwspBo;
            return ARC_BLT_RING_BASE;
        }
        if (outHwsp) *outHwsp = fRcsHwspBo;
        return ARC_RENDER_RING_BASE;
    default:
        if (outHwsp) *outHwsp = nullptr;
        return 0;
    }
}

bool ArcXeService::initEngines()
{
    fEngineLock = IOLockAlloc();
    if (!fEngineLock) return false;

    /* Allocate 4KB Global HWSP in GGTT for RCS0 (Render+Compute) and BCS0 (Copy/Blitter) */
    fRcsHwspBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                ARC_MTL_PAT_IDX_WB_1WAY_COH, true);
    fBcsHwspBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                ARC_MTL_PAT_IDX_WB_1WAY_COH, true);
    if (!fRcsHwspBo || !fBcsHwspBo) {
        AXELOG("initEngines: failed to allocate engine HWSPs");
        return false;
    }

    if (!forceWakeGet()) return false;

    const struct { uint32_t base; ArcBo *hwsp; const char *name; } engines[] = {
        { ARC_RENDER_RING_BASE,   fRcsHwspBo, "RCS0" },
        { ARC_BLT_RING_BASE,      fBcsHwspBo, "BCS0" },
    };

    for (const auto &e : engines) {
        if (rd32(ARC_RING_MI_MODE(e.base)) == 0xFFFFFFFFu) {
            AXELOG("engine %s at 0x%05x unpowered/fused off; skipping", e.name, e.base);
            if (e.base == ARC_BLT_RING_BASE) {
                releaseBo(fBcsHwspBo);
                fBcsHwspBo = nullptr;
            }
            continue;
        }
        wr32(ARC_RING_HWS_PGA(e.base), e.hwsp->ggttOffset);
        (void)rd32(ARC_RING_HWS_PGA(e.base));
        wr32(ARC_RING_HWSTAM(e.base), 0xFFFFFFFFu);
        wr32(ARC_RING_IMR(e.base), 0xFFFFFFFFu); /* Mask ring IRQs (polling/seqno completion used) */
        /* Default CS MOCS for commands without explicit MOCS (BSpec 72161: MTL uc_index = 9) */
        wr32(ARC_RING_CMD_CCTL(e.base),
             ((0x3F00u | 0x007Eu) << 16) | (ARC_MTL_MOCS_UC_INDEX << 8) | (ARC_MTL_MOCS_UC_INDEX << 1));
        /* Masked write: enable Execlist mode (disable legacy ringbuffer mode) */
        wr32(ARC_RING_GFX_MODE(e.base), ARC_MASKED_ENABLE(ARC_GFX_DISABLE_LEGACY_MODE));
        /* Ensure ring streamer is not halted in MI_MODE */
        wr32(ARC_RING_MI_MODE(e.base), ARC_MASKED_DISABLE((1u << 9) | (1u << 11)));
        AXELOG("engine %s armed: base=0x%05x HWSP_GGTT=0x%08x", e.name, e.base, e.hwsp->ggttOffset);
    }

    forceWakePut();
    return true;
}

void ArcXeService::teardownEngines()
{
    if (fRcsHwspBo) { releaseBo(fRcsHwspBo); fRcsHwspBo = nullptr; }
    if (fCcsHwspBo) { releaseBo(fCcsHwspBo); fCcsHwspBo = nullptr; }
    if (fBcsHwspBo) { releaseBo(fBcsHwspBo); fBcsHwspBo = nullptr; }
    if (fGucCtbBo)  { releaseBo(fGucCtbBo);  fGucCtbBo  = nullptr; }
    if (fGucFwBo)   { releaseBo(fGucFwBo);   fGucFwBo   = nullptr; }
    if (fEngineLock) {
        IOLockFree(fEngineLock);
        fEngineLock = nullptr;
    }
}

/* ============================================================================
 * 2. Logical Ring Context (LRC) Creation & Batch Submission
 * ============================================================================ */

ArcExecQueue *ArcXeService::createExecQueue(ArcVm *vm, uint16_t engineClass,
                                            uint16_t engineInstance, uint32_t ringSize)
{
    if (!vm || !vm->pml4) return nullptr;
    ArcBo *hwsp = nullptr;
    const uint32_t base = engineMmioBase(engineClass, engineInstance, &hwsp);
    if (!base || !hwsp) return nullptr;

    if (ringSize < 16384 || (ringSize & 0xFFFu))
        ringSize = 16384;

    /*
     * LRC Layout (Linux drivers/gpu/drm/xe/xe_lrc.c: xe_gt_lrc_size):
     *   [0 .. ringSize)              : Ring Buffer
     *   [ringSize .. +4K)            : Per-Process HW Status Page (PPHWSP)
     *   [ringSize+4K .. +ctxImgSize) : Engine Context Image (13 * 4K = 52 KiB on Xe-LPG RCS0 + 4K margin)
     */
    const uint32_t pphwspOff  = ringSize;
    const uint32_t ctxImgOff  = ringSize + 4096u;
    const uint32_t ctxImgSize = (base == ARC_RENDER_RING_BASE || base == ARC_COMPUTE0_RING_BASE)
                              ? (14u * 4096u) : (2u * 4096u);
    const uint32_t totalSize  = ctxImgOff + ctxImgSize;

    ArcBo *lrcBo = createKernelBo(totalSize, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                  ARC_MTL_PAT_IDX_WB_1WAY_COH, true);
    if (!lrcBo || !lrcBo->kernMap) {
        if (lrcBo) releaseBo(lrcBo);
        return nullptr;
    }

    ArcExecQueue *q = IONewZero(ArcExecQueue, 1);
    if (!q) {
        releaseBo(lrcBo);
        return nullptr;
    }

    q->submitLock = IOLockAlloc();
    if (!q->submitLock) {
        releaseBo(lrcBo);
        IODelete(q, ArcExecQueue, 1);
        return nullptr;
    }

    uint8_t *cpuBase   = reinterpret_cast<uint8_t *>(lrcBo->kernMap->getVirtualAddress());
    q->vmId            = vm->vmId;
    q->engineClass     = engineClass;
    q->engineInstance  = engineInstance;
    q->mmioBase        = base;
    q->swCtxId         = (uint32_t)(OSIncrementAtomic((SInt32 *)&fNextSwCtxId) & 0x7FFFu);
    if (q->swCtxId == 0) q->swCtxId = 1;
    q->lrcBo           = lrcBo;
    q->ringGgttAddr    = lrcBo->ggttOffset;
    q->pphwspGgttAddr  = lrcBo->ggttOffset + pphwspOff;
    q->ctxImgGgttAddr  = lrcBo->ggttOffset + ctxImgOff;
    q->ringSize        = ringSize;
    q->ringTail        = 0;
    q->ringCpu         = reinterpret_cast<volatile uint32_t *>(cpuBase);
    q->ctxRegsCpu      = reinterpret_cast<volatile uint32_t *>(cpuBase + ctxImgOff);
    q->seqnoCpu        = reinterpret_cast<volatile uint64_t *>(cpuBase + pphwspOff + 0x80u);
    q->seqnoGgttAddr   = q->pphwspGgttAddr + 0x80u;
    q->pml4Phys        = vm->pml4->physAddr;
    *q->seqnoCpu       = 0ULL;

    /*
     * Populate Meteor Lake Xe-LPG (12.70/12.71) Engine Context Register State Image.
     * Matches Linux drivers/gpu/drm/xe/xe_lrc.c: set_offsets() with mtl_rcs_offsets[] (RCS0)
     * and gen12_xcs_offsets[] (BCS0), terminated by MI_BATCH_BUFFER_END | BIT(0) (END_CONTEXT).
     */
    const bool isRcs = (base == ARC_RENDER_RING_BASE || base == ARC_COMPUTE0_RING_BASE);
    volatile uint32_t *reg = q->ctxRegsCpu;
    const uint32_t lriPostedCs = ARC_MI_LRI_FORCE_POSTED | ARC_MI_LRI_CS_MMIO;

    reg[0x00] = ARC_MI_NOOP;
    reg[0x01] = ARC_MI_LOAD_REGISTER_IMM(isRcs ? 15u : 13u) | lriPostedCs;

    reg[0x02] = base + 0x244u; /* CTX_CONTEXT_CONTROL */
    reg[0x03] = ARC_MASKED_ENABLE(ARC_CTX_CTRL_INHIBIT_SYN_SWITCH | ARC_CTX_CTRL_RESTORE_INHIBIT);
    reg[0x04] = base + 0x034u; /* RING_BUFFER_HEAD */
    reg[0x05] = 0u;
    reg[0x06] = base + 0x030u; /* RING_BUFFER_TAIL */
    reg[0x07] = 0u;
    reg[0x08] = base + 0x038u; /* RING_BUFFER_START */
    reg[0x09] = q->ringGgttAddr;
    reg[0x0A] = base + 0x03Cu; /* RING_BUFFER_CONTROL */
    reg[0x0B] = ARC_RING_CTL_SIZE(q->ringSize) | ARC_RING_CTL_ENABLE;
    reg[0x0C] = base + 0x168u; /* BB_ADDR_UDW */
    reg[0x0D] = 0u;
    reg[0x0E] = base + 0x140u; /* BB_ADDR */
    reg[0x0F] = 0u;
    reg[0x10] = base + 0x110u; /* BB_STATE */
    reg[0x11] = 0u;
    reg[0x12] = base + 0x1C0u; /* BB_PER_CTX_PTR */
    reg[0x13] = 0u;
    reg[0x14] = base + 0x1C4u; /* RCS_INDIRECT_CTX */
    reg[0x15] = 0u;
    reg[0x16] = base + 0x1C8u; /* RCS_INDIRECT_CTX_OFFSET */
    reg[0x17] = 0u;
    reg[0x18] = base + 0x180u; /* CCID */
    reg[0x19] = 0u;
    reg[0x1A] = base + 0x2B4u; /* SEMAPHORE_TOKEN */
    reg[0x1B] = 0u;

    if (isRcs) {
        reg[0x1C] = base + 0x120u; /* PRT_BB_STATE */
        reg[0x1D] = 0u;
        reg[0x1E] = base + 0x124u; /* PRT_BB_STATE_UDW */
        reg[0x1F] = 0u;
        reg[0x20] = ARC_MI_NOOP;
    } else {
        for (uint32_t i = 0x1Cu; i <= 0x20u; i++)
            reg[i] = ARC_MI_NOOP;
    }

    reg[0x21] = ARC_MI_LOAD_REGISTER_IMM(9u) | lriPostedCs;
    reg[0x22] = base + 0x3A8u; /* CTX_TIMESTAMP */
    reg[0x23] = 0u;
    reg[0x24] = base + 0x28Cu; /* PDP3_UDW */
    reg[0x25] = 0u;
    reg[0x26] = base + 0x288u; /* PDP3_LDW */
    reg[0x27] = 0u;
    reg[0x28] = base + 0x284u; /* PDP2_UDW */
    reg[0x29] = 0u;
    reg[0x2A] = base + 0x280u; /* PDP2_LDW */
    reg[0x2B] = 0u;
    reg[0x2C] = base + 0x27Cu; /* PDP1_UDW */
    reg[0x2D] = 0u;
    reg[0x2E] = base + 0x278u; /* PDP1_LDW */
    reg[0x2F] = 0u;
    reg[0x30] = base + 0x274u; /* PDP0_UDW */
    reg[0x31] = (uint32_t)(q->pml4Phys >> 32);
    reg[0x32] = base + 0x270u; /* PDP0_LDW */
    reg[0x33] = (uint32_t)(q->pml4Phys & 0xFFFFFFFFULL);

    if (isRcs) {
        reg[0x34] = ARC_MI_NOOP;
        reg[0x35] = ARC_MI_NOOP;
        reg[0x36] = ARC_MI_LOAD_REGISTER_IMM(2u) | lriPostedCs;
        reg[0x37] = base + 0x5A8u;
        reg[0x38] = 0u;
        reg[0x39] = base + 0x5ACu;
        reg[0x3A] = 0u;
        for (uint32_t i = 0x3Bu; i <= 0x40u; i++)
            reg[i] = ARC_MI_NOOP;
        reg[0x41] = ARC_MI_LOAD_REGISTER_IMM(1u) | ARC_MI_LRI_CS_MMIO; /* Unposted R_PWR_CLK_STATE */
        reg[0x42] = base + 0x0C8u;
        reg[0x43] = 0u;
        reg[0x44] = ARC_MI_BATCH_BUFFER_END_CTX; /* 0x05000001: End Context Restore! */
    } else {
        reg[0x34] = ARC_MI_BATCH_BUFFER_END_CTX; /* 0x05000001: End Context Restore! */
    }

    /* Enable arbitration at start of ring */
    q->ringCpu[0] = ARC_MI_ARB_ON_OFF | ARC_MI_ARB_ENABLE;
    q->ringCpu[1] = ARC_MI_NOOP;
    q->ringTail   = 8u;
    reg[ARC_CTX_RING_HEAD_VAL] = 0u;
    reg[ARC_CTX_RING_TAIL_VAL] = q->ringTail;
    __asm__ __volatile__("mfence" ::: "memory");

    return q;
}

void ArcXeService::destroyExecQueue(ArcExecQueue *q)
{
    if (!q) return;
    if (q->lastSubmittedSeqno > *q->seqnoCpu) {
        uint64_t done = 0; bool to = false;
        (void)waitQueueSeqno(q, q->lastSubmittedSeqno, 250000000ULL, &done, &to);
    }
    if (q->lrcBo) {
        releaseBo(q->lrcBo);
        q->lrcBo = nullptr;
    }
    if (q->submitLock) {
        IOLockFree(q->submitLock);
        q->submitLock = nullptr;
    }
    IODelete(q, ArcExecQueue, 1);
}

static inline void ringEmitDword(ArcExecQueue *q, uint32_t dw)
{
    q->ringCpu[q->ringTail >> 2] = dw;
    q->ringTail = (q->ringTail + 4u) & (q->ringSize - 1u);
}

IOReturn ArcXeService::submitBatch(ArcExecQueue *q, uint64_t batchGpuVa, uint64_t *outSeqno)
{
    if (!q || !q->lrcBo || fStopping) return kIOReturnBadArgument;

    IOLockLock(q->submitLock);
    IOLockLock(fEngineLock);

    if (!forceWakeGet()) {
        IOLockUnlock(fEngineLock);
        IOLockUnlock(q->submitLock);
        return kIOReturnDeviceError;
    }

    const uint64_t seqno = ++q->lastSubmittedSeqno;
    const uint32_t invalScratchGgtt = q->pphwspGgttAddr + 0x60u;

    /* Never allow a multi-dword CS instruction packet to straddle the end of the ring buffer */
    if ((q->ringSize - q->ringTail) < 160u) {
        while (q->ringTail != 0u) {
            ringEmitDword(q, ARC_MI_NOOP);
        }
    }

    /* 1. Pre-batch Cache & TLB Invalidate (Linux xe_ring_ops.c: emit_pipe_invalidate) */
    if (q->engineClass == kArcXeEngineClassRender || q->engineClass == kArcXeEngineClassCompute) {
        uint32_t pcFlags0 = ARC_PC0_L3_RO_CACHE_INVALIDATE;
        uint32_t pcFlags1 = ARC_PC_CS_STALL | ARC_PC_TLB_INVALIDATE |
                            ARC_PC_CMD_CACHE_INVALIDATE | ARC_PC_INST_CACHE_INVALIDATE |
                            ARC_PC_TEX_CACHE_INVALIDATE | ARC_PC_VF_CACHE_INVALIDATE |
                            ARC_PC_CONST_CACHE_INVALIDATE | ARC_PC_STATE_CACHE_INVALIDATE |
                            ARC_PC_GLOBAL_GTT | ARC_PC_QW_WRITE;
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6) | pcFlags0);
        ringEmitDword(q, pcFlags1);
        ringEmitDword(q, invalScratchGgtt);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
    } else {
        ringEmitDword(q, ARC_MI_FLUSH_DW | ARC_MI_FLUSH_DW_INV_TLB |
                         ARC_MI_FLUSH_DW_OP_STOREDW | ARC_MI_FLUSH_DW_IMM_LEN);
        ringEmitDword(q, invalScratchGgtt | ARC_MI_FLUSH_DW_USE_GGTT);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
    }

    /* 2. Jump to Userspace 48-bit PPGTT Batch Buffer */
    ringEmitDword(q, ARC_MI_BATCH_BUFFER_START_PPGTT);
    ringEmitDword(q, (uint32_t)(batchGpuVa & 0xFFFFFFFFULL));
    ringEmitDword(q, (uint32_t)(batchGpuVa >> 32));

    /* 3. Post-batch Render/Compute Cache Flush + Fence Seqno Write to GGTT PPHWSP (xe_ring_ops.c:491-500) */
    if (q->engineClass == kArcXeEngineClassRender || q->engineClass == kArcXeEngineClassCompute) {
        /* 3a. Quiesce & flush 3D/Compute caches (HDC + Tile + RT + Depth + DC) */
        const uint32_t flush1 = ARC_PC_CS_STALL | ARC_PC_FLUSH_ENABLE | ARC_PC_DC_FLUSH_ENABLE |
                                ARC_PC_TILE_CACHE_FLUSH | ARC_PC_RT_CACHE_FLUSH |
                                ARC_PC_DEPTH_CACHE_FLUSH | ARC_PC_DEPTH_STALL;
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6) | ARC_PC0_HDC_PIPELINE_FLUSH);
        ringEmitDword(q, flush1);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);

        /* 3b. Post-sync 64-bit seqno write into GGTT PPHWSP (PIPE_CONTROL_GLOBAL_GTT_IVB in dw1!) */
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6));
        ringEmitDword(q, ARC_PC_GLOBAL_GTT | ARC_PC_QW_WRITE | ARC_PC_FLUSH_ENABLE | ARC_PC_CS_STALL);
        ringEmitDword(q, q->seqnoGgttAddr);
        ringEmitDword(q, 0u);
        ringEmitDword(q, (uint32_t)(seqno & 0xFFFFFFFFULL));
        ringEmitDword(q, (uint32_t)(seqno >> 32));

        /* 3c. Serialized CS GGTT QWORD store of seqno */
        ringEmitDword(q, ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_USE_GGTT | ARC_MI_SDI_QWORD_LEN);
        ringEmitDword(q, q->seqnoGgttAddr);
        ringEmitDword(q, 0u);
        ringEmitDword(q, (uint32_t)(seqno & 0xFFFFFFFFULL));
        ringEmitDword(q, (uint32_t)(seqno >> 32));
    } else {
        /* Blitter engine: 4-dword MI_FLUSH_DW + 64-bit GGTT seqno store */
        ringEmitDword(q, ARC_MI_FLUSH_DW | ARC_MI_FLUSH_DW_IMM_LEN);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);

        ringEmitDword(q, ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_USE_GGTT | ARC_MI_SDI_QWORD_LEN);
        ringEmitDword(q, q->seqnoGgttAddr);
        ringEmitDword(q, 0u);
        ringEmitDword(q, (uint32_t)(seqno & 0xFFFFFFFFULL));
        ringEmitDword(q, (uint32_t)(seqno >> 32));
    }

    ringEmitDword(q, ARC_MI_USER_INTERRUPT);
    ringEmitDword(q, ARC_MI_ARB_ON_OFF | ARC_MI_ARB_ENABLE);
    ringEmitDword(q, ARC_MI_ARB_CHECK);
    if (q->ringTail & 4u) {
        ringEmitDword(q, ARC_MI_NOOP); /* Keep ring tail 8-byte aligned */
    }

    /* Update LRC context image tail and root PML4 before kicking ELSP (Linux xe_execlist.c: __start_lrc) */
    q->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = q->ringTail;
    q->ctxRegsCpu[ARC_CTX_PDP0_UDW_VAL]  = (uint32_t)(q->pml4Phys >> 32);
    q->ctxRegsCpu[ARC_CTX_PDP0_LDW_VAL]  = (uint32_t)(q->pml4Phys & 0xFFFFFFFFULL);
    __asm__ __volatile__("mfence" ::: "memory");

    /* Build 64-bit Xe-LPG LRC Descriptor (LRCA points to PPHWSP at [31:12]) */
    const uint64_t lrcDesc = (uint64_t)(q->pphwspGgttAddr & ~0xFFFu)
                           | ARC_LRC_DESC_VALID
                           | ARC_LRC_DESC_LEGACY_64B_PPGTT
                           | ARC_LRC_DESC_PRIVILEGE_PPGTT
                           | ((uint64_t)q->swCtxId << ARC_LRC_DESC_XEHP_SW_CTX_SHIFT);

    ArcBo *hwsp = nullptr;
    (void)engineMmioBase(q->engineClass, q->engineInstance, &hwsp);
    if (hwsp) {
        wr32(ARC_RING_HWS_PGA(q->mmioBase), hwsp->ggttOffset);
        (void)rd32(ARC_RING_HWS_PGA(q->mmioBase));
    }

    wr32(ARC_RING_EXECLIST_SQ_LO(q->mmioBase), (uint32_t)(lrcDesc & 0xFFFFFFFFULL));
    wr32(ARC_RING_EXECLIST_SQ_HI(q->mmioBase), (uint32_t)(lrcDesc >> 32));
    wr32(ARC_RING_EXECLIST_CONTROL(q->mmioBase), ARC_EL_CTRL_LOAD);
    (void)rd32(ARC_RING_EXECLIST_STATUS_LO(q->mmioBase));

    forceWakePut();
    IOLockUnlock(fEngineLock);
    IOLockUnlock(q->submitLock);

    if (outSeqno) *outSeqno = seqno;
    return kIOReturnSuccess;
}

IOReturn ArcXeService::waitQueueSeqno(ArcExecQueue *q, uint64_t targetSeqno, uint64_t timeoutNs,
                                      uint64_t *outCompleted, bool *outTimedOut)
{
    if (!q || !q->seqnoCpu) return kIOReturnBadArgument;
    if (targetSeqno == 0) targetSeqno = q->lastSubmittedSeqno;

    if (*q->seqnoCpu >= targetSeqno) {
        if (outCompleted) *outCompleted = *q->seqnoCpu;
        if (outTimedOut)  *outTimedOut  = false;
        return kIOReturnSuccess;
    }

    const uint64_t startAbs = mach_absolute_time();
    uint32_t spins = 0;

    for (;;) {
        const uint64_t cur = *q->seqnoCpu;
        if (cur >= targetSeqno) {
            if (outCompleted) *outCompleted = cur;
            if (outTimedOut)  *outTimedOut  = false;
            return kIOReturnSuccess;
        }

        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - startAbs, &elapsedNs);
        if (elapsedNs >= timeoutNs) {
            if (forceWakeGet()) {
                const uint32_t b = q->mmioBase;
                AXELOG("waitQueueSeqno TIMEOUT (q=%u target=%llu cur=%llu): "
                       "EL_STATUS=0x%08x_%08x LRCA=0x%08x HEAD=0x%x TAIL=0x%x START=0x%08x CTL=0x%08x "
                       "ACTHD=0x%08x_%08x IPEHR=0x%08x ESR=0x%08x",
                       q->queueId, (unsigned long long)targetSeqno, (unsigned long long)cur,
                       rd32(ARC_RING_EXECLIST_STATUS_HI(b)), rd32(ARC_RING_EXECLIST_STATUS_LO(b)),
                       rd32(ARC_RING_CURRENT_LRCA(b)),
                       rd32(ARC_RING_HEAD(b)), rd32(ARC_RING_TAIL(b)),
                       rd32(ARC_RING_START(b)), rd32(ARC_RING_CTL(b)),
                       rd32(ARC_RING_ACTHD_UDW(b)), rd32(ARC_RING_ACTHD(b)),
                       rd32(ARC_RING_IPEHR(b)), rd32(ARC_RING_ESR(b)));
                forceWakePut();
            }
            if (outCompleted) *outCompleted = cur;
            if (outTimedOut)  *outTimedOut  = true;
            return kIOReturnSuccess;
        }

        /* Fast microsecond spin for the first 200 us (low-latency compositor frames), then yield */
        if (spins++ < 40) {
            IODelay(5);
        } else {
            IOSleep(1);
        }
    }
}

/* ============================================================================
 * 3. GuC v70 WOPCM Firmware Loader (/Library/ArcDriver/fw/mtl_guc_70.bin)
 * ============================================================================ */

bool ArcXeService::tryLoadGucFirmwareFromDisk(const char *fwPath)
{
    vfs_context_t ctx = vfs_context_create(nullptr);
    if (!ctx) return false;

    vnode_t vp = NULLVP;
    int err = vnode_open(fwPath, FREAD, 0, 0, &vp, ctx);
    if (err != 0 || vp == NULLVP) {
        vfs_context_rele(ctx);
        return false;
    }

    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    err = vnode_getattr(vp, &va, ctx);
    if (err != 0 || va.va_data_size < sizeof(ArcUcCssHeader) + 256 || va.va_data_size > (8 * 1024 * 1024)) {
        vnode_close(vp, FREAD, ctx);
        vfs_context_rele(ctx);
        return false;
    }

    const uint64_t fwBytes = va.va_data_size;
    ArcBo *fwBo = createKernelBo(fwBytes, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                 ARC_MTL_PAT_IDX_WB_1WAY_COH, true);
    if (!fwBo || !fwBo->kernMap) {
        if (fwBo) releaseBo(fwBo);
        vnode_close(vp, FREAD, ctx);
        vfs_context_rele(ctx);
        return false;
    }

    void *dst = reinterpret_cast<void *>(fwBo->kernMap->getVirtualAddress());
    int resid = 0;
    err = vn_rdwr(UIO_READ, vp, (caddr_t)dst, (int)fwBytes, 0, UIO_SYSSPACE, 0,
                  vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    vnode_close(vp, FREAD, ctx);
    vfs_context_rele(ctx);

    if (err != 0 || resid != 0) {
        AXELOG("GuC: vn_rdwr failed (err=%d resid=%d)", err, resid);
        releaseBo(fwBo);
        return false;
    }

    const ArcUcCssHeader *css = reinterpret_cast<const ArcUcCssHeader *>(dst);
    const uint32_t hdrBytes   = css->header_size_dw * 4u;
    const uint32_t rsaBytes   = css->key_size_dw * 4u;
    const uint32_t totalBytes = css->size_dw * 4u;
    const uint32_t ucodeBytes = totalBytes - hdrBytes - rsaBytes;

    if (totalBytes > fwBytes || css->key_size_dw != ARC_UOS_RSA_SCRATCH_COUNT) {
        AXELOG("GuC: invalid CSS header (size=%u fw=%llu key_dw=%u)",
               totalBytes, (unsigned long long)fwBytes, css->key_size_dw);
        releaseBo(fwBo);
        return false;
    }

    const uint32_t verMaj = (css->sw_version >> 16) & 0xFFu;
    const uint32_t verMin = (css->sw_version >> 8)  & 0xFFu;
    const uint32_t verPat = css->sw_version & 0xFFu;
    AXELOG("GuC firmware read OK: %s v%u.%u.%u (%u bytes, GGTT=0x%x)",
           fwPath, verMaj, verMin, verPat, totalBytes, fwBo->ggttOffset);

    fGucFwBo = fwBo;

    if (!fUseGuc) {
        AXELOG("GuC firmware staged in GGTT (direct Execlist active; pass arc_guc=1 to boot GuC DMA)");
        return true;
    }

    if (!forceWakeGet()) return false;

    /* Program WOPCM partitioning if not already locked by BIOS/previous reset */
    uint32_t wopcmSizeReg = rd32(ARC_REG_GUC_WOPCM_SIZE);
    if (!(wopcmSizeReg & ARC_GUC_WOPCM_SIZE_LOCKED)) {
        wr32(ARC_REG_GUC_WOPCM_SIZE, ARC_MTL_GUC_WOPCM_SIZE);
    }
    uint32_t wopcmOffReg = rd32(ARC_REG_DMA_GUC_WOPCM_OFFSET);
    if (!(wopcmOffReg & ARC_GUC_WOPCM_OFFSET_VALID)) {
        wr32(ARC_REG_DMA_GUC_WOPCM_OFFSET, ARC_MTL_GUC_WOPCM_BASE | ARC_GUC_WOPCM_OFFSET_VALID);
    }

    wr32(ARC_REG_GUC_SHIM_CONTROL, ARC_GUC_SHIM_DEFAULT);

    /* Copy 256-byte RSA signature (located immediately after CSS header + uCode) to UOS_RSA_SCRATCH */
    const uint32_t *rsaDwords = reinterpret_cast<const uint32_t *>(
        reinterpret_cast<const uint8_t *>(dst) + hdrBytes + ucodeBytes);
    for (uint32_t i = 0; i < ARC_UOS_RSA_SCRATCH_COUNT; i++) {
        wr32(ARC_REG_UOS_RSA_SCRATCH(i), rsaDwords[i]);
    }

    /* DMA CSS Header + uCode from GGTT into GuC WOPCM SRAM */
    const uint32_t dmaXferBytes = hdrBytes + ucodeBytes;
    wr32(ARC_REG_DMA_ADDR_0_LOW, fwBo->ggttOffset);
    wr32(ARC_REG_DMA_ADDR_0_HIGH, ARC_DMA_SPACE_GGTT);
    wr32(ARC_REG_DMA_ADDR_1_LOW, ARC_MTL_GUC_WOPCM_BASE);
    wr32(ARC_REG_DMA_ADDR_1_HIGH, ARC_DMA_SPACE_WOPCM);
    wr32(ARC_REG_DMA_COPY_SIZE, dmaXferBytes);
    wr32(ARC_REG_DMA_CTRL, ARC_DMA_CTRL_UOS_MOVE | ARC_DMA_CTRL_START);

    /* Wait up to 200 ms for DMA completion and GuC BootROM RSA authentication */
    for (uint32_t ms = 0; ms < 200; ms++) {
        fGucStatus = rd32(ARC_REG_GUC_STATUS);
        const uint32_t ukernel = (fGucStatus >> ARC_GS_UKERNEL_SHIFT) & ARC_GS_UKERNEL_MASK;
        if ((fGucStatus & ARC_GS_AUTH_GOOD) && ukernel >= ARC_GS_UKERNEL_READY) {
            fGucLoaded = true;
            break;
        }
        IOSleep(1);
    }
    fGucStatus = rd32(ARC_REG_GUC_STATUS);
    forceWakePut();

    AXELOG("GuC v%u.%u.%u DMA load result: status=0x%08x (%s)",
           verMaj, verMin, verPat, fGucStatus, fGucLoaded ? "AUTHENTICATED & READY" : "NOT READY");
    return fGucLoaded;
}

void ArcXeService::gucLoaderThreadEntry(void *arg, wait_result_t)
{
    ArcXeService *self = static_cast<ArcXeService *>(arg);
    static const char *const kFwPaths[] = {
        "/Library/ArcDriver/fw/mtl_guc_70.bin",
        "/usr/local/share/arcfw/mtl_guc_70.bin",
    };

    /* Wait until rootfs VFS is mounted (OpenCore injects Safe Boot kexts during early PCI init!) */
    IOSleep(8000);

    /* Poll gently for up to 30 s while root volume finishes mounting at boot */
    for (uint32_t attempt = 0; attempt < 30 && !self->fStopping; attempt++) {
        for (const char *p : kFwPaths) {
            if (self->tryLoadGucFirmwareFromDisk(p)) {
                self->release();
                thread_terminate(current_thread());
                return;
            }
        }
        IOSleep(1000);
    }
    AXELOG("mtl_guc_70.bin not found on disk (continuing on Direct Execlist engine backend)");
    self->release();
    thread_terminate(current_thread());
}
