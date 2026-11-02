/*
 * SPDX-License-Identifier: MIT
 *
 * Shared Kernel <-> Userspace IOUserClient ABI for ArcXe.kext.
 * Consumed by:
 *   - kexts/ArcXe (XNU Kernel Mode Driver)
 *   - mesa-anv/anv_xnu_drm_shim.c (Mesa ANV Vulkan ICD on macOS)
 *   - plugin/ArcMTLDriver.bundle (IOSurface zero-copy UMA bridge)
 *   - tools/arc_probe (CLI hardware & GPU execution diagnostic)
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCXE_IOKIT_SERVICE_NAME            "ArcXeService"
#define ARC_ACCEL_IOKIT_SERVICE_NAME        "ArcAccel"
#define ARC_XE_ACCEL_OPEN_TYPE              0x41520000u  /* 'AR' << 16: opens IOAccelArcXeBridge via ArcAccel */
#define ARCXE_UAPI_VERSION                  1u

/* IOConnectCallMethod Selectors */
enum ArcXeMethodSelector : uint32_t {
    kArcXeMethodQueryDevice             = 0,
    kArcXeMethodGemCreate               = 1,
    kArcXeMethodGemImportIOSurface      = 2,
    kArcXeMethodGemImportUserPtr        = 3,
    kArcXeMethodGemClose                = 4,
    kArcXeMethodGemMmapInfo             = 5,
    kArcXeMethodVmCreate                = 6,
    kArcXeMethodVmDestroy               = 7,
    kArcXeMethodVmBind                  = 8,
    kArcXeMethodExecQueueCreate         = 9,
    kArcXeMethodExecQueueDestroy        = 10,
    kArcXeMethodExec                    = 11,
    kArcXeMethodWaitQueueIdle           = 12,
    kArcXeMethodSyncobjCreate           = 13,
    kArcXeMethodSyncobjDestroy          = 14,
    kArcXeMethodSyncobjWait             = 15,
    kArcXeMethodSyncobjSignalReset      = 16,
    kArcXeMethodCount                   = 17,
};

/* Engine Classes (matches drm_xe_engine_class_instance) */
enum ArcXeEngineClass : uint16_t {
    kArcXeEngineClassRender             = 0,  /* RCS0: 3D Render + Compute + Ray Tracing */
    kArcXeEngineClassCopy               = 1,  /* BCS0: Async Blitter / Copy */
    kArcXeEngineClassVideoDecode        = 2,
    kArcXeEngineClassVideoEnhance       = 3,
    kArcXeEngineClassCompute            = 4,  /* CCS0: Compute */
};

/* BO Allocation Flags */
#define ARCXE_GEM_FLAG_CPU_WB_COHERENT      (1u << 0)  /* Default UMA 1-way/2-way coherent WB */
#define ARCXE_GEM_FLAG_CPU_WC               (1u << 1)  /* Write-Combined (Scanout / Streaming) */
#define ARCXE_GEM_FLAG_SCANOUT              (1u << 2)  /* Pin in GGTT for ArcFB direct scanout */
#define ARCXE_GEM_FLAG_NEEDS_CPU_VIS        (1u << 3)

/* VM_BIND Operations */
enum ArcXeVmBindOp : uint32_t {
    kArcXeVmBindOpMap                   = 0,
    kArcXeVmBindOpUnmap                 = 1,
    kArcXeVmBindOpMapUserPtr            = 2,
    kArcXeVmBindOpMapNull               = 3,
};

/* Submission Backend Active Mode */
enum ArcXeSubmitBackend : uint32_t {
    kArcXeSubmitExeclist                = 0,  /* Direct Hardware Execlist (RCS0/CCS0/BCS0) */
    kArcXeSubmitGuc                     = 1,  /* GuC v70 CTB Firmware Submission */
};

/* 0. kArcXeMethodQueryDevice (Struct Out) */
struct ArcXeQueryDeviceOut {
    uint32_t uapi_version;
    uint16_t vendor_id;                 /* 0x8086 */
    uint16_t device_id;                 /* 0x7D55 */
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint8_t  revision_id;
    uint8_t  va_bits;                   /* 48 */
    uint16_t max_page_Level;            /* 4 (PML4) */
    uint32_t graphics_verx100;          /* 1270 (Xe-LPG) */
    uint32_t media_verx100;             /* 1300 (Xe-LPM+) */
    uint32_t gmd_id_gt0;
    uint32_t gmd_id_gt1;
    uint32_t crystal_clock_hz;          /* e.g. 38400000 or 24000000 */
    uint32_t gpu_timestamp_freq_hz;     /* Typically 19200000 or 12500000 on MTL */
    uint32_t geom_dss_mask;             /* Active Xe-core mask (up to 8 bits on 155H) */
    uint32_t compute_dss_mask;
    uint32_t eu_per_dss_mask;           /* Up to 16 Vector Engines per Xe-core */
    uint32_t total_xe_cores;
    uint32_t total_eus;
    uint32_t submit_backend;            /* ArcXeSubmitBackend */
    uint32_t guc_fw_loaded;             /* 1 if mtl_guc_70.bin authenticated & READY */
    uint32_t guc_status_reg;
    uint64_t bar0_phys;
    uint64_t bar2_phys;
    uint64_t bar2_size;
    uint64_t stolen_base_phys;
    uint64_t stolen_size;
    uint64_t ggtt_usable_start;
    uint64_t ggtt_usable_size;
    uint64_t sysmem_total_bytes;
    uint64_t gpu_timestamp_ticks;
};

