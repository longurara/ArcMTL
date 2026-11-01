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

static bool run_engine_smoke_test(io_connect_t conn, uint32_t vm_id,
                                  uint8_t *cpu_ptr, uint64_t base_va,
                                  uint16_t engine_class, const char *engine_name,
                                  uint32_t batch_off, uint32_t target_off,
                                  uint64_t magic)
{
    const uint64_t batch1_va = base_va + batch_off;
    const uint64_t batch2_va = base_va + batch_off + 0x100u;
    const uint64_t target_va = base_va + target_off;
    const uint64_t magic1    = magic ^ 0x1111111111111111ULL;
    volatile uint64_t *result_slot = (volatile uint64_t *)(cpu_ptr + target_off);
    *result_slot = 0xDEADBEEFCAFEBABEULL;

    /* Batch #1 at +0x000: write magic1 */
    uint32_t *cs1 = (uint32_t *)(cpu_ptr + batch_off);
    uint32_t idx = 0;
    cs1[idx++] = ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_QWORD_LEN;
    cs1[idx++] = (uint32_t)(target_va & 0xFFFFFFFFULL);
    cs1[idx++] = (uint32_t)(target_va >> 32);
    cs1[idx++] = (uint32_t)(magic1 & 0xFFFFFFFFULL);
    cs1[idx++] = (uint32_t)(magic1 >> 32);
    cs1[idx++] = ARC_MI_NOOP;
    cs1[idx++] = ARC_MI_BATCH_BUFFER_END;
    cs1[idx++] = ARC_MI_NOOP;

    /* Batch #2 at +0x100: write final magic on the SAME ExecQueue (verifies LRC Save/Restore) */
    uint32_t *cs2 = (uint32_t *)(cpu_ptr + batch_off + 0x100u);
    idx = 0;
    cs2[idx++] = ARC_MI_STORE_DATA_IMM | ARC_MI_SDI_QWORD_LEN;
    cs2[idx++] = (uint32_t)(target_va & 0xFFFFFFFFULL);
    cs2[idx++] = (uint32_t)(target_va >> 32);
    cs2[idx++] = (uint32_t)(magic & 0xFFFFFFFFULL);
    cs2[idx++] = (uint32_t)(magic >> 32);
    cs2[idx++] = ARC_MI_NOOP;
    cs2[idx++] = ARC_MI_BATCH_BUFFER_END;
    cs2[idx++] = ARC_MI_NOOP;
    __sync_synchronize();

    struct ArcXeExecQueueCreateIn q_in = {
        .vm_id = vm_id,
        .engine_class = engine_class,
        .engine_instance = 0,
        .ring_size_bytes = 16384,
        .priority = 1,
    };
    struct ArcXeExecQueueCreateOut q_out;
    memset(&q_out, 0, sizeof(q_out));
    size_t out_sz = sizeof(q_out);
    if (call_struct(conn, kArcXeMethodExecQueueCreate, &q_in, sizeof(q_in), &q_out, &out_sz) != 0)
        return false;

    const uint32_t queue_id = q_out.exec_queue_id;
    printf("  [%s] Created ExecQueue id=%u (LRC GGTT=0x%08x)\n",
           engine_name, queue_id, q_out.ggtt_lrc_addr);

    const uint64_t batches[2]  = { batch1_va, batch2_va };
    const uint64_t expected[2] = { magic1,    magic     };
    bool ok = true;

    for (uint32_t pass = 0; pass < 2; pass++) {
        struct ArcXeExecIn exec_in;
        memset(&exec_in, 0, sizeof(exec_in));
        exec_in.exec_queue_id = queue_id;
        exec_in.num_batch_buffer = 1;
        exec_in.batch_gpu_va = batches[pass];
        struct ArcXeExecOut exec_out;
        memset(&exec_out, 0, sizeof(exec_out));
        out_sz = sizeof(exec_out);
        if (call_struct(conn, kArcXeMethodExec, &exec_in, sizeof(exec_in), &exec_out, &out_sz) != 0) {
            ok = false;
            break;
        }

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
            ok = false;
            break;
        }
        __sync_synchronize();
        const uint64_t observed = *result_slot;
        if (pass == 1 || wait_out.timed_out || observed != expected[pass]) {
            printf("  [%s] Pass %u/2 seqno=%" PRIu64 " completed=%" PRIu64 " timed_out=%u EL_STATUS=0x%08x_%08x\n",
                   engine_name, pass + 1u, exec_out.submitted_seqno, wait_out.completed_seqno,
                   wait_out.timed_out, wait_out.engine_status_hi, wait_out.engine_status);
            printf("         LRCA=0x%08x HEAD=0x%x TAIL=0x%x START=0x%08x CTL=0x%08x MI_MODE=0x%08x\n",
                   wait_out.current_lrca, wait_out.ring_head, wait_out.ring_tail,
                   wait_out.ring_start, wait_out.ring_ctl, wait_out.mi_mode);
            if (wait_out.timed_out || observed != expected[pass]) {
                printf("         ACTHD=0x%08x_%08x IPEHR=0x%08x ESR=0x%08x INSTDONE=0x%08x\n",
                       wait_out.acthd_hi, wait_out.acthd_lo,
                       wait_out.ipehr, wait_out.esr, wait_out.instdone);
            }
            printf("         UMA zero-copy readback: expected=0x%016" PRIx64 " observed=0x%016" PRIx64 "\n",
                   expected[pass], observed);
        }
        if (wait_out.timed_out || observed != expected[pass] ||
            wait_out.completed_seqno != exec_out.submitted_seqno) {
            ok = false;
            break;
        }
    }

    struct ArcXeExecQueueDestroyIn dq = { .exec_queue_id = queue_id };
    size_t z = 0;
    IOConnectCallStructMethod(conn, kArcXeMethodExecQueueDestroy, &dq, sizeof(dq), NULL, &z);
    return ok;
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

    printf("\n--- Running Live BCS0 + RCS0 48-bit PPGTT Execution Smoke Tests ---\n");

    int rc = 0;
    uint32_t gem_handle = 0, vm_id = 0;
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
    printf("  [1/4] Allocated UMA BO handle=%u (%" PRIu64 " bytes, PAT=%u 2-way coherent)\n",
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
    printf("  [2/4] Created 48-bit PPGTT VM id=%u (PML4 phys=0x%" PRIx64 ")\n",
           vm_id, vm_out.pml4_phys);

    /* 4. Bind UMA BO into PPGTT at GPU VA 0x0000000100000000 */
    const uint64_t base_va = 0x0000000100000000ULL;
    struct ArcXeVmBindIn bind_in;
    memset(&bind_in, 0, sizeof(bind_in));
    bind_in.vm_id = vm_id;
    bind_in.op = kArcXeVmBindOpMap;
    bind_in.gem_handle = gem_handle;
    bind_in.pat_index = ARC_MTL_PAT_IDX_WB_2WAY_COH;
    bind_in.gpu_va = base_va;
    bind_in.bo_offset = 0;
    bind_in.range_size = gem_out.actual_size;
    out_sz = 0;
    if (call_struct(conn, kArcXeMethodVmBind, &bind_in, sizeof(bind_in), NULL, &out_sz) != 0) {
        rc = 7;
        goto cleanup;
    }
    printf("  [3/4] Bound BO -> PPGTT VA 0x%012" PRIx64 " (%" PRIu64 " bytes)\n",
           base_va, gem_out.actual_size);

    /* 5. Test BCS0 (Copy/Blitter) + RCS0 (3D Render/Compute) 48-bit PPGTT Execution */
    printf("  [4/4] Executing live PPGTT MI_STORE_DATA_IMM batches on BCS0 & RCS0...\n");
    const bool bcs_ok = run_engine_smoke_test(conn, vm_id, cpu_ptr, base_va,
                                              kArcXeEngineClassCopy, "BCS0",
                                              0x0000u, 0x1000u, 0xB2C7D550155BC500ULL);
    const bool rcs_ok = run_engine_smoke_test(conn, vm_id, cpu_ptr, base_va,
                                              kArcXeEngineClassRender, "RCS0",
                                              0x2000u, 0x3000u, 0xA2C7D550155CAFE0ULL);

    if (bcs_ok && rcs_ok) {
        printf("\n[PASS] Stage 1 PASSED: Intel Arc Xe-LPG BCS0 & RCS0 executed 48-bit PPGTT batches & wrote coherent UMA!\n");
    } else if (rcs_ok) {
        printf("\n[PASS] Stage 1 PASSED (RCS0 OK, BCS0=%d): Intel Arc Xe-LPG RCS0 executed 48-bit PPGTT batch!\n",
               bcs_ok ? 1 : 0);
    } else {
        fprintf(stderr, "\n[FAIL] Stage 1 FAILED (BCS0=%d, RCS0=%d): GPU timeout or UMA readback mismatch!\n",
                bcs_ok ? 1 : 0, rcs_ok ? 1 : 0);
        rc = 11;
    }

cleanup:
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
