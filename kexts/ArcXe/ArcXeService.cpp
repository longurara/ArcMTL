/*
 * ArcXeService.cpp — PCI Bring-up, Forcewake, PAT, Topology & Kernel ABI for Meteor Lake Xe-LPG (8086:7D55)
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#include "ArcXeService.h"
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSArray.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/sysctl.h>

OSDefineMetaClassAndStructors(ArcXeService, IOService)

static uint32_t popcount32(uint32_t x)
{
    uint32_t c = 0;
    while (x) {
        x &= (x - 1u);
        c++;
    }
    return c;
}

/* ============================================================================
 * 1. Forcewake, Topology, Clocks, PAT & Global TLB Invalidation
 * ============================================================================ */

bool ArcXeService::forceWakeGet()
{
    if (!fMmio || !fFwLock) return false;

    IOLockLock(fFwLock);
    if (fFwRefCount++ > 0) {
        IOLockUnlock(fFwLock);
        return true;
    }

    /* Wake Primary GT0 Domain + Render Domain (masked register writes) */
    wr32(ARC_REG_FORCEWAKE_GT, ARC_FW_KERNEL_ENABLE);
    wr32(ARC_REG_FORCEWAKE_RENDER, ARC_FW_KERNEL_ENABLE);

    bool gtOk = false;
    bool renderOk = false;
    for (uint32_t us = 0; us < 50000; us += 10) {
        if (!gtOk && (rd32(ARC_REG_FORCEWAKE_ACK_GT_MTL) & ARC_FW_KERNEL_BIT))
            gtOk = true;
        if (!renderOk && (rd32(ARC_REG_FORCEWAKE_ACK_RENDER) & ARC_FW_KERNEL_BIT))
            renderOk = true;
        if (gtOk && renderOk)
            break;
        IODelay(10);
    }

    /* Best-effort wake standalone Media GT1 (Xe-LPM+ at +0x380000) */
    gt1Wr32(ARC_REG_FORCEWAKE_GT, ARC_FW_KERNEL_ENABLE);
    for (uint32_t us = 0; us < 5000; us += 10) {
        if (gt1Rd32(ARC_REG_FORCEWAKE_ACK_GT_MTL) & ARC_FW_KERNEL_BIT)
            break;
        IODelay(10);
    }

    if (!gtOk) {
        AXELOG("forceWakeGet: GT0 forcewake ACK timeout (ACK_GT=0x%x ACK_RENDER=0x%x)",
               rd32(ARC_REG_FORCEWAKE_ACK_GT_MTL), rd32(ARC_REG_FORCEWAKE_ACK_RENDER));
    }

    IOLockUnlock(fFwLock);
    return true;
}

void ArcXeService::forceWakePut()
{
    if (!fMmio || !fFwLock) return;

    IOLockLock(fFwLock);
    if (fFwRefCount <= 0) {
        fFwRefCount = 0;
        IOLockUnlock(fFwLock);
        return;
    }
    if (--fFwRefCount == 0) {
        gt1Wr32(ARC_REG_FORCEWAKE_GT, ARC_FW_KERNEL_DISABLE);
        wr32(ARC_REG_FORCEWAKE_RENDER, ARC_FW_KERNEL_DISABLE);
        wr32(ARC_REG_FORCEWAKE_GT, ARC_FW_KERNEL_DISABLE);
        (void)rd32(ARC_REG_FORCEWAKE_ACK_GT_MTL);
    }
    IOLockUnlock(fFwLock);
}

