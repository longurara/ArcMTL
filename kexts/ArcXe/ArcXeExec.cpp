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

void ArcXeService::armEngineHw(uint32_t base, ArcBo *hwsp)
{
    if (!base || !hwsp) return;

    wr32(ARC_RING_HWS_PGA(base), hwsp->ggttOffset);
    (void)rd32(ARC_RING_HWS_PGA(base));
    wr32(ARC_RING_HWSTAM(base), 0xFFFFFFFFu);
    wr32(ARC_RING_IMR(base), 0xFFFFFFFFu); /* Mask ring IRQs (polling/seqno completion used) */
    wr32(ARC_RING_EIR(base), 0xFFFFFFFFu);
    wr32(ARC_RING_ESR(base), 0u);
    /* Default CS MOCS for commands without explicit MOCS (BSpec 72161: MTL uc_index = 9) */
    wr32(ARC_RING_CMD_CCTL(base),
         ((0x3F00u | 0x007Eu) << 16) | (ARC_MTL_MOCS_UC_INDEX << 8) | (ARC_MTL_MOCS_UC_INDEX << 1));
    if (base == ARC_BLT_RING_BASE) {
        /* BLIT_CCTL (non-masked): [14:9] = dst MOCS, [6:1] = src MOCS (xe_hw_engine.c:340-360) */
        const uint32_t curBlitCctl = rd32(ARC_RING_BLIT_CCTL(base));
        wr32(ARC_RING_BLIT_CCTL(base),
             (curBlitCctl & ~0x7E7Eu) | (ARC_MTL_MOCS_UC_INDEX << 9) | (ARC_MTL_MOCS_UC_INDEX << 1));
    } else if (base == ARC_RENDER_RING_BASE) {
        /*
         * Program privileged RCS0 registers required by Mesa ANV genX_init_state.c (which Intel CS
         * drops as NO-OPs when ANV emits them inside non-privileged PPGTT user batches):
         *   - CS_DEBUG_MODE2 BIT(4) (CONSTANT_BUFFERAddressOffsetDisable): 3DSTATE_CONSTANT_* takes 48-bit PPGTT VAs!
         * Wa_18022495364 / BIT(6) is for IP 12.00..12.10, not Meteor Lake.
         */
        wr32(ARC_REG_CS_CHICKEN1, ARC_MASKED_DISABLE(ARC_CS_PREEMPT_3D_OBJECT_LEVEL));
        wr32(ARC_REG_FF_MODE2,    ARC_FF_MODE2_ANV_TIMERS);
        wr32(ARC_RING_CS_DEBUG_MODE2(base), ARC_MASKED_ENABLE(ARC_CS_DEBUG_MODE2_RCS_DEFAULT));
    }
    /* Masked write: enable Execlist mode (disable legacy ringbuffer mode) */
    wr32(ARC_RING_GFX_MODE(base), ARC_MASKED_ENABLE(ARC_GFX_DISABLE_LEGACY_MODE));
    /* Ensure ring streamer is not halted in MI_MODE (STOP_RING is BIT(8); BIT(9) is read-only MODE_IDLE) */
    wr32(ARC_RING_MI_MODE(base), ARC_MASKED_DISABLE(ARC_MI_STOP_RING));
    (void)rd32(ARC_RING_MI_MODE(base));
}

bool ArcXeService::resetEngineHw(uint32_t base, ArcBo *hwsp)
{
    ArcExecQueue *active = (base == ARC_BLT_RING_BASE) ? fLastBcsQueue : fLastRcsQueue;
    if (active && !active->isIdleKernelCtx)
        active->faulted = true;
    /*
     * Hardware Engine Domain Reset (Linux i915/gt/intel_reset.c: gen8_engine_reset_prepare +
     * gen6_hw_domain_reset + gen8_engine_reset_cancel). Caller MUST hold forceWakeGet().
     * Only resets GT Render/Blitter domain — Display Engine & DSM GOP scanout are untouched.
     */
    const uint32_t domainMask = (base == ARC_BLT_RING_BASE) ? ARC_GRDOM_BLT : ARC_GRDOM_RENDER;
    ArcExecQueue *idleQ = (base == ARC_BLT_RING_BASE) ? fBcsIdleQueue : fRcsIdleQueue;
    if (base == ARC_BLT_RING_BASE)
        fLastBcsQueue = nullptr;
    else
        fLastRcsQueue = nullptr;
    if (idleQ && idleQ->ctxRegsCpu) {
        idleQ->ringTail        = idleQ->initRingTail;
        idleQ->hasSavedHwState = false;
        idleQ->isLoadedOnHw    = false;
        idleQ->ctxRegsCpu[ARC_CTX_CONTEXT_CONTROL_VAL] =
            ARC_MASKED_ENABLE(ARC_CTX_CTRL_INHIBIT_SYN_SWITCH | ARC_CTX_CTRL_RESTORE_INHIBIT);
        idleQ->ctxRegsCpu[ARC_CTX_RING_HEAD_VAL] = 0u;
        idleQ->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = idleQ->initRingTail;
    }

    wr32(ARC_RING_MI_MODE(base), ARC_MASKED_ENABLE(ARC_MI_STOP_RING));
    for (uint32_t us = 0; us < 2000; us += 10) {
        if (rd32(ARC_RING_MI_MODE(base)) & ARC_MI_MODE_IDLE)
            break;
        IODelay(10);
    }

    const uint32_t ack = rd32(ARC_RING_RESET_CTL(base));
    if (ack & ARC_RESET_CTL_CAT_ERROR) {
        wr32(ARC_RING_RESET_CTL(base), ARC_MASKED_ENABLE(ARC_RESET_CTL_CAT_ERROR));
        for (uint32_t us = 0; us < 10000; us += 10) {
            if ((rd32(ARC_RING_RESET_CTL(base)) & ARC_RESET_CTL_CAT_ERROR) == 0)
                break;
            IODelay(10);
        }
    } else if ((ack & ARC_RESET_CTL_READY_TO_RESET) == 0) {
        wr32(ARC_RING_RESET_CTL(base), ARC_MASKED_ENABLE(ARC_RESET_CTL_REQUEST_RESET));
        for (uint32_t us = 0; us < 10000; us += 10) {
            if (rd32(ARC_RING_RESET_CTL(base)) & ARC_RESET_CTL_READY_TO_RESET)
                break;
            IODelay(10);
        }
    }

    wr32(ARC_REG_GDRST, domainMask);
    bool resetOk = false;
    for (uint32_t us = 0; us < 50000; us += 10) {
        if ((rd32(ARC_REG_GDRST) & domainMask) == 0) {
            resetOk = true;
            break;
        }
        IODelay(10);
    }
    IODelay(50);

    wr32(ARC_RING_RESET_CTL(base), ARC_MASKED_DISABLE(ARC_RESET_CTL_REQUEST_RESET));

    /* Re-apply MOCS and Xe-LPG MCR workarounds after render domain reset */
    if (domainMask == ARC_GRDOM_RENDER) {
        programMtlMocsTable();
        applyXeLpgWorkarounds();
    }
    armEngineHw(base, hwsp);
    /* A reset discards the idle image too. Rebuild it before any new user
     * context runs, under the caller's engine lock and forcewake reference. */
    if (resetOk && idleQ)
        resetOk = parkEngineOnIdleContextLocked(base, nullptr);
    return resetOk;
}

