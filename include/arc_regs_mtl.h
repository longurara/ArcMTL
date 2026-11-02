/*
 * SPDX-License-Identifier: MIT
 *
 * Intel Meteor Lake (Xe-LPG Graphics IP 12.70 / Media Xe-LPM+ IP 13.00 / Display IP 14)
 * Hardware Register, GTT/PAT, Execlist/LRC, GuC v70, and MI Instruction Definitions.
 *
 * Derived from MIT-licensed Linux drivers/gpu/drm/xe/regs/ and drivers/gpu/drm/i915/display/.
 * Target Primary Hardware: Intel Core Ultra 7 155H - Intel(R) Arc(TM) Graphics (PCI 8086:7D55).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
#define ARC_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define ARC_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

/* ============================================================================
 * 1. PCI Configuration & Device IDs (Meteor Lake / Arrow Lake Xe-LPG)
 * ============================================================================ */
#define ARC_PCI_VENDOR_INTEL                0x8086u

#define ARC_DEV_MTL_U_7D40                  0x7D40u
#define ARC_DEV_ARL_U_7D41                  0x7D41u
#define ARC_DEV_MTL_U_7D45                  0x7D45u
#define ARC_DEV_ARL_H_7D51                  0x7D51u
#define ARC_DEV_MTL_H_ARC_7D55              0x7D55u   /* Core Ultra 7 155H Arc Graphics (8 Xe-cores) */
#define ARC_DEV_MTL_H_7D60                  0x7D60u
#define ARC_DEV_ARL_S_7D67                  0x7D67u
#define ARC_DEV_ARL_H_7DD1                  0x7DD1u
#define ARC_DEV_MTL_H_7DD5                  0x7DD5u

static inline bool arcIsSupportedXeLpgDevice(uint16_t vendor, uint16_t device)
{
    if (vendor != ARC_PCI_VENDOR_INTEL) return false;
    switch (device) {
    case ARC_DEV_MTL_U_7D40:
    case ARC_DEV_ARL_U_7D41:
    case ARC_DEV_MTL_U_7D45:
    case ARC_DEV_ARL_H_7D51:
    case ARC_DEV_MTL_H_ARC_7D55:
    case ARC_DEV_MTL_H_7D60:
    case ARC_DEV_ARL_S_7D67:
    case ARC_DEV_ARL_H_7DD1:
    case ARC_DEV_MTL_H_7DD5:
        return true;
    default:
        return false;
    }
}

/* PCI Config Registers */
#define ARC_PCI_GMCH_CTRL                   0x50u
#define   ARC_GMCH_GGMS_SHIFT               6u
#define   ARC_GMCH_GGMS_MASK                0x3u      /* GSM size = (ggms ? 1MB << ggms : 0), 8MB on MTL */
#define   ARC_GMCH_GMS_SHIFT                8u
#define   ARC_GMCH_GMS_MASK                 0xFFu
#define ARC_PCI_BDSM_64_LO                  0xC0u     /* 64-bit Base of Data Stolen Memory (low) */
#define ARC_PCI_BDSM_64_HI                  0xC4u     /* 64-bit Base of Data Stolen Memory (high) */

/* BAR0 (16 MiB GTTMMADR) Layout on Meteor Lake */
#define ARC_BAR0_MMIO_OFFSET                0x000000u
#define ARC_BAR0_MMIO_SIZE                  0x800000u /* 8 MiB MMIO */
#define   ARC_GT0_PRIMARY_MMIO_BASE         0x000000u /* Primary Render/Compute GT0 (Xe-LPG 12.70) */
#define   ARC_GT1_MEDIA_MMIO_BASE           0x380000u /* Standalone Media GT1 (Xe-LPM+ 13.00) */
#define ARC_BAR0_GGTT_GSM_OFFSET            0x800000u /* 8 MiB GGTT PTE Aperture -> 4 GiB Global VA */
#define ARC_BAR0_GGTT_GSM_SIZE              0x800000u
#define ARC_BAR0_TOTAL_SIZE                 0x1000000u

/* Stolen Memory Mirror Registers in MMIO */
#define ARC_REG_DSMBASE_LO                  0x1080C0u
#define ARC_REG_DSMBASE_HI                  0x1080C4u
#define ARC_REG_GSMBASE_LO                  0x108100u
#define ARC_REG_GSMBASE_HI                  0x108104u

/* ============================================================================
 * 2. GT Identification, Forcewake, Fuses & Clocks (Xe-LPG GT0)
 * ============================================================================ */
#define ARC_REG_GMD_ID                      0x000D8Cu
#define   ARC_GMD_ARCH_SHIFT                22u
#define   ARC_GMD_ARCH_MASK                 0x3FFu
#define   ARC_GMD_RELEASE_SHIFT             14u
#define   ARC_GMD_RELEASE_MASK              0xFFu
#define   ARC_GMD_REVID_MASK                0x3Fu