void ArcXeService::readTopologyAndClocks()
{
    /* 1. GMD_ID on Primary GT0 (Xe-LPG 12.70) and Standalone Media GT1 (Xe-LPM+ 13.00) */
    fGmdIdGt0 = rd32(ARC_REG_GMD_ID);
    if (fGmdIdGt0 != 0 && fGmdIdGt0 != 0xFFFFFFFFu) {
        uint32_t arch = (fGmdIdGt0 >> ARC_GMD_ARCH_SHIFT) & ARC_GMD_ARCH_MASK;
        uint32_t rel  = (fGmdIdGt0 >> ARC_GMD_RELEASE_SHIFT) & ARC_GMD_RELEASE_MASK;
        if (arch >= 12 && arch <= 30)
            fGraphicsVerX100 = arch * 100u + rel;
    }

    fGmdIdGt1 = gt1Rd32(ARC_REG_GMD_ID);
    if (fGmdIdGt1 != 0 && fGmdIdGt1 != 0xFFFFFFFFu) {
        uint32_t arch = (fGmdIdGt1 >> ARC_GMD_ARCH_SHIFT) & ARC_GMD_ARCH_MASK;
        uint32_t rel  = (fGmdIdGt1 >> ARC_GMD_RELEASE_SHIFT) & ARC_GMD_RELEASE_MASK;
        if (arch >= 12 && arch <= 30)
            fMediaVerX100 = arch * 100u + rel;
    }

    /* 2. Xe-Core (DSS) & Vector Engine (EU) Fuse Masks */
    uint32_t geomDss = rd32(ARC_REG_GT_GEOMETRY_DSS_ENABLE);
    uint32_t compDss = rd32(ARC_REG_GT_COMPUTE_DSS_ENABLE);
    if (geomDss == 0xFFFFFFFFu) geomDss = 0;
    if (compDss == 0xFFFFFFFFu) compDss = 0;

    if ((geomDss | compDss) != 0) {
        fGeomDssMask    = geomDss;
        fComputeDssMask = compDss;
    } else {
        /* Safe fallback for Core Ultra H Arc Graphics (8 Xe-cores) */
        fGeomDssMask    = 0xFFu;
        fComputeDssMask = 0xFFu;
    }
    fTotalXeCores = popcount32(fGeomDssMask | fComputeDssMask);
    if (fTotalXeCores == 0) fTotalXeCores = 8;

    uint32_t fuse4 = rd32(ARC_REG_XEHP_FUSE4);
    uint32_t euNib = (fuse4 != 0xFFFFFFFFu)
                   ? ((fuse4 >> ARC_EU_EN_PER_DSS_SHIFT) & ARC_EU_EN_PER_DSS_MASK)
                   : 0xFu;
    uint32_t euMask16 = 0;
    for (uint32_t b = 0; b < 4; b++) {
        if (euNib & (1u << b))
            euMask16 |= (0xFu << (b * 4u));
    }
    fEuPerDssMask = euMask16 ? euMask16 : 0xFFFFu;
    fTotalEus     = fTotalXeCores * popcount32(fEuPerDssMask);

    /* 3. Crystal Clock & CS Timestamp Frequency (Linux xe_gt_clock.c) */
    uint32_t rpm0 = rd32(ARC_REG_RPM_CONFIG0);
    if (rpm0 != 0 && rpm0 != 0xFFFFFFFFu) {
        const uint32_t clkSel   = (rpm0 >> ARC_RPM_CRYSTAL_CLOCK_SHIFT) & ARC_RPM_CRYSTAL_CLOCK_MASK;
        const uint32_t ctcShift = (rpm0 >> ARC_RPM_SHIFT_SHIFT) & ARC_RPM_SHIFT_MASK;
        switch (clkSel) {
        case 0:  fCrystalClockHz = 24000000u; break;
        case 1:  fCrystalClockHz = 19200000u; break;
        case 2:  fCrystalClockHz = 38400000u; break;
        case 3:  fCrystalClockHz = 25000000u; break;
        default: fCrystalClockHz = 38400000u; break;
        }
        const uint32_t divShift = (3u >= ctcShift) ? (3u - ctcShift) : 1u;
        fTimestampFreqHz = fCrystalClockHz >> divShift;
        if (fTimestampFreqHz == 0)
            fTimestampFreqHz = 19200000u;
    }

    AXELOG("Topology: Graphics IP %u.%02u (GMD=0x%08x), Media IP %u.%02u (GMD=0x%08x), "
           "%u Xe-cores (DSS=0x%x), %u Vector Engines (EU/DSS=0x%x), TS=%u Hz",
           fGraphicsVerX100 / 100u, fGraphicsVerX100 % 100u, fGmdIdGt0,
           fMediaVerX100 / 100u, fMediaVerX100 % 100u, fGmdIdGt1,
           fTotalXeCores, fGeomDssMask | fComputeDssMask,
           fTotalEus, fEuPerDssMask, fTimestampFreqHz);
}