/* 1. kArcXeMethodGemCreate */
struct ArcXeGemCreateIn {
    uint64_t size;
    uint32_t flags;                     /* ARCXE_GEM_FLAG_* */
    uint16_t pat_index;                 /* ARC_MTL_PAT_IDX_* */
    uint16_t pad0;
};
struct ArcXeGemCreateOut {
    uint32_t gem_handle;
    uint32_t pad0;
    uint64_t actual_size;
    uint32_t ggtt_offset;               /* Non-zero if ARCXE_GEM_FLAG_SCANOUT */
    uint32_t pad1;
};

/* 2. kArcXeMethodGemImportIOSurface */
struct ArcXeGemImportIOSurfaceIn {
    uint32_t iosurface_id;
    uint16_t pat_index;
    uint16_t pin_ggtt_for_scanout;
};
struct ArcXeGemImportIOSurfaceOut {
    uint32_t gem_handle;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint32_t pixel_format;
    uint32_t ggtt_offset;
    uint64_t alloc_size;
};

/* 3. kArcXeMethodGemImportUserPtr */
struct ArcXeGemImportUserPtrIn {
    uint64_t user_addr;
    uint64_t size;
    uint16_t pat_index;
    uint16_t pad0;
    uint32_t flags;
};
struct ArcXeGemImportUserPtrOut {
    uint32_t gem_handle;
    uint32_t pad0;
    uint64_t actual_size;
};

/* 4. kArcXeMethodGemClose */
struct ArcXeGemCloseIn {
    uint32_t gem_handle;
    uint32_t pad0;
};

/* 5. kArcXeMethodGemMmapInfo
 * Note: Actual virtual address mapping into caller's task uses:
 *   IOConnectMapMemory64(connect, gem_handle, mach_task_self(), &addr, &size, kIOMapAnywhere)
 */
struct ArcXeGemMmapInfoIn {
    uint32_t gem_handle;
    uint32_t pad0;
};
struct ArcXeGemMmapInfoOut {
    uint64_t size;
    uint64_t fake_mmap_offset;          /* ((uint64_t)gem_handle << 32) for Mesa DRM compatibility */
};

/* 6. kArcXeMethodVmCreate / 7. kArcXeMethodVmDestroy */
struct ArcXeVmCreateIn {
    uint32_t flags;
    uint32_t pad0;
};
struct ArcXeVmCreateOut {
    uint32_t vm_id;
    uint32_t pad0;
    uint64_t pml4_phys;
};
struct ArcXeVmDestroyIn {
    uint32_t vm_id;
    uint32_t pad0;
};

/* 8. kArcXeMethodVmBind */
#define ARCXE_MAX_SYNCS_PER_OP              8u

struct ArcXeSyncEntry {
    uint32_t syncobj_handle;
    uint32_t flags;                     /* bit 0: signal (1) vs wait (0); bit 1: timeline */
    uint64_t timeline_value;
};

struct ArcXeVmBindIn {
    uint32_t vm_id;
    uint32_t op;                        /* ArcXeVmBindOp */
    uint32_t gem_handle;                /* 0 for Unmap / MapNull */
    uint16_t pat_index;                 /* ARC_MTL_PAT_IDX_* */
    uint16_t flags;
    uint64_t gpu_va;                    /* 48-bit canonical/non-canonical GPU VA (4KB aligned) */
    uint64_t bo_offset;
    uint64_t range_size;
    uint64_t user_ptr;                  /* Used if op == kArcXeVmBindOpMapUserPtr */
    uint32_t num_syncs;
    uint32_t pad0;
    struct ArcXeSyncEntry syncs[ARCXE_MAX_SYNCS_PER_OP];
};

/* 9. kArcXeMethodExecQueueCreate / 10. kArcXeMethodExecQueueDestroy */
struct ArcXeExecQueueCreateIn {
    uint32_t vm_id;
    uint16_t engine_class;              /* ArcXeEngineClass */
    uint16_t engine_instance;
    uint32_t ring_size_bytes;           /* Default 16384 */
    uint32_t priority;
};
struct ArcXeExecQueueCreateOut {
    uint32_t exec_queue_id;
    uint32_t ggtt_lrc_addr;
};
struct ArcXeExecQueueDestroyIn {
    uint32_t exec_queue_id;
    uint32_t pad0;
};

/* 11. kArcXeMethodExec */
struct ArcXeExecIn {
    uint32_t exec_queue_id;
    uint32_t num_batch_buffer;          /* Typically 1 */
    uint64_t batch_gpu_va;              /* 48-bit PPGTT address of batch buffer */
    uint32_t num_syncs;
    uint32_t flags;
    struct ArcXeSyncEntry syncs[ARCXE_MAX_SYNCS_PER_OP];
};
struct ArcXeExecOut {
    uint64_t submitted_seqno;
};