/* Forcewake Registers (Masked write: upper 16 bits = mask, lower 16 bits = value) */
#define ARC_REG_FORCEWAKE_GT                0x00A188u
#define ARC_REG_FORCEWAKE_ACK_GT_MTL        0x000DFCu
#define ARC_REG_FORCEWAKE_RENDER            0x00A278u
#define ARC_REG_FORCEWAKE_ACK_RENDER        0x000D84u
#define ARC_REG_FORCEWAKE_MEDIA_VDBOX0      0x00A540u
#define ARC_REG_FORCEWAKE_ACK_MEDIA_VDBOX0  0x000D50u
#define   ARC_FW_KERNEL_BIT                 (1u << 0)
#define   ARC_FW_KERNEL_ENABLE              ((1u << 16) | (1u << 0))
#define   ARC_FW_KERNEL_DISABLE             ((1u << 16) | 0u)

/* Fuse & Topology Registers */
#define ARC_REG_XEHP_FUSE4                  0x009114u
#define   ARC_CFEG_WMTP_DISABLE             (1u << 20)
#define   ARC_EU_EN_PER_DSS_SHIFT           16u
#define   ARC_EU_EN_PER_DSS_MASK            0xFu
#define ARC_REG_MIRROR_FUSE3                0x009118u
#define ARC_REG_MIRROR_FUSE1                0x00911Cu
#define ARC_REG_FUSE2                       0x009120u
#define ARC_REG_GT_GEOMETRY_DSS_ENABLE      0x00913Cu
#define ARC_REG_GT_COMPUTE_DSS_ENABLE       0x009144u

/* Crystal Clock & Timestamps */
#define ARC_REG_TIMESTAMP_OVERRIDE          0x044074u
#define ARC_REG_CTC_MODE                    0x00A26Cu
#define ARC_REG_RPM_CONFIG0                 0x000D00u
#define   ARC_RPM_CRYSTAL_CLOCK_SHIFT       3u
#define   ARC_RPM_CRYSTAL_CLOCK_MASK        0x7u
#define   ARC_RPM_SHIFT_SHIFT               1u
#define   ARC_RPM_SHIFT_MASK                0x3u

/* Master Interrupt Registers (Gen11+ / Xe-LPG) */
#define ARC_REG_GFX_MSTR_IRQ                0x190010u
#define   ARC_MASTER_IRQ_ENABLE             (1u << 31)
#define   ARC_DISPLAY_IRQ_BIT               (1u << 16)
#define   ARC_GT_DW0_IRQ_BIT                (1u << 0)
#define   ARC_GT_DW1_IRQ_BIT                (1u << 1)
#define ARC_REG_GT_INTR_DW0                 0x190018u
#define ARC_REG_GT_INTR_DW1                 0x19001Cu
#define ARC_REG_RENDER_COPY_INTR_ENABLE     0x190030u
#define ARC_REG_GUC_SG_INTR_ENABLE          0x190038u
#define ARC_REG_GUC_SG_INTR_MASK            0x1900E8u
#define ARC_REG_RCS0_RSVD_INTR_MASK         0x190090u
#define ARC_REG_BCS_RSVD_INTR_MASK          0x1900A0u
#define ARC_REG_CCS0_CCS1_INTR_MASK         0x190100u

/* ============================================================================
 * 3. Page Attribute Table (PAT), MOCS & GGTT / 48-bit PPGTT PTE Encodings (Xe-LPG)
 * ============================================================================ */
#define ARC_REG_MTL_MCR_SELECTOR            0x000FD4u
#define ARC_REG_SF_MCR_SELECTOR             0x000FD8u
#define ARC_REG_MCR_SELECTOR                0x000FDCu
#define ARC_REG_GAM_MCR_SELECTOR            0x000FE0u
#define   ARC_MCR_MULTICAST                 (1u << 31)

#define ARC_REG_GLOBAL_MOCS(i)              (0x004000u + (uint32_t)(i) * 4u)
#define   ARC_MOCS_IG_PAT                   (1u << 8)
#define   ARC_MOCS_L4_WB                    (0u << 2)
#define   ARC_MOCS_L4_WT                    (1u << 2)
#define   ARC_MOCS_L4_UC                    (3u << 2)
#define ARC_REG_LNCFCMOCS(i)                (0x00B020u + (uint32_t)(i) * 4u)
#define   ARC_L3_LKUP                       (1u << 7)
#define   ARC_L3_GLBGO                      (1u << 6)
#define   ARC_L3_1_UC                       (1u << 4)
#define   ARC_L3_3_WB                       (3u << 4)
#define ARC_MTL_NUM_MOCS_ENTRIES            16u
#define ARC_MTL_MOCS_UC_INDEX               9u

#define ARC_REG_PAT_ATS                     0x0047FCu
#define ARC_REG_PAT_INDEX(i)                ((i) < 8 ? (0x004800u + (uint32_t)(i) * 4u) \
                                                     : (0x004848u + ((uint32_t)(i) - 8u) * 4u))
#define ARC_REG_PAT_PTA                     0x004820u

/* Xe-LPG PAT Entry Bitfields (Linux drivers/gpu/drm/xe/xe_pat.c: xelpg_pat_table) */
#define ARC_XELPG_PAT_L4_WB                 (0u << 2)
#define ARC_XELPG_PAT_L4_WT                 (1u << 2)
#define ARC_XELPG_PAT_L4_UC                 (3u << 2)
#define ARC_XELPG_PAT_COH_NONE              (0u << 0)
#define ARC_XELPG_PAT_COH_1WAY              (2u << 0)
#define ARC_XELPG_PAT_COH_2WAY              (3u << 0)