void ArcXeService::programMtlPatTable()
{
    /*
     * Canonical Meteor Lake Xe-LPG PAT Table (Linux drivers/gpu/drm/xe/xe_pat.c: xelpg_pat_table):
     *   [0] = 0x00 : L4 WB, Non-coherent
     *   [1] = 0x04 : L4 WT, Non-coherent (Scanout / Write-Combined)
     *   [2] = 0x0C : L4 UC, Non-coherent
     *   [3] = 0x02 : L4 WB, 1-Way Coherent (Default UMA Coherent)
     *   [4] = 0x03 : L4 WB, 2-Way Coherent (Shared CPU/GPU Atomics)
     */
    static const uint32_t kXelpgPat[ARC_MTL_PAT_NUM_ENTRIES] = {
        ARC_XELPG_PAT_L4_WB | ARC_XELPG_PAT_COH_NONE,  /* 0: 0x00 */
        ARC_XELPG_PAT_L4_WT | ARC_XELPG_PAT_COH_NONE,  /* 1: 0x04 */
        ARC_XELPG_PAT_L4_UC | ARC_XELPG_PAT_COH_NONE,  /* 2: 0x0C */
        ARC_XELPG_PAT_L4_WB | ARC_XELPG_PAT_COH_1WAY,  /* 3: 0x02 */
        ARC_XELPG_PAT_L4_WB | ARC_XELPG_PAT_COH_2WAY,  /* 4: 0x03 */
    };

    for (uint32_t i = 0; i < ARC_MTL_PAT_NUM_ENTRIES; i++) {
        wr32(ARC_REG_PAT_INDEX(i), kXelpgPat[i]);
        gt1Wr32(ARC_REG_PAT_INDEX(i), kXelpgPat[i]);
    }
    /* Coherent page-table walk attribute (L4 WB + 1-Way Coherent, matching PAT[3]) */
    wr32(ARC_REG_PAT_PTA, ARC_XELPG_PAT_L4_WB | ARC_XELPG_PAT_COH_1WAY);
    gt1Wr32(ARC_REG_PAT_PTA, ARC_XELPG_PAT_L4_WB | ARC_XELPG_PAT_COH_1WAY);
    (void)rd32(ARC_REG_PAT_INDEX(3));

    AXELOG("Programmed Xe-LPG PAT[0..4] = {0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x}",
           rd32(ARC_REG_PAT_INDEX(0)), rd32(ARC_REG_PAT_INDEX(1)),
           rd32(ARC_REG_PAT_INDEX(2)), rd32(ARC_REG_PAT_INDEX(3)),
           rd32(ARC_REG_PAT_INDEX(4)));
}

void ArcXeService::programMtlMocsTable()
{
    /*
     * Canonical Meteor Lake Xe-LPG MOCS Table (Linux drivers/gpu/drm/xe/xe_mocs.c: mtl_mocs_desc):
     *   Mesa ANV tags buffers and surfaces with MOCS indices 1..15; IG_PAT (bit 8) MUST be set
     *   on entries 1..15 so Xe-LPG honours our coherent UMA PPGTT/GGTT PAT attributes instead
     *   of falling back to non-coherent MOCS 0. Unused indices (10..13) map to entry 1.
     */
    struct MocsEntry { uint32_t glob; uint16_t l3cc; };
    static const MocsEntry kMtlMocs[ARC_MTL_NUM_MOCS_ENTRIES] = {
        { 0u,                                   ARC_L3_LKUP | ARC_L3_3_WB },               /*  0: Error / Reserved */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_3_WB },               /*  1: Cached L3 + L4 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_1_UC },               /*  2: L4 - GO:L3 */
        { ARC_MOCS_IG_PAT | ARC_MOCS_L4_UC,     ARC_L3_LKUP | ARC_L3_1_UC },               /*  3: Uncached - GO:L3 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_GLBGO | ARC_L3_1_UC },/*  4: L4 - GO:Mem */
        { ARC_MOCS_IG_PAT | ARC_MOCS_L4_UC,     ARC_L3_LKUP | ARC_L3_GLBGO | ARC_L3_1_UC },/*  5: Uncached - GO:Mem */
        { ARC_MOCS_IG_PAT,                      ARC_L3_1_UC },                             /*  6: L4 - L3:NoLKUP; GO:L3 */
        { ARC_MOCS_IG_PAT | ARC_MOCS_L4_UC,     ARC_L3_1_UC },                             /*  7: UC - L3:NoLKUP; GO:L3 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_GLBGO | ARC_L3_1_UC },              /*  8: L4 - L3:NoLKUP; GO:Mem */
        { ARC_MOCS_IG_PAT | ARC_MOCS_L4_UC,     ARC_L3_GLBGO | ARC_L3_1_UC },              /*  9: UC - L3:NoLKUP; GO:Mem */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_3_WB },               /* 10: Unused -> entry 1 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_3_WB },               /* 11: Unused -> entry 1 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_3_WB },               /* 12: Unused -> entry 1 */
        { ARC_MOCS_IG_PAT,                      ARC_L3_LKUP | ARC_L3_3_WB },               /* 13: Unused -> entry 1 */
        { ARC_MOCS_IG_PAT | ARC_MOCS_L4_WT,     ARC_L3_LKUP | ARC_L3_3_WB },               /* 14: Display L3 + L4:WT */
        { ARC_MOCS_IG_PAT,                      ARC_L3_GLBGO | ARC_L3_1_UC },              /* 15: CCS Non-Displayable */
    };

    /* Steer GT0 MCR selectors to group 0 / instance 0 with multicast write broadcast enabled */
    wr32(ARC_REG_MTL_MCR_SELECTOR, ARC_MCR_MULTICAST);
    wr32(ARC_REG_SF_MCR_SELECTOR,  ARC_MCR_MULTICAST);
    wr32(ARC_REG_MCR_SELECTOR,     ARC_MCR_MULTICAST);
    wr32(ARC_REG_GAM_MCR_SELECTOR, ARC_MCR_MULTICAST);
    (void)rd32(ARC_REG_MTL_MCR_SELECTOR);

    for (uint32_t i = 0; i < ARC_MTL_NUM_MOCS_ENTRIES; i++) {
        wr32(ARC_REG_GLOBAL_MOCS(i), kMtlMocs[i].glob);
    }
    for (uint32_t i = 0; i < (ARC_MTL_NUM_MOCS_ENTRIES / 2u); i++) {
        const uint32_t l3pair = (uint32_t)kMtlMocs[2u * i].l3cc |
                                ((uint32_t)kMtlMocs[2u * i + 1u].l3cc << 16);
        wr32(ARC_REG_LNCFCMOCS(i), l3pair);
    }
    (void)rd32(ARC_REG_MTL_MCR_SELECTOR);
    AXELOG("Programmed Xe-LPG GLOBAL_MOCS[0..15] (IG_PAT=1) & LNCFCMOCS[0..7]");
}

