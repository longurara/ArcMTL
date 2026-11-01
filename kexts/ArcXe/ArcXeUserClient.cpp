/*
 * ArcXeUserClient.cpp — 17-Method IOUserClient & Zero-Copy UMA Memory Mapping for Mesa ANV & ArcMTLDriver
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#include "ArcXeService.h"
#include <mach/vm_param.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(ArcXeUserClient, IOUserClient)

/* ============================================================================
 * 1. Client Lifecycle & Resource Cleanup
 * ============================================================================ */

bool ArcXeUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties))
        return false;
    fTask = owningTask;
    fClientLock = IOLockAlloc();
    return fClientLock != nullptr;
}

bool ArcXeUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider))
        return false;
    fOwner = OSDynamicCast(ArcXeService, provider);
    return fOwner != nullptr;
}

IOReturn ArcXeUserClient::clientClose()
{
    if (fClientLock && fOwner) {
        IOLockLock(fClientLock);
        for (uint32_t i = 0; i < kArcMaxSyncobjs; i++) {
            fSyncobjs[i] = {};
        }
        for (uint32_t i = 1; i < kArcMaxQueuesPerClient; i++) {
            if (fQueues[i]) {
                fOwner->destroyExecQueue(fQueues[i]);
                fQueues[i] = nullptr;
            }
        }
        for (uint32_t i = 1; i < kArcMaxVmsPerClient; i++) {
            if (fVms[i]) {
                fOwner->destroyVm(fVms[i]);
                fVms[i] = nullptr;
            }
        }
        for (uint32_t i = 1; i < kArcMaxBosPerClient; i++) {
            if (fBos[i]) {
                fOwner->releaseBo(fBos[i]);
                fBos[i] = nullptr;
            }
        }
        IOLockUnlock(fClientLock);
    }

    if (!isInactive())
        terminate();
    return kIOReturnSuccess;
}

void ArcXeUserClient::stop(IOService *provider)
{
    fOwner = nullptr;
    IOUserClient::stop(provider);
}

void ArcXeUserClient::free()
{
    if (fClientLock) {
        IOLockFree(fClientLock);
        fClientLock = nullptr;
    }
    IOUserClient::free();
}

ArcBo *ArcXeUserClient::lookupBoLocked(uint32_t handle) const
{
    if (handle == 0 || handle >= kArcMaxBosPerClient)
        return nullptr;
    return fBos[handle];
}

ArcVm *ArcXeUserClient::lookupVmLocked(uint32_t vmId) const
{
    if (vmId == 0 || vmId >= kArcMaxVmsPerClient)
        return nullptr;
    return fVms[vmId];
}

ArcExecQueue *ArcXeUserClient::lookupQueueLocked(uint32_t queueId) const
{
    if (queueId == 0 || queueId >= kArcMaxQueuesPerClient)
        return nullptr;
    return fQueues[queueId];
}

/* ============================================================================
 * 2. Zero-Copy Userspace UMA Mapping via IOConnectMapMemory64(connect, gem_handle, ...)
 * ============================================================================ */

IOReturn ArcXeUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (!fOwner || !fClientLock || !options || !memory)
        return kIOReturnBadArgument;

    IOLockLock(fClientLock);
    ArcBo *bo = lookupBoLocked(type);
    if (!bo || !bo->memDesc) {
        IOLockUnlock(fClientLock);
        return kIOReturnNotFound;
    }

    bo->memDesc->retain();
    *memory  = bo->memDesc;
    *options = (bo->flags & ARCXE_GEM_FLAG_CPU_WC) ? kIOMapWriteCombineCache : kIOMapDefaultCache;
    IOLockUnlock(fClientLock);
    return kIOReturnSuccess;
}

/* ============================================================================
 * 3. External Method Dispatcher (Selectors 0..16)
 * ============================================================================ */