/* Canonical Meteor Lake PAT Indices expected by Mesa ANV (intel_device_info.c) */
#define ARC_MTL_PAT_IDX_WB_NONCOH           0u  /* 0x00: L4 WB, Non-coherent */
#define ARC_MTL_PAT_IDX_WT_NONCOH           1u  /* 0x04: L4 WT, Non-coherent (Scanout / WC) */
#define ARC_MTL_PAT_IDX_UC_NONCOH           2u  /* 0x0C: L4 UC, Non-coherent */
#define ARC_MTL_PAT_IDX_WB_1WAY_COH         3u  /* 0x02: L4 WB, 1-Way Coherent (Default UMA Coherent!) */
#define ARC_MTL_PAT_IDX_WB_2WAY_COH         4u  /* 0x03: L4 WB, 2-Way Coherent (Atomics / Shared CPU+GPU) */
#define ARC_MTL_PAT_NUM_ENTRIES             5u

/* Common Page Table Flags */
#define ARC_PTE_PRESENT                     (1ULL << 0)
#define ARC_PTE_RW                          (1ULL << 1)
#define ARC_PTE_ADDR_MASK                   0x000FFFFFFFFFF000ULL  /* Bits [51:12] */
#define ARC_DMA_PHYS_MASK_46BIT             0x00003FFFFFFFF000ULL  /* MTL 46-bit physical DMA mask */

/* Xe-LPG GGTT PTE Flags & PAT Bits (Bits 1, 52 & 53) */
#define ARC_XELPG_GGTT_PTE_DM               (1ULL << 1)  /* Stolen Memory (GSM + DSM) offset rather than System RAM PA */
#define ARC_XELPG_GGTT_PTE_PAT0             (1ULL << 52)
#define ARC_XELPG_GGTT_PTE_PAT1             (1ULL << 53)

static inline uint64_t arcEncodeGgttPte(uint64_t physAddr, uint16_t patIndex)
{
    uint64_t pte = (physAddr & ARC_PTE_ADDR_MASK) | ARC_PTE_PRESENT;
    if (patIndex & 1u) pte |= ARC_XELPG_GGTT_PTE_PAT0;
    if (patIndex & 2u) pte |= ARC_XELPG_GGTT_PTE_PAT1;
    return pte;
}

/* Xe-LPG 48-bit 4-Level PPGTT PTE PAT Bits (Bits 3, 4, 7, 62) */
#define ARC_PPGTT_PTE_PAT0                  (1ULL << 3)
#define ARC_PPGTT_PTE_PAT1                  (1ULL << 4)
#define ARC_PPGTT_PTE_PAT2                  (1ULL << 7)
#define ARC_PPGTT_PTE_NULL                  (1ULL << 9)
#define ARC_PPGTT_PTE_AE                    (1ULL << 10)  /* Atomic Enable */
#define ARC_XELPG_PPGTT_PTE_PAT3            (1ULL << 62)

static inline uint64_t arcEncodePpgttPte(uint64_t physAddr, uint16_t patIndex, bool readWrite, bool nullPage)
{
    uint64_t pte = (physAddr & ARC_PTE_ADDR_MASK) | ARC_PTE_PRESENT;
    if (readWrite) pte |= ARC_PTE_RW;
    if (nullPage)  pte |= ARC_PPGTT_PTE_NULL;
    if (patIndex & (1u << 0)) pte |= ARC_PPGTT_PTE_PAT0;
    if (patIndex & (1u << 1)) pte |= ARC_PPGTT_PTE_PAT1;
    if (patIndex & (1u << 2)) pte |= ARC_PPGTT_PTE_PAT2;
    if (patIndex & (1u << 3)) pte |= ARC_XELPG_PPGTT_PTE_PAT3;
    return pte;
}

/*
 * Encode non-leaf 48-bit PPGTT directory descriptor (CTX_PDP0 -> PML4 -> PDP -> PD -> PT).
 * Per Linux drivers/gpu/drm/xe/xe_vm.c (xelp_pde_encode_bo / xe_vm_pdp4_descriptor), non-leaf
 * PDEs have 2 PAT bits (PAT0=bit 3, PAT1=bit 4) and MUST select PAT[3] (ARC_MTL_PAT_IDX_WB_1WAY_COH,
 * 1-way coherent L4 WB) on Meteor Lake UMA so the GPU hardware page-table walker snoops CPU caches
 * instead of defaulting to PAT[0] (0x00 = COH_NONE).
 */
static inline uint64_t arcEncodePpgttPde(uint64_t nextTablePhysAddr)
{
    return (nextTablePhysAddr & ARC_PTE_ADDR_MASK)
         | ARC_PTE_PRESENT
         | ARC_PTE_RW
         | ARC_PPGTT_PTE_PAT0
         | ARC_PPGTT_PTE_PAT1;
}