void ArcXeService::applyXeLpgWorkarounds()
{
    /*
     * Meteor Lake Xe-LPG (Graphics IP 12.70..12.74) GT, Engine & LRC Hardware Workarounds + Render Tunings
     * (Linux drivers/gpu/drm/xe/xe_wa.c, xe_tuning.c & xe_hw_engine.c).
     * Must be applied with forcewake held and re-applied after any GRDOM_RENDER reset.
     */
    wr32(ARC_REG_MTL_MCR_SELECTOR, 0u);
    wr32(ARC_REG_SF_MCR_SELECTOR,  0u);
    wr32(ARC_REG_MCR_SELECTOR,     0u);
    wr32(ARC_REG_GAM_MCR_SELECTOR, 0u);
    (void)rd32(ARC_REG_MTL_MCR_SELECTOR);

    const uint32_t compMod   = rd32(ARC_REG_COMP_MOD_CTRL);
    const uint32_t sqcnt1    = rd32(ARC_REG_SQCNT1);
    const uint32_t l3sqcreg5 = rd32(ARC_REG_XEHP_L3SQCREG5);

    wr32(ARC_REG_MTL_MCR_SELECTOR, ARC_MCR_MULTICAST);
    wr32(ARC_REG_SF_MCR_SELECTOR,  ARC_MCR_MULTICAST);
    wr32(ARC_REG_MCR_SELECTOR,     ARC_MCR_MULTICAST);
    wr32(ARC_REG_GAM_MCR_SELECTOR, ARC_MCR_MULTICAST);
    (void)rd32(ARC_REG_MTL_MCR_SELECTOR);

    /* GT & MCR workarounds + L3 PWM tuning + RCU CCS mode */
    wr32(ARC_REG_MISCCPCTL,      rd32(ARC_REG_MISCCPCTL) & ~ARC_DOP_CLOCK_GATE_RENDER_ENABLE); /* Wa_14015795083 */
    wr32(ARC_REG_COMP_MOD_CTRL,  compMod | ARC_FORCE_MISS_FTLB);                               /* Wa_14018575942 */
    wr32(ARC_REG_SQCNT1,         sqcnt1  | ARC_SQCNT1_ENFORCE_RAR);                            /* Wa_22016670082 */
    wr32(ARC_REG_XEHP_L3SQCREG5, (l3sqcreg5 & ~ARC_L3_PWM_TIMER_INIT_VAL_MASK) | ARC_L3_PWM_TIMER_INIT_7F);
    wr32(ARC_REG_RCU_MODE,       ARC_MASKED_ENABLE(ARC_RCU_MODE_CCS_ENABLE));

    /* Engine & LRC masked MCR chicken workarounds + Render Indirect State Base Address Override (ANV/BLORP) */
    wr32(ARC_REG_SAMPLER_MODE,          ARC_MASKED_ENABLE(ARC_INDIRECT_STATE_BASE_ADDR_OVERRIDE)); /* xe_tuning.c:91-95 */
    wr32(ARC_REG_COMMON_SLICE_CHICKEN3, ARC_MASKED_ENABLE(ARC_DISABLE_STATE_CACHE_PERF_FIX));      /* xe_lrc.c / anv_queue.c */
    wr32(ARC_REG_ROW_CHICKEN3,          ARC_MASKED_ENABLE(ARC_DIS_FIX_EOT1_FLUSH));                /* Wa_14017856879 */
    wr32(ARC_REG_XEHP_HDC_CHICKEN0, ARC_MASKED_ENABLE(ARC_DIS_ATOMIC_CHAIN_TYPED_WRITES));     /* Wa_14015150844 */
    wr32(ARC_REG_ROW_CHICKEN2,      ARC_MASKED_ENABLE(ARC_DISABLE_TDL_SVHS_GATING));           /* Wa_14020495402 */
    wr32(ARC_REG_XEHP_PSS_CHICKEN,  ARC_MASKED_ENABLE(ARC_FD_END_COLLECT));                    /* Wa_14019877138 */
    wr32(ARC_REG_CACHE_MODE_1,      ARC_MASKED_ENABLE(ARC_MSAA_OPTIMIZATION_REDUC_DIS));       /* Wa_18019271663 */
    (void)rd32(ARC_REG_SAMPLER_MODE);
}

