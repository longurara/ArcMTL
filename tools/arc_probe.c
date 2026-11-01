/*
 * tools/arc_probe.c — Stage 1 Kernel & Xe-LPG Hardware Execution Diagnostic for ArcXe.kext
 * SPDX-License-Identifier: MIT
 *
 * Usage:
 *   ./out/tools/arc_probe              # Full hardware query + RCS0 PPGTT MI_STORE_DATA_IMM test
 *   ./out/tools/arc_probe --info-only  # Read-only hardware & GuC status query
 */

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arc_regs_mtl.h"
#include "arc_uapi.h"

static int call_struct(io_connect_t conn, uint32_t sel,
                       const void *in_buf, size_t in_sz,
                       void *out_buf, size_t *out_sz)
{
    kern_return_t kr = IOConnectCallStructMethod(conn, sel, in_buf, in_sz, out_buf, out_sz);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[FAIL] IOConnectCallStructMethod(selector=%u) -> 0x%08x\n", sel, kr);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool info_only = (argc > 1 && strcmp(argv[1], "--info-only") == 0);

    printf("==================================================================\n");
    printf(" ArcXe Stage 1 Hardware & Command Streamer Probe (macOS 15+)\n");
    printf("==================================================================\n");

    io_service_t svc = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching(ARCXE_IOKIT_SERVICE_NAME));
    if (svc == IO_OBJECT_NULL) {
        fprintf(stderr, "[FAIL] '%s' not found in IORegistry.\n"
                        "       Verify ArcXe.kext is loaded (`kmutil showloaded | grep -i arc`)\n"
                        "       and '-arcoff' is NOT present in NVRAM boot-args.\n",
                ARCXE_IOKIT_SERVICE_NAME);
        return 1;
    }

    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || conn == IO_OBJECT_NULL) {
        fprintf(stderr, "[FAIL] IOServiceOpen(%s) failed: 0x%08x\n", ARCXE_IOKIT_SERVICE_NAME, kr);
        return 2;
    }

    struct ArcXeQueryDeviceOut info;
    memset(&info, 0, sizeof(info));
    size_t out_sz = sizeof(info);
    if (call_struct(conn, kArcXeMethodQueryDevice, NULL, 0, &info, &out_sz) != 0) {
        IOServiceClose(conn);
        return 3;
    }

    printf("  UAPI Version       : %u\n", info.uapi_version);
    printf("  PCI Identity       : %04x:%04x (subsys %04x:%04x, rev 0x%02x)\n",
           info.vendor_id, info.device_id, info.subvendor_id, info.subdevice_id, info.revision_id);
    printf("  Graphics IP (GT0)  : %u.%02u (GMD_ID=0x%08x) — Xe-LPG\n",
           info.graphics_verx100 / 100, info.graphics_verx100 % 100, info.gmd_id_gt0);
    printf("  Media IP    (GT1)  : %u.%02u (GMD_ID=0x%08x) — Xe-LPM+\n",
           info.media_verx100 / 100, info.media_verx100 % 100, info.gmd_id_gt1);
    printf("  Topology           : %u Xe-cores, %u Vector Engines (EUs)\n",
           info.total_xe_cores, info.total_eus);
    printf("  DSS / EU Masks     : geom=0x%08x compute=0x%08x eu_per_dss=0x%04x\n",
           info.geom_dss_mask, info.compute_dss_mask, info.eu_per_dss_mask);
    printf("  Virtual Addressing : %u-bit (%u-level PML4 PPGTT)\n",
           info.va_bits, info.max_page_Level);
    printf("  Clocks & Timestamp : crystal=%u Hz, cs_ts=%u Hz, live_ticks=%" PRIu64 "\n",
           info.crystal_clock_hz, info.gpu_timestamp_freq_hz, info.gpu_timestamp_ticks);
    printf("  BAR0 (GTTMMADR)    : 0x%012" PRIx64 "\n", info.bar0_phys);
    printf("  BAR2 (GMADR)       : 0x%012" PRIx64 " (%" PRIu64 " MiB)\n",
           info.bar2_phys, info.bar2_size / (1024ULL * 1024ULL));
    printf("  DSM Stolen Memory  : 0x%012" PRIx64 " (%" PRIu64 " MiB)\n",
           info.stolen_base_phys, info.stolen_size / (1024ULL * 1024ULL));
    printf("  Usable GGTT        : [0x%08" PRIx64 " .. +%" PRIu64 " MiB]\n",
           info.ggtt_usable_start, info.ggtt_usable_size / (1024ULL * 1024ULL));
    printf("  System UMA Memory  : %" PRIu64 " MiB\n",
           info.sysmem_total_bytes / (1024ULL * 1024ULL));
    printf("  Submission Backend : %s (GuC loaded=%u, GUC_STATUS=0x%08x)\n",
           info.submit_backend == kArcXeSubmitGuc ? "GuC v70 CTB" : "Direct Hardware Execlist",
           info.guc_fw_loaded, info.guc_status_reg);

    if (info_only) {
        IOServiceClose(conn);
        printf("==================================================================\n");
        return 0;
    }

    printf("\n--- Running Live RCS0 48-bit PPGTT Execution Smoke Test ---\n");

    int rc = 0;
    uint32_t gem_handle = 0, vm_id = 0, queue_id = 0;
    mach_vm_address_t map_addr = 0;
    mach_vm_size_t map_size = 0;

    /* 1. Allocate 16 KiB 2-way coherent UMA BO */
    struct ArcXeGemCreateIn gem_in = {
        .size = 16384,
        .flags = ARCXE_GEM_FLAG_CPU_WB_COHERENT | ARCXE_GEM_FLAG_NEEDS_CPU_VIS,
        .pat_index = ARC_MTL_PAT_IDX_WB_2WAY_COH,
        .pad0 = 0,
    };
    struct ArcXeGemCreateOut gem_out;
    memset(&gem_out, 0, sizeof(gem_out));
    out_sz = sizeof(gem_out);
    if (call_struct(conn, kArcXeMethodGemCreate, &gem_in, sizeof(gem_in), &gem_out, &out_sz) != 0) {
        rc = 4;
        goto cleanup;
    }
    gem_handle = gem_out.gem_handle;
    printf("  [1/6] Allocated UMA BO handle=%u (%" PRIu64 " bytes, PAT=%u 2-way coherent)\n",
           gem_handle, gem_out.actual_size, ARC_MTL_PAT_IDX_WB_2WAY_COH);

    /* 2. Zero-copy map UMA BO into userspace */
    kr = IOConnectMapMemory64(conn, gem_handle, mach_task_self(),
                              &map_addr, &map_size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS || !map_addr) {
        fprintf(stderr, "[FAIL] IOConnectMapMemory64(handle=%u) -> 0x%08x\n", gem_handle, kr);
        rc = 5;
        goto cleanup;
    }
    uint8_t *cpu_ptr = (uint8_t *)(uintptr_t)map_addr;
    memset(cpu_ptr, 0, (size_t)map_size);

    /* 3. Create 48-bit 4-level PPGTT VM */
    struct ArcXeVmCreateIn vm_in = { .flags = 0, .pad0 = 0 };
    struct ArcXeVmCreateOut vm_out;
    memset(&vm_out, 0, sizeof(vm_out));
    out_sz = sizeof(vm_out);
    if (call_struct(conn, kArcXeMethodVmCreate, &vm_in, sizeof(vm_in), &vm_out, &out_sz) != 0) {
        rc = 6;
        goto cleanup;
    }
    vm_id = vm_out.vm_id;
    printf("  [2/6] Created 48-bit PPGTT VM id=%u (PML4 phys=0x%" PRIx64 ")\n",
           vm_id, vm_out.pml4_phys);

    /* 4. Bind UMA BO into PPGTT at GPU VA 0x0000000100000000 */
    const uint64_t batch_va  = 0x0000000100000000ULL;
    const uint64_t target_va = batch_va + 0x1000ULL;
    struct ArcXeVmBindIn bind_in;
    memset(&bind_in, 0, sizeof(bind_in));
    bind_in.vm_id = vm_id;
    bind_in.op = kArcXeVmBindOpMap;
    bind_in.gem_handle = gem_handle;
    bind_in.pat_index = ARC_MTL_PAT_IDX_WB_2WAY_COH;
    bind_in.gpu_va = batch_va;
    bind_in.bo_offset = 0;
    bind_in.range_size = gem_out.actual_size;
    out_sz = 0;
    if (call_struct(conn, kArcXeMethodVmBind, &bind_in, sizeof(bind_in), NULL, &out_sz) != 0) {
        rc = 7;
        goto cleanup;
    }
    printf("  [3/6] Bound BO -> PPGTT VA 0x%012" PRIx64 " (%" PRIu64 " bytes)\n",
           batch_va, gem_out.actual_size);

    /* 5. Write Xe-LPG MI_STORE_DATA_IMM (QWORD) + MI_BATCH_BUFFER_END */
    const uint64_t magic = 0xA2C7D550155CAFE0ULL;
    volatile uint64_t *result_slot = (volatile uint64_t *)(cpu_ptr + 0x1000);
    *result_slot = 0xDEADBEEFCAFEBABEULL;

    uint32_t *cs = (uint32_t *)cpu_ptr;
    uint32_t idx = 0;
    cs[idx++] = ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_QWORD_LEN;
    cs[idx++] = (uint32_t)(target_va & 0xFFFFFFFFULL);
    cs[idx++] = (uint32_t)(target_va >> 32);
    cs[idx++] = (uint32_t)(magic & 0xFFFFFFFFULL);
    cs[idx++] = (uint32_t)(magic >> 32);
    cs[idx++] = ARC_MI_NOOP;
    cs[idx++] = ARC_MI_BATCH_BUFFER_END;
    cs[idx++] = ARC_MI_NOOP;
    __sync_synchronize();

    /* 6. Create RCS0 queue, execute batch, wait for PPHWSP seqno fence */
    struct ArcXeExecQueueCreateIn q_in = {
        .vm_id = vm_id,
        .engine_class = kArcXeEngineClassRender,
        .engine_instance = 0,
        .ring_size_bytes = 16384,
        .priority = 1,
    };
    struct ArcXeExecQueueCreateOut q_out;
    memset(&q_out, 0, sizeof(q_out));
    out_sz = sizeof(q_out);
    if (call_struct(conn, kArcXeMethodExecQueueCreate, &q_in, sizeof(q_in), &q_out, &out_sz) != 0) {
        rc = 8;
        goto cleanup;
    }
    queue_id = q_out.exec_queue_id;
    printf("  [4/6] Created RCS0 ExecQueue id=%u (LRC GGTT=0x%08x)\n",
           queue_id, q_out.ggtt_lrc_addr);

    struct ArcXeExecIn exec_in;
    memset(&exec_in, 0, sizeof(exec_in));
    exec_in.exec_queue_id = queue_id;
    exec_in.num_batch_buffer = 1;
    exec_in.batch_gpu_va = batch_va;
    struct ArcXeExecOut exec_out;
    memset(&exec_out, 0, sizeof(exec_out));
    out_sz = sizeof(exec_out);
    if (call_struct(conn, kArcXeMethodExec, &exec_in, sizeof(exec_in), &exec_out, &out_sz) != 0) {
        rc = 9;
        goto cleanup;
    }
    printf("  [5/6] Submitted batch -> fence seqno=%" PRIu64 "\n", exec_out.submitted_seqno);

    struct ArcXeWaitQueueIn wait_in = {
        .exec_queue_id = queue_id,
        .pad0 = 0,
        .target_seqno = exec_out.submitted_seqno,
        .timeout_ns = 2000000000ULL, /* 2s */
    };
    struct ArcXeWaitQueueOut wait_out;
    memset(&wait_out, 0, sizeof(wait_out));
    out_sz = sizeof(wait_out);
    if (call_struct(conn, kArcXeMethodWaitQueueIdle, &wait_in, sizeof(wait_in), &wait_out, &out_sz) != 0) {
        rc = 10;
        goto cleanup;
    }

    __sync_synchronize();
    uint64_t observed = *result_slot;
    printf("  [6/6] Fence completed_seqno=%" PRIu64 " timed_out=%u engine_status=0x%08x\n",
           wait_out.completed_seqno, wait_out.timed_out, wait_out.engine_status);
    printf("        UMA zero-copy readback: expected=0x%016" PRIx64 " observed=0x%016" PRIx64 "\n",
           magic, observed);

    if (!wait_out.timed_out && observed == magic) {
        printf("\n[PASS] Stage 1 PASSED: Intel Arc Xe-LPG RCS0 executed PPGTT batch & wrote coherent UMA!\n");
    } else {
        fprintf(stderr, "\n[FAIL] Stage 1 FAILED: GPU timeout or UMA readback mismatch!\n");
        rc = 11;
    }

cleanup:
    if (queue_id) {
        struct ArcXeExecQueueDestroyIn dq = { .exec_queue_id = queue_id };
        size_t z = 0;
        IOConnectCallStructMethod(conn, kArcXeMethodExecQueueDestroy, &dq, sizeof(dq), NULL, &z);
    }
    if (vm_id) {
        struct ArcXeVmDestroyIn dv = { .vm_id = vm_id };
        size_t z = 0;
        IOConnectCallStructMethod(conn, kArcXeMethodVmDestroy, &dv, sizeof(dv), NULL, &z);
    }
    if (map_addr) {
        IOConnectUnmapMemory64(conn, gem_handle, mach_task_self(), map_addr);
    }
    if (gem_handle) {
        struct ArcXeGemCloseIn cg = { .gem_handle = gem_handle };
        size_t z = 0;
        IOConnectCallStructMethod(conn, kArcXeMethodGemClose, &cg, sizeof(cg), NULL, &z);
    }
    IOServiceClose(conn);
    return rc;
}