/* GGTT Layout Boundaries & AuxCCS / TLB Invalidation Registers */
#define ARC_GGTT_WOPCM_RESERVED_BASE        0x01800000u  /* Start dynamic GGTT allocs at 24 MiB (above WOPCM + GOP) */
#define ARC_GUC_GGTT_TOP                    0xFEE00000u  /* Top of GuC-accessible GGTT */
#define ARC_REG_GFX_AUX_TABLE_BASE_LO       0x004200u
#define ARC_REG_CCS_AUX_INV                 0x004208u
#define ARC_REG_BCS_AUX_TABLE_BASE_LO       0x004210u
#define   ARC_AUX_INV                       (1u << 0)
#define ARC_REG_GFX_TLB_INV_CR              0x00CED8u    /* XeHP/MTL RCS, MCR, unmasked */
#define ARC_REG_BLT_TLB_INV_CR              0x00CEE4u    /* XeHP/MTL BCS, MCR, unmasked */
#define   ARC_TLB_INVALIDATE                (1u << 0)

/* ============================================================================
 * 4. Hardware Engines, Execlist Submission & LRC Context Layout (Xe-LPG)
 * ============================================================================ */
#define ARC_RENDER_RING_BASE                0x002000u    /* RCS0 (3D Render + Compute) */
#define ARC_COMPUTE0_RING_BASE              0x01A000u    /* CCS0 (Dedicated Compute) */
#define ARC_BLT_RING_BASE                   0x022000u    /* BCS0 (Copy / Blitter) */

#define ARC_RING_TAIL(base)                 ((base) + 0x030u)
#define ARC_RING_HEAD(base)                 ((base) + 0x034u)
#define ARC_RING_START(base)                ((base) + 0x038u)
#define ARC_RING_CTL(base)                  ((base) + 0x03Cu)
#define   ARC_RING_CTL_ENABLE               (1u << 0)
#define   ARC_RING_CTL_SIZE(bytes)          (((uint32_t)(bytes) - 4096u) & 0x001FF000u)
#define ARC_RING_ACTHD_UDW(base)            ((base) + 0x05Cu)
#define ARC_RING_IPEHR(base)                ((base) + 0x068u)
#define ARC_RING_INSTDONE(base)             ((base) + 0x06Cu)
#define ARC_RING_ACTHD(base)                ((base) + 0x074u)
#define ARC_RING_HWS_PGA(base)              ((base) + 0x080u)
#define ARC_RING_HWSTAM(base)               ((base) + 0x098u)
#define ARC_RING_MI_MODE(base)              ((base) + 0x09Cu)
#define   ARC_MI_STOP_RING                  (1u << 8)
#define   ARC_MI_MODE_IDLE                  (1u << 9)
#define ARC_RING_IMR(base)                  ((base) + 0x0A8u)
#define ARC_RING_EIR(base)                  ((base) + 0x0B0u)
#define ARC_RING_EMR(base)                  ((base) + 0x0B4u)
#define ARC_RING_ESR(base)                  ((base) + 0x0B8u)
#define ARC_RING_CMD_CCTL(base)             ((base) + 0x0C4u)
#define ARC_RING_RESET_CTL(base)            ((base) + 0x0D0u)
#define   ARC_RESET_CTL_CAT_ERROR           (1u << 2)
#define   ARC_RESET_CTL_READY_TO_RESET      (1u << 1)
#define   ARC_RESET_CTL_REQUEST_RESET       (1u << 0)
#define ARC_RING_CS_DEBUG_MODE2(base)       ((base) + 0x0D8u) /* Masked: CONSTANT_BUFFERAddressOffsetDisable */
#define   ARC_CONSTANT_BUFFER_OFFSET_DISABLE (1u << 4)        /* 3DSTATE_CONSTANT_* uses 48-bit PPGTT VA (ANV gfx125_init_device_state) */
/* Wa_18022495364 (bit 6) is scoped to IP 12.00..12.10 in xe_wa_oob.rules
 * and i915 gen12_emit_indirect_ctx_rcs. It must NOT be enabled on MTL 12.7x. */
#define   ARC_CS_DEBUG_MODE2_RCS_DEFAULT    ARC_CONSTANT_BUFFER_OFFSET_DISABLE
#define ARC_RING_BLIT_CCTL(base)            ((base) + 0x204u) /* Non-masked: [14:9]=dst MOCS, [6:1]=src MOCS */
#define ARC_RING_EXECLIST_STATUS_LO(base)   ((base) + 0x234u)
#define ARC_RING_EXECLIST_STATUS_HI(base)   ((base) + 0x238u)
#define ARC_RING_CURRENT_LRCA(base)         ((base) + 0x240u)
#define ARC_RING_CONTEXT_CONTROL(base)      ((base) + 0x244u)
#define   ARC_CTX_CTRL_INHIBIT_SYN_SWITCH   (1u << 3)
#define   ARC_CTX_CTRL_RESTORE_INHIBIT      (1u << 0)
#define ARC_RING_GFX_MODE(base)             ((base) + 0x29Cu)
#define   ARC_GFX_DISABLE_LEGACY_MODE       (1u << 3)
#define ARC_RING_TIMESTAMP(base)            ((base) + 0x358u)
#define ARC_RING_TIMESTAMP_UDW(base)        ((base) + 0x35Cu)
#define ARC_RING_EXECLIST_SQ_LO(base)       ((base) + 0x510u)
#define ARC_RING_EXECLIST_SQ_HI(base)       ((base) + 0x514u)
#define ARC_RING_EXECLIST_CONTROL(base)     ((base) + 0x550u)
#define   ARC_EL_CTRL_LOAD                  (1u << 0)