void ArcXeService::invalidateGlobalTlb()
{
    if (!forceWakeGet()) return;
    /* Gen12.50+ / Xe-LPG per-domain TLB invalidation registers (Linux i915 intel_tlb.c) + legacy GTCR */
    static const uint32_t kTlbRegs[] = { 0x00CED8u, 0x00CF04u, 0x00CEECu, ARC_REG_GTCR };
    for (uint32_t reg : kTlbRegs)
        wr32(reg, ARC_GTCR_INVALIDATE);
    for (uint32_t us = 0; us < 2000; us += 10) {
        if ((rd32(0x00CED8u) & 1u) == 0 && (rd32(ARC_REG_GTCR) & ARC_GTCR_INVALIDATE) == 0)
            break;
        IODelay(10);
    }
    forceWakePut();
}

/* ============================================================================
 * 2. IOService Lifecycle (probe / start / stop / newUserClient / callPlatformFunction)
 * ============================================================================ */

IOService *ArcXeService::probe(IOService *provider, SInt32 *score)
{
    uint32_t off = 0;
    if (PE_parse_boot_argn("-arcoff", &off, sizeof(off))) {
        AXELOG("disabled via -arcoff boot-arg");
        return nullptr;
    }

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID);
    const uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
    if (!arcIsSupportedXeLpgDevice(ven, dev))
        return nullptr;

    if (score) *score = 50000;
    return IOService::probe(provider, score);
}