bool ArcXeService::initEngines()
{
    fEngineLock = IOLockAlloc();
    if (!fEngineLock) return false;

    /* Allocate 4KB Global HWSP in GGTT for RCS0 (Render+Compute) and BCS0 (Copy/Blitter) */
    fRcsHwspBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                ARC_MTL_PAT_IDX_WB_2WAY_COH, true);
    fBcsHwspBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                ARC_MTL_PAT_IDX_WB_2WAY_COH, true);
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
        const bool rstOk = resetEngineHw(e.base, e.hwsp);
        AXELOG("engine %s armed (reset=%d): base=0x%05x HWSP_GGTT=0x%08x MI_MODE=0x%08x",
               e.name, rstOk ? 1 : 0, e.base, e.hwsp->ggttOffset, rd32(ARC_RING_MI_MODE(e.base)));
    }

    if (!fUseGuc) {
        fEnginesHoldForceWake = true;
    } else {
        forceWakePut();
    }

    /*
     * Allocate permanent per-engine kernel idle contexts (SW_CTX_ID = 0, matching Linux xe_execlist.c
     * port->lrc) so active user contexts can always be cleanly Context-Saved off RCS0/BCS0 before
     * subsequent multi-batch restores or LRC GGTT teardown.
     */
    fKernelIdleVm = createVm();
    if (!fKernelIdleVm) {
        AXELOG("initEngines: failed to allocate kernel idle VM");
        return false;
    }
    fRcsIdleQueue = createExecQueue(fKernelIdleVm, kArcXeEngineClassRender, 0, 16384, true);
    if (!fRcsIdleQueue) {
        AXELOG("initEngines: failed to allocate RCS0 kernel idle LRC");
        return false;
    }
    fRcsIdleQueue->swCtxId         = 0u;
    fRcsIdleQueue->isIdleKernelCtx = true;
    if (fRcsIdleQueue->holdsForceWake) {
        forceWakePut();
        fRcsIdleQueue->holdsForceWake = false;
    }

    if (fBcsHwspBo) {
        fBcsIdleQueue = createExecQueue(fKernelIdleVm, kArcXeEngineClassCopy, 0, 16384, true);
        if (fBcsIdleQueue) {
            fBcsIdleQueue->swCtxId         = 0u;
            fBcsIdleQueue->isIdleKernelCtx = true;
            if (fBcsIdleQueue->holdsForceWake) {
                forceWakePut();
                fBcsIdleQueue->holdsForceWake = false;
            }
        }
    }
    /* Bootstrap the parking contexts while the engines still have reset state.
     * Their first RESTORE_INHIBIT run must not inherit a user's 3D state under
     * the kernel idle VM. Later switches save/restore this initialized image. */
    if (!forceWakeGet()) return false;
    IOLockLock(fEngineLock);
    const bool idleReady = parkEngineOnIdleContextLocked(ARC_RENDER_RING_BASE, nullptr) &&
        (!fBcsIdleQueue || parkEngineOnIdleContextLocked(ARC_BLT_RING_BASE, nullptr));
    IOLockUnlock(fEngineLock);
    forceWakePut();
    if (!idleReady) AXELOG("initEngines: kernel idle context bootstrap failed");
    return idleReady;
}

void ArcXeService::teardownEngines()
{
    if (fRcsIdleQueue) { destroyExecQueue(fRcsIdleQueue); fRcsIdleQueue = nullptr; }
    if (fBcsIdleQueue) { destroyExecQueue(fBcsIdleQueue); fBcsIdleQueue = nullptr; }
    if (fKernelIdleVm) { destroyVm(fKernelIdleVm);        fKernelIdleVm = nullptr; }
    if (fRcsHwspBo) { releaseBo(fRcsHwspBo); fRcsHwspBo = nullptr; }
    if (fCcsHwspBo) { releaseBo(fCcsHwspBo); fCcsHwspBo = nullptr; }
    if (fBcsHwspBo) { releaseBo(fBcsHwspBo); fBcsHwspBo = nullptr; }
    if (fGucCtbBo)  { releaseBo(fGucCtbBo);  fGucCtbBo  = nullptr; }
    if (fGucFwBo)   { releaseBo(fGucFwBo);   fGucFwBo   = nullptr; }
    if (fEnginesHoldForceWake) {
        forceWakePut();
        fEnginesHoldForceWake = false;
    }
    if (fEngineLock) {
        IOLockFree(fEngineLock);
        fEngineLock = nullptr;
    }
}

/* ============================================================================
 * 2. Logical Ring Context (LRC) Creation & Batch Submission
 * ============================================================================ */

static inline void clflushRange(volatile const void *ptr, size_t bytes)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~static_cast<uintptr_t>(63u);
    const uintptr_t end   = (reinterpret_cast<uintptr_t>(ptr) + bytes + 63u) & ~static_cast<uintptr_t>(63u);
    __asm__ __volatile__("mfence" ::: "memory");
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ __volatile__("clflush (%0)" :: "r"(reinterpret_cast<const void *>(p)) : "memory");
    }
    __asm__ __volatile__("mfence" ::: "memory");
}

