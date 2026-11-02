/*
 * SPDX-License-Identifier: MIT
 *
 * Darwin / XNU IOKit Transport Backend for Mesa Intel ANV (`libvulkan_intel.dylib`).
 *
 * Bridges Mesa's Xe KMD backend (`src/intel/vulkan/xe/*`, `src/intel/dev/xe/*`) directly to
 * `ArcXe.kext` (`ArcXeUserClient`) on macOS 15 Sequoia / 26 Tahoe via IOKit.
 */

#ifdef __APPLE__

#include "anv_xnu_drm_shim.h"
#include <xf86drm.h>

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/xe_drm.h"
#include "drm-uapi/gpu_scheduler.h"
#include "intel/dev/intel_hwconfig_types.h"
#include "util/u_sync_provider.h"

#include "arc_uapi.h"
#include "arc_regs_mtl.h"

__attribute__((constructor))
static void arc_xnu_shim_init_env(void)
{
    /* ArcXe sets BTP+BTI RCC keying in every render LRC. Mesa's upstream
     * anv_state_cache_perf_fix option defaults false (only vkd3d opts in).
     * Set the Darwin default before instance drirc parsing; preserve an
     * explicit anv_state_cache_perf_fix=false for legacy-flush comparison. */
    setenv("anv_state_cache_perf_fix", "true", 0);
    /*
     * Default Mesa ANV's TBIMR (Tile-Based Immediate Mode Renderer, driconf 'intel_tbimr') off on
     * Direct Execlist unless ARCMTL_TBIMR=1 is explicitly exported. On >=2560x1600 framebuffers
     * (unlike <=256x256 offscreen tests), Mesa ANV enables 3DSTATE_TBIMR_TILE_PASS_INFO + TBIMREnable;
     * immediate-mode 3D rasterization avoids multi-tile TBIMR scoreboard stalls without GuC.
     */
    const char *tbimr = getenv("ARCMTL_TBIMR");
    if (!tbimr || strcmp(tbimr, "1") != 0) {
        setenv("intel_tbimr", "false", 0);
    }
}

/* ============================================================================
 * 1. Session, Mmap, VM_BIND Tracking & Timeline Tables
 * ============================================================================ */

#define ARC_MAX_FDS             32
#define ARC_MAX_MMAPS           4096
#define ARC_MAX_BIND_RECORDS    8192
#define ARC_MAX_SYNCOBJ_TRACK   2048
#define ARC_VIRTUAL_BIND_Q_BIT  0x80000000u

#define ARC_TRACE_EVENTS 128u
struct ArcTraceEvent {
    uint64_t index, call, time_ns;
    uint32_t method, finished, status, bytes;
    uint8_t data[sizeof(struct ArcXeVmBindIn)];
};

struct ArcFdSession {
    bool                 in_use;
    int                  fd;
    io_service_t         service;
    io_connect_t         connect;
    struct ArcXeQueryDeviceOut dev_query;
    uint32_t             next_virtual_bind_q;
    uint64_t             sync_points[ARC_MAX_SYNCOBJ_TRACK];
    uint64_t             trace_next;
    bool                 diagnostics_dumped;
    struct ArcTraceEvent trace[ARC_TRACE_EVENTS];
};

struct ArcMmapRecord {
    bool         in_use;
    int          fd;
    io_connect_t connect;
    uint32_t     gem_handle;
    mach_vm_address_t base_addr;
    void        *user_ptr;
    size_t       mapped_len;
};

struct ArcVmBindRecord {
    bool     in_use;
    bool     dump_valid;
    int      fd;
    uint32_t vm_id;
    uint32_t gem_handle;
    uint64_t gpu_va;
    uint64_t range_size;
    uint64_t bo_offset;
};

static pthread_mutex_t      g_arc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ArcFdSession  g_sessions[ARC_MAX_FDS];
static struct ArcMmapRecord g_mmaps[ARC_MAX_MMAPS];
static struct ArcVmBindRecord g_binds[ARC_MAX_BIND_RECORDS];

static struct ArcFdSession *lookup_session_locked(int fd)
{
    if (fd < 0) return NULL;
    for (int i = 0; i < ARC_MAX_FDS; i++) {
        if (g_sessions[i].in_use && g_sessions[i].fd == fd)
            return &g_sessions[i];
    }
    return NULL;
}

/* Keep a bounded flight recorder in memory. No logging or MMIO on the hot path.
 * The begin event survives a call that fails or never returns. */
static uint64_t record_call(io_connect_t conn, uint32_t method, uint64_t call,
                            bool finished, kern_return_t status, const void *data, size_t bytes)
{
    if (method < kArcXeMethodGemCreate || method > kArcXeMethodExec)
        return 0; /* Polling waits would evict the submissions we need. */
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t index = 0;
    pthread_mutex_lock(&g_arc_lock);
    for (int i = 0; i < ARC_MAX_FDS; ++i) {
        struct ArcFdSession *s = &g_sessions[i];
        if (!s->in_use || s->connect != conn) continue;
        index = ++s->trace_next;
        struct ArcTraceEvent *e = &s->trace[(index - 1u) % ARC_TRACE_EVENTS];
        memset(e, 0, sizeof(*e));
        e->index = index; e->call = finished ? call : index;
        e->time_ns = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
        e->method = method; e->finished = finished; e->status = (uint32_t)status;
        if (data) {
            e->bytes = bytes < sizeof(e->data) ? (uint32_t)bytes : (uint32_t)sizeof(e->data);
            memcpy(e->data, data, e->bytes);
        }
        break;
    }
    pthread_mutex_unlock(&g_arc_lock);
    return index;
}

static kern_return_t traced_struct_call(io_connect_t conn, uint32_t method,
                                         const void *input, size_t input_size,
                                         void *output, size_t *output_size)
{
    const uint64_t call = record_call(conn, method, 0, false, 0, input, input_size);
    const size_t capacity = output_size ? *output_size : 0;
    const kern_return_t kr = IOConnectCallStructMethod(conn, method, input, input_size, output, output_size);
    const size_t returned = output_size && *output_size < capacity ? *output_size : capacity;
    record_call(conn, method, call, true, kr, kr == KERN_SUCCESS ? output : NULL, returned);
    return kr;
}

/* All subsequent shim calls use the recorder, including failed binds/submits. */
#define IOConnectCallStructMethod traced_struct_call

/*
 * Open an IOKit connection to ArcXeUserClient / IOAccelArcXeBridge:
 *   1. Try ArcAccel (IOGraphicsAccelerator2 / IOAccelerator) with ARC_XE_ACCEL_OPEN_TYPE first.
 *      macOS WindowServer (com.apple.WindowServer.sb) and App Sandbox Seatbelt profiles allow
 *      iokit-open on IOAccel* user clients attached under an IOAccelerator service, whereas
 *      direct opens on non-IOAccelerator services are rejected with kIOReturnNotPermitted.
 *   2. Fall back to ArcXeService directly when ArcAccel is disabled (e.g. safe Stage-1 CLI tests).
 */
static kern_return_t arc_xnu_open_iokit_connection(io_service_t *out_svc, io_connect_t *out_conn)
{
    *out_svc  = IO_OBJECT_NULL;
    *out_conn = IO_OBJECT_NULL;

    CFMutableDictionaryRef match = IOServiceMatching(ARC_ACCEL_IOKIT_SERVICE_NAME);
    if (match) {
        io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault, match);
        if (accel != IO_OBJECT_NULL) {
            io_connect_t conn = IO_OBJECT_NULL;
            kern_return_t kr = IOServiceOpen(accel, mach_task_self(), ARC_XE_ACCEL_OPEN_TYPE, &conn);
            if (kr == KERN_SUCCESS && conn != IO_OBJECT_NULL) {
                *out_svc  = accel;
                *out_conn = conn;
                return KERN_SUCCESS;
            }
            IOObjectRelease(accel);
        }
    }

    match = IOServiceMatching(ARCXE_IOKIT_SERVICE_NAME);
    if (!match)
        return KERN_FAILURE;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (svc == IO_OBJECT_NULL)
        return KERN_FAILURE;

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        IOObjectRelease(svc);
        return (kr != KERN_SUCCESS) ? kr : KERN_FAILURE;
    }

    *out_svc  = svc;
    *out_conn = conn;
    return KERN_SUCCESS;
}

/*
 * Allocate a sandbox-safe owned POSIX descriptor for Mesa's fd table (dup/close/fcntl):
 *   - WindowServer & GPU sandboxes forbid O_RDWR on /dev/null; try O_RDONLY first.
 *   - Tighter sandboxes that forbid /dev/null altogether allow an anonymous EOF pipe().
 */
static int arc_xnu_alloc_token_fd(void)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
        return fd;

    int ends[2];
    if (pipe(ends) != 0)
        return -1;
    close(ends[1]);
    if (fcntl(ends[0], F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(ends[0]);
        errno = saved;
        return -1;
    }
    return ends[0];
}