/* Logical Ring Context (LRC) Descriptor & Context Register Offsets (in DWORDs from Context Image start) */
#define ARC_LRC_DESC_VALID                  (1ULL << 0)
#define ARC_LRC_DESC_LEGACY_64B_PPGTT       (3ULL << 3)   /* 48-bit 4-level PPGTT */
#define ARC_LRC_DESC_PRIVILEGE_PPGTT        (1ULL << 8)
#define ARC_LRC_DESC_XEHP_SW_CTX_SHIFT      39u           /* Bits [54:39] on Graphics IP >= 12.50 */

#define ARC_CTX_LRI_HEADER_0                0x01u
#define ARC_CTX_CONTEXT_CONTROL_REG         0x02u
#define ARC_CTX_CONTEXT_CONTROL_VAL         0x03u
#define ARC_CTX_RING_HEAD_REG               0x04u
#define ARC_CTX_RING_HEAD_VAL               0x05u
#define ARC_CTX_RING_TAIL_REG               0x06u
#define ARC_CTX_RING_TAIL_VAL               0x07u
#define ARC_CTX_RING_START_REG              0x08u
#define ARC_CTX_RING_START_VAL              0x09u
#define ARC_CTX_RING_CTL_REG                0x0Au
#define ARC_CTX_RING_CTL_VAL                0x0Bu
#define ARC_CTX_BB_PER_CTX_PTR_REG          0x12u
#define ARC_CTX_BB_PER_CTX_PTR_VAL          0x13u
#define ARC_CTX_PDP0_UDW_REG                0x30u
#define ARC_CTX_PDP0_UDW_VAL                0x31u
#define ARC_CTX_PDP0_LDW_REG                0x32u
#define ARC_CTX_PDP0_LDW_VAL                0x33u

/* ============================================================================
 * 5. Intel GPU Command Streamer (MI_* & GFX_OP_PIPE_CONTROL) Instructions
 * ============================================================================ */
#define ARC_MI_NOOP                         0x00000000u
#define ARC_MI_USER_INTERRUPT               (0x02u << 23)
#define ARC_MI_ARB_CHECK                    (0x05u << 23)
#define   ARC_MI_PREPARSER_DISABLE(dis)     (ARC_MI_ARB_CHECK | (1u << 8) | ((dis) ? 1u : 0u))
#define ARC_MI_ARB_ON_OFF                   (0x08u << 23)
#define   ARC_MI_ARB_DISABLE                0u
#define   ARC_MI_ARB_ENABLE                 (1u << 0)
#define ARC_CMD_3DSTATE_MESH_CONTROL        ((0x3u << 29) | (0x3u << 27) | (0x77u << 16) | 1u) /* 3 dwords (Wa_14019789679) */
#define ARC_MI_BATCH_BUFFER_END             (0x0Au << 23)
#define ARC_MI_BATCH_BUFFER_END_CTX         ((0x0Au << 23) | 1u) /* BIT(0) = End Context Restore */
#define ARC_MI_LOAD_REGISTER_IMM(n)         ((0x22u << 23) | (2u * (uint32_t)(n) - 1u))
#define   ARC_MI_LRI_FORCE_POSTED           (1u << 12)
#define   ARC_MI_LRI_CS_MMIO                (1u << 19)
#define ARC_MI_STORE_DATA_IMM               (0x20u << 23)
#define   ARC_MI_SDI_USE_GGTT               (1u << 22)
#define   ARC_MI_SDI_DWORD_LEN              2u                   /* MI_SDI_NUM_DW(1): 4 dwords - 2 */
#define   ARC_MI_SDI_QWORD_LEN              ((1u << 21) | 3u)    /* MI_SDI_NUM_QW(1): BIT(21) | (5 - 2) */
#define ARC_MI_FLUSH_DW                     (0x26u << 23)
#define   ARC_MI_FLUSH_DW_STORE_INDEX       (1u << 21)
#define   ARC_MI_FLUSH_DW_INV_TLB           (1u << 18)
#define   ARC_MI_FLUSH_DW_OP_STOREDW        (1u << 14)
#define   ARC_MI_FLUSH_DW_IMM_LEN           2u                   /* 4-dword MI_FLUSH_DW length (4 - 2) */
#define   ARC_MI_FLUSH_DW_USE_GGTT          (1u << 2)
#define ARC_MI_BATCH_BUFFER_START_PPGTT     ((0x31u << 23) | (1u << 8) | 1u) /* 3 dwords, PPGTT (bit 8) */
#define ARC_MI_BATCH_BUFFER_START_GGTT      ((0x31u << 23) | 1u)             /* 3 dwords, GGTT */