ArcExecQueue *ArcXeService::createExecQueue(ArcVm *vm, uint16_t engineClass,
                                            uint16_t engineInstance, uint32_t ringSize,
                                            bool isIdleKernelCtx)
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
                                  ARC_MTL_PAT_IDX_WB_2WAY_COH, true);
    if (!lrcBo || !lrcBo->kernMap) {
        if (lrcBo) releaseBo(lrcBo);
        return nullptr;
    }

    /*
     * Dedicated 4 KiB Fence & Pipe-Control Scratch Page in GGTT (Linux xe_lrc.c: lrc->seqno_bo).
     * Must NEVER share PPHWSP [0x00..0x1FF]: Xe-LPG hardware writes CSB/context-status/timestamp
     * reports directly into PPHWSP + 0x80 (e.g. 0x00000400_04004A20), which would prematurely
     * satisfy (*q->seqnoCpu >= seqno) on multi-batch submissions.
     */
    ArcBo *fenceBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_SCANOUT,
                                    ARC_MTL_PAT_IDX_WB_2WAY_COH, true);
    if (!fenceBo || !fenceBo->kernMap) {
        if (fenceBo) releaseBo(fenceBo);
        releaseBo(lrcBo);
        return nullptr;
    }

    ArcExecQueue *q = IONewZero(ArcExecQueue, 1);
    if (!q) {
        releaseBo(fenceBo);
        releaseBo(lrcBo);
        return nullptr;
    }

    q->submitLock = IOLockAlloc();
    if (!q->submitLock) {
        releaseBo(fenceBo);
        releaseBo(lrcBo);
        IODelete(q, ArcExecQueue, 1);
        return nullptr;
    }

    uint8_t *cpuBase      = reinterpret_cast<uint8_t *>(lrcBo->kernMap->getVirtualAddress());
    uint8_t *fenceCpuBase = reinterpret_cast<uint8_t *>(fenceBo->kernMap->getVirtualAddress());
    q->vmId               = vm->vmId;
    q->engineClass        = engineClass;
    q->engineInstance     = engineInstance;
    q->mmioBase           = base;
    q->swCtxId            = static_cast<uint32_t>(OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&fNextSwCtxId))) & 0x7FFFu;
    if (q->swCtxId == 0) q->swCtxId = 1;
    q->lrcBo              = lrcBo;
    q->fenceBo            = fenceBo;
    q->ringGgttAddr       = lrcBo->ggttOffset;
    q->pphwspGgttAddr     = lrcBo->ggttOffset + pphwspOff;
    q->ctxImgGgttAddr     = lrcBo->ggttOffset + ctxImgOff;
    q->ringSize           = ringSize;
    q->ringTail           = 0;
    q->ringCpu            = reinterpret_cast<volatile uint32_t *>(cpuBase);
    q->ctxRegsCpu         = reinterpret_cast<volatile uint32_t *>(cpuBase + ctxImgOff);
    q->seqnoCpu           = reinterpret_cast<volatile uint64_t *>(fenceCpuBase + 0x00u);
    q->seqnoGgttAddr      = fenceBo->ggttOffset + 0x00u;
    q->invalScratchGgtt   = fenceBo->ggttOffset + 0x40u;
    q->pml4Phys           = vm->pml4->physAddr;
    *q->seqnoCpu          = 0ULL;

    /*
     * Populate Meteor Lake Xe-LPG (12.70/12.71) Engine Context Register State Image.
     * Matches Linux drivers/gpu/drm/i915/gt/intel_lrc.c: reg_offsets() for IP_VER >= (12, 55),
     * where BOTH mtl_rcs_offsets[] (RCS0) and dg2_xcs_offsets[] (BCS0) begin with LRI(15, POSTED)
     * including PRT_BB_STATE (0x120) and PRT_BB_STATE_UDW (0x124) at reg[0x1C..0x1F], followed
     * by NOP(1) at 0x20 and LRI(9, POSTED) at 0x21, terminated by MI_BATCH_BUFFER_END | BIT(0).
     */
    const bool isRcs = (base == ARC_RENDER_RING_BASE || base == ARC_COMPUTE0_RING_BASE);
    volatile uint32_t *reg = q->ctxRegsCpu;
    const uint32_t lriPostedCs = ARC_MI_LRI_FORCE_POSTED | ARC_MI_LRI_CS_MMIO;

    reg[0x00] = ARC_MI_NOOP;
    reg[0x01] = ARC_MI_LOAD_REGISTER_IMM(15u) | lriPostedCs;

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
    reg[0x1C] = base + 0x120u; /* PRT_BB_STATE (present on both RCS0 & BCS0 for IP >= 12.55) */
    reg[0x1D] = 0u;
    reg[0x1E] = base + 0x124u; /* PRT_BB_STATE_UDW */
    reg[0x1F] = 0u;
    reg[0x20] = ARC_MI_NOOP;

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
    const uint64_t pdp0Desc = arcEncodePpgttPde(q->pml4Phys);
    reg[0x30] = base + 0x274u; /* PDP0_UDW */
    reg[0x31] = (uint32_t)(pdp0Desc >> 32);
    reg[0x32] = base + 0x270u; /* PDP0_LDW */
    reg[0x33] = (uint32_t)(pdp0Desc & 0xFFFFFFFFULL);

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

    /*
     * Initial Ring Setup for Xe-LPG RCS0 (Linux drivers/gpu/drm/xe/xe_lrc.c & xe_wa.c):
     *   1. MI_ARB_ON_OFF(ENABLE)
     *   2. LRI(COMMON_SLICE_CHICKEN3, DISABLE_STATE_CACHE_PERF_FIX) — requested by Mesa ANV on Xe
     *   3. LRI_CS_MMIO(CS_DEBUG_MODE2, CONSTANT_BUFFER_OFFSET_DISABLE)
     *   4. Zeroed 3DSTATE_MESH_CONTROL — Wa_14019789679 (initial run uses RESTORE_INHIBIT=1)
     */
    static const struct { uint32_t instr; uint16_t numDw; } kXeHpgSvgState[] = {
        { ARC_GFXPIPE_3D_CMD(0x0, 0x15), 11 }, /* 3DSTATE_CONSTANT_VS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x19), 11 }, /* 3DSTATE_CONSTANT_HS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x1A), 11 }, /* 3DSTATE_CONSTANT_DS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x16), 11 }, /* 3DSTATE_CONSTANT_GS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x09), 69 }, /* 3DSTATE_VERTEX_ELEMENTS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x55),  5 }, /* 3DSTATE_VF_COMPONENT_PACKING */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x4A),  2 }, /* 3DSTATE_VF_SGVS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x56),  3 }, /* 3DSTATE_VF_SGVS_2 */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x10),  9 }, /* 3DSTATE_VS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x26),  2 }, /* 3DSTATE_BINDING_TABLE_POINTERS_VS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x2B),  2 }, /* 3DSTATE_SAMPLER_STATE_POINTERS_VS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x58),  3 }, /* 3DSTATE_URB_ALLOC_VS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x1E),  5 }, /* 3DSTATE_STREAMOUT */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x60),  8 }, /* 3DSTATE_SO_BUFFER_INDEX_0 */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x61),  8 }, /* 3DSTATE_SO_BUFFER_INDEX_1 */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x62),  8 }, /* 3DSTATE_SO_BUFFER_INDEX_2 */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x63),  8 }, /* 3DSTATE_SO_BUFFER_INDEX_3 */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x12),  4 }, /* 3DSTATE_CLIP */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x6C),  6 }, /* 3DSTATE_PRIMITIVE_REPLICATION */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x81),  2 }, /* 3DSTATE_CLIP_MESH */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x13),  4 }, /* 3DSTATE_SF */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x0F),  2 }, /* 3DSTATE_SCISSOR_STATE_POINTERS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x21),  2 }, /* 3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x50),  5 }, /* 3DSTATE_RASTER */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x6E),  4 }, /* 3DSTATE_TBIMR_TILE_PASS_INFO */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x52),  6 }, /* 3DSTATE_WM_HZ_OP */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x0D),  2 }, /* 3DSTATE_MULTISAMPLE */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x1B),  9 }, /* 3DSTATE_HS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x27),  2 }, /* 3DSTATE_BINDING_TABLE_POINTERS_HS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x2C),  2 }, /* 3DSTATE_SAMPLER_STATE_POINTERS_HS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x59),  3 }, /* 3DSTATE_URB_ALLOC_HS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7C),  3 }, /* 3DSTATE_TASK_CONTROL */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7D),  7 }, /* 3DSTATE_TASK_SHADER */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7E), 10 }, /* 3DSTATE_TASK_SHADER_DATA */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x80),  3 }, /* 3DSTATE_URB_ALLOC_TASK */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x1C),  5 }, /* 3DSTATE_TE */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x79),  2 }, /* 3DSTATE_TASK_REDISTRIB */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x1D), 11 }, /* 3DSTATE_DS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x28),  2 }, /* 3DSTATE_BINDING_TABLE_POINTERS_DS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x2D),  2 }, /* 3DSTATE_SAMPLER_STATE_POINTERS_DS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x5A),  3 }, /* 3DSTATE_URB_ALLOC_DS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x11), 10 }, /* 3DSTATE_GS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x29),  2 }, /* 3DSTATE_BINDING_TABLE_POINTERS_GS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x2E),  2 }, /* 3DSTATE_SAMPLER_STATE_POINTERS_GS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x5B),  3 }, /* 3DSTATE_URB_ALLOC_GS */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x77),  3 }, /* 3DSTATE_MESH_CONTROL */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7B), 10 }, /* 3DSTATE_MESH_SHADER_DATA */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7F),  3 }, /* 3DSTATE_URB_ALLOC_MESH */
        { ARC_GFXPIPE_3D_CMD(0x0, 0x7A),  8 }, /* 3DSTATE_MESH_SHADER */
    };

    q->isIdleKernelCtx = isIdleKernelCtx;
    q->holdsForceWake  = forceWakeGet();

    uint32_t l3sqcreg5Val = 0x00200000u | ARC_L3_PWM_TIMER_INIT_7F;
    uint32_t blitCctlVal  = (ARC_MTL_MOCS_UC_INDEX << 9) | (ARC_MTL_MOCS_UC_INDEX << 1);
    if (q->holdsForceWake) {
        if (isRcs) {
            /* Serialize temporary steering with reset/diagnostic MMIO and
             * restore the exact selector instead of clobbering its group and
             * instance fields. Reads themselves are always unicast. */
            IOLockLock(fEngineLock);
            const uint32_t savedMcrSelector = rd32(ARC_REG_MTL_MCR_SELECTOR);
            wr32(ARC_REG_MTL_MCR_SELECTOR, 0u);
            (void)rd32(ARC_REG_MTL_MCR_SELECTOR);
            l3sqcreg5Val = (rd32(ARC_REG_XEHP_L3SQCREG5) & ~ARC_L3_PWM_TIMER_INIT_VAL_MASK) | ARC_L3_PWM_TIMER_INIT_7F;
            wr32(ARC_REG_MTL_MCR_SELECTOR, savedMcrSelector);
            (void)rd32(ARC_REG_MTL_MCR_SELECTOR);
            IOLockUnlock(fEngineLock);
        } else if (base == ARC_BLT_RING_BASE) {
            blitCctlVal = (rd32(ARC_RING_BLIT_CCTL(base)) & ~0x7E7Eu) |
                          (ARC_MTL_MOCS_UC_INDEX << 9) | (ARC_MTL_MOCS_UC_INDEX << 1);
        }
    }

    uint32_t initDw = 0;
    q->ringCpu[initDw++] = ARC_MI_ARB_ON_OFF | ARC_MI_ARB_ENABLE;
    if (isRcs) {
        /*
         * Emit all Xe-LPG LRC context-saved workarounds & tunings (Linux xe_wa.c:lrc_was,
         * xe_tuning.c:lrc_tunings, xe_gt.c:emit_wa_job) plus privileged registers expected by
         * Mesa ANV genX_init_state.c in the privileged GGTT ring buffer so every LRC context image
         * (both user queues and fRcsIdleQueue) bakes in:
         *   - Wa_18019271663 (CACHE_MODE_1.MSAA_OPTIMIZATION_REDUC_DIS)
         *   - Wa_14019877138 (XEHP_PSS_CHICKEN.FD_END_COLLECT — Pixel Scoreboard Fragment-Done!)
         *   - Tuning: L3 cache (XEHP_L3SQCREG5 = 0x7F)
         *   - CONSTANT_BUFFERAddressOffsetDisable (CS_DEBUG_MODE2 BIT(4))
         */
        q->ringCpu[initDw++] = ARC_MI_LOAD_REGISTER_IMM(7u);
        q->ringCpu[initDw++] = ARC_REG_COMMON_SLICE_CHICKEN3;
        q->ringCpu[initDw++] = ARC_MASKED_ENABLE(ARC_DISABLE_STATE_CACHE_PERF_FIX);
        q->ringCpu[initDw++] = ARC_REG_CACHE_MODE_1;
        q->ringCpu[initDw++] = ARC_MASKED_ENABLE(ARC_MSAA_OPTIMIZATION_REDUC_DIS);
        q->ringCpu[initDw++] = ARC_REG_XEHP_PSS_CHICKEN;
        q->ringCpu[initDw++] = ARC_MASKED_ENABLE(ARC_FD_END_COLLECT);
        q->ringCpu[initDw++] = ARC_REG_XEHP_L3SQCREG5;
        q->ringCpu[initDw++] = l3sqcreg5Val;
        q->ringCpu[initDw++] = ARC_REG_CHICKEN_RASTER_2;
        q->ringCpu[initDw++] = ARC_MASKED_ENABLE(ARC_TBIMR_FAST_CLIP);
        q->ringCpu[initDw++] = ARC_REG_FF_MODE2;
        q->ringCpu[initDw++] = ARC_FF_MODE2_ANV_TIMERS;
        q->ringCpu[initDw++] = ARC_REG_CS_CHICKEN1;
        q->ringCpu[initDw++] = ARC_MASKED_DISABLE(ARC_CS_PREEMPT_3D_OBJECT_LEVEL);

        q->ringCpu[initDw++] = ARC_MI_LOAD_REGISTER_IMM(1u) | ARC_MI_LRI_CS_MMIO;
        q->ringCpu[initDw++] = 0x0D8u; /* CS_DEBUG_MODE2(RCS0): BIT(4) CONSTANT_BUFFERAddressOffsetDisable */
        q->ringCpu[initDw++] = ARC_MASKED_ENABLE(ARC_CS_DEBUG_MODE2_RCS_DEFAULT);

        /*
         * Wa_14019789679: Every RCS0 LRC (both user queues and fRcsIdleQueue) must initialize
         * SVG 3DSTATE headers on its initial RESTORE_INHIBIT=1 run so hardware Context-Save
         * writes valid 3DSTATE command headers instead of zeroed MI_NOOP dwords into LRC memory.
         */
        for (const auto &st : kXeHpgSvgState) {
            q->ringCpu[initDw++] = st.instr | (uint32_t)(st.numDw - 2u);
            for (uint16_t z = 1; z < st.numDw; z++) {
                q->ringCpu[initDw++] = 0u;
            }
        }
        /* Non-zero default 3DSTATE_DRAWING_RECTANGLE = {0, 0}..{65535, 65535} (matches Mesa ANV init_render_queue_state) */
        q->ringCpu[initDw++] = ARC_GFXPIPE_3D_CMD(0x1, 0x00) | 2u;
        q->ringCpu[initDw++] = 0u;
        q->ringCpu[initDw++] = 0xFFFFFFFFu;
        q->ringCpu[initDw++] = 0u;
    } else {
        /* BCS0 context-saved BLIT_CCTL default UC MOCS (Linux xe_hw_engine.c:352-360) */
        q->ringCpu[initDw++] = ARC_MI_LOAD_REGISTER_IMM(1u) | ARC_MI_LRI_CS_MMIO;
        q->ringCpu[initDw++] = 0x204u; /* BLIT_CCTL(0) */
        q->ringCpu[initDw++] = blitCctlVal;
    }
    if (initDw & 1u)
        q->ringCpu[initDw++] = ARC_MI_NOOP;
    q->initRingTail = initDw * 4u;
    q->ringTail     = q->initRingTail;
    reg[ARC_CTX_RING_HEAD_VAL] = 0u;
    reg[ARC_CTX_RING_TAIL_VAL] = q->ringTail;
    /* Flush all 76 KiB of lrcBo + 4 KiB fenceBo so zeroed WB cachelines never collide with HW Context-Save */
    clflushRange(cpuBase, totalSize);
    clflushRange(fenceCpuBase, 4096);

    return q;
}