IOReturn ArcXeUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                         IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;

    if (!fOwner || !fClientLock || !args)
        return kIOReturnNotReady;

    switch (selector) {
    /* 0. kArcXeMethodQueryDevice */
    case kArcXeMethodQueryDevice: {
        if (!args->structureOutput || args->structureOutputSize < sizeof(ArcXeQueryDeviceOut))
            return kIOReturnBadArgument;

        auto *out = static_cast<ArcXeQueryDeviceOut *>(args->structureOutput);
        bzero(out, sizeof(*out));
        out->uapi_version          = ARCXE_UAPI_VERSION;
        out->vendor_id             = fOwner->fVendorId;
        out->device_id             = fOwner->fDeviceId;
        out->subvendor_id          = fOwner->fSubVendorId;
        out->subdevice_id          = fOwner->fSubDeviceId;
        out->revision_id           = fOwner->fRevId;
        out->va_bits               = 48;
        out->max_page_Level        = 4;
        out->graphics_verx100      = fOwner->fGraphicsVerX100;
        out->media_verx100         = fOwner->fMediaVerX100;
        out->gmd_id_gt0            = fOwner->fGmdIdGt0;
        out->gmd_id_gt1            = fOwner->fGmdIdGt1;
        out->crystal_clock_hz      = fOwner->fCrystalClockHz;
        out->gpu_timestamp_freq_hz = fOwner->fTimestampFreqHz;
        out->geom_dss_mask         = fOwner->fGeomDssMask;
        out->compute_dss_mask      = fOwner->fComputeDssMask;
        out->eu_per_dss_mask       = fOwner->fEuPerDssMask;
        out->total_xe_cores        = fOwner->fTotalXeCores;
        out->total_eus             = fOwner->fTotalEus;
        out->submit_backend        = (fOwner->fUseGuc && fOwner->fGucLoaded)
                                   ? kArcXeSubmitGuc : kArcXeSubmitExeclist;
        out->guc_fw_loaded         = fOwner->fGucLoaded ? 1u : 0u;
        out->guc_status_reg        = fOwner->fGucStatus;
        out->bar0_phys             = fOwner->fBar0Phys;
        out->bar2_phys             = fOwner->fBar2Phys;
        out->bar2_size             = fOwner->fBar2Size;
        out->stolen_base_phys      = fOwner->fStolenPhys;
        out->stolen_size           = fOwner->fStolenSize;
        out->ggtt_usable_start     = fOwner->fGgttUsableStart;
        out->ggtt_usable_size      = fOwner->fGgttUsableSize;
        out->sysmem_total_bytes    = max_mem ? max_mem : (16ULL << 30);

        if (fOwner->forceWakeGet()) {
            uint32_t hi1 = fOwner->rd32(ARC_RING_TIMESTAMP_UDW(ARC_RENDER_RING_BASE));
            uint32_t lo  = fOwner->rd32(ARC_RING_TIMESTAMP(ARC_RENDER_RING_BASE));
            uint32_t hi2 = fOwner->rd32(ARC_RING_TIMESTAMP_UDW(ARC_RENDER_RING_BASE));
            if (hi1 != hi2)
                lo = fOwner->rd32(ARC_RING_TIMESTAMP(ARC_RENDER_RING_BASE));
            out->gpu_timestamp_ticks = ((uint64_t)hi2 << 32) | lo;
            fOwner->forceWakePut();
        }

        args->structureOutputSize = sizeof(ArcXeQueryDeviceOut);
        return kIOReturnSuccess;
    }

    /* 1. kArcXeMethodGemCreate */
    case kArcXeMethodGemCreate: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeGemCreateIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeGemCreateOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeGemCreateIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeGemCreateOut *>(args->structureOutput);
        if (in->size == 0)
            return kIOReturnBadArgument;

        uint16_t pat = (in->pat_index < ARC_MTL_PAT_NUM_ENTRIES)
                     ? in->pat_index : ARC_MTL_PAT_IDX_WB_1WAY_COH;
        ArcBo *bo = fOwner->createKernelBo(in->size, in->flags, pat, false);
        if (!bo)
            return kIOReturnNoMemory;

        IOLockLock(fClientLock);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < kArcMaxBosPerClient; i++) {
            if (!fBos[i]) {
                slot = i;
                break;
            }
        }
        if (!slot) {
            IOLockUnlock(fClientLock);
            fOwner->releaseBo(bo);
            return kIOReturnNoResources;
        }
        bo->handle = slot;
        fBos[slot] = bo;
        IOLockUnlock(fClientLock);

        bzero(out, sizeof(*out));
        out->gem_handle  = slot;
        out->actual_size = bo->size;
        out->ggtt_offset = bo->ggttOffset;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 2. kArcXeMethodGemImportIOSurface (delegated in userspace via IOSurfaceGetBaseAddress + ImportUserPtr) */
    case kArcXeMethodGemImportIOSurface:
        return kIOReturnUnsupported;

    /* 3. kArcXeMethodGemImportUserPtr */
    case kArcXeMethodGemImportUserPtr: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeGemImportUserPtrIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeGemImportUserPtrOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeGemImportUserPtrIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeGemImportUserPtrOut *>(args->structureOutput);
        if (!in->user_addr || !in->size)
            return kIOReturnBadArgument;

        uint16_t pat = (in->pat_index < ARC_MTL_PAT_NUM_ENTRIES)
                     ? in->pat_index : ARC_MTL_PAT_IDX_WB_1WAY_COH;
        ArcBo *bo = fOwner->importTaskMemoryBo(fTask, in->user_addr, in->size, pat, in->flags);
        if (!bo)
            return kIOReturnVMError;

        IOLockLock(fClientLock);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < kArcMaxBosPerClient; i++) {
            if (!fBos[i]) {
                slot = i;
                break;
            }
        }
        if (!slot) {
            IOLockUnlock(fClientLock);
            fOwner->releaseBo(bo);
            return kIOReturnNoResources;
        }
        bo->handle = slot;
        fBos[slot] = bo;
        IOLockUnlock(fClientLock);

        bzero(out, sizeof(*out));
        out->gem_handle  = slot;
        out->actual_size = bo->size;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 4. kArcXeMethodGemClose */
    case kArcXeMethodGemClose: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeGemCloseIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeGemCloseIn *>(args->structureInput);

        IOLockLock(fClientLock);
        ArcBo *bo = lookupBoLocked(in->gem_handle);
        if (!bo) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        fBos[in->gem_handle] = nullptr;
        IOLockUnlock(fClientLock);

        fOwner->releaseBo(bo);
        return kIOReturnSuccess;
    }

    /* 5. kArcXeMethodGemMmapInfo */
    case kArcXeMethodGemMmapInfo: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeGemMmapInfoIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeGemMmapInfoOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeGemMmapInfoIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeGemMmapInfoOut *>(args->structureOutput);

        IOLockLock(fClientLock);
        ArcBo *bo = lookupBoLocked(in->gem_handle);
        if (!bo) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        out->size             = bo->size;
        out->fake_mmap_offset = ((uint64_t)in->gem_handle) << 32;
        IOLockUnlock(fClientLock);

        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 6. kArcXeMethodVmCreate */
    case kArcXeMethodVmCreate: {
        if (!args->structureOutput || args->structureOutputSize < sizeof(ArcXeVmCreateOut))
            return kIOReturnBadArgument;

        ArcVm *vm = fOwner->createVm();
        if (!vm)
            return kIOReturnNoMemory;

        IOLockLock(fClientLock);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < kArcMaxVmsPerClient; i++) {
            if (!fVms[i]) {
                slot = i;
                break;
            }
        }
        if (!slot) {
            IOLockUnlock(fClientLock);
            fOwner->destroyVm(vm);
            return kIOReturnNoResources;
        }
        vm->vmId   = slot;
        fVms[slot] = vm;
        IOLockUnlock(fClientLock);

        auto *out = static_cast<ArcXeVmCreateOut *>(args->structureOutput);
        bzero(out, sizeof(*out));
        out->vm_id     = slot;
        out->pml4_phys = vm->pml4->physAddr;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 7. kArcXeMethodVmDestroy */
    case kArcXeMethodVmDestroy: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeVmDestroyIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeVmDestroyIn *>(args->structureInput);

        IOLockLock(fClientLock);
        ArcVm *vm = lookupVmLocked(in->vm_id);
        if (!vm) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        fVms[in->vm_id] = nullptr;
        IOLockUnlock(fClientLock);

        fOwner->destroyVm(vm);
        return kIOReturnSuccess;
    }

    /* 8. kArcXeMethodVmBind */
    case kArcXeMethodVmBind: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeVmBindIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeVmBindIn *>(args->structureInput);
        if (in->range_size == 0)
            return kIOReturnBadArgument;

        uint16_t pat = (in->pat_index < ARC_MTL_PAT_NUM_ENTRIES)
                     ? in->pat_index : ARC_MTL_PAT_IDX_WB_1WAY_COH;

        IOLockLock(fClientLock);
        ArcVm *vm = lookupVmLocked(in->vm_id);
        if (!vm) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }

        ArcBo *bo = nullptr;
        ArcBo *tempUserBo = nullptr;

        if (in->op == kArcXeVmBindOpMap) {
            bo = lookupBoLocked(in->gem_handle);
            if (!bo) {
                IOLockUnlock(fClientLock);
                return kIOReturnNotFound;
            }
            fOwner->retainBo(bo);
        }
        IOLockUnlock(fClientLock);

        if (in->op == kArcXeVmBindOpMapUserPtr) {
            tempUserBo = fOwner->importTaskMemoryBo(fTask, in->user_ptr, in->range_size, pat,
                                                    ARCXE_GEM_FLAG_CPU_WB_COHERENT);
            if (!tempUserBo)
                return kIOReturnVMError;
            bo = tempUserBo;
        }

        const bool isUnmap = (in->op == kArcXeVmBindOpUnmap);
        const bool isNull  = (in->op == kArcXeVmBindOpMapNull);
        const uint64_t boOff = (in->op == kArcXeVmBindOpMapUserPtr) ? 0ULL : in->bo_offset;
        const bool ok = fOwner->vmBindBo(vm, in->gpu_va, bo, boOff, in->range_size, pat, isUnmap, isNull);

        if (tempUserBo) {
            /* Keep userptr pages pinned by leaking neither BO nor descriptor: store in anonymous BO slot if space,
             * or release after bind if backed by already-resident process heap/IOSurface */
            IOLockLock(fClientLock);
            bool saved = false;
            for (uint32_t i = kArcMaxBosPerClient - 1; i >= kArcMaxBosPerClient - 512; i--) {
                if (!fBos[i]) {
                    tempUserBo->handle = i;
                    fBos[i] = tempUserBo;
                    saved = true;
                    break;
                }
            }
            IOLockUnlock(fClientLock);
            if (!saved)
                fOwner->releaseBo(tempUserBo);
        } else if (bo) {
            fOwner->releaseBo(bo);
        }

        if (!ok)
            return kIOReturnVMError;

        /* VM_BIND completes synchronously in CPU page tables + GTCR TLB invalidate, so signal out-syncs immediately */
        if (in->num_syncs > 0) {
            IOLockLock(fClientLock);
            const uint32_t n = (in->num_syncs <= ARCXE_MAX_SYNCS_PER_OP) ? in->num_syncs : ARCXE_MAX_SYNCS_PER_OP;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t h = in->syncs[i].syncobj_handle;
                if ((in->syncs[i].flags & 1u) && h > 0 && h < kArcMaxSyncobjs && fSyncobjs[h].inUse) {
                    fSyncobjs[h].signaled   = true;
                    fSyncobjs[h].boundQueue = nullptr;
                    fSyncobjs[h].waitSeqno  = 0;
                    if (in->syncs[i].timeline_value > fSyncobjs[h].timelineValue)
                        fSyncobjs[h].timelineValue = in->syncs[i].timeline_value;
                }
            }
            IOLockUnlock(fClientLock);
        }
        return kIOReturnSuccess;
    }

    /* 9. kArcXeMethodExecQueueCreate */
    case kArcXeMethodExecQueueCreate: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeExecQueueCreateIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeExecQueueCreateOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeExecQueueCreateIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeExecQueueCreateOut *>(args->structureOutput);

        IOLockLock(fClientLock);
        ArcVm *vm = lookupVmLocked(in->vm_id);
        if (!vm) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        ArcExecQueue *q = fOwner->createExecQueue(vm, in->engine_class, in->engine_instance,
                                                  in->ring_size_bytes ? in->ring_size_bytes : 16384u);
        if (!q) {
            IOLockUnlock(fClientLock);
            return kIOReturnNoResources;
        }
        uint32_t slot = 0;
        for (uint32_t i = 1; i < kArcMaxQueuesPerClient; i++) {
            if (!fQueues[i]) {
                slot = i;
                break;
            }
        }
        if (!slot) {
            IOLockUnlock(fClientLock);
            fOwner->destroyExecQueue(q);
            return kIOReturnNoResources;
        }
        q->queueId    = slot;
        fQueues[slot] = q;
        IOLockUnlock(fClientLock);

        bzero(out, sizeof(*out));
        out->exec_queue_id = slot;
        out->ggtt_lrc_addr = q->pphwspGgttAddr;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 10. kArcXeMethodExecQueueDestroy */
    case kArcXeMethodExecQueueDestroy: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeExecQueueDestroyIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeExecQueueDestroyIn *>(args->structureInput);

        IOLockLock(fClientLock);
        ArcExecQueue *q = lookupQueueLocked(in->exec_queue_id);
        if (!q) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }
        for (uint32_t i = 1; i < kArcMaxSyncobjs; i++) {
            if (fSyncobjs[i].boundQueue == q) {
                fSyncobjs[i].boundQueue = nullptr;
                fSyncobjs[i].signaled   = true;
            }
        }
        fQueues[in->exec_queue_id] = nullptr;
        IOLockUnlock(fClientLock);

        fOwner->destroyExecQueue(q);
        return kIOReturnSuccess;
    }

    /* 11. kArcXeMethodExec */
    case kArcXeMethodExec: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeExecIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeExecOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeExecIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeExecOut *>(args->structureOutput);

        IOLockLock(fClientLock);
        ArcExecQueue *q = lookupQueueLocked(in->exec_queue_id);
        if (!q) {
            IOLockUnlock(fClientLock);
            return kIOReturnNotFound;
        }

        /* Collect any in-fence wait dependencies before dropping client lock */
        struct WaitDep { ArcExecQueue *wq; uint64_t seq; };
        WaitDep deps[ARCXE_MAX_SYNCS_PER_OP] = {};
        uint32_t depCount = 0;
        const uint32_t nSyncs = (in->num_syncs <= ARCXE_MAX_SYNCS_PER_OP) ? in->num_syncs : ARCXE_MAX_SYNCS_PER_OP;
        for (uint32_t i = 0; i < nSyncs; i++) {
            const uint32_t h = in->syncs[i].syncobj_handle;
            if ((in->syncs[i].flags & 1u) == 0 && h > 0 && h < kArcMaxSyncobjs && fSyncobjs[h].inUse) {
                if (!fSyncobjs[h].signaled && fSyncobjs[h].boundQueue && fSyncobjs[h].boundQueue != q) {
                    deps[depCount++] = { fSyncobjs[h].boundQueue, fSyncobjs[h].waitSeqno };
                }
            }
        }
        IOLockUnlock(fClientLock);

        /* Wait for cross-engine input fences if any */
        for (uint32_t i = 0; i < depCount; i++) {
            uint64_t done = 0;
            bool to = false;
            fOwner->waitQueueSeqno(deps[i].wq, deps[i].seq, 2000000000ULL, &done, &to);
        }

        uint64_t seqno = 0;
        if (in->num_batch_buffer > 0) {
            IOReturn rc = fOwner->submitBatch(q, in->batch_gpu_va, &seqno);
            if (rc != kIOReturnSuccess)
                return rc;
        } else {
            seqno = q->lastSubmittedSeqno;
        }

        /* Bind output syncobjs to this queue's fence seqno */
        if (nSyncs > 0) {
            IOLockLock(fClientLock);
            for (uint32_t i = 0; i < nSyncs; i++) {
                const uint32_t h = in->syncs[i].syncobj_handle;
                if ((in->syncs[i].flags & 1u) && h > 0 && h < kArcMaxSyncobjs && fSyncobjs[h].inUse) {
                    if (seqno == 0 || *q->seqnoCpu >= seqno) {
                        fSyncobjs[h].signaled   = true;
                        fSyncobjs[h].boundQueue = nullptr;
                        fSyncobjs[h].waitSeqno  = 0;
                    } else {
                        fSyncobjs[h].signaled   = false;
                        fSyncobjs[h].boundQueue = q;
                        fSyncobjs[h].waitSeqno  = seqno;
                    }
                    if (in->syncs[i].timeline_value > fSyncobjs[h].timelineValue)
                        fSyncobjs[h].timelineValue = in->syncs[i].timeline_value;
                }
            }
            IOLockUnlock(fClientLock);
        }

        out->submitted_seqno = seqno;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 12. kArcXeMethodWaitQueueIdle */
    case kArcXeMethodWaitQueueIdle: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeWaitQueueIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeWaitQueueOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeWaitQueueIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeWaitQueueOut *>(args->structureOutput);

        IOLockLock(fClientLock);
        ArcExecQueue *q = lookupQueueLocked(in->exec_queue_id);
        IOLockUnlock(fClientLock);
        if (!q)
            return kIOReturnNotFound;

        uint64_t completed = 0;
        bool timedOut = false;
        fOwner->waitQueueSeqno(q, in->target_seqno, in->timeout_ns, &completed, &timedOut);

        bzero(out, sizeof(*out));
        out->completed_seqno = completed;
        out->timed_out       = timedOut ? 1u : 0u;
        if (fOwner->forceWakeGet()) {
            out->engine_status = fOwner->rd32(ARC_RING_EXECLIST_STATUS_LO(q->mmioBase));
            fOwner->forceWakePut();
        }
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 13. kArcXeMethodSyncobjCreate */
    case kArcXeMethodSyncobjCreate: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeSyncobjCreateIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeSyncobjCreateOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeSyncobjCreateIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeSyncobjCreateOut *>(args->structureOutput);

        IOLockLock(fClientLock);
        uint32_t slot = 0;
        for (uint32_t i = 1; i < kArcMaxSyncobjs; i++) {
            if (!fSyncobjs[i].inUse) {
                slot = i;
                fSyncobjs[i].inUse         = true;
                fSyncobjs[i].signaled      = (in->signaled != 0);
                fSyncobjs[i].timelineValue = in->signaled ? 1ULL : 0ULL;
                fSyncobjs[i].boundQueue    = nullptr;
                fSyncobjs[i].waitSeqno     = 0;
                break;
            }
        }
        IOLockUnlock(fClientLock);
        if (!slot)
            return kIOReturnNoResources;

        out->handle = slot;
        out->pad0   = 0;
        args->structureOutputSize = sizeof(*out);
        return kIOReturnSuccess;
    }

    /* 14. kArcXeMethodSyncobjDestroy */
    case kArcXeMethodSyncobjDestroy: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeSyncobjDestroyIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeSyncobjDestroyIn *>(args->structureInput);
        if (in->handle == 0 || in->handle >= kArcMaxSyncobjs)
            return kIOReturnBadArgument;

        IOLockLock(fClientLock);
        fSyncobjs[in->handle] = {};
        IOLockUnlock(fClientLock);
        return kIOReturnSuccess;
    }

    /* 15. kArcXeMethodSyncobjWait */
    case kArcXeMethodSyncobjWait: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeSyncobjWaitIn) ||
            !args->structureOutput || args->structureOutputSize < sizeof(ArcXeSyncobjWaitOut))
            return kIOReturnBadArgument;

        const auto *in = static_cast<const ArcXeSyncobjWaitIn *>(args->structureInput);
        auto *out      = static_cast<ArcXeSyncobjWaitOut *>(args->structureOutput);
        bzero(out, sizeof(*out));

        const uint32_t count = (in->count_handles <= ARCXE_MAX_SYNCS_PER_OP)
                             ? in->count_handles : ARCXE_MAX_SYNCS_PER_OP;
        if (count == 0)
            return kIOReturnSuccess;

        const bool waitAll = (in->flags & ARCXE_SYNCOBJ_WAIT_ALL) != 0;
        const uint64_t maxWaitNs = (in->timeout_nsec <= 0)
                                 ? 0ULL
                                 : ((uint64_t)in->timeout_nsec > 5000000000ULL
                                     ? 5000000000ULL : (uint64_t)in->timeout_nsec);

        uint64_t startNs = 0;
        clock_get_uptime(&startNs);
        uint64_t absStartNs = 0;
        absolutetime_to_nanoseconds(startNs, &absStartNs);

        while (true) {
            bool allReady = true;
            bool anyReady = false;
            uint32_t firstIdx = 0;
            ArcExecQueue *pendingQ = nullptr;
            uint64_t pendingSeq = 0;

            IOLockLock(fClientLock);
            for (uint32_t i = 0; i < count; i++) {
                const uint32_t h = in->handles[i];
                bool ready = false;
                if (h == 0 || h >= kArcMaxSyncobjs || !fSyncobjs[h].inUse) {
                    ready = true;
                } else if (fSyncobjs[h].signaled) {
                    ready = true;
                } else if (fSyncobjs[h].boundQueue) {
                    if (*fSyncobjs[h].boundQueue->seqnoCpu >= fSyncobjs[h].waitSeqno) {
                        fSyncobjs[h].signaled   = true;
                        fSyncobjs[h].boundQueue = nullptr;
                        ready = true;
                    } else if (!pendingQ) {
                        pendingQ   = fSyncobjs[h].boundQueue;
                        pendingSeq = fSyncobjs[h].waitSeqno;
                    }
                }

                if (ready && !anyReady) {
                    anyReady = true;
                    firstIdx = i;
                }
                if (!ready)
                    allReady = false;
            }
            IOLockUnlock(fClientLock);

            if ((waitAll && allReady) || (!waitAll && anyReady)) {
                out->first_signaled = firstIdx;
                out->timed_out      = 0;
                args->structureOutputSize = sizeof(*out);
                return kIOReturnSuccess;
            }

            uint64_t curAbs = 0, curNs = 0;
            clock_get_uptime(&curAbs);
            absolutetime_to_nanoseconds(curAbs, &curNs);
            if (curNs - absStartNs >= maxWaitNs) {
                out->timed_out = 1;
                args->structureOutputSize = sizeof(*out);
                return kIOReturnSuccess;
            }

            const uint64_t remNs = maxWaitNs - (curNs - absStartNs);
            const uint64_t sliceNs = (remNs < 5000000ULL) ? remNs : 5000000ULL;
            if (pendingQ) {
                uint64_t done = 0;
                bool to = false;
                fOwner->waitQueueSeqno(pendingQ, pendingSeq, sliceNs, &done, &to);
            } else {
                IOSleep(1);
            }
        }
    }

    /* 16. kArcXeMethodSyncobjSignalReset */
    case kArcXeMethodSyncobjSignalReset: {
        if (!args->structureInput || args->structureInputSize < sizeof(ArcXeSyncobjSignalResetIn))
            return kIOReturnBadArgument;
        const auto *in = static_cast<const ArcXeSyncobjSignalResetIn *>(args->structureInput);
        const uint32_t count = (in->count_handles <= ARCXE_MAX_SYNCS_PER_OP)
                             ? in->count_handles : ARCXE_MAX_SYNCS_PER_OP;

        IOLockLock(fClientLock);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t h = in->handles[i];
            if (h > 0 && h < kArcMaxSyncobjs && fSyncobjs[h].inUse) {
                if (in->is_reset) {
                    fSyncobjs[h].signaled      = false;
                    fSyncobjs[h].timelineValue = 0;
                    fSyncobjs[h].boundQueue    = nullptr;
                    fSyncobjs[h].waitSeqno     = 0;
                } else {
                    fSyncobjs[h].signaled      = true;
                    fSyncobjs[h].boundQueue    = nullptr;
                    fSyncobjs[h].waitSeqno     = 0;
                    if (in->points[i] > fSyncobjs[h].timelineValue)
                        fSyncobjs[h].timelineValue = in->points[i];
                }
            }
        }
        IOLockUnlock(fClientLock);
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}