int arc_xnu_open_device(void)
{
    io_service_t svc  = IO_OBJECT_NULL;
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr  = arc_xnu_open_iokit_connection(&svc, &conn);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        errno = (kr == kIOReturnNotPermitted || kr == kIOReturnNotPrivileged) ? EACCES : ENODEV;
        return -1;
    }

    struct ArcXeQueryDeviceOut q = {0};
    size_t out_sz = sizeof(q);
    kr = IOConnectCallStructMethod(conn, kArcXeMethodQueryDevice, NULL, 0, &q, &out_sz);
    if (kr != KERN_SUCCESS || q.vendor_id != 0x8086) {
        IOServiceClose(conn);
        IOObjectRelease(svc);
        errno = EIO;
        return -1;
    }

    /* Allocate a real sandbox-safe POSIX descriptor so dup/close/fcntl in Mesa work natively */
    int fd = arc_xnu_alloc_token_fd();
    if (fd < 0) {
        IOServiceClose(conn);
        IOObjectRelease(svc);
        return -1;
    }

    pthread_mutex_lock(&g_arc_lock);
    int slot = -1;
    for (int i = 0; i < ARC_MAX_FDS; i++) {
        if (!g_sessions[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_arc_lock);
        close(fd);
        IOServiceClose(conn);
        IOObjectRelease(svc);
        errno = EMFILE;
        return -1;
    }

    memset(&g_sessions[slot], 0, sizeof(g_sessions[slot]));
    g_sessions[slot].in_use              = true;
    g_sessions[slot].fd                  = fd;
    g_sessions[slot].service             = svc;
    g_sessions[slot].connect             = conn;
    g_sessions[slot].dev_query           = q;
    g_sessions[slot].next_virtual_bind_q = 1;
    pthread_mutex_unlock(&g_arc_lock);

    return fd;
}

void arc_xnu_close_device(int fd)
{
    if (fd < 0) return;

    io_connect_t conn = IO_OBJECT_NULL;
    io_service_t svc  = IO_OBJECT_NULL;

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(fd);
    if (s) {
        conn = s->connect;
        svc  = s->service;
        for (int i = 0; i < ARC_MAX_BIND_RECORDS; i++) {
            if (g_binds[i].in_use && g_binds[i].fd == fd)
                g_binds[i].in_use = false;
        }
        s->in_use = false;
    }
    pthread_mutex_unlock(&g_arc_lock);

    if (conn != IO_OBJECT_NULL) IOServiceClose(conn);
    if (svc  != IO_OBJECT_NULL) IOObjectRelease(svc);
    close(fd);
}

bool arc_xnu_is_fd(int fd)
{
    pthread_mutex_lock(&g_arc_lock);
    bool ok = lookup_session_locked(fd) != NULL;
    pthread_mutex_unlock(&g_arc_lock);
    return ok;
}

/* ============================================================================
 * 2. Zero-Copy UMA Buffer Mapping (IOConnectMapMemory64)
 * ============================================================================ */

void *arc_xnu_gem_mmap(int fd, uint32_t gem_handle, uint64_t offset, uint64_t size, void *placed_addr)
{
    (void)placed_addr; /* ANV disables placed slab parent when memoryMapPlaced is false */

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(fd);
    io_connect_t conn = s ? s->connect : IO_OBJECT_NULL;
    pthread_mutex_unlock(&g_arc_lock);

    if (conn == IO_OBJECT_NULL || gem_handle == 0) {
        errno = EBADF;
        return MAP_FAILED;
    }

    mach_vm_address_t map_addr = 0;
    mach_vm_size_t    map_size = 0;
    kern_return_t kr = IOConnectMapMemory64(conn, gem_handle, mach_task_self(),
                                            &map_addr, &map_size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !map_addr || offset + size > map_size) {
        if (kr == KERN_SUCCESS && map_addr)
            IOConnectUnmapMemory64(conn, gem_handle, mach_task_self(), map_addr);
        errno = ENOMEM;
        return MAP_FAILED;
    }

    void *ret_ptr = (void *)((uintptr_t)map_addr + (uintptr_t)offset);

    pthread_mutex_lock(&g_arc_lock);
    for (int i = 0; i < ARC_MAX_MMAPS; i++) {
        if (!g_mmaps[i].in_use) {
            g_mmaps[i].in_use     = true;
            g_mmaps[i].fd         = fd;
            g_mmaps[i].connect    = conn;
            g_mmaps[i].gem_handle = gem_handle;
            g_mmaps[i].base_addr  = map_addr;
            g_mmaps[i].user_ptr   = ret_ptr;
            g_mmaps[i].mapped_len = (size_t)size;
            break;
        }
    }
    pthread_mutex_unlock(&g_arc_lock);

    return ret_ptr;
}