/* 3D Pipeline PIPE_CONTROL (6 dwords on Gen12 / Xe-LPG) */
#define ARC_GFX_OP_PIPE_CONTROL(len)        ((0x3u << 29) | (0x3u << 27) | (0x2u << 24) | ((uint32_t)(len) - 2u))
/* Dword 0 flags */
#define   ARC_PC0_L3_RO_CACHE_INVALIDATE    (1u << 10)
#define   ARC_PC0_HDC_PIPELINE_FLUSH        (1u << 9)
#define   ARC_PC_GROUP0_HDC_FLUSH           ARC_PC0_HDC_PIPELINE_FLUSH
/* Dword 1 flags */
#define   ARC_PC_CMD_CACHE_INVALIDATE       (1u << 29)
#define   ARC_PC_TILE_CACHE_FLUSH           (1u << 28)
#define   ARC_PC_GLOBAL_GTT                 (1u << 24)    /* PIPE_CONTROL_GLOBAL_GTT_IVB (in dword 1!) */
#define   ARC_PC_CS_STALL                   (1u << 20)
#define   ARC_PC_TLB_INVALIDATE             (1u << 18)
#define   ARC_PC_QW_WRITE                   (1u << 14)
#define   ARC_PC_DEPTH_STALL                (1u << 13)
#define   ARC_PC_RT_CACHE_FLUSH             (1u << 12)
#define   ARC_PC_INST_CACHE_INVALIDATE      (1u << 11)
#define   ARC_PC_TEX_CACHE_INVALIDATE       (1u << 10)
#define   ARC_PC_INDIRECT_STATE_DISABLE     (1u << 9)
#define   ARC_PC_FLUSH_ENABLE               (1u << 7)
#define   ARC_PC_DC_FLUSH_ENABLE            (1u << 5)
#define   ARC_PC_VF_CACHE_INVALIDATE        (1u << 4)
#define   ARC_PC_CONST_CACHE_INVALIDATE     (1u << 3)
#define   ARC_PC_STATE_CACHE_INVALIDATE     (1u << 2)
#define   ARC_PC_STALL_AT_SCOREBOARD        (1u << 1)
#define   ARC_PC_DEPTH_CACHE_FLUSH          (1u << 0)

/* Xe-LPG 12.70..12.74 Hardware Workaround & Render Tuning Registers (Linux xe_wa.c / xe_tuning.c / xe_hw_engine.c / Mesa ANV genX_init_state.c) */
#define ARC_GFXPIPE_3D_CMD(op, sub)         ((0x3u << 29) | (0x3u << 27) | ((uint32_t)(op) << 24) | ((uint32_t)(sub) << 16))
#define ARC_REG_CS_CHICKEN1                 0x002580u /* Masked RCS0 (xe_engine_regs.h:212 / ANV genX_init_state.c:525) */
#define   ARC_CS_PREEMPT_3D_OBJECT_LEVEL    (1u << 0) /* Disable BIT(0) -> MidcmdbufferPreemption */
#define ARC_REG_CHICKEN_RASTER_1            0x006204u /* Masked MCR (xe_gt_regs.h:126) */
#define   ARC_DIS_SF_ROUND_NEAREST_EVEN     (1u << 8)
#define ARC_REG_CHICKEN_RASTER_2            0x006208u /* Masked MCR (xe_gt_regs.h:130 / ANV genX_init_state.c:627) */
#define   ARC_TBIMR_FAST_CLIP               (1u << 5)
#define ARC_REG_FF_MODE2                    0x006604u /* Non-masked MCR (xe_gt_regs.h:147 / ANV: GS=224, TDS=4, HS=224) */
#define   ARC_FF_MODE2_ANV_TIMERS           0xE004E000u
#define ARC_REG_XEHPG_INSTDONE_GEOM_SVG     0x00666Cu /* Read-only MCR 3D Geometry/SVG INSTDONE */
#define ARC_REG_CACHE_MODE_1                0x007004u /* Masked MCR */
#define   ARC_MSAA_OPTIMIZATION_REDUC_DIS   (1u << 11)/* Wa_18019271663 */
#define ARC_REG_XEHP_PSS_CHICKEN            0x007044u /* Masked MCR */
#define   ARC_FD_END_COLLECT                (1u << 5) /* Wa_14019877138 */
#define ARC_REG_XEHPG_SC_INSTDONE           0x007100u /* Read-only Subslice INSTDONE */
#define ARC_REG_COMMON_SLICE_CHICKEN3       0x007304u /* Masked MCR (xe_lrc.c / anv_queue.c) */
#define   ARC_DISABLE_STATE_CACHE_PERF_FIX  (1u << 13)
#define ARC_REG_SQCNT1                      0x008718u /* Non-masked MCR */
#define   ARC_SQCNT1_ENFORCE_RAR            (1u << 23)/* Wa_22016670082 */
#define ARC_REG_MISCCPCTL                   0x009424u /* Non-masked GT */
#define   ARC_DOP_CLOCK_GATE_RENDER_ENABLE  (1u << 1) /* Wa_14015795083 */
#define ARC_REG_XEHP_L3SQCREG5              0x00B158u /* Non-masked MCR */
#define   ARC_L3_PWM_TIMER_INIT_VAL_MASK    0x000003FFu
#define   ARC_L3_PWM_TIMER_INIT_7F          0x0000007Fu
#define ARC_REG_COMP_MOD_CTRL               0x00CF30u /* Non-masked GAM MCR */
#define   ARC_FORCE_MISS_FTLB               (1u << 3) /* Wa_14018575942 */
#define ARC_REG_SAMPLER_INSTDONE            0x00E160u /* Read-only MCR Sampler INSTDONE */
#define ARC_REG_ROW_INSTDONE                0x00E164u /* Read-only MCR EU Row INSTDONE */
#define ARC_REG_SAMPLER_MODE                0x00E18Cu /* Masked MCR */
#define   ARC_INDIRECT_STATE_BASE_ADDR_OVERRIDE (1u << 0) /* Xe-LPG Render Tuning (xe_tuning.c:91-95) */
#define ARC_REG_ROW_CHICKEN3                0x00E49Cu /* Masked MCR */
#define   ARC_DIS_FIX_EOT1_FLUSH            (1u << 9) /* Wa_14017856879 */
#define ARC_REG_ROW_CHICKEN2                0x00E4F4u /* Masked MCR */
#define   ARC_DISABLE_TDL_SVHS_GATING       (1u << 1) /* Wa_14020495402 */
#define ARC_REG_XEHP_HDC_CHICKEN0           0x00E5F0u /* Masked MCR */
#define   ARC_DIS_ATOMIC_CHAIN_TYPED_WRITES (1u << 3) /* Wa_14015150844 */
#define ARC_REG_RCU_MODE                    0x014800u /* Masked GT */
#define   ARC_RCU_MODE_CCS_ENABLE           (1u << 0) /* xe_hw_engine.c:457 */