void ArcXeService::destroyExecQueue(ArcExecQueue *q)
{
    if (!q) return;
    if (!q->faulted && q->seqnoCpu && q->lastSubmittedSeqno > *q->seqnoCpu) {
        uint64_t done = 0; bool to = false;
        (void)waitQueueSeqno(q, q->lastSubmittedSeqno, 500000000ULL, &done, &to);
    }
    if (!q->isIdleKernelCtx && fEngineLock) {
        IOLockLock(fEngineLock);
        if (fLastRcsQueue == q || fLastBcsQueue == q) {
            /*
            * Park the hardware engine onto its permanent kernel idle LRC BEFORE freeing q->lrcBo's
            * GGTT range, so CURRENT_LRCA never points at freed/reallocated GGTT memory.
            */
            if (forceWakeGet()) {
                (void)parkEngineOnIdleContextLocked(q->mmioBase, q);
                forceWakePut();
            }
            if (fLastRcsQueue == q) fLastRcsQueue = nullptr;
            if (fLastBcsQueue == q) fLastBcsQueue = nullptr;
        }
        IOLockUnlock(fEngineLock);
    }
    if (q->holdsForceWake) {
        forceWakePut();
        q->holdsForceWake = false;
    }
    if (q->fenceBo) {
        releaseBo(q->fenceBo);
        q->fenceBo  = nullptr;
        q->seqnoCpu = nullptr;
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

/* Parser progress only, never a completion fence. Keep these stores on a
 * separate cache line from the fence (0x00) and invalidate scratch (0x40). */
static inline void ringEmitProgress(ArcExecQueue *q, uint32_t slot, uint64_t seqno)
{
    ringEmitDword(q, ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_USE_GGTT | ARC_MI_SDI_QWORD_LEN);
    ringEmitDword(q, q->seqnoGgttAddr + 0x80u + slot * 8u);
    ringEmitDword(q, 0u);
    ringEmitDword(q, (uint32_t)seqno);
    ringEmitDword(q, (uint32_t)(seqno >> 32));
}

bool ArcXeService::parkEngineOnIdleContextLocked(uint32_t base, ArcExecQueue *prevQueue)
{
    ArcExecQueue *idleQ  = (base == ARC_BLT_RING_BASE) ? fBcsIdleQueue : fRcsIdleQueue;
    ArcExecQueue *&lastQ = (base == ARC_BLT_RING_BASE) ? fLastBcsQueue : fLastRcsQueue;
    if (!idleQ || !idleQ->lrcBo || !idleQ->fenceBo || !idleQ->seqnoCpu) {
        return false;
    }

    /* Ensure the previous context's trailing ring dwords retired to HEAD == TAIL before switching out */
    bool drained = false;
    for (uint32_t us = 0; us < 10000u; us += 5u) {
        /* HEAD also carries a wrap counter; compare address bits only. */
        if ((rd32(ARC_RING_HEAD(base)) & 0x001FFFFCu) ==
            (rd32(ARC_RING_TAIL(base)) & 0x001FFFF8u)) {
            drained = true;
            break;
        }
        IODelay(5);
    }
    if (!drained) {
        AXELOG("idle park: predecessor ring did not drain base=0x%x", base);
        return false;
    }

    /* Evict any CPU cachelines of prevQueue's LRC before hardware Context-Saves prevQueue */
    if (prevQueue && prevQueue->lrcBo && prevQueue->ringCpu) {
        clflushRange(prevQueue->ringCpu, prevQueue->lrcBo->size);
    }

    const uint64_t idleSeq = ++idleQ->lastSubmittedSeqno;
    if ((idleQ->ringSize - idleQ->ringTail) < 64u) {
        while (idleQ->ringTail != 0u) {
            ringEmitDword(idleQ, ARC_MI_NOOP);
        }
    }

    ringEmitDword(idleQ, ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_USE_GGTT | ARC_MI_SDI_QWORD_LEN);
    ringEmitDword(idleQ, idleQ->seqnoGgttAddr);
    ringEmitDword(idleQ, 0u);
    ringEmitDword(idleQ, (uint32_t)(idleSeq & 0xFFFFFFFFULL));
    ringEmitDword(idleQ, (uint32_t)(idleSeq >> 32));
    ringEmitDword(idleQ, ARC_MI_USER_INTERRUPT);
    if (idleQ->ringTail & 4u) {
        ringEmitDword(idleQ, ARC_MI_NOOP);
    }

    /* Hardware clears RESTORE_INHIBIT when saving the initialized idle LRC.
     * Preserve that image just as for user contexts. Forcing the bit on every
     * park inherited the outgoing user's 3D state under a different PPGTT. */
    if (!idleQ->hasSavedHwState)
        idleQ->ctxRegsCpu[ARC_CTX_CONTEXT_CONTROL_VAL] =
            ARC_MASKED_ENABLE(ARC_CTX_CTRL_INHIBIT_SYN_SWITCH | ARC_CTX_CTRL_RESTORE_INHIBIT);
    idleQ->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = idleQ->ringTail;
    clflushRange(idleQ->ringCpu, idleQ->lrcBo->size);

    /* Kernel idle context always uses SW_CTX_ID = 0 (Linux xe_execlist.c: port->lrc) */
    const uint64_t idleDesc = (uint64_t)(idleQ->pphwspGgttAddr & ~0xFFFu)
                            | ARC_LRC_DESC_VALID
                            | ARC_LRC_DESC_LEGACY_64B_PPGTT
                            | ARC_LRC_DESC_PRIVILEGE_PPGTT;

    wr32(ARC_RING_EXECLIST_SQ_LO(base), (uint32_t)(idleDesc & 0xFFFFFFFFULL));
    wr32(ARC_RING_EXECLIST_SQ_HI(base), (uint32_t)(idleDesc >> 32));
    wr32(ARC_RING_EXECLIST_CONTROL(base), ARC_EL_CTRL_LOAD);
    (void)rd32(ARC_RING_EXECLIST_STATUS_LO(base));

    bool parkedOk = false;
    for (uint32_t us = 0; us < 250000u; us += 5u) {
        if (*idleQ->seqnoCpu >= idleSeq) {
            parkedOk = true;
            break;
        }
        IODelay(5);
    }
    bool idleDrained = false;
    for (uint32_t us = 0; parkedOk && us < 10000u; us += 5u) {
        if ((rd32(ARC_RING_CURRENT_LRCA(base)) & ~0xFFFu) == (idleQ->pphwspGgttAddr & ~0xFFFu) &&
            (rd32(ARC_RING_HEAD(base)) & 0x001FFFFCu) ==
            (rd32(ARC_RING_TAIL(base)) & 0x001FFFF8u)) {
            idleDrained = true;
            break;
        }
        IODelay(5);
    }

    parkedOk = parkedOk && idleDrained;
    if (parkedOk) {
        if (prevQueue) {
            if (prevQueue->lrcBo && prevQueue->ringCpu) {
                clflushRange(prevQueue->ringCpu, prevQueue->lrcBo->size);
            }
            prevQueue->isLoadedOnHw    = false;
            prevQueue->hasSavedHwState = true;
            ++prevQueue->idleParkCount;
        }
        idleQ->isLoadedOnHw = true;
        lastQ = nullptr;
    }
    if (!parkedOk)
        AXELOG("idle park incomplete base=0x%x fence=%llu target=%llu", base,
               (unsigned long long)*idleQ->seqnoCpu, (unsigned long long)idleSeq);
    return parkedOk;
}

static inline void readEngineDiagMmio(const ArcXeService *svc, uint32_t b, ArcXeWaitQueueOut *d)
{
    if (!d) return;
    d->engine_status    = svc->rd32(ARC_RING_EXECLIST_STATUS_LO(b));
    d->engine_status_hi = svc->rd32(ARC_RING_EXECLIST_STATUS_HI(b));
    d->current_lrca     = svc->rd32(ARC_RING_CURRENT_LRCA(b));
    d->ring_head        = svc->rd32(ARC_RING_HEAD(b));
    d->ring_tail        = svc->rd32(ARC_RING_TAIL(b));
    d->ring_start       = svc->rd32(ARC_RING_START(b));
    d->ring_ctl         = svc->rd32(ARC_RING_CTL(b));
    d->acthd_lo         = svc->rd32(ARC_RING_ACTHD(b));
    d->acthd_hi         = svc->rd32(ARC_RING_ACTHD_UDW(b));
    d->ipehr            = svc->rd32(ARC_RING_IPEHR(b));
    d->esr              = svc->rd32(ARC_RING_ESR(b));
    d->instdone         = svc->rd32(ARC_RING_INSTDONE(b));
    d->mi_mode          = svc->rd32(ARC_RING_MI_MODE(b));
}

static inline void readRcsSubunitDiag(const ArcXeService *svc, uint32_t b,
                                      uint32_t &outDbg2, uint32_t &outGeom,
                                      uint32_t &outSc, uint32_t &outSamp, uint32_t &outRow)
{
    outDbg2 = svc->rd32(ARC_RING_CS_DEBUG_MODE2(b));
    if (b != ARC_RENDER_RING_BASE) {
        outGeom = outSc = outSamp = outRow = 0u;
        return;
    }
    svc->wr32(ARC_REG_MTL_MCR_SELECTOR, 0u);
    svc->wr32(ARC_REG_SF_MCR_SELECTOR,  0u);
    svc->wr32(ARC_REG_MCR_SELECTOR,     0u);
    (void)svc->rd32(ARC_REG_MTL_MCR_SELECTOR);
    outGeom = svc->rd32(ARC_REG_XEHPG_INSTDONE_GEOM_SVG);
    outSc   = svc->rd32(ARC_REG_XEHPG_SC_INSTDONE);
    outSamp = svc->rd32(ARC_REG_SAMPLER_INSTDONE);
    outRow  = svc->rd32(ARC_REG_ROW_INSTDONE);
    svc->wr32(ARC_REG_MTL_MCR_SELECTOR, ARC_MCR_MULTICAST);
    svc->wr32(ARC_REG_SF_MCR_SELECTOR,  ARC_MCR_MULTICAST);
    svc->wr32(ARC_REG_MCR_SELECTOR,     ARC_MCR_MULTICAST);
    (void)svc->rd32(ARC_REG_MTL_MCR_SELECTOR);
}

IOReturn ArcXeService::submitBatch(ArcExecQueue *q, uint64_t batchGpuVa, uint64_t *outSeqno)
{
    if (!q || !q->lrcBo || !q->fenceBo || fStopping) return kIOReturnBadArgument;

    IOLockLock(q->submitLock);
    IOLockLock(fEngineLock);

    if (q->faulted) {
        IOLockUnlock(fEngineLock);
        IOLockUnlock(q->submitLock);
        return kIOReturnNotResponding;
    }

    if (!forceWakeGet()) {
        IOLockUnlock(fEngineLock);
        IOLockUnlock(q->submitLock);
        return kIOReturnDeviceError;
    }

    /*
     * Direct Execlist Multi-Submission & Lite-Restore / Context-Switch Lifecycle (Linux xe_execlist.c:88-115):
     * 1. Wait for any in-flight batch on this physical engine (including prior async batches on the
     *    same queue, such as Mesa ANV's init_render_queue_state) to complete in dedicated fenceBo,
     *    and wait for trailing ring instructions (MI_USER_INTERRUPT / MI_ARB_CHECK) to retire to HEAD == TAIL.
     * 2. RCS transitions through the permanent idle context once drained, like
     *    xe_execlist_port_idle. This polling backend has no completion IRQ worker
     *    to do it earlier. Require proof of idle execution before marking the
     *    user context saved. BCS retains its existing lite-restore path.
     * 3. If switching between two different user queues (`lastQ != q`), allocate a fresh `swCtxId`
     *    and submit `q` directly so hardware Context-Saves `lastQ` into `lastQ->lrcBo` and Context-Restores `q`.
     */
    ArcExecQueue *idleQ  = (q->mmioBase == ARC_BLT_RING_BASE) ? fBcsIdleQueue : fRcsIdleQueue;
    ArcExecQueue *&lastQ = (q->mmioBase == ARC_BLT_RING_BASE) ? fLastBcsQueue : fLastRcsQueue;
    if (lastQ && lastQ->seqnoCpu) {
        ArcExecQueue *prevQ = lastQ;
        const uint64_t prevTarget = prevQ->lastSubmittedSeqno;
        for (uint32_t us = 0; us < 5000000u && *prevQ->seqnoCpu < prevTarget; us += 5u) {
            IODelay(5);
        }
        const bool prevDone = (*prevQ->seqnoCpu >= prevTarget);
        bool ready = false;
        if (prevDone) {
            if (q->mmioBase == ARC_RENDER_RING_BASE) {
                ready = parkEngineOnIdleContextLocked(q->mmioBase, prevQ);
            } else {
                for (uint32_t us = 0; us < 10000u; us += 5u) {
                    if ((rd32(ARC_RING_HEAD(q->mmioBase)) & 0x001FFFFCu) ==
                        (rd32(ARC_RING_TAIL(q->mmioBase)) & 0x001FFFF8u)) {
                        ready = true;
                        break;
                    }
                    IODelay(5);
                }
            }
        }
        if (ready) {
            /* If switching to a different queue, evict prevQ's LRC cachelines before HW Context-Saves prevQ */
            if (prevQ != q && prevQ->lrcBo && prevQ->ringCpu) {
                clflushRange(prevQ->ringCpu, prevQ->lrcBo->size);
            }
        } else {
            ArcXeWaitQueueOut snap = {};
            uint32_t dbg2 = 0, geomId = 0, scId = 0, sampId = 0, rowId = 0;
            readEngineDiagMmio(this, q->mmioBase, &snap);
            readRcsSubunitDiag(this, q->mmioBase, dbg2, geomId, scId, sampId, rowId);
            AXELOG("submitBatch predecessor stall (prevQ=%u target=%llu cur=%llu): "
                   "EL_STATUS=0x%08x_%08x LRCA=0x%08x HEAD=0x%x TAIL=0x%x START=0x%08x CTL=0x%08x "
                   "ACTHD=0x%08x_%08x IPEHR=0x%08x ESR=0x%08x INSTDONE=0x%08x DBG2=0x%08x "
                   "GEOM=0x%08x SC=0x%08x SAMP=0x%08x ROW=0x%08x MI_MODE=0x%08x -> resetting engine",
                   prevQ->queueId, (unsigned long long)prevTarget, (unsigned long long)(*prevQ->seqnoCpu),
                   snap.engine_status_hi, snap.engine_status, snap.current_lrca,
                   snap.ring_head, snap.ring_tail, snap.ring_start, snap.ring_ctl,
                   snap.acthd_hi, snap.acthd_lo, snap.ipehr, snap.esr, snap.instdone, dbg2,
                   geomId, scId, sampId, rowId, snap.mi_mode);

            ArcBo *hwsp = nullptr;
            (void)engineMmioBase(q->engineClass, q->engineInstance, &hwsp);
            prevQ->faulted = true;
            const bool resetOk = hwsp && resetEngineHw(q->mmioBase, hwsp);
            /* Discarded work is not completed work. Keep the GPU fence value. */
            prevQ->isLoadedOnHw    = false;
            prevQ->hasSavedHwState = false;
            if (prevQ->ctxRegsCpu) {
                prevQ->ctxRegsCpu[ARC_CTX_CONTEXT_CONTROL_VAL] =
                    ARC_MASKED_ENABLE(ARC_CTX_CTRL_INHIBIT_SYN_SWITCH | ARC_CTX_CTRL_RESTORE_INHIBIT);
                prevQ->ctxRegsCpu[ARC_CTX_RING_HEAD_VAL] = prevQ->ringTail;
                prevQ->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = prevQ->ringTail;
                clflushRange(prevQ->ctxRegsCpu, 0x140);
            }
            lastQ = nullptr;
            if (!resetOk || q->faulted) {
                forceWakePut();
                IOLockUnlock(fEngineLock);
                IOLockUnlock(q->submitLock);
                return kIOReturnNotResponding;
            }
        }
    }

    const uint64_t seqno = ++q->lastSubmittedSeqno;
    const uint32_t invalScratchGgtt = q->invalScratchGgtt;

    /* Never allow a multi-dword CS instruction packet to straddle the end of the ring buffer */
    if ((q->ringSize - q->ringTail) < 256u) {
        while (q->ringTail != 0u) {
            ringEmitDword(q, ARC_MI_NOOP);
        }
    }

    q->lastBatchGpuVa = batchGpuVa;
    q->submitRingStart = q->ringTail;
    ringEmitProgress(q, 0u, seqno);

    /* Wa_14016712196 also precedes INVALIDATE on Meteor Lake, not only the
     * end-of-batch flush. See i915 gen8_engine_cs.c: gen12_emit_flush_rcs /
     * mtl_dummy_pipe_control. Emit before disabling the preparser, with no
     * post-sync operation or extra stall bits on this depth-only packet. */
    if (q->engineClass == kArcXeEngineClassRender || q->engineClass == kArcXeEngineClassCompute) {
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6));
        ringEmitDword(q, ARC_PC_DEPTH_CACHE_FLUSH);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
    }

    /* 1. Pre-batch CS Preparser Disable + CS_DEBUG_MODE2 Guard + Read Cache Invalidate.
     * Page-table updates already finish synchronous MMIO invalidation of every
     * active engine in vmBindBo/allocGgttAndBind before returning success.
     * Do not also request a PIPE_CONTROL TLB invalidate on every submission.
     * Xe likewise sets that bit only for jobs with ring_ops_flush_tlb. */
    ringEmitDword(q, ARC_MI_PREPARSER_DISABLE(true));
    if (q->engineClass == kArcXeEngineClassRender || q->engineClass == kArcXeEngineClassCompute) {
        /* Preserve absolute constant-buffer addressing on MTL. Do not inject
         * the IP 12.00..12.10 state-cache workaround (BIT(6)) here. */
        ringEmitDword(q, ARC_MI_LOAD_REGISTER_IMM(1u) | ARC_MI_LRI_CS_MMIO);
        ringEmitDword(q, 0x0D8u);
        ringEmitDword(q, ARC_MASKED_ENABLE(ARC_CS_DEBUG_MODE2_RCS_DEFAULT));

        uint32_t pcFlags0 = ARC_PC0_L3_RO_CACHE_INVALIDATE;
        uint32_t pcFlags1 = ARC_PC_CS_STALL |
                            ARC_PC_CMD_CACHE_INVALIDATE | ARC_PC_INST_CACHE_INVALIDATE |
                            ARC_PC_TEX_CACHE_INVALIDATE | ARC_PC_VF_CACHE_INVALIDATE |
                            ARC_PC_CONST_CACHE_INVALIDATE | ARC_PC_STATE_CACHE_INVALIDATE |
                            ARC_PC_GLOBAL_GTT | ARC_PC_QW_WRITE;
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6) | pcFlags0);
        ringEmitDword(q, pcFlags1);
        ringEmitDword(q, invalScratchGgtt);
        ringEmitDword(q, 0u);
        ringEmitDword(q, (uint32_t)seqno);
        ringEmitDword(q, (uint32_t)(seqno >> 32));
    } else {
        ringEmitDword(q, ARC_MI_FLUSH_DW | ARC_MI_FLUSH_DW_INV_TLB |
                         ARC_MI_FLUSH_DW_OP_STOREDW | ARC_MI_FLUSH_DW_IMM_LEN);
        ringEmitDword(q, invalScratchGgtt | ARC_MI_FLUSH_DW_USE_GGTT);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
    }
    ringEmitDword(q, ARC_MI_PREPARSER_DISABLE(false));
    ringEmitProgress(q, 1u, seqno);

    /* 2. Jump to Userspace 48-bit PPGTT Batch Buffer */
    q->submitBatchStart = q->ringTail;
    ringEmitDword(q, ARC_MI_BATCH_BUFFER_START_PPGTT);
    ringEmitDword(q, (uint32_t)(batchGpuVa & 0xFFFFFFFFULL));
    ringEmitDword(q, (uint32_t)(batchGpuVa >> 32));
    q->submitBatchEnd = q->ringTail;
    ringEmitProgress(q, 2u, seqno);

    /* 3. Post-batch Non-Preemptible Cache Flush + Fence Seqno Write to GGTT fenceBo (xe_ring_ops.c:489-500) */
    ringEmitDword(q, ARC_MI_ARB_ON_OFF | ARC_MI_ARB_DISABLE);
    if (q->engineClass == kArcXeEngineClassRender || q->engineClass == kArcXeEngineClassCompute) {
        /*
         * 3a. Wa_14016712196: separate depth-only flush, matching xe_ring_ops.c.
         * Wa_1409600907's additional depth stall is only for IP12.00..12.50,
         * not MTL12.70..12.74 (xe_wa_oob.rules).
         * Do not add a post-sync write to this preliminary packet.
         */
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6));
        ringEmitDword(q, ARC_PC_DEPTH_CACHE_FLUSH);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);

        /* 3b. Match Xe's MTL HDC + Tile + RT + Depth + DC flush with CS stall.
         * Pixel-scoreboard stall belongs to BTI changes, not this fence flush. */
        const uint32_t flush1 = ARC_PC_CS_STALL |
                                ARC_PC_FLUSH_ENABLE | ARC_PC_DC_FLUSH_ENABLE |
                                ARC_PC_TILE_CACHE_FLUSH | ARC_PC_RT_CACHE_FLUSH |
                                ARC_PC_DEPTH_CACHE_FLUSH;
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6) | ARC_PC0_HDC_PIPELINE_FLUSH);
        ringEmitDword(q, flush1);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);

        /* 3c. Post-sync 64-bit seqno write into GGTT fenceBo (PIPE_CONTROL_GLOBAL_GTT_IVB in dw1!) */
        q->submitFlushEnd = q->ringTail;
        ringEmitDword(q, ARC_GFX_OP_PIPE_CONTROL(6));
        ringEmitDword(q, ARC_PC_GLOBAL_GTT | ARC_PC_QW_WRITE | ARC_PC_FLUSH_ENABLE | ARC_PC_CS_STALL);
        ringEmitDword(q, q->seqnoGgttAddr);
        ringEmitDword(q, 0u);
        ringEmitDword(q, (uint32_t)(seqno & 0xFFFFFFFFULL));
        ringEmitDword(q, (uint32_t)(seqno >> 32));

        /* PIPE_CONTROL is the sole RCS completion writer, as in xe_ring_ops.
         * An extra MI store must not independently signal this fence. */
    } else {
        /* Blitter engine: 4-dword MI_FLUSH_DW write-cache flush + 64-bit GGTT seqno store */
        ringEmitDword(q, ARC_MI_FLUSH_DW | ARC_MI_FLUSH_DW_OP_STOREDW | ARC_MI_FLUSH_DW_IMM_LEN);
        ringEmitDword(q, invalScratchGgtt | ARC_MI_FLUSH_DW_USE_GGTT);
        ringEmitDword(q, 0u);
        ringEmitDword(q, 0u);

        q->submitFlushEnd = q->ringTail;
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

    q->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = q->ringTail;

    if (lastQ == q && q->isLoadedOnHw) {
        /*
         * Hardware Execlist Lite Restore (Linux xe_execlist.c:99-110):
         * Same context is already active on CURRENT_LRCA. Keep q->swCtxId unchanged, flush ring +
         * context header, and reload ELSP so CS takes RING_TAIL from the LRC image
         * without any 52 KiB 3D Context Save/Restore.
         */
        clflushRange(q->ringCpu, q->ringSize);
        clflushRange(q->ctxRegsCpu, 0x140);
        /* Publish the new tail only through EL_CTRL_LOAD, as xe_execlist does.
         * A direct MMIO tail write can start execution before that reload. */

        const uint64_t lrcDesc = (uint64_t)(q->pphwspGgttAddr & ~0xFFFu)
                               | ARC_LRC_DESC_VALID
                               | ARC_LRC_DESC_LEGACY_64B_PPGTT
                               | ARC_LRC_DESC_PRIVILEGE_PPGTT
                               | ((uint64_t)q->swCtxId << ARC_LRC_DESC_XEHP_SW_CTX_SHIFT);

        wr32(ARC_RING_EXECLIST_SQ_LO(q->mmioBase), (uint32_t)(lrcDesc & 0xFFFFFFFFULL));
        wr32(ARC_RING_EXECLIST_SQ_HI(q->mmioBase), (uint32_t)(lrcDesc >> 32));
        wr32(ARC_RING_EXECLIST_CONTROL(q->mmioBase), ARC_EL_CTRL_LOAD);
        (void)rd32(ARC_RING_EXECLIST_STATUS_LO(q->mmioBase));
    } else {
        /*
         * Full Context Load / Context Switch between distinct queues (Linux xe_execlist.c:99-110):
         * Hardware automatically clears RESTORE_INHIBIT in CTX_CONTEXT_CONTROL when Context-Saving
         * a queue, so never clobber reg[0x03..0x05] from the CPU when hasSavedHwState is true.
         */
        ArcExecQueue *switchedOutQ = (lastQ && lastQ != q) ? lastQ : nullptr;
        if (!q->hasSavedHwState) {
            const uint64_t pdp0Desc = arcEncodePpgttPde(q->pml4Phys);
            q->ctxRegsCpu[ARC_CTX_PDP0_UDW_VAL] = (uint32_t)(pdp0Desc >> 32);
            q->ctxRegsCpu[ARC_CTX_PDP0_LDW_VAL] = (uint32_t)(pdp0Desc & 0xFFFFFFFFULL);
        }
        clflushRange(q->ringCpu, q->lrcBo->size);

        uint32_t nextCtxId = static_cast<uint32_t>(OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&fNextSwCtxId))) & 0x7FFFu;
        if (nextCtxId == 0u) {
            nextCtxId = static_cast<uint32_t>(OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&fNextSwCtxId))) & 0x7FFFu;
            if (nextCtxId == 0u) nextCtxId = 1u;
        }
        q->swCtxId = nextCtxId;

        const uint64_t lrcDesc = (uint64_t)(q->pphwspGgttAddr & ~0xFFFu)
                               | ARC_LRC_DESC_VALID
                               | ARC_LRC_DESC_LEGACY_64B_PPGTT
                               | ARC_LRC_DESC_PRIVILEGE_PPGTT
                               | ((uint64_t)q->swCtxId << ARC_LRC_DESC_XEHP_SW_CTX_SHIFT);

        wr32(ARC_RING_EXECLIST_SQ_LO(q->mmioBase), (uint32_t)(lrcDesc & 0xFFFFFFFFULL));
        wr32(ARC_RING_EXECLIST_SQ_HI(q->mmioBase), (uint32_t)(lrcDesc >> 32));
        wr32(ARC_RING_EXECLIST_CONTROL(q->mmioBase), ARC_EL_CTRL_LOAD);
        (void)rd32(ARC_RING_EXECLIST_STATUS_LO(q->mmioBase));

        if (switchedOutQ) {
            for (uint32_t us = 0; us < 10000u; us += 5u) {
                if ((rd32(ARC_RING_CURRENT_LRCA(q->mmioBase)) & ~0xFFFu) == (q->pphwspGgttAddr & ~0xFFFu))
                    break;
                IODelay(5);
            }
            if (switchedOutQ->lrcBo && switchedOutQ->ringCpu) {
                clflushRange(switchedOutQ->ringCpu, switchedOutQ->lrcBo->size);
            }
            switchedOutQ->isLoadedOnHw    = false;
            switchedOutQ->hasSavedHwState = true;
        }
        if (idleQ && idleQ->isLoadedOnHw) {
            idleQ->isLoadedOnHw = false;
            idleQ->hasSavedHwState = true;
        }
    }
    q->isLoadedOnHw = true;
    lastQ = q;

    forceWakePut();
    IOLockUnlock(fEngineLock);
    IOLockUnlock(q->submitLock);

    if (outSeqno) *outSeqno = seqno;
    return kIOReturnSuccess;
}

