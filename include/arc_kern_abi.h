/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 *
 * Kernel-to-Kernel ABI between ArcXe.kext, ArcFB.kext, and ArcAccel.kext.
 * Invoked via IOService::callPlatformFunction().
 */

#pragma once

#include <stdint.h>

#define ARC_KERN_ABI_VERSION                1u

/* ArcFB.kext Platform Functions */
#define ARC_FB_FN_GET_SCANOUT               "arcGetScanoutInfo"
#define ARC_FB_FN_FLIP_SURFACE              "arcFlipToGgttSurface"

/* ArcXe.kext Platform Functions (called by ArcAccel.kext) */
#define ARC_XE_FN_PIN_SURFACE_GGTT          "arcPinSurfaceGgtt"
#define ARC_XE_FN_UNPIN_SURFACE_GGTT        "arcUnpinSurfaceGgtt"
#define ARC_XE_FN_GET_UTILIZATION           "arcGetGpuUtilization"

struct ArcScanoutInfoReq {
    uint32_t version;
    uint32_t pipe;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t ggtt_boot_offset;
    uint64_t phys_addr;
    void    *kva;                       /* Write-combined CPU virtual mapping of scanout */
};

struct ArcFlipSurfaceReq {
    uint32_t version;
    uint32_t flags;                     /* bit 0: restore boot GOP surface */
    uint32_t ggtt_offset;               /* 4KB-aligned GGTT offset of new scanout buffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;                     /* Must be multiple of 64 bytes for linear XRGB8888 */
    uint32_t tile_mode;                 /* 0 = linear, 5 = Tile4 */
    int32_t  status;
};

struct ArcPinGgttReq {
    uint32_t version;
    uint32_t pad0;
    void    *iomem_desc;                /* IOMemoryDescriptor * of IOAccelResource2 / IOSurface */
    uint64_t size;
    uint32_t out_ggtt_offset;
    int32_t  status;
};