/* ============================================================================
 * 6. GuC v70 Firmware Loader, Engine Domain Reset & CTB Registers (mtl_guc_70.bin)
 * ============================================================================ */
#define ARC_REG_GEN11_SOFT_RESET            0x190090u
#define ARC_REG_GDRST                       0x00941Cu
#define   ARC_GRDOM_FULL                    (1u << 0)
#define   ARC_GRDOM_RENDER                  (1u << 1)
#define   ARC_GRDOM_BLT                     (1u << 2)
#define   ARC_GRDOM_GUC                     (1u << 3)

#define ARC_REG_GUC_STATUS                  0x00C000u
#define   ARC_GS_MIA_IN_RESET               (1u << 0)
#define   ARC_GS_BOOTROM_SHIFT              1u
#define   ARC_GS_BOOTROM_MASK               0x7Fu
#define   ARC_GS_UKERNEL_SHIFT              8u
#define   ARC_GS_UKERNEL_MASK               0xFFu
#define   ARC_GS_UKERNEL_READY              0xF0u
#define   ARC_GS_AUTH_GOOD                  (2u << 30)

#define ARC_REG_GUC_WOPCM_SIZE              0x00C050u
#define   ARC_GUC_WOPCM_SIZE_LOCKED         (1u << 0)
#define   ARC_GUC_WOPCM_SIZE_MASK           0xFFFFF000u
#define ARC_REG_GUC_SHIM_CONTROL            0x00C064u
#define   ARC_GUC_SHIM_DEFAULT              ((1u << 21) | (1u << 15) | (1u << 10) | \
                                             (1u << 9)  | (1u << 4)  | (1u << 2)  | (1u << 1))
#define ARC_REG_SOFT_SCRATCH(n)             (0x00C180u + (uint32_t)(n) * 4u)
#define ARC_REG_UOS_RSA_SCRATCH(i)          (0x00C200u + (uint32_t)(i) * 4u)
#define ARC_UOS_RSA_SCRATCH_COUNT           64u

#define ARC_REG_DMA_ADDR_0_LOW              0x00C300u
#define ARC_REG_DMA_ADDR_0_HIGH             0x00C304u
#define ARC_REG_DMA_ADDR_1_LOW              0x00C308u
#define ARC_REG_DMA_ADDR_1_HIGH             0x00C30Cu
#define   ARC_DMA_SPACE_WOPCM               (7u << 16)
#define   ARC_DMA_SPACE_GGTT                (8u << 16)
#define ARC_REG_DMA_COPY_SIZE               0x00C310u
#define ARC_REG_DMA_CTRL                    0x00C314u
#define   ARC_DMA_CTRL_UOS_MOVE             (1u << 4)
#define   ARC_DMA_CTRL_START                (1u << 0)
#define ARC_REG_DMA_GUC_WOPCM_OFFSET        0x00C340u
#define   ARC_GUC_WOPCM_OFFSET_VALID        (1u << 0)
#define ARC_REG_GUC_SEND_INTERRUPT          0x00C4C8u
#define   ARC_GUC_SEND_TRIGGER              (1u << 0)
#define ARC_REG_GUC_HOST_INTERRUPT          0x1901F0u