int arc_xnu_gem_munmap(void *addr, size_t length)
{
    if (!addr) return 0;

    io_connect_t      conn = IO_OBJECT_NULL;
    uint32_t          gem_handle = 0;
    mach_vm_address_t base_addr  = 0;

    pthread_mutex_lock(&g_arc_lock);
    for (int i = 0; i < ARC_MAX_MMAPS; i++) {
        if (g_mmaps[i].in_use && g_mmaps[i].user_ptr == addr) {
            conn       = g_mmaps[i].connect;
            gem_handle = g_mmaps[i].gem_handle;
            base_addr  = g_mmaps[i].base_addr;
            g_mmaps[i].in_use = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_arc_lock);

    if (conn != IO_OBJECT_NULL && base_addr != 0) {
        IOConnectUnmapMemory64(conn, gem_handle, mach_task_self(), base_addr);
        return 0;
    }
    return munmap(addr, length);
}

int arc_xnu_futex_wake(uint32_t *addr, int32_t count)
{
    if (!addr || count <= 0)
        return 0;
    if (count == 1)
        return os_sync_wake_by_address_any(addr, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    return os_sync_wake_by_address_all(addr, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

int arc_xnu_futex_wait(uint32_t *addr, int32_t value, const struct timespec *timeout)
{
    if (!addr)
        return -1;
    if (!timeout) {
        return os_sync_wait_on_address(addr, (uint64_t)(uint32_t)value, sizeof(uint32_t),
                                       OS_SYNC_WAIT_ON_ADDRESS_NONE);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t sec  = (int64_t)timeout->tv_sec - (int64_t)now.tv_sec;
    int64_t nsec = (int64_t)timeout->tv_nsec - (int64_t)now.tv_nsec;
    int64_t total_ns = sec * 1000000000LL + nsec;
    if (total_ns <= 0) {
        errno = ETIMEDOUT;
        return ETIMEDOUT;
    }
    int r = os_sync_wait_on_address_with_timeout(addr, (uint64_t)(uint32_t)value, sizeof(uint32_t),
                                                 OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                 OS_CLOCK_MACH_ABSOLUTE_TIME,
                                                 (uint64_t)total_ns);
    return (r < 0 && errno == ETIMEDOUT) ? ETIMEDOUT : r;
}

/* ============================================================================
 * 3. DRM_IOCTL_XE_DEVICE_QUERY Builders
 * ============================================================================ */

static int handle_xe_device_query(struct ArcFdSession *s, struct drm_xe_device_query *q)
{
    const struct ArcXeQueryDeviceOut *dq = &s->dev_query;

    switch (q->query) {
    /* A. DRM_XE_DEVICE_QUERY_CONFIG */
    case DRM_XE_DEVICE_QUERY_CONFIG: {
        const uint32_t num_params = 5;
        const uint32_t need = (uint32_t)(sizeof(struct drm_xe_query_config) + num_params * sizeof(uint64_t));
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct drm_xe_query_config *cfg = (struct drm_xe_query_config *)(uintptr_t)q->data;
        memset(cfg, 0, need);
        cfg->num_params = num_params;
        cfg->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] =
            ((uint64_t)dq->revision_id << 16) | dq->device_id;
        cfg->info[DRM_XE_QUERY_CONFIG_FLAGS] = DRM_XE_QUERY_CONFIG_FLAG_HAS_LOW_LATENCY;
        cfg->info[DRM_XE_QUERY_CONFIG_MIN_ALIGNMENT] = 4096;
        cfg->info[DRM_XE_QUERY_CONFIG_VA_BITS] = dq->va_bits ? dq->va_bits : 48;
        cfg->info[DRM_XE_QUERY_CONFIG_MAX_EXEC_QUEUE_PRIORITY] = DRM_SCHED_PRIORITY_HIGH;
        return 0;
    }

    /* B. DRM_XE_DEVICE_QUERY_MEM_REGIONS (UMA System Memory Region 0) */
    case DRM_XE_DEVICE_QUERY_MEM_REGIONS: {
        const uint32_t need = (uint32_t)(sizeof(struct drm_xe_query_mem_regions) + sizeof(struct drm_xe_mem_region));
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct drm_xe_query_mem_regions *mr = (struct drm_xe_query_mem_regions *)(uintptr_t)q->data;
        memset(mr, 0, need);
        mr->num_mem_regions             = 1;
        mr->mem_regions[0].mem_class    = DRM_XE_MEM_REGION_CLASS_SYSMEM;
        mr->mem_regions[0].instance     = 0;
        mr->mem_regions[0].min_page_size= 4096;
        mr->mem_regions[0].total_size   = dq->sysmem_total_bytes ? dq->sysmem_total_bytes : (16ULL << 30);
        mr->mem_regions[0].used         = 0;
        return 0;
    }

    /* C. DRM_XE_DEVICE_QUERY_GT_LIST (GT0 Primary Xe-LPG 12.70 + GT1 Media Xe-LPM+ 13.00) */
    case DRM_XE_DEVICE_QUERY_GT_LIST: {
        const uint32_t num_gt = 2;
        const uint32_t need = (uint32_t)(sizeof(struct drm_xe_query_gt_list) + num_gt * sizeof(struct drm_xe_gt));
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct drm_xe_query_gt_list *gtl = (struct drm_xe_query_gt_list *)(uintptr_t)q->data;
        memset(gtl, 0, need);
        gtl->num_gt = num_gt;

        gtl->gt_list[0].type             = DRM_XE_QUERY_GT_TYPE_MAIN;
        gtl->gt_list[0].tile_id          = 0;
        gtl->gt_list[0].gt_id            = 0;
        gtl->gt_list[0].reference_clock  = dq->gpu_timestamp_freq_hz ? dq->gpu_timestamp_freq_hz : 19200000u;
        gtl->gt_list[0].near_mem_regions = 1ULL;
        gtl->gt_list[0].ip_ver_major     = (uint16_t)(dq->graphics_verx100 / 100u);
        gtl->gt_list[0].ip_ver_minor     = (uint16_t)(dq->graphics_verx100 % 100u);
        gtl->gt_list[0].ip_ver_rev       = (uint16_t)(dq->gmd_id_gt0 & 0x3Fu);

        gtl->gt_list[1].type             = DRM_XE_QUERY_GT_TYPE_MEDIA;
        gtl->gt_list[1].tile_id          = 0;
        gtl->gt_list[1].gt_id            = 1;
        gtl->gt_list[1].reference_clock  = gtl->gt_list[0].reference_clock;
        gtl->gt_list[1].near_mem_regions = 1ULL;
        gtl->gt_list[1].ip_ver_major     = (uint16_t)(dq->media_verx100 / 100u);
        gtl->gt_list[1].ip_ver_minor     = (uint16_t)(dq->media_verx100 % 100u);
        gtl->gt_list[1].ip_ver_rev       = (uint16_t)(dq->gmd_id_gt1 & 0x3Fu);
        return 0;
    }

    /* D. DRM_XE_DEVICE_QUERY_ENGINES (Unified RCS0 Render + Compute + Copy on 128-EU Xe-LPG) */
    case DRM_XE_DEVICE_QUERY_ENGINES: {
        const uint32_t num_eng = 1;
        const uint32_t need = (uint32_t)(sizeof(struct drm_xe_query_engines) + num_eng * sizeof(struct drm_xe_engine));
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct drm_xe_query_engines *qe = (struct drm_xe_query_engines *)(uintptr_t)q->data;
        memset(qe, 0, need);
        qe->num_engines = num_eng;
        qe->engines[0].instance.engine_class    = DRM_XE_ENGINE_CLASS_RENDER;
        qe->engines[0].instance.engine_instance = 0;
        qe->engines[0].instance.gt_id           = 0;
        return 0;
    }

    /* E. DRM_XE_DEVICE_QUERY_GT_TOPOLOGY (8-byte masks per entry) */
    case DRM_XE_DEVICE_QUERY_GT_TOPOLOGY: {
        const uint32_t entry_sz = (uint32_t)(sizeof(struct drm_xe_query_topology_mask) + 8u);
        const uint32_t need     = entry_sz * 4u;
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        uint8_t *p = (uint8_t *)(uintptr_t)q->data;
        memset(p, 0, need);

        const uint64_t geom_mask = dq->geom_dss_mask ? dq->geom_dss_mask : 0xFFULL;
        const uint64_t comp_mask = dq->compute_dss_mask ? dq->compute_dss_mask : 0xFFULL;
        const uint64_t l3_mask   = 0xFFULL;
        const uint64_t eu_mask   = dq->eu_per_dss_mask ? dq->eu_per_dss_mask : 0xFFFFULL;

        const struct { uint32_t type; uint64_t mask; } items[4] = {
            { DRM_XE_TOPO_DSS_GEOMETRY, geom_mask },
            { DRM_XE_TOPO_DSS_COMPUTE,  comp_mask },
            { DRM_XE_TOPO_L3_BANK,      l3_mask   },
            { DRM_XE_TOPO_EU_PER_DSS,   eu_mask   },
        };
        for (int i = 0; i < 4; i++) {
            struct drm_xe_query_topology_mask *tm = (struct drm_xe_query_topology_mask *)(p + i * entry_sz);
            tm->gt_id     = 0;
            tm->type      = items[i].type;
            tm->num_bytes = 8;
            memcpy(tm->mask, &items[i].mask, 8);
        }
        return 0;
    }

    /* F. DRM_XE_DEVICE_QUERY_HWCONFIG (Meteor Lake Xe-LPG TLV Table) */
    case DRM_XE_DEVICE_QUERY_HWCONFIG: {
        const uint32_t eus_per_dss = dq->eu_per_dss_mask ? (uint32_t)__builtin_popcount(dq->eu_per_dss_mask) : 16u;
        const uint32_t table[] = {
            INTEL_HWCONFIG_MAX_NUM_EU_PER_DSS,      1, eus_per_dss,
            INTEL_HWCONFIG_NUM_THREADS_PER_EU,      1, 8,
            INTEL_HWCONFIG_TOTAL_VS_THREADS,        1, 546,
            INTEL_HWCONFIG_TOTAL_GS_THREADS,        1, 336,
            INTEL_HWCONFIG_TOTAL_HS_THREADS,        1, 336,
            INTEL_HWCONFIG_TOTAL_DS_THREADS,        1, 546,
            INTEL_HWCONFIG_TOTAL_PS_THREADS,        1, 128,
            INTEL_HWCONFIG_URB_SIZE_PER_SLICE_IN_KB,1, 640,
        };
        const uint32_t need = (uint32_t)sizeof(table);
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        memcpy((void *)(uintptr_t)q->data, table, need);
        return 0;
    }

    /* G. DRM_XE_DEVICE_QUERY_ENGINE_CYCLES */
    case DRM_XE_DEVICE_QUERY_ENGINE_CYCLES: {
        const uint32_t need = (uint32_t)sizeof(struct drm_xe_query_engine_cycles);
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct ArcXeQueryDeviceOut live = {0};
        size_t out_sz = sizeof(live);
        IOConnectCallStructMethod(s->connect, kArcXeMethodQueryDevice, NULL, 0, &live, &out_sz);

        struct timespec now_ts = {0, 0};
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        const uint64_t cpu_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL + (uint64_t)now_ts.tv_nsec;

        struct drm_xe_query_engine_cycles *ec = (struct drm_xe_query_engine_cycles *)(uintptr_t)q->data;
        ec->engine_cycles = live.gpu_timestamp_ticks;
        ec->cpu_timestamp = cpu_ns;
        ec->cpu_delta     = 100;
        ec->width         = 36;
        return 0;
    }

    /* H. DRM_XE_DEVICE_QUERY_UC_FW_VERSION */
    case DRM_XE_DEVICE_QUERY_UC_FW_VERSION: {
        if (dq->submit_backend != kArcXeSubmitGuc) {
            errno = ENODEV;
            return -1;
        }
        const uint32_t need = (uint32_t)sizeof(struct drm_xe_query_uc_fw_version);
        if (q->size == 0) {
            q->size = need;
            return 0;
        }
        if (q->size < need || !q->data) {
            errno = EINVAL;
            return -1;
        }
        struct drm_xe_query_uc_fw_version *fw = (struct drm_xe_query_uc_fw_version *)(uintptr_t)q->data;
        fw->branch_ver = 0;
        fw->major_ver  = 1;
        fw->minor_ver  = 1;
        fw->patch_ver  = 3;
        return 0;
    }

    default:
        errno = EINVAL;
        return -1;
    }
}

/* ============================================================================
 * 4. VM_BIND Record Helpers (for DRM_XE_VM_BIND_OP_UNMAP_ALL)
 * ============================================================================ */

/* Conservatively disable diagnostics for overlapping replacements/partial unmaps.
 * Keep the cleanup records: a partial unmap need not remove the entire BO. */
static void invalidate_bind_dump_locked(int fd, uint32_t vm_id, uint64_t va, uint64_t size)
{
    for (int i = 0; i < ARC_MAX_BIND_RECORDS; ++i) {
        struct ArcVmBindRecord *b = &g_binds[i];
        if (!b->in_use || b->fd != fd || b->vm_id != vm_id || !size) continue;
        if (va >= b->gpu_va ? va - b->gpu_va < b->range_size : b->gpu_va - va < size)
            b->dump_valid = false;
    }
}

static void record_vm_bind_locked(int fd, uint32_t vm_id, uint32_t gem_handle, uint64_t gpu_va, uint64_t range_size, uint64_t bo_offset)
{
    if (!gem_handle || !range_size) return;
    int empty = -1;
    for (int i = 0; i < ARC_MAX_BIND_RECORDS; i++) {
        if (g_binds[i].in_use && g_binds[i].fd == fd &&
            g_binds[i].vm_id == vm_id && g_binds[i].gem_handle == gem_handle &&
            g_binds[i].gpu_va == gpu_va) {
            g_binds[i].range_size = range_size;
            g_binds[i].bo_offset = bo_offset;
            g_binds[i].dump_valid = true;
            return;
        }
        if (!g_binds[i].in_use && empty < 0)
            empty = i;
    }
    if (empty >= 0) {
        g_binds[empty].in_use     = true;
        g_binds[empty].fd         = fd;
        g_binds[empty].vm_id      = vm_id;
        g_binds[empty].gem_handle = gem_handle;
        g_binds[empty].gpu_va     = gpu_va;
        g_binds[empty].range_size = range_size;
        g_binds[empty].bo_offset = bo_offset;
        g_binds[empty].dump_valid = true;
    }
}

/* Caller holds g_arc_lock, including while copying CPU-mapped command bytes.
 * ACTHD has no VM id in this ABI: refuse ambiguous addresses across VMs.
 * This is only a bounded snapshot, not a command-boundary decoder. */
static unsigned snapshot_batch_locked(int fd, uint64_t acthd, uint32_t words[48], uint64_t *start)
{
    const uint64_t va = acthd & 0x0000fffffffffffcULL;
    const struct ArcVmBindRecord *b = NULL;
    for (int i = 0; i < ARC_MAX_BIND_RECORDS; ++i) {
        const struct ArcVmBindRecord *candidate = &g_binds[i];
        if (!candidate->in_use || candidate->fd != fd || va < candidate->gpu_va ||
            va - candidate->gpu_va >= candidate->range_size) continue;
        if (b || !candidate->dump_valid) return 0;
        b = candidate;
    }
    if (!b) return 0;
    const uint64_t delta = va - b->gpu_va;
    if (b->bo_offset > UINT64_MAX - delta) return 0;
    const uint64_t offset = b->bo_offset + delta;
    for (int i = 0; i < ARC_MAX_MMAPS; ++i) {
        const struct ArcMmapRecord *m = &g_mmaps[i];
        if (!m->in_use || m->fd != fd || m->gem_handle != b->gem_handle) continue;
        const uint64_t map_offset = (uintptr_t)m->user_ptr - (uintptr_t)m->base_addr;
        if (offset < map_offset || offset - map_offset >= m->mapped_len) continue;
        const uint64_t pos = offset - map_offset;
        uint64_t before = delta < pos ? delta : pos;
        if (before > 64) before = 64;
        before &= ~3ULL;
        uint64_t after = b->range_size - delta;
        if (after > m->mapped_len - pos) after = m->mapped_len - pos;
        if (after > 128) after = 128;
        if (after < 4) return 0;
        const unsigned count = (unsigned)((before + after) / 4);
        memcpy(words, (const char *)m->user_ptr + pos - before, count * 4);
        *start = va - before;
        return count;
    }
    return 0;
}

static void dump_trace_syncs(const struct ArcXeSyncEntry *syncs, uint32_t count)
{
    for (uint32_t i = 0; i < count && i < ARCXE_MAX_SYNCS_PER_OP; ++i)
        fprintf(stderr, "      sync handle=%u flags=0x%x point=%llu\n", syncs[i].syncobj_handle,
                syncs[i].flags, (unsigned long long)syncs[i].timeline_value);
}

static void dump_trace_event(const struct ArcTraceEvent *e)
{
    static const char *names[] = {"QUERY", "GEM_CREATE", "IMPORT_SURFACE", "IMPORT_USERPTR",
        "GEM_CLOSE", "MMAP_INFO", "VM_CREATE", "VM_DESTROY", "VM_BIND", "QUEUE_CREATE", "QUEUE_DESTROY", "EXEC"};
    const char *name = e->method <= kArcXeMethodExec ? names[e->method] : "UNKNOWN";
    fprintf(stderr, "    #%llu call=%llu t_ns=%llu %s %s kr=0x%08x bytes=%u\n",
            (unsigned long long)e->index, (unsigned long long)e->call, (unsigned long long)e->time_ns,
            name, e->finished ? "end" : "begin", e->status, e->bytes);
    if (!e->finished && e->method == kArcXeMethodExec && e->bytes >= sizeof(struct ArcXeExecIn)) {
        struct ArcXeExecIn in; memcpy(&in, e->data, sizeof(in));
        fprintf(stderr, "      queue=%u batch=0x%012llx buffers=%u flags=0x%x\n",
                in.exec_queue_id, (unsigned long long)in.batch_gpu_va, in.num_batch_buffer, in.flags);
        dump_trace_syncs(in.syncs, in.num_syncs);
    } else if (!e->finished && e->method == kArcXeMethodVmBind && e->bytes >= sizeof(struct ArcXeVmBindIn)) {
        struct ArcXeVmBindIn in; memcpy(&in, e->data, sizeof(in));
        fprintf(stderr, "      vm=%u op=%u gem=%u va=0x%012llx size=0x%llx offset=0x%llx pat=%u flags=0x%x\n",
                in.vm_id, in.op, in.gem_handle, (unsigned long long)in.gpu_va,
                (unsigned long long)in.range_size, (unsigned long long)in.bo_offset, in.pat_index, in.flags);
        dump_trace_syncs(in.syncs, in.num_syncs);
    } else {
        /* Exact UAPI payload for allocation, queue/VM lifecycle and returned seqno. */
        for (uint32_t i = 0; i + 4u <= e->bytes; i += 4u) {
            uint32_t word; memcpy(&word, e->data + i, sizeof(word));
            if (!(i & 31u)) fprintf(stderr, "      +%02x:", i);
            fprintf(stderr, " %08x", word);
            if ((i & 31u) == 28u || i + 4u == e->bytes) fputc('\n', stderr);
        }
    }
}

static void dump_timeout_history(int fd)
{
    struct Snapshot {
        uint64_t next;
        uint32_t bind_count, map_count;
        struct ArcTraceEvent trace[ARC_TRACE_EVENTS];
        struct ArcVmBindRecord binds[ARC_MAX_BIND_RECORDS];
        struct ArcMmapRecord maps[ARC_MAX_MMAPS];
    };
    struct Snapshot *snap = (struct Snapshot *)calloc(1, sizeof(*snap));
    if (!snap) { fprintf(stderr, "  [ArcXe HISTORY] snapshot allocation failed\n"); return; }
    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(fd);
    if (!s || s->diagnostics_dumped) {
        pthread_mutex_unlock(&g_arc_lock); free(snap); return;
    }
    s->diagnostics_dumped = true;
    snap->next = s->trace_next;
    memcpy(snap->trace, s->trace, sizeof(snap->trace));
    for (int i = 0; i < ARC_MAX_BIND_RECORDS; ++i)
        if (g_binds[i].in_use && g_binds[i].fd == fd) snap->binds[snap->bind_count++] = g_binds[i];
    for (int i = 0; i < ARC_MAX_MMAPS; ++i)
        if (g_mmaps[i].in_use && g_mmaps[i].fd == fd) snap->maps[snap->map_count++] = g_mmaps[i];
    pthread_mutex_unlock(&g_arc_lock);
    fprintf(stderr, "  [ArcXe HISTORY] fd=%d latest=%llu capacity=%u (begin/end records; monotonic time)\n",
            fd, (unsigned long long)snap->next, ARC_TRACE_EVENTS);
    const uint64_t first = snap->next > ARC_TRACE_EVENTS ? snap->next - ARC_TRACE_EVENTS + 1u : 1u;
    for (uint64_t i = first; i <= snap->next; ++i)
        dump_trace_event(&snap->trace[(i - 1u) % ARC_TRACE_EVENTS]);
    fprintf(stderr, "  [ArcXe GPU MAPS] tracked BO ranges=%u capacity=%u (valid=0 means stale/partly unmapped)\n",
            snap->bind_count, ARC_MAX_BIND_RECORDS);
    for (uint32_t i = 0; i < snap->bind_count; ++i) {
        const struct ArcVmBindRecord *b = &snap->binds[i];
        fprintf(stderr, "    vm=%u gem=%u va=0x%012llx size=0x%llx offset=0x%llx valid=%u\n",
                b->vm_id, b->gem_handle, (unsigned long long)b->gpu_va, (unsigned long long)b->range_size,
                (unsigned long long)b->bo_offset, b->dump_valid);
    }
    fprintf(stderr, "  [ArcXe CPU MAPS] count=%u capacity=%u\n", snap->map_count, ARC_MAX_MMAPS);
    for (uint32_t i = 0; i < snap->map_count; ++i) {
        const struct ArcMmapRecord *m = &snap->maps[i];
        fprintf(stderr, "    gem=%u base=0x%llx ptr=%p size=0x%llx\n", m->gem_handle,
                (unsigned long long)m->base_addr, m->user_ptr, (unsigned long long)m->mapped_len);
    }
    free(snap);
}

static void dump_timeout_batch(int fd, uint64_t acthd)
{
    uint32_t words[48];
    uint64_t start = 0;
    pthread_mutex_lock(&g_arc_lock);
    const unsigned count = snapshot_batch_locked(fd, acthd, words, &start);
    pthread_mutex_unlock(&g_arc_lock);
    if (!count) {
        fprintf(stderr, "  [ArcXe BATCH DWORDS] ACTHD has no unambiguous CPU-mapped PPGTT range\n");
        return;
    }
    fprintf(stderr, "  [ArcXe BATCH DWORDS] bounded snapshot around ACTHD; command boundaries not inferred\n");
    for (unsigned i = 0; i < count; i += 4) {
        fprintf(stderr, "    %012llx:", (unsigned long long)(start + i * 4));
        for (unsigned j = i; j < count && j < i + 4; ++j)
            fprintf(stderr, " %08x", words[j]);
        fprintf(stderr, "\n");
    }
}

/* ============================================================================
 * 5. Main DRM / Xe Ioctl Translator (`intel_ioctl` -> `arc_xnu_ioctl`)
 * ============================================================================ */

struct ArcSyncProvider {
    struct util_sync_provider base;
    int                       fd;
};

static int arc_sync_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle);
static int arc_sync_destroy(struct util_sync_provider *p, uint32_t handle);
static int arc_sync_wait(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
                         int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
static int arc_sync_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                                  unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                  uint32_t *first_signaled);
static int arc_sync_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count);
static int arc_sync_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count);
static int arc_sync_timeline_signal(struct util_sync_provider *p, const uint32_t *handles,
                                    uint64_t *points, uint32_t count);
static int arc_sync_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                          uint32_t count, uint32_t flags);
static int arc_sync_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
                             uint32_t src_handle, uint64_t src_point, uint32_t flags);