/* Called under fEngineLock and forcewake, before resetting the engine. */
static void captureTimeoutDetail(const ArcXeService *svc, const ArcExecQueue *q,
                                 const ArcXeWaitQueueOut &snap, ArcXeTimeoutDetail *d,
                                 const ArcExecQueue *idle = nullptr)
{
    if (!d) return;
    bzero(d, sizeof(*d));
    const uint32_t selector = svc->rd32(ARC_REG_MTL_MCR_SELECTOR);
    svc->wr32(ARC_REG_MTL_MCR_SELECTOR, 0u); /* primary GAM aggregates faults */
    (void)svc->rd32(ARC_REG_MTL_MCR_SELECTOR);
    d->fault_reg = svc->rd32(ARC_REG_XEHP_RING_FAULT);
    d->fault_data0 = svc->rd32(ARC_REG_XEHP_FAULT_TLB_DATA0);
    d->fault_data1 = svc->rd32(ARC_REG_XEHP_FAULT_TLB_DATA1);
    if (q->mmioBase == ARC_RENDER_RING_BASE) {
        auto &h = d->hw;
        h.cmd_cctl = svc->rd32(ARC_RING_CMD_CCTL(q->mmioBase));
        h.gfx_mode = svc->rd32(ARC_RING_GFX_MODE(q->mmioBase));
        h.cs_chicken1 = svc->rd32(ARC_REG_CS_CHICKEN1);
        h.l3_alloc = svc->rd32(ARC_REG_L3ALLOC);
        h.l3_sqcreg5 = svc->rd32(ARC_REG_XEHP_L3SQCREG5);
        h.sampler_mode = svc->rd32(ARC_REG_SAMPLER_MODE);
        h.rcc = svc->rd32(ARC_REG_COMMON_SLICE_CHICKEN3);
        h.pss_chicken = svc->rd32(ARC_REG_XEHP_PSS_CHICKEN);
        h.sc_extra = svc->rd32(ARC_REG_SC_INSTDONE_EXTRA);
        h.sc_extra2 = svc->rd32(ARC_REG_SC_INSTDONE_EXTRA2);
        for (uint32_t i = 0; i < 5; ++i) h.pat[i] = svc->rd32(ARC_REG_PAT_INDEX(i));
        h.mocs1 = svc->rd32(ARC_REG_GLOBAL_MOCS(1));
        h.mocs9 = svc->rd32(ARC_REG_GLOBAL_MOCS(9));
        h.lncf0 = svc->rd32(ARC_REG_LNCFCMOCS(0));
        h.lncf4 = svc->rd32(ARC_REG_LNCFCMOCS(4));
        h.version = 1;
    }
    svc->wr32(ARC_REG_MTL_MCR_SELECTOR, selector);
    (void)svc->rd32(ARC_REG_MTL_MCR_SELECTOR);
    /* HEAD describes the active LRCA, which may still be the parking context.
     * Never index the waiting user's ring with an unrelated hardware HEAD. */
    const uint32_t lrca = snap.current_lrca & ~0xfffu;
    const ArcExecQueue *active = nullptr;
    if (lrca == q->pphwspGgttAddr && snap.ring_start == q->ringGgttAddr)
        active = q;
    else if (idle && lrca == idle->pphwspGgttAddr && snap.ring_start == idle->ringGgttAddr)
        active = idle;
    if (active && active->ringCpu && active->ringSize >= 64u) {
        d->ring_offset = (snap.ring_head - 32u) & (active->ringSize - 1u) & ~3u;
        d->ring_count = 16;
        for (uint32_t i = 0; i < d->ring_count; ++i)
            d->ring_dw[i] = active->ringCpu[((d->ring_offset + i * 4u) & (active->ringSize - 1u)) / 4u];
    }
    if (q->ctxRegsCpu) {
        d->context_control = q->ctxRegsCpu[ARC_CTX_CONTEXT_CONTROL_VAL];
        d->context_head = q->ctxRegsCpu[ARC_CTX_RING_HEAD_VAL];
        d->context_tail = q->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL];
    }
    d->idle_park_count = q->idleParkCount;
    d->valid = 1;
}