/* Default MTL WOPCM Partitioning (2 MiB Total WOPCM, GuC at 640 KiB offset) */
#define ARC_MTL_WOPCM_TOTAL_SIZE            0x200000u
#define ARC_MTL_GUC_WOPCM_BASE              0x0A0000u
#define ARC_MTL_GUC_WOPCM_SIZE              (ARC_MTL_WOPCM_TOTAL_SIZE - ARC_MTL_GUC_WOPCM_BASE - 0x10000u)

/* Intel GuC CSS Firmware Header (128 bytes = 32 dwords) */
struct ArcUcCssHeader {
    uint32_t module_type;
    uint32_t header_size_dw;        /* Typically 32 (128 bytes) */
    uint32_t header_version;
    uint32_t module_id;
    uint32_t module_vendor;
    uint32_t date;
    uint32_t size_dw;               /* Total size in dwords (header + key + RSA + uCode) */
    uint32_t key_size_dw;           /* Typically 64 (256 bytes) */
    uint32_t modulus_size_dw;
    uint32_t exponent_size_dw;
    uint32_t time[2];
    char     username[8];
    char     buildnumber[12];
    uint32_t sw_version;            /* Major[23:16], Minor[15:8], Patch[7:0] */
    uint32_t vf_version;
    uint32_t reserved0[11];
    uint32_t private_data_size;
    uint32_t header_info;
};
ARC_STATIC_ASSERT(sizeof(struct ArcUcCssHeader) == 128, "Intel GuC CSS header must be 128 bytes");

/* GuC MMIO & CTB Handshake Constants (GuC v70 ABI) */
#define ARC_GUC_HXG_TYPE_REQUEST            0u
#define ARC_GUC_HXG_TYPE_FAST_REQUEST       2u
#define ARC_GUC_HXG_TYPE_RESPONSE_SUCCESS   7u
#define ARC_GUC_ACTION_REGISTER_CTB         0x4505u
#define ARC_GUC_ACTION_DEREGISTER_CTB       0x4506u
#define ARC_GUC_CTB_TYPE_HOST2GUC           0u
#define ARC_GUC_CTB_TYPE_GUC2HOST           1u

struct ArcGucCtbDesc {
    uint32_t head;
    uint32_t tail;
    uint32_t status;
    uint32_t reserved[13];
};
ARC_STATIC_ASSERT(sizeof(struct ArcGucCtbDesc) == 64, "GuC CTB descriptor must be 64 bytes");

/* ============================================================================
 * 7. Display IP Version 14 (Meteor Lake) Pipe, Plane & South PWM Registers
 * ============================================================================ */
#define ARC_DISP_HTOTAL(t)                  (0x60000u + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_HBLANK(t)                  (0x60004u + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_HSYNC(t)                   (0x60008u + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_VTOTAL(t)                  (0x6000Cu + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_VBLANK(t)                  (0x60010u + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_VSYNC(t)                   (0x60014u + (uint32_t)(t) * 0x1000u)
#define ARC_DISP_PIPESRC(t)                 (0x6001Cu + (uint32_t)(t) * 0x1000u)

#define ARC_DISP_TRANSCONF(p)               (0x70008u + (uint32_t)(p) * 0x1000u)
#define   ARC_DISP_TRANS_ENABLE             (1u << 31)
#define ARC_DISP_PIPE_FRMCOUNT(p)           (0x70040u + (uint32_t)(p) * 0x1000u)
#define ARC_DISP_PIPE_FLIPCOUNT(p)          (0x70044u + (uint32_t)(p) * 0x1000u)

#define ARC_DISP_PLANE_CTL_1(p)             (0x70180u + (uint32_t)(p) * 0x1000u)
#define   ARC_PLANE_CTL_ENABLE              (1u << 31)
#define   ARC_PLANE_CTL_FMT_SHIFT           23u
#define   ARC_PLANE_CTL_FMT_MASK            0x1Fu
#define   ARC_PLANE_CTL_FMT_XRGB8888        8u
#define   ARC_PLANE_CTL_FMT_XBGR8888        10u
#define   ARC_PLANE_CTL_TILE_SHIFT          10u
#define   ARC_PLANE_CTL_TILE_MASK           0x7u
#define   ARC_PLANE_CTL_TILE_LINEAR         0u
#define   ARC_PLANE_CTL_TILE_X              1u
#define   ARC_PLANE_CTL_TILE_4              5u        /* Display 14 Tile4 */
#define ARC_DISP_PLANE_STRIDE_1(p)          (0x70188u + (uint32_t)(p) * 0x1000u)
#define ARC_DISP_PLANE_POS_1(p)             (0x7018Cu + (uint32_t)(p) * 0x1000u)
#define ARC_DISP_PLANE_SIZE_1(p)            (0x70190u + (uint32_t)(p) * 0x1000u)
#define ARC_DISP_PLANE_SURF_1(p)            (0x7019Cu + (uint32_t)(p) * 0x1000u)

/* South Display (MTP PCH) Backlight PWM */
#define ARC_DISP_BLC_PWM_CTL                0xC8250u
#define   ARC_BLC_PWM_ENABLE                (1u << 31)
#define ARC_DISP_BLC_PWM_FREQ               0xC8254u
#define ARC_DISP_BLC_PWM_DUTY               0xC8258u