bool ArcXeService::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;

    fVendorId    = fPCI->configRead16(kIOPCIConfigVendorID);
    fDeviceId    = fPCI->configRead16(kIOPCIConfigDeviceID);
    fSubVendorId = fPCI->configRead16(kIOPCIConfigSubSystemVendorID);
    fSubDeviceId = fPCI->configRead16(kIOPCIConfigSubSystemID);
    fRevId       = fPCI->configRead8(kIOPCIConfigRevisionID);

    uint32_t useGucArg = 0;
    if (PE_parse_boot_argn("arc_guc", &useGucArg, sizeof(useGucArg)) && useGucArg != 0)
        fUseGuc = true;

    /* Enable PCI Memory Space + Bus Master DMA */
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    /* Map BAR0 (16 MiB GTTMMADR: 0..8M MMIO + 8M..16M GGTT GSM PTE array) */
    IODeviceMemory *bar0Mem = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!bar0Mem || bar0Mem->getLength() < ARC_BAR0_TOTAL_SIZE) {
        AXELOG("BAR0 missing or smaller than 16 MiB (len=0x%llx)",
               bar0Mem ? (unsigned long long)bar0Mem->getLength() : 0ULL);
        return false;
    }
    fBar0Phys = bar0Mem->getPhysicalAddress();
    fBar0Map  = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!fBar0Map) {
        AXELOG("failed to map BAR0");
        return false;
    }
    fMmio    = reinterpret_cast<volatile uint8_t *>(fBar0Map->getVirtualAddress());
    fGsmPtes = reinterpret_cast<volatile uint64_t *>(
                   reinterpret_cast<uintptr_t>(fMmio) + ARC_BAR0_GGTT_GSM_OFFSET);

    /* Query BAR2 (256 MiB GMADR aperture on 64-bit BAR index 0x18) */
    IODeviceMemory *bar2Mem = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (bar2Mem) {
        fBar2Phys = bar2Mem->getPhysicalAddress();
        fBar2Size = bar2Mem->getLength();
    }

    /* Read Data Stolen Memory (DSMBASE) configured by BIOS */
    const uint64_t dsmLo = rd32(ARC_REG_DSMBASE_LO);
    const uint64_t dsmHi = rd32(ARC_REG_DSMBASE_HI);
    fStolenPhys = ((dsmHi << 32) | dsmLo) & ~0xFFFFFULL;
    const uint16_t gmchCtrl = fPCI->configRead16(ARC_PCI_GMCH_CTRL);
    const uint32_t gms = (gmchCtrl >> ARC_GMCH_GMS_SHIFT) & ARC_GMCH_GMS_MASK;
    if (gms <= 0x04u)
        fStolenSize = (uint64_t)gms * 32ULL * 1024ULL * 1024ULL;
    else if (gms >= 0xF0u && gms <= 0xFEu)
        fStolenSize = (uint64_t)(gms - 0xEFu) * 4ULL * 1024ULL * 1024ULL;

    AXELOG("Attaching Intel Arc Xe-LPG %04x:%04x (subsys %04x:%04x rev 0x%02x) "
           "BAR0=0x%llx BAR2=0x%llx (%llu MiB) DSM=0x%llx (%llu MiB)",
           fVendorId, fDeviceId, fSubVendorId, fSubDeviceId, fRevId,
           (unsigned long long)fBar0Phys,
           (unsigned long long)fBar2Phys, (unsigned long long)(fBar2Size >> 20),
           (unsigned long long)fStolenPhys, (unsigned long long)(fStolenSize >> 20));

    fFwLock = IOLockAlloc();
    if (!fFwLock) return false;

    if (!forceWakeGet()) {
        AXELOG("initial forceWakeGet failed");
        return false;
    }

    readTopologyAndClocks();
    programMtlPatTable();
    programMtlMocsTable();
    applyXeLpgWorkarounds();

    if (!initGgtt()) {
        forceWakePut();
        return false;
    }

    if (!initEngines()) {
        teardownGgtt();
        forceWakePut();
        return false;
    }

    invalidateGlobalTlb();
    forceWakePut();

    /* Publish discovery properties on both ArcXeService and parent IOPCIDevice */
    setProperty("model", "Intel(R) Arc(TM) Graphics");
    setProperty("arc-graphics-verx100", fGraphicsVerX100, 32);
    setProperty("arc-media-verx100", fMediaVerX100, 32);
    setProperty("arc-xe-cores", fTotalXeCores, 32);
    setProperty("arc-vector-engines", fTotalEus, 32);
    fPCI->setProperty("arcxe-claimed", kOSBooleanTrue);
    fPCI->setProperty("model", "Intel(R) Arc(TM) Graphics");

    /*
     * Register Data Stolen Memory (DSM, e.g. 0x78800000..+128 MiB) in parent IOPCIDevice's
     * IODeviceMemory array (tagged with PCI BDSM config offset 0xC0 so BAR0/BAR2 lookups are
     * unaffected). On Meteor Lake Xe-LPG, UEFI GOP places PE_state.video.v_baseAddr in DSM
     * outside BAR0/BAR2; exposing DSM in IODeviceMemory allows Apple's IOBootNDRV::fromRegistryEntry
     * (IONDRVFramebuffer) to match v_baseAddr and launch WindowServer cleanly whenever ArcFB
     * is not overriding head 0.
     */
    if (fStolenPhys && fStolenSize) {
        OSArray *curMem = fPCI->getDeviceMemory();
        bool hasDsm = false;
        if (curMem) {
            const unsigned int cnt = curMem->getCount();
            for (unsigned int i = 0; i < cnt; i++) {
                if (auto *m = OSDynamicCast(IOMemoryDescriptor, curMem->getObject(i))) {
                    if (m->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) == (addr64_t)fStolenPhys) {
                        hasDsm = true;
                        break;
                    }
                }
            }
        }
        if (!hasDsm) {
            OSArray *newMem = curMem ? OSArray::withArray(curMem) : OSArray::withCapacity(4);
            if (newMem) {
                if (IODeviceMemory *dsmMem = IODeviceMemory::withRange(
                        (IOPhysicalAddress)fStolenPhys, (IOPhysicalLength)fStolenSize)) {
                    dsmMem->setTag(ARC_PCI_BDSM_64_LO);
                    newMem->setObject(dsmMem);
                    dsmMem->release();
                }
                const uint64_t vBase = (uint64_t)PE_state.video.v_baseAddr;
                const uint64_t vLen  = ((uint64_t)PE_state.video.v_rowBytes *
                                        (uint64_t)PE_state.video.v_height + 0xFFFULL) & ~0xFFFULL;
                if (vBase && vLen && (vBase < fStolenPhys || (vBase + vLen) > (fStolenPhys + fStolenSize))) {
                    if (IODeviceMemory *gopMem = IODeviceMemory::withRange(
                            (IOPhysicalAddress)(vBase & ~0xFFFULL), (IOPhysicalLength)vLen)) {
                        gopMem->setTag(0xC8u);
                        newMem->setObject(gopMem);
                        gopMem->release();
                    }
                }
                fPCI->setDeviceMemory(newMem);
                newMem->release();
                AXELOG("Registered DSM [0x%llx .. +0x%llx) in IOPCIDevice IODeviceMemory (v_baseAddr=0x%llx)",
                       (unsigned long long)fStolenPhys, (unsigned long long)fStolenSize,
                       (unsigned long long)PE_state.video.v_baseAddr);
            }
        }
        const uint64_t vb = (uint64_t)PE_state.video.v_baseAddr;
        if (vb >= fStolenPhys && vb < (fStolenPhys + fStolenSize)) {
            fPCI->setProperty("AAPL,boot-display", kOSBooleanTrue);
        }
    }

    registerService();
    /* Re-trigger matching on parent IOPCIDevice so IONDRVFramebuffer / ArcFB / ArcAccel attach cleanly */
    fPCI->registerService();

    /* Only spawn background GuC firmware loader when arc_guc=1 is explicitly requested */
    if (fUseGuc) {
        this->retain();
        thread_t fwThread = THREAD_NULL;
        if (kernel_thread_start(&ArcXeService::gucLoaderThreadEntry, this, &fwThread) == KERN_SUCCESS) {
            thread_deallocate(fwThread);
        } else {
            this->release();
        }
    }

    AXELOG("ArcXeService READY (backend=%s)", fUseGuc ? "GuC-v70" : "Direct-Execlist");
    return true;
}