IOReturn ArcXeService::waitQueueSeqno(ArcExecQueue *q, uint64_t targetSeqno, uint64_t timeoutNs,
                                      uint64_t *outCompleted, bool *outTimedOut,
                                      ArcXeWaitQueueOut *outDiag,
                                      bool resetOnTimeout,
                                      uint32_t *outSubunitDiag5, uint32_t *outProgress,
                                      ArcXeTimeoutDetail *outDetail)
{
    if (!q || !q->seqnoCpu) return kIOReturnBadArgument;
    if (q->faulted) return kIOReturnNotResponding;
    if (targetSeqno == 0) targetSeqno = q->lastSubmittedSeqno;

    if (*q->seqnoCpu >= targetSeqno) {
        const uint64_t cur = *q->seqnoCpu;
        if (outCompleted) *outCompleted = cur;
        if (outTimedOut)  *outTimedOut  = false;
        if (outDiag) {
            bzero(outDiag, sizeof(*outDiag));
            outDiag->completed_seqno = cur;
            outDiag->timed_out       = 0u;
            if (forceWakeGet()) {
                readEngineDiagMmio(this, q->mmioBase, outDiag);
                forceWakePut();
            }
        }
        return kIOReturnSuccess;
    }

    const uint64_t startAbs = mach_absolute_time();
    uint32_t spins = 0;

    for (;;) {
        if (q->faulted) return kIOReturnNotResponding;
        const uint64_t cur = *q->seqnoCpu;
        if (cur >= targetSeqno) {
            if (outCompleted) *outCompleted = cur;
            if (outTimedOut)  *outTimedOut  = false;
            if (outDiag) {
                bzero(outDiag, sizeof(*outDiag));
                outDiag->completed_seqno = cur;
                outDiag->timed_out       = 0u;
                if (forceWakeGet()) {
                    readEngineDiagMmio(this, q->mmioBase, outDiag);
                    forceWakePut();
                }
            }
            return kIOReturnSuccess;
        }

        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - startAbs, &elapsedNs);
        if (elapsedNs >= timeoutNs) {
            /* Intermediate 5 ms polling slices from kArcXeMethodSyncobjWait must NEVER reset the engine! */
            if (!resetOnTimeout && !outDiag) {
                if (outCompleted) *outCompleted = cur;
                if (outTimedOut)  *outTimedOut  = true;
                return kIOReturnSuccess;
            }

            ArcXeWaitQueueOut snap = {};
            snap.completed_seqno = cur;
            snap.timed_out       = 1u;

            IOLockLock(fEngineLock);
            /* A concurrent submit may have finished this fence or reset the
             * engine while we acquired the lock. Never reset a newer batch. */
            if (q->faulted || *q->seqnoCpu >= targetSeqno) {
                const bool failed = q->faulted;
                IOLockUnlock(fEngineLock);
                if (failed) return kIOReturnNotResponding;
                continue;
            }
            if (forceWakeGet()) {
                const uint32_t b = q->mmioBase;
                uint32_t dbg2 = 0, geomId = 0, scId = 0, sampId = 0, rowId = 0;
                readEngineDiagMmio(this, b, &snap);
                readRcsSubunitDiag(this, b, dbg2, geomId, scId, sampId, rowId);
                captureTimeoutDetail(this, q, snap, outDetail,
                                     b == ARC_RENDER_RING_BASE ? fRcsIdleQueue : fBcsIdleQueue);
                if (outProgress) {
                    *outProgress = ARCXE_PROGRESS_VALID |
                        (q->seqnoCpu[16] == targetSeqno ? ARCXE_PROGRESS_BEGIN : 0u) |
                        (q->seqnoCpu[17] == targetSeqno ? ARCXE_PROGRESS_BEFORE_BB : 0u) |
                        (q->seqnoCpu[18] == targetSeqno ? ARCXE_PROGRESS_RETURNED : 0u) |
                        (q->seqnoCpu[8] == targetSeqno ? ARCXE_PROGRESS_INVALIDATED : 0u);
                }
                if (outSubunitDiag5) {
                    outSubunitDiag5[0] = dbg2;
                    outSubunitDiag5[1] = geomId;
                    outSubunitDiag5[2] = scId;
                    outSubunitDiag5[3] = sampId;
                    outSubunitDiag5[4] = rowId;
                }
                if (resetOnTimeout) {
                    AXELOG("ring progress q=%u submitted=%llu batch=0x%llx "
                           "offsets start=0x%x bb=0x%x return=0x%x flushed=0x%x tail=0x%x "
                           "GPU parser begin=%llu beforeBB=%llu returned=%llu invalidate=%llu fence=%llu",
                           q->queueId, (unsigned long long)q->lastSubmittedSeqno,
                           (unsigned long long)q->lastBatchGpuVa,
                           q->submitRingStart, q->submitBatchStart, q->submitBatchEnd,
                           q->submitFlushEnd, q->ringTail,
                           (unsigned long long)q->seqnoCpu[16],
                           (unsigned long long)q->seqnoCpu[17],
                           (unsigned long long)q->seqnoCpu[18],
                           (unsigned long long)q->seqnoCpu[8],
                           (unsigned long long)*q->seqnoCpu);
                    AXELOG("waitQueueSeqno TIMEOUT (q=%u target=%llu cur=%llu): "
                           "EL_STATUS=0x%08x_%08x LRCA=0x%08x HEAD=0x%x TAIL=0x%x START=0x%08x CTL=0x%08x "
                           "ACTHD=0x%08x_%08x IPEHR=0x%08x ESR=0x%08x INSTDONE=0x%08x DBG2=0x%08x "
                           "GEOM=0x%08x SC=0x%08x SAMP=0x%08x ROW=0x%08x MI_MODE=0x%08x -> resetting engine",
                           q->queueId, (unsigned long long)targetSeqno, (unsigned long long)cur,
                           snap.engine_status_hi, snap.engine_status,
                           snap.current_lrca,
                           snap.ring_head, snap.ring_tail,
                           snap.ring_start, snap.ring_ctl,
                           snap.acthd_hi, snap.acthd_lo,
                           snap.ipehr, snap.esr, snap.instdone, dbg2,
                           geomId, scId, sampId, rowId, snap.mi_mode);

                    ArcBo *hwsp = nullptr;
                    (void)engineMmioBase(q->engineClass, q->engineInstance, &hwsp);
                    q->faulted = true;
                    if (hwsp) {
                        (void)resetEngineHw(b, hwsp);
                    }
                    /* Leave seqnoCpu at the last genuinely completed batch. */
                    q->isLoadedOnHw    = false;
                    q->hasSavedHwState = false;
                    if (q->ctxRegsCpu) {
                        q->ctxRegsCpu[ARC_CTX_CONTEXT_CONTROL_VAL] =
                            ARC_MASKED_ENABLE(ARC_CTX_CTRL_INHIBIT_SYN_SWITCH | ARC_CTX_CTRL_RESTORE_INHIBIT);
                        q->ctxRegsCpu[ARC_CTX_RING_HEAD_VAL] = q->ringTail;
                        q->ctxRegsCpu[ARC_CTX_RING_TAIL_VAL] = q->ringTail;
                        clflushRange(q->ctxRegsCpu, 0x140);
                    }
                    ArcExecQueue *&lastQ = (b == ARC_BLT_RING_BASE) ? fLastBcsQueue : fLastRcsQueue;
                    if (lastQ == q) {
                        lastQ = nullptr;
                    }
                }
                forceWakePut();
            }
            IOLockUnlock(fEngineLock);

            if (outDiag)      *outDiag      = snap;
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