/* 12. kArcXeMethodWaitQueueIdle */
struct ArcXeWaitQueueIn {
    uint32_t exec_queue_id;
    uint32_t pad0;
    uint64_t target_seqno;              /* 0 = wait for latest submitted seqno */
    uint64_t timeout_ns;
};
struct ArcXeWaitQueueOut {
    uint64_t completed_seqno;
    uint32_t timed_out;
    uint32_t engine_status;
    uint32_t engine_status_hi;
    uint32_t current_lrca;
    uint32_t ring_head;
    uint32_t ring_tail;
    uint32_t ring_start;
    uint32_t ring_ctl;
    uint32_t acthd_lo;
    uint32_t acthd_hi;
    uint32_t ipehr;
    uint32_t esr;
    uint32_t instdone;
    uint32_t mi_mode;
};

/* 13..16. Syncobj Operations (for Mesa vk_drm_syncobj compatibility) */
struct ArcXeSyncobjCreateIn {
    uint32_t signaled;                  /* 1 = create pre-signaled */
    uint32_t pad0;
};
struct ArcXeSyncobjCreateOut {
    uint32_t handle;
    uint32_t pad0;
};
struct ArcXeSyncobjDestroyIn {
    uint32_t handle;
    uint32_t pad0;
};

#define ARCXE_SYNCOBJ_WAIT_ALL              (1u << 0)
struct ArcXeSyncobjWaitIn {
    uint32_t count_handles;
    uint32_t flags;
    int64_t  timeout_nsec;              /* Absolute CLOCK_MONOTONIC or relative ns */
    uint32_t handles[ARCXE_MAX_SYNCS_PER_OP];
    uint64_t points[ARCXE_MAX_SYNCS_PER_OP]; /* 0 for binary syncobj */
};
/* Timeout parser breadcrumbs; these do not certify batch completion. */
#define ARCXE_PROGRESS_VALID       (1u << 31)
#define ARCXE_PROGRESS_BEGIN       (1u << 0)
#define ARCXE_PROGRESS_BEFORE_BB   (1u << 1)
#define ARCXE_PROGRESS_RETURNED    (1u << 2)
#define ARCXE_PROGRESS_INVALIDATED (1u << 3)
struct ArcXeSyncobjWaitOut {
    uint32_t first_signaled;
    uint32_t timed_out;
    struct ArcXeWaitQueueOut diag;
    uint32_t dbg2;
    uint32_t geom_instdone;
    uint32_t sc_instdone;
    uint32_t sampler_instdone;
    uint32_t row_instdone;
    uint32_t progress_flags;          /* Former pad0: same ABI size/offset. Zero on older kernels. */
};

/* Optional tails of SyncobjWait output. Both the original 96-byte prefix and
 * the extended 200/280-byte replies remain stable. New callers request 520
 * bytes; kernels negotiate the largest complete version that fits. */
struct ArcXeTimeoutHwState {
    uint32_t version;                 /* 1: primary MCR instance, before reset */
    uint32_t cmd_cctl, gfx_mode, cs_chicken1;
    uint32_t l3_alloc, l3_sqcreg5, sampler_mode, rcc, pss_chicken;
    uint32_t sc_extra, sc_extra2;
    uint32_t pat[5];
    uint32_t mocs1, mocs9, lncf0, lncf4;
};
struct ArcXeTimeoutDssSample {
    uint32_t steering;
    uint32_t sampler_mode, row_chicken2, row_chicken3, hdc_chicken0;
    uint32_t sampler_instdone, row_instdone;
};
struct ArcXeTimeoutMcrState {
    uint32_t version, selector, enabled_dss, count;
    struct ArcXeTimeoutDssSample samples[8];
};
#define ARCXE_SYNCOBJ_WAIT_V2_SIZE 280u
#define ARCXE_SYNCOBJ_WAIT_V1_SIZE 200u /* original extended reply */
struct ArcXeTimeoutDetail {
    uint32_t valid;
    uint32_t fault_reg;
    uint32_t fault_data0;
    uint32_t fault_data1;
    uint32_t ring_offset;
    uint32_t ring_count;
    uint32_t ring_dw[16];
    uint32_t context_control;
    uint32_t context_head;
    uint32_t context_tail;
    uint32_t idle_park_count;
    struct ArcXeTimeoutHwState hw;
    struct ArcXeTimeoutMcrState mcr;
};
struct ArcXeSyncobjWaitOutExtended {
    struct ArcXeSyncobjWaitOut base;
    struct ArcXeTimeoutDetail detail;
};

struct ArcXeSyncobjSignalResetIn {
    uint32_t count_handles;
    uint32_t is_reset;                  /* 1 = reset to 0, 0 = signal */
    uint32_t handles[ARCXE_MAX_SYNCS_PER_OP];
    uint64_t points[ARCXE_MAX_SYNCS_PER_OP];
};

#ifdef __cplusplus
}
#endif