void ArcXeService::stop(IOService *provider)
{
    fStopping = true;
    teardownEngines();
    teardownGgtt();
    if (fFwLock) {
        IOLockFree(fFwLock);
        fFwLock = nullptr;
    }
    OSSafeReleaseNULL(fBar0Map);
    fMmio    = nullptr;
    fGsmPtes = nullptr;
    IOService::stop(provider);
}

IOReturn ArcXeService::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                     OSDictionary *properties, IOUserClient **handler)
{
    if (!handler) return kIOReturnBadArgument;

    ArcXeUserClient *client = new ArcXeUserClient;
    if (!client) return kIOReturnNoMemory;

    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

IOReturn ArcXeService::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                            void *param1, void *param2, void *param3, void *param4)
{
    if (functionName) {
        if (functionName->isEqualTo(ARC_XE_FN_PIN_SURFACE_GGTT)) {
            auto *req = static_cast<ArcPinGgttReq *>(param1);
            if (!req || req->version != ARC_KERN_ABI_VERSION || !req->iomem_desc || req->size == 0)
                return kIOReturnBadArgument;

            auto *md = static_cast<IOMemoryDescriptor *>(req->iomem_desc);
            uint64_t now = 0;
            clock_get_uptime(&now);

            auto flushScanoutCache = [](IOMemoryMap *m, uint64_t bytes) {
                if (!m || !m->getVirtualAddress() || !bytes)
                    return;
                const uint64_t len = (bytes <= m->getLength()) ? bytes : m->getLength();
                const uintptr_t start = static_cast<uintptr_t>(m->getVirtualAddress()) & ~static_cast<uintptr_t>(63u);
                const uintptr_t end   = (static_cast<uintptr_t>(m->getVirtualAddress()) + (uintptr_t)len + 63u) & ~static_cast<uintptr_t>(63u);
                for (uintptr_t p = start; p < end; p += 64u) {
                    __asm__ __volatile__("clflush (%0)" :: "r"(reinterpret_cast<const void *>(p)) : "memory");
                }
                __asm__ __volatile__("mfence" ::: "memory");
            };

            IOLockLock(fEngineLock);
            /* 1. Fast path: already pinned in scanout LRU cache */
            for (uint32_t i = 0; i < kMaxScanoutPins; i++) {
                if (fScanoutPins[i].md == md) {
                    fScanoutPins[i].lastUsedTick = now;
                    flushScanoutCache(fScanoutPins[i].kernMap, req->size);
                    req->out_ggtt_offset = fScanoutPins[i].ggttOffset;
                    req->status = 0;
                    IOLockUnlock(fEngineLock);
                    return kIOReturnSuccess;
                }
            }

            /* 2. Choose empty slot or oldest LRU slot to evict */
            uint32_t victim = 0;
            bool foundEmpty = false;
            for (uint32_t i = 0; i < kMaxScanoutPins; i++) {
                if (!fScanoutPins[i].md) {
                    victim = i;
                    foundEmpty = true;
                    break;
                }
                if (fScanoutPins[i].lastUsedTick < fScanoutPins[victim].lastUsedTick)
                    victim = i;
            }

            if (!foundEmpty && fScanoutPins[victim].md) {
                freeGgttRange(fScanoutPins[victim].ggttOffset, fScanoutPins[victim].ggttSize);
                if (fScanoutPins[victim].kernMap) {
                    fScanoutPins[victim].kernMap->release();
                    fScanoutPins[victim].kernMap = nullptr;
                }
                if (fScanoutPins[victim].prepared) {
                    fScanoutPins[victim].md->complete();
                    fScanoutPins[victim].prepared = false;
                }
                fScanoutPins[victim].md->release();
                fScanoutPins[victim] = {};
            }

            /* 3. Wire IOSurface pages & bind Display scanout surface into GGTT with WT non-coherent scanout PAT */
            const bool prepOk = (md->prepare() == kIOReturnSuccess);
            uint32_t ggttOff = 0, ggttSz = 0;
            if (!allocGgttAndBind(md, req->size, ARC_MTL_PAT_IDX_WT_NONCOH, &ggttOff, &ggttSz)) {
                if (prepOk)
                    md->complete();
                req->status = -1;
                IOLockUnlock(fEngineLock);
                return kIOReturnNoResources;
            }

            IOMemoryMap *kmap = md->map(kIOMapReadOnly);
            flushScanoutCache(kmap, req->size);

            md->retain();
            fScanoutPins[victim].md           = md;
            fScanoutPins[victim].kernMap      = kmap;
            fScanoutPins[victim].ggttOffset   = ggttOff;
            fScanoutPins[victim].ggttSize     = ggttSz;
            fScanoutPins[victim].lastUsedTick = now;
            fScanoutPins[victim].prepared     = prepOk;

            req->out_ggtt_offset = ggttOff;
            req->status = 0;
            IOLockUnlock(fEngineLock);
            return kIOReturnSuccess;
        }

        if (functionName->isEqualTo(ARC_XE_FN_UNPIN_SURFACE_GGTT)) {
            auto *req = static_cast<ArcPinGgttReq *>(param1);
            if (!req) return kIOReturnBadArgument;
            auto *md = static_cast<IOMemoryDescriptor *>(req->iomem_desc);
            IOLockLock(fEngineLock);
            for (uint32_t i = 0; i < kMaxScanoutPins; i++) {
                if ((md && fScanoutPins[i].md == md) ||
                    (req->out_ggtt_offset && fScanoutPins[i].ggttOffset == req->out_ggtt_offset)) {
                    freeGgttRange(fScanoutPins[i].ggttOffset, fScanoutPins[i].ggttSize);
                    if (fScanoutPins[i].kernMap) fScanoutPins[i].kernMap->release();
                    if (fScanoutPins[i].prepared && fScanoutPins[i].md) fScanoutPins[i].md->complete();
                    if (fScanoutPins[i].md) fScanoutPins[i].md->release();
                    fScanoutPins[i] = {};
                }
            }
            IOLockUnlock(fEngineLock);
            return kIOReturnSuccess;
        }
    }

    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

extern "C" {
static kern_return_t _arcxe_start(kmod_info_t *, void *) { return KERN_SUCCESS; }
static kern_return_t _arcxe_stop(kmod_info_t *, void *)  { return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(com.arc.ArcXe, "0.1.0", _arcxe_start, _arcxe_stop)
}