int arc_xnu_ioctl(int fd, unsigned long request, void *arg)
{
    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(fd);
    pthread_mutex_unlock(&g_arc_lock);

    if (!s || !arg) {
        errno = !s ? EBADF : EINVAL;
        return -1;
    }

    switch (request) {
    case DRM_IOCTL_XE_DEVICE_QUERY:
        return handle_xe_device_query(s, (struct drm_xe_device_query *)arg);

    case DRM_IOCTL_XE_GEM_CREATE: {
        struct drm_xe_gem_create *gc = (struct drm_xe_gem_create *)arg;
        struct ArcXeGemCreateIn in = {0};
        struct ArcXeGemCreateOut out = {0};
        in.size = gc->size;
        if (gc->cpu_caching == DRM_XE_GEM_CPU_CACHING_WC) {
            in.flags     = ARCXE_GEM_FLAG_CPU_WC;
            in.pat_index = ARC_MTL_PAT_IDX_WT_NONCOH;
        } else if (gc->cpu_caching == DRM_XE_GEM_CPU_CACHING_WB) {
            in.flags     = ARCXE_GEM_FLAG_CPU_WB_COHERENT;
            in.pat_index = ARC_MTL_PAT_IDX_WB_1WAY_COH;
        } else {
            errno = EINVAL;
            return -1;
        }
        if (gc->flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT)
            in.flags |= ARCXE_GEM_FLAG_SCANOUT;

        size_t out_sz = sizeof(out);
        kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodGemCreate,
                                                     &in, sizeof(in), &out, &out_sz);
        if (kr != KERN_SUCCESS || out.gem_handle == 0) {
            errno = ENOMEM;
            return -1;
        }
        gc->handle = out.gem_handle;
        gc->size   = out.actual_size;
        return 0;
    }

    case DRM_IOCTL_XE_GEM_MMAP_OFFSET: {
        struct drm_xe_gem_mmap_offset *mo = (struct drm_xe_gem_mmap_offset *)arg;
        struct ArcXeGemMmapInfoIn in = { .gem_handle = mo->handle };
        struct ArcXeGemMmapInfoOut out = {0};
        size_t out_sz = sizeof(out);
        kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodGemMmapInfo,
                                                     &in, sizeof(in), &out, &out_sz);
        if (kr != KERN_SUCCESS) {
            errno = EINVAL;
            return -1;
        }
        mo->offset = out.fake_mmap_offset;
        return 0;
    }

    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *cl = (struct drm_gem_close *)arg;
        struct ArcXeGemCloseIn in = { .gem_handle = cl->handle };
        pthread_mutex_lock(&g_arc_lock);
        for (int i = 0; i < ARC_MAX_BIND_RECORDS; i++) {
            if (g_binds[i].in_use && g_binds[i].fd == fd && g_binds[i].gem_handle == cl->handle)
                g_binds[i].in_use = false;
        }
        pthread_mutex_unlock(&g_arc_lock);
        IOConnectCallStructMethod(s->connect, kArcXeMethodGemClose, &in, sizeof(in), NULL, NULL);
        return 0;
    }

    case DRM_IOCTL_XE_VM_CREATE: {
        struct drm_xe_vm_create *vc = (struct drm_xe_vm_create *)arg;
        struct ArcXeVmCreateIn in = { .flags = vc->flags };
        struct ArcXeVmCreateOut out = {0};
        size_t out_sz = sizeof(out);
        kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodVmCreate,
                                                     &in, sizeof(in), &out, &out_sz);
        if (kr != KERN_SUCCESS || out.vm_id == 0) {
            errno = ENOMEM;
            return -1;
        }
        vc->vm_id = out.vm_id;
        return 0;
    }

    case DRM_IOCTL_XE_VM_DESTROY: {
        struct drm_xe_vm_destroy *vd = (struct drm_xe_vm_destroy *)arg;
        struct ArcXeVmDestroyIn in = { .vm_id = vd->vm_id };
        pthread_mutex_lock(&g_arc_lock);
        for (int r = 0; r < ARC_MAX_BIND_RECORDS; ++r)
            if (g_binds[r].in_use && g_binds[r].fd == fd && g_binds[r].vm_id == vd->vm_id)
                g_binds[r].dump_valid = false;
        pthread_mutex_unlock(&g_arc_lock);
        IOConnectCallStructMethod(s->connect, kArcXeMethodVmDestroy, &in, sizeof(in), NULL, NULL);
        return 0;
    }

    case DRM_IOCTL_XE_VM_BIND: {
        struct drm_xe_vm_bind *vb = (struct drm_xe_vm_bind *)arg;
        const struct drm_xe_vm_bind_op *ops = (vb->num_binds > 1)
            ? (const struct drm_xe_vm_bind_op *)(uintptr_t)vb->vector_of_binds
            : &vb->bind;
        const struct drm_xe_sync *syncs = (const struct drm_xe_sync *)(uintptr_t)vb->syncs;

        for (uint32_t i = 0; i < vb->num_binds; i++) {
            const struct drm_xe_vm_bind_op *b = &ops[i];
            if (b->op == DRM_XE_VM_BIND_OP_PREFETCH)
                continue;

            /* Handle DRM_XE_VM_BIND_OP_UNMAP_ALL using recorded (vm_id, gem_handle) mappings */
            if (b->op == DRM_XE_VM_BIND_OP_UNMAP_ALL) {
                for (int r = 0; r < ARC_MAX_BIND_RECORDS; ++r) {
                    pthread_mutex_lock(&g_arc_lock);
                    const struct ArcVmBindRecord entry = g_binds[r];
                    pthread_mutex_unlock(&g_arc_lock);
                    if (!entry.in_use || entry.fd != fd || entry.vm_id != vb->vm_id ||
                        entry.gem_handle != b->obj) continue;
                    struct ArcXeVmBindIn uin = {0};
                    uin.vm_id      = vb->vm_id;
                    uin.op         = kArcXeVmBindOpUnmap;
                    uin.gpu_va     = entry.gpu_va;
                    uin.range_size = entry.range_size;
                    pthread_mutex_lock(&g_arc_lock);
                    invalidate_bind_dump_locked(fd, vb->vm_id, entry.gpu_va, entry.range_size);
                    pthread_mutex_unlock(&g_arc_lock);
                    if (IOConnectCallStructMethod(s->connect, kArcXeMethodVmBind,
                                                  &uin, sizeof(uin), NULL, NULL) != KERN_SUCCESS) {
                        dump_timeout_history(fd);
                        errno = EIO;
                        return -1; /* Never signal successful bind completion on failure. */
                    }
                    pthread_mutex_lock(&g_arc_lock);
                    if (g_binds[r].in_use && g_binds[r].fd == entry.fd &&
                        g_binds[r].vm_id == entry.vm_id && g_binds[r].gem_handle == entry.gem_handle &&
                        g_binds[r].gpu_va == entry.gpu_va && g_binds[r].range_size == entry.range_size)
                        g_binds[r].in_use = false;
                    pthread_mutex_unlock(&g_arc_lock);
                }
                continue;
            }

            struct ArcXeVmBindIn in = {0};
            in.vm_id      = vb->vm_id;
            in.gem_handle = b->obj;
            in.pat_index  = b->pat_index;
            in.flags      = (uint16_t)b->flags;
            in.gpu_va     = b->addr;
            in.range_size = b->range;

            if (b->op == DRM_XE_VM_BIND_OP_UNMAP) {
                in.op = kArcXeVmBindOpUnmap;
            } else if (b->op == DRM_XE_VM_BIND_OP_MAP_USERPTR) {
                in.op       = kArcXeVmBindOpMapUserPtr;
                in.user_ptr = b->userptr;
            } else if (b->flags & DRM_XE_VM_BIND_FLAG_NULL) {
                in.op = kArcXeVmBindOpMapNull;
            } else {
                in.op        = kArcXeVmBindOpMap;
                in.bo_offset = b->obj_offset;
            }

            /* Attach syncs on final bind op in batch */
            if (i + 1 == vb->num_binds && vb->num_syncs > 0 && syncs) {
                uint32_t ns = vb->num_syncs < ARCXE_MAX_SYNCS_PER_OP ? vb->num_syncs : ARCXE_MAX_SYNCS_PER_OP;
                in.num_syncs = ns;
                pthread_mutex_lock(&g_arc_lock);
                for (uint32_t k = 0; k < ns; k++) {
                    in.syncs[k].syncobj_handle = syncs[k].handle;
                    in.syncs[k].flags = (syncs[k].flags & DRM_XE_SYNC_FLAG_SIGNAL) ? 1u : 0u;
                    in.syncs[k].timeline_value = syncs[k].timeline_value;
                    if ((syncs[k].flags & DRM_XE_SYNC_FLAG_SIGNAL) &&
                        syncs[k].handle < ARC_MAX_SYNCOBJ_TRACK &&
                        syncs[k].timeline_value > s->sync_points[syncs[k].handle]) {
                        s->sync_points[syncs[k].handle] = syncs[k].timeline_value;
                    }
                }
                pthread_mutex_unlock(&g_arc_lock);
            }

            pthread_mutex_lock(&g_arc_lock);
            invalidate_bind_dump_locked(fd, vb->vm_id, b->addr, b->range);
            pthread_mutex_unlock(&g_arc_lock);
            kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodVmBind,
                                                         &in, sizeof(in), NULL, NULL);
            if (kr != KERN_SUCCESS) {
                dump_timeout_history(fd);
                errno = ENOMEM;
                return -1;
            }

            pthread_mutex_lock(&g_arc_lock);
            if (in.op == kArcXeVmBindOpMap && b->obj != 0)
                record_vm_bind_locked(fd, vb->vm_id, b->obj, b->addr, b->range, b->obj_offset);
            pthread_mutex_unlock(&g_arc_lock);
        }

        /* Ensure out-syncs are signaled even if batch only had UNMAP_ALL / PREFETCH */
        if (vb->num_syncs > 0 && syncs) {
            uint32_t sig_handles[ARCXE_MAX_SYNCS_PER_OP];
            uint64_t sig_points[ARCXE_MAX_SYNCS_PER_OP];
            uint32_t n_sig = 0;
            pthread_mutex_lock(&g_arc_lock);
            for (uint32_t k = 0; k < vb->num_syncs && n_sig < ARCXE_MAX_SYNCS_PER_OP; k++) {
                if (syncs[k].flags & DRM_XE_SYNC_FLAG_SIGNAL) {
                    sig_handles[n_sig] = syncs[k].handle;
                    sig_points[n_sig]  = syncs[k].timeline_value;
                    if (syncs[k].handle < ARC_MAX_SYNCOBJ_TRACK &&
                        syncs[k].timeline_value > s->sync_points[syncs[k].handle])
                        s->sync_points[syncs[k].handle] = syncs[k].timeline_value;
                    n_sig++;
                }
            }
            pthread_mutex_unlock(&g_arc_lock);
            if (n_sig > 0) {
                struct ArcXeSyncobjSignalResetIn sr = {0};
                sr.count_handles = n_sig;
                sr.is_reset      = 0;
                memcpy(sr.handles, sig_handles, n_sig * sizeof(uint32_t));
                memcpy(sr.points, sig_points, n_sig * sizeof(uint64_t));
                IOConnectCallStructMethod(s->connect, kArcXeMethodSyncobjSignalReset, &sr, sizeof(sr), NULL, NULL);
            }
        }
        return 0;
    }

    case DRM_IOCTL_XE_EXEC_QUEUE_CREATE: {
        struct drm_xe_exec_queue_create *qc = (struct drm_xe_exec_queue_create *)arg;
        const struct drm_xe_engine_class_instance *inst =
            (const struct drm_xe_engine_class_instance *)(uintptr_t)qc->instances;
        if (!inst || qc->num_placements == 0) {
            errno = EINVAL;
            return -1;
        }
        if (inst[0].engine_class == DRM_XE_ENGINE_CLASS_VM_BIND) {
            pthread_mutex_lock(&g_arc_lock);
            qc->exec_queue_id = ARC_VIRTUAL_BIND_Q_BIT | (s->next_virtual_bind_q++);
            pthread_mutex_unlock(&g_arc_lock);
            return 0;
        }

        struct ArcXeExecQueueCreateIn in = {
            .vm_id            = qc->vm_id,
            .engine_class     = inst[0].engine_class,
            .engine_instance  = inst[0].engine_instance,
            .ring_size_bytes  = 16384,
            .priority         = 1,
        };
        struct ArcXeExecQueueCreateOut out = {0};
        size_t out_sz = sizeof(out);
        kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodExecQueueCreate,
                                                     &in, sizeof(in), &out, &out_sz);
        if (kr != KERN_SUCCESS || out.exec_queue_id == 0) {
            dump_timeout_history(fd);
            errno = ENOMEM;
            return -1;
        }
        qc->exec_queue_id = out.exec_queue_id;
        return 0;
    }

    case DRM_IOCTL_XE_EXEC_QUEUE_DESTROY: {
        struct drm_xe_exec_queue_destroy *qd = (struct drm_xe_exec_queue_destroy *)arg;
        if (qd->exec_queue_id & ARC_VIRTUAL_BIND_Q_BIT)
            return 0;
        struct ArcXeExecQueueDestroyIn in = { .exec_queue_id = qd->exec_queue_id };
        IOConnectCallStructMethod(s->connect, kArcXeMethodExecQueueDestroy, &in, sizeof(in), NULL, NULL);
        return 0;
    }

    case DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY: {
        struct drm_xe_exec_queue_get_property *qp = (struct drm_xe_exec_queue_get_property *)arg;
        qp->value = 0; /* Not banned / no reset */
        return 0;
    }

    case DRM_IOCTL_XE_EXEC: {
        struct drm_xe_exec *ex = (struct drm_xe_exec *)arg;
        const struct drm_xe_sync *syncs = (const struct drm_xe_sync *)(uintptr_t)ex->syncs;
        struct ArcXeExecIn in = {0};
        struct ArcXeExecOut out = {0};
        in.exec_queue_id    = ex->exec_queue_id;
        in.num_batch_buffer = ex->num_batch_buffer;
        in.batch_gpu_va     = ex->address;

        if (ex->num_syncs > 0 && syncs) {
            uint32_t ns = ex->num_syncs < ARCXE_MAX_SYNCS_PER_OP ? ex->num_syncs : ARCXE_MAX_SYNCS_PER_OP;
            in.num_syncs = ns;
            pthread_mutex_lock(&g_arc_lock);
            for (uint32_t i = 0; i < ns; i++) {
                in.syncs[i].syncobj_handle = syncs[i].handle;
                in.syncs[i].flags = (syncs[i].flags & DRM_XE_SYNC_FLAG_SIGNAL) ? 1u : 0u;
                in.syncs[i].timeline_value = syncs[i].timeline_value;
                if ((syncs[i].flags & DRM_XE_SYNC_FLAG_SIGNAL) &&
                    syncs[i].handle < ARC_MAX_SYNCOBJ_TRACK &&
                    syncs[i].timeline_value > s->sync_points[syncs[i].handle]) {
                    s->sync_points[syncs[i].handle] = syncs[i].timeline_value;
                }
            }
            pthread_mutex_unlock(&g_arc_lock);
        }

        size_t out_sz = sizeof(out);
        kern_return_t kr = IOConnectCallStructMethod(s->connect, kArcXeMethodExec,
                                                     &in, sizeof(in), &out, &out_sz);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "  [ArcXe EXEC ERROR] queue=%u batch=0x%012llx kr=0x%08x\n",
                    in.exec_queue_id, (unsigned long long)in.batch_gpu_va, (unsigned)kr);
            dump_timeout_history(fd);
            errno = EIO;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_XE_WAIT_USER_FENCE: {
        struct drm_xe_wait_user_fence *wf = (struct drm_xe_wait_user_fence *)arg;
        const volatile uint64_t *ptr = (const volatile uint64_t *)(uintptr_t)wf->addr;
        if (!ptr) return 0;
        const int64_t max_us = (wf->timeout <= 0) ? 0 : (wf->timeout / 1000 > 2000000 ? 2000000 : wf->timeout / 1000);
        int64_t waited = 0;
        while ((*ptr & wf->mask) != (wf->value & wf->mask)) {
            if (waited >= max_us) {
                errno = ETIME;
                return -1;
            }
            usleep(100);
            waited += 100;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_CREATE: {
        struct drm_syncobj_create *sc = (struct drm_syncobj_create *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        int err = arc_sync_create(&sp.base, sc->flags, &sc->handle);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_DESTROY: {
        struct drm_syncobj_destroy *sd = (struct drm_syncobj_destroy *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        int err = arc_sync_destroy(&sp.base, sd->handle);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_WAIT: {
        struct drm_syncobj_wait *sw = (struct drm_syncobj_wait *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        uint32_t *handles = (uint32_t *)(uintptr_t)sw->handles;
        uint32_t first = 0;
        int err = arc_sync_wait(&sp.base, handles, sw->count_handles,
                                sw->timeout_nsec, sw->flags, &first);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        sw->first_signaled = first;
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
        struct drm_syncobj_timeline_wait *tw = (struct drm_syncobj_timeline_wait *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        uint32_t *handles = (uint32_t *)(uintptr_t)tw->handles;
        uint64_t *points  = (uint64_t *)(uintptr_t)tw->points;
        uint32_t first = 0;
        int err = arc_sync_timeline_wait(&sp.base, handles, points, tw->count_handles,
                                         tw->timeout_nsec, tw->flags, &first);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        tw->first_signaled = first;
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_RESET: {
        struct drm_syncobj_array *sa = (struct drm_syncobj_array *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        const uint32_t *handles = (const uint32_t *)(uintptr_t)sa->handles;
        int err = arc_sync_reset(&sp.base, handles, sa->count_handles);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_SIGNAL: {
        struct drm_syncobj_array *sa = (struct drm_syncobj_array *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        const uint32_t *handles = (const uint32_t *)(uintptr_t)sa->handles;
        int err = arc_sync_signal(&sp.base, handles, sa->count_handles);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL: {
        struct drm_syncobj_timeline_array *ta = (struct drm_syncobj_timeline_array *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        const uint32_t *handles = (const uint32_t *)(uintptr_t)ta->handles;
        uint64_t *points        = (uint64_t *)(uintptr_t)ta->points;
        int err = arc_sync_timeline_signal(&sp.base, handles, points, ta->count_handles);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_QUERY: {
        struct drm_syncobj_timeline_array *ta = (struct drm_syncobj_timeline_array *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        uint32_t *handles = (uint32_t *)(uintptr_t)ta->handles;
        uint64_t *points  = (uint64_t *)(uintptr_t)ta->points;
        int err = arc_sync_query(&sp.base, handles, points, ta->count_handles, ta->flags);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    case DRM_IOCTL_SYNCOBJ_TRANSFER: {
        struct drm_syncobj_transfer *st = (struct drm_syncobj_transfer *)arg;
        struct ArcSyncProvider sp = { .fd = fd };
        int err = arc_sync_transfer(&sp.base, st->dst_handle, st->dst_point,
                                    st->src_handle, st->src_point, st->flags);
        if (err < 0) {
            errno = -err;
            return -1;
        }
        return 0;
    }

    default:
        errno = ENOTTY;
        return -1;
    }
}

/* ============================================================================
 * 6. Mesa `util_sync_provider` Implementation (Zero libdrm Dependency)
 * ============================================================================ */

static int arc_sync_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(ap->fd);
    io_connect_t conn = s ? s->connect : IO_OBJECT_NULL;
    pthread_mutex_unlock(&g_arc_lock);
    if (conn == IO_OBJECT_NULL || !handle) return -EBADF;

    struct ArcXeSyncobjCreateIn in = { .signaled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1u : 0u };
    struct ArcXeSyncobjCreateOut out = {0};
    size_t out_sz = sizeof(out);
    if (IOConnectCallStructMethod(conn, kArcXeMethodSyncobjCreate, &in, sizeof(in), &out, &out_sz) != KERN_SUCCESS)
        return -ENOMEM;

    pthread_mutex_lock(&g_arc_lock);
    s = lookup_session_locked(ap->fd);
    if (s && out.handle < ARC_MAX_SYNCOBJ_TRACK)
        s->sync_points[out.handle] = in.signaled ? 1ULL : 0ULL;
    pthread_mutex_unlock(&g_arc_lock);

    *handle = out.handle;
    return 0;
}

static int arc_sync_destroy(struct util_sync_provider *p, uint32_t handle)
{
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(ap->fd);
    io_connect_t conn = s ? s->connect : IO_OBJECT_NULL;
    if (s && handle < ARC_MAX_SYNCOBJ_TRACK)
        s->sync_points[handle] = 0;
    pthread_mutex_unlock(&g_arc_lock);
    if (conn == IO_OBJECT_NULL) return -EBADF;

    struct ArcXeSyncobjDestroyIn in = { .handle = handle };
    IOConnectCallStructMethod(conn, kArcXeMethodSyncobjDestroy, &in, sizeof(in), NULL, NULL);
    return 0;
}

static int arc_sync_timeline_wait(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                                  unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                  uint32_t *first_signaled)
{
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    if (num_handles == 0) return 0;

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(ap->fd);
    io_connect_t conn = s ? s->connect : IO_OBJECT_NULL;
    pthread_mutex_unlock(&g_arc_lock);
    if (conn == IO_OBJECT_NULL) return -EBADF;

    /* Convert Mesa's absolute CLOCK_MONOTONIC deadline (os_time_get_nano) to relative wait duration */
    struct timespec now_ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    const int64_t now_ns = (int64_t)now_ts.tv_sec * 1000000000LL + (int64_t)now_ts.tv_nsec;
    int64_t rel_ns = 0;
    if (timeout_nsec == INT64_MAX)
        rel_ns = 5000000000LL;
    else if (timeout_nsec > now_ns)
        rel_ns = timeout_nsec - now_ns;

    struct ArcXeSyncobjWaitIn in = {0};
    struct ArcXeSyncobjWaitOutExtended response = {0};
    in.count_handles = num_handles < ARCXE_MAX_SYNCS_PER_OP ? num_handles : ARCXE_MAX_SYNCS_PER_OP;
    in.flags         = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? ARCXE_SYNCOBJ_WAIT_ALL : 0u;
    in.timeout_nsec  = rel_ns;
    for (uint32_t i = 0; i < in.count_handles; i++) {
        in.handles[i] = handles[i];
        in.points[i]  = points ? points[i] : 0ULL;
    }

    size_t out_sz = sizeof(response);
    if (IOConnectCallStructMethod(conn, kArcXeMethodSyncobjWait, &in, sizeof(in), &response, &out_sz) != KERN_SUCCESS)
    {
        dump_timeout_history(ap->fd);
        return -EIO;
    }
    if (out_sz < sizeof(response.base)) return -EIO;
    const struct ArcXeSyncobjWaitOut out = response.base;
    if (out.timed_out) {
        if (out.diag.timed_out) {
            fprintf(stderr,
                    "  [ArcXe GPU TIMEOUT] completed=%llu EL_STATUS=0x%08x_%08x LRCA=0x%08x HEAD=0x%x TAIL=0x%x "
                    "START=0x%08x CTL=0x%08x ACTHD=0x%08x_%08x IPEHR=0x%08x ESR=0x%08x INSTDONE=0x%08x "
                    "DBG2=0x%08x GEOM=0x%08x SC=0x%08x SAMP=0x%08x ROW=0x%08x MI_MODE=0x%08x\n",
                    (unsigned long long)out.diag.completed_seqno,
                    out.diag.engine_status_hi, out.diag.engine_status,
                    out.diag.current_lrca,
                    out.diag.ring_head, out.diag.ring_tail,
                    out.diag.ring_start, out.diag.ring_ctl,
                    out.diag.acthd_hi, out.diag.acthd_lo,
                    out.diag.ipehr, out.diag.esr, out.diag.instdone,
                    out.dbg2, out.geom_instdone, out.sc_instdone,
                    out.sampler_instdone, out.row_instdone, out.diag.mi_mode);
            if (out.progress_flags & ARCXE_PROGRESS_VALID)
                fprintf(stderr, "  [ArcXe GPU PROGRESS] begin=%u invalidate=%u beforeBB=%u returned=%u "
                                "(parser markers, not completion fences)\n",
                        !!(out.progress_flags & ARCXE_PROGRESS_BEGIN),
                        !!(out.progress_flags & ARCXE_PROGRESS_INVALIDATED),
                        !!(out.progress_flags & ARCXE_PROGRESS_BEFORE_BB),
                        !!(out.progress_flags & ARCXE_PROGRESS_RETURNED));
            if (out_sz >= ARCXE_SYNCOBJ_WAIT_V1_SIZE && response.detail.valid) {
                const struct ArcXeTimeoutDetail *d = &response.detail;
                fprintf(stderr, "  [ArcXe GPU FAULT] reg=0x%08x data0=0x%08x data1=0x%08x\n",
                        d->fault_reg, d->fault_data0, d->fault_data1);
                if (d->fault_reg != UINT32_MAX && (d->fault_reg & 1u)) {
                    const uint64_t address = ((uint64_t)(d->fault_data1 & 15u) << 44) |
                                             ((uint64_t)d->fault_data0 << 12);
                    fprintf(stderr, "  [ArcXe GPU FAULT] address=0x%012llx space=%s\n",
                            (unsigned long long)address, (d->fault_data1 & 16u) ? "GGTT" : "PPGTT");
                }
                fprintf(stderr, "  [ArcXe CONTEXT] idle_parks=%u control=0x%08x head=0x%x tail=0x%x\n",
                        d->idle_park_count, d->context_control, d->context_head, d->context_tail);
                const uint32_t count = d->ring_count <= 16u ? d->ring_count : 16u;
                fprintf(stderr, "  [ArcXe RING DWORDS] active LRCA=0x%08x ring=0x%08x start=0x%x count=%u (wraps at ring end)\n",
                        out.diag.current_lrca, out.diag.ring_start, d->ring_offset, count);
                for (uint32_t i = 0; i < count; ++i) {
                    if (!(i & 3u)) fprintf(stderr, "    +%02x:", i * 4u);
                    fprintf(stderr, " %08x", d->ring_dw[i]);
                    if ((i & 3u) == 3u || i + 1u == count) fputc('\n', stderr);
                }
                if (out_sz >= ARCXE_SYNCOBJ_WAIT_V2_SIZE && d->hw.version == 1) {
                    const struct ArcXeTimeoutHwState *h = &d->hw;
                    fprintf(stderr, "  [ArcXe HW STATE] primary instance: CMD_CCTL=%08x GFX_MODE=%08x CS_CHICKEN1=%08x L3ALLOC=%08x L3SQCREG5=%08x SAMPLER_MODE=%08x RCC=%08x PSS=%08x SC_EXTRA=%08x SC_EXTRA2=%08x\n",
                            h->cmd_cctl, h->gfx_mode, h->cs_chicken1, h->l3_alloc,
                            h->l3_sqcreg5, h->sampler_mode, h->rcc, h->pss_chicken,
                            h->sc_extra, h->sc_extra2);
                    fprintf(stderr, "  [ArcXe CACHE ATTR] PAT=%08x,%08x,%08x,%08x,%08x MOCS1=%08x MOCS9=%08x LNCF0=%08x LNCF4=%08x\n",
                            h->pat[0], h->pat[1], h->pat[2], h->pat[3], h->pat[4],
                            h->mocs1, h->mocs9, h->lncf0, h->lncf4);
                }
            }
            if (out_sz >= ARCXE_SYNCOBJ_WAIT_V3_SIZE && response.detail.mcr.version == 1) {
                const struct ArcXeTimeoutMcrState *m = &response.detail.mcr;
                fprintf(stderr, "  [ArcXe MCR] selector=%08x enabled_dss=%08x count=%u\n",
                        m->selector, m->enabled_dss, m->count);
                for (uint32_t i = 0; i < m->count && i < 8; ++i) {
                    const struct ArcXeTimeoutDssSample *v = &m->samples[i];
                    fprintf(stderr, "    [ArcXe DSS] steering=%03x SAMPLER=%08x ROW2=%08x ROW3=%08x HDC0=%08x SAMP_DONE=%08x ROW_DONE=%08x\n",
                            v->steering, v->sampler_mode, v->row_chicken2, v->row_chicken3,
                            v->hdc_chicken0, v->sampler_instdone, v->row_instdone);
                }
            }
            if (out_sz >= sizeof(response) && response.detail.rcu.version == 1)
                fprintf(stderr, "  [ArcXe RCU] MODE=%08x CCS_ENABLE=%u (compute submits on RCS)\n",
                        response.detail.rcu.mode, response.detail.rcu.mode & 1u);
            dump_timeout_batch(ap->fd, ((uint64_t)out.diag.acthd_hi << 32) | out.diag.acthd_lo);
            dump_timeout_history(ap->fd);
        }
        errno = ETIME;
        return -ETIME;
    }
    if (first_signaled) *first_signaled = out.first_signaled;
    return 0;
}

static int arc_sync_wait(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
                         int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
    return arc_sync_timeline_wait(p, handles, NULL, num_handles, timeout_nsec, flags, first_signaled);
}

static int arc_sync_reset_or_signal(struct util_sync_provider *p, const uint32_t *handles,
                                    const uint64_t *points, uint32_t count, bool is_reset)
{
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    if (count == 0) return 0;

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(ap->fd);
    io_connect_t conn = s ? s->connect : IO_OBJECT_NULL;
    if (s) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t h = handles[i];
            if (h < ARC_MAX_SYNCOBJ_TRACK) {
                if (is_reset) s->sync_points[h] = 0;
                else if (points && points[i] > s->sync_points[h]) s->sync_points[h] = points[i];
                else if (!points && s->sync_points[h] == 0) s->sync_points[h] = 1ULL;
            }
        }
    }
    pthread_mutex_unlock(&g_arc_lock);
    if (conn == IO_OBJECT_NULL) return -EBADF;

    for (uint32_t off = 0; off < count; off += ARCXE_MAX_SYNCS_PER_OP) {
        struct ArcXeSyncobjSignalResetIn in = {0};
        uint32_t batch = (count - off < ARCXE_MAX_SYNCS_PER_OP) ? (count - off) : ARCXE_MAX_SYNCS_PER_OP;
        in.count_handles = batch;
        in.is_reset      = is_reset ? 1u : 0u;
        for (uint32_t i = 0; i < batch; i++) {
            in.handles[i] = handles[off + i];
            in.points[i]  = points ? points[off + i] : (is_reset ? 0ULL : 1ULL);
        }
        IOConnectCallStructMethod(conn, kArcXeMethodSyncobjSignalReset, &in, sizeof(in), NULL, NULL);
    }
    return 0;
}

static int arc_sync_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
    return arc_sync_reset_or_signal(p, handles, NULL, count, true);
}

static int arc_sync_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t count)
{
    return arc_sync_reset_or_signal(p, handles, NULL, count, false);
}

static int arc_sync_timeline_signal(struct util_sync_provider *p, const uint32_t *handles,
                                    uint64_t *points, uint32_t count)
{
    return arc_sync_reset_or_signal(p, handles, points, count, false);
}

static int arc_sync_query(struct util_sync_provider *p, uint32_t *handles, uint64_t *points,
                          uint32_t count, uint32_t flags)
{
    (void)flags;
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    /* Ensure any GPU-bound syncobj completion updates before returning timeline value */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t first = 0;
        arc_sync_timeline_wait(p, &handles[i], NULL, 1, 0, 0, &first);
    }

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(ap->fd);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t h = handles[i];
        points[i] = (s && h < ARC_MAX_SYNCOBJ_TRACK) ? s->sync_points[h] : 0ULL;
    }
    pthread_mutex_unlock(&g_arc_lock);
    return 0;
}

static int arc_sync_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
                             uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
    (void)flags;
    uint64_t val = src_point;
    if (val == 0)
        arc_sync_query(p, &src_handle, &val, 1, 0);
    uint64_t target = dst_point ? dst_point : val;
    return arc_sync_timeline_signal(p, &dst_handle, &target, 1);
}

static int arc_sync_unsupported_fd(struct util_sync_provider *p, uint32_t h, int *fd)
{
    (void)p; (void)h; (void)fd;
    return -ENOTSUP;
}

static int arc_sync_unsupported_import(struct util_sync_provider *p, uint32_t h, int fd)
{
    (void)p; (void)h; (void)fd;
    return -ENOTSUP;
}

static int arc_sync_unsupported_f2h(struct util_sync_provider *p, int fd, uint32_t *h)
{
    (void)p; (void)fd; (void)h;
    return -ENOTSUP;
}

static void arc_sync_finalize(struct util_sync_provider *p)
{
    free(p);
}

static struct util_sync_provider *arc_sync_clone(struct util_sync_provider *p)
{
    struct ArcSyncProvider *ap = (struct ArcSyncProvider *)p;
    return arc_xnu_sync_provider(ap->fd);
}

struct util_sync_provider *arc_xnu_sync_provider(int fd)
{
    struct ArcSyncProvider *ap = calloc(1, sizeof(*ap));
    if (!ap) return NULL;
    ap->fd                     = fd;
    ap->base.create            = arc_sync_create;
    ap->base.destroy           = arc_sync_destroy;
    ap->base.handle_to_fd      = arc_sync_unsupported_fd;
    ap->base.fd_to_handle      = arc_sync_unsupported_f2h;
    ap->base.import_sync_file  = arc_sync_unsupported_import;
    ap->base.export_sync_file  = arc_sync_unsupported_fd;
    ap->base.wait              = arc_sync_wait;
    ap->base.reset             = arc_sync_reset;
    ap->base.signal            = arc_sync_signal;
    ap->base.timeline_signal   = arc_sync_timeline_signal;
    ap->base.timeline_wait     = arc_sync_timeline_wait;
    ap->base.query             = arc_sync_query;
    ap->base.transfer          = arc_sync_transfer;
    ap->base.finalize          = arc_sync_finalize;
    ap->base.clone             = arc_sync_clone;
    return &ap->base;
}

/* ============================================================================
 * 7. Lightweight `xf86drm` Shim Functions for Mesa Device Enumeration
 * ============================================================================ */

static drmDevicePtr alloc_single_arc_drm_device(const struct ArcXeQueryDeviceOut *q)
{
    drmDevicePtr dev = calloc(1, sizeof(drmDevice));
    if (!dev) return NULL;
    dev->nodes        = calloc(DRM_NODE_MAX, sizeof(char *));
    dev->businfo.pci  = calloc(1, sizeof(drmPciBusInfo));
    dev->deviceinfo.pci = calloc(1, sizeof(drmPciDeviceInfo));
    if (!dev->nodes || !dev->businfo.pci || !dev->deviceinfo.pci) {
        drmFreeDevice(&dev);
        return NULL;
    }

    dev->available_nodes         = (1 << DRM_NODE_RENDER);
    dev->nodes[DRM_NODE_PRIMARY] = strdup("");
    dev->nodes[DRM_NODE_CONTROL] = strdup("");
    dev->nodes[DRM_NODE_RENDER]  = strdup(ARC_XNU_FAKE_RENDER_NODE);
    dev->bustype                 = DRM_BUS_PCI;
    dev->businfo.pci->domain     = 0;
    dev->businfo.pci->bus        = 0;
    dev->businfo.pci->dev        = 2; /* Intel iGPU BDF 00:02.0 */
    dev->businfo.pci->func       = 0;

    dev->deviceinfo.pci->vendor_id    = q->vendor_id;
    dev->deviceinfo.pci->device_id    = q->device_id;
    dev->deviceinfo.pci->subvendor_id = q->subvendor_id;
    dev->deviceinfo.pci->subdevice_id = q->subdevice_id;
    dev->deviceinfo.pci->revision_id  = q->revision_id;
    return dev;
}

int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
    (void)flags;
    io_service_t svc  = IO_OBJECT_NULL;
    io_connect_t conn = IO_OBJECT_NULL;
    if (arc_xnu_open_iokit_connection(&svc, &conn) != KERN_SUCCESS || conn == IO_OBJECT_NULL)
        return 0;

    struct ArcXeQueryDeviceOut q = {0};
    size_t out_sz = sizeof(q);
    bool ok = false;
    if (IOConnectCallStructMethod(conn, kArcXeMethodQueryDevice, NULL, 0, &q, &out_sz) == KERN_SUCCESS)
        ok = (q.vendor_id == 0x8086);
    IOServiceClose(conn);
    IOObjectRelease(svc);
    if (!ok) return 0;

    if (!devices || max_devices <= 0)
        return 1;

    devices[0] = alloc_single_arc_drm_device(&q);
    return devices[0] ? 1 : -ENOMEM;
}

void drmFreeDevices(drmDevicePtr devices[], int count)
{
    if (!devices) return;
    for (int i = 0; i < count; i++)
        drmFreeDevice(&devices[i]);
}

int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
    (void)flags;
    if (!device) return -EINVAL;

    pthread_mutex_lock(&g_arc_lock);
    struct ArcFdSession *s = lookup_session_locked(fd);
    struct ArcXeQueryDeviceOut q = s ? s->dev_query : (struct ArcXeQueryDeviceOut){0};
    pthread_mutex_unlock(&g_arc_lock);

    if (q.vendor_id != 0x8086)
        return -ENODEV;

    *device = alloc_single_arc_drm_device(&q);
    return *device ? 0 : -ENOMEM;
}

void drmFreeDevice(drmDevicePtr *device)
{
    if (!device || !*device) return;
    drmDevicePtr d = *device;
    if (d->nodes) {
        for (int i = 0; i < DRM_NODE_MAX; i++)
            free(d->nodes[i]);
        free(d->nodes);
    }
    free(d->businfo.pci);
    free(d->deviceinfo.pci);
    free(d);
    *device = NULL;
}

drmVersionPtr drmGetVersion(int fd)
{
    if (!arc_xnu_is_fd(fd)) return NULL;
    drmVersionPtr v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->version_major = 1;
    v->version_minor = 1;
    v->name          = strdup("xe");
    v->name_len      = 2;
    v->date          = strdup("20261101");
    v->date_len      = 8;
    v->desc          = strdup("Intel Arc Xe XNU IOKit KMD");
    v->desc_len      = (int)strlen(v->desc);
    return v;
}

void drmFreeVersion(drmVersionPtr v)
{
    if (!v) return;
    free(v->name);
    free(v->date);
    free(v->desc);
    free(v);
}

int drmIoctl(int fd, unsigned long request, void *arg)
{
    return arc_xnu_ioctl(fd, request, arg);
}

char *drmGetRenderDeviceNameFromFd(int fd)
{
    return arc_xnu_is_fd(fd) ? strdup(ARC_XNU_FAKE_RENDER_NODE) : NULL;
}

int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size)
{
    (void)fd; (void)drmCommandIndex; (void)data; (void)size;
    errno = ENOTSUP;
    return -1;
}

#endif /* __APPLE__ */
