/*
 * ArcXeMemory.cpp — GGTT, 48-bit 4-Level PPGTT, and UMA Buffer Object Management for Meteor Lake Xe-LPG
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#include "ArcXeService.h"
#include <libkern/c++/OSNumber.h>

/* ============================================================================
 * 1. Global GTT (GGTT) Allocator & PTE Programming
 * ============================================================================ */

bool ArcXeService::initGgtt()
{
    fGgttLock = IOLockAlloc();
    if (!fGgttLock) return false;

    /* Allocate 4KB physical scratch page so unmapped GGTT entries never fault into random RAM */
    fScratchPageBo = createKernelBo(4096, ARCXE_GEM_FLAG_CPU_WB_COHERENT, ARC_MTL_PAT_IDX_WB_NONCOH, true);
    if (!fScratchPageBo) {
        AXELOG("initGgtt: failed to allocate scratch page");
        return false;
    }

    /* Check if ArcFB or BIOS GOP has a scanout surface above 24 MiB so we never clobber it */
    uint64_t usableStart = ARC_GGTT_WOPCM_RESERVED_BASE;
    for (int p = 0; p < 4; p++) {
        uint32_t conf = rd32(ARC_DISP_TRANSCONF(p));
        uint32_t ctl  = rd32(ARC_DISP_PLANE_CTL_1(p));
        if ((conf & ARC_DISP_TRANS_ENABLE) && (ctl & ARC_PLANE_CTL_ENABLE)) {
            uint32_t surf   = rd32(ARC_DISP_PLANE_SURF_1(p)) & ~0xFFFu;
            uint32_t sizeR  = rd32(ARC_DISP_PLANE_SIZE_1(p));
            uint32_t height = ((sizeR >> 16) & 0xFFFFu) + 1u;
            uint32_t stride = (rd32(ARC_DISP_PLANE_STRIDE_1(p)) & 0x7FFu) * 64u;
            uint64_t end    = ((uint64_t)surf + (uint64_t)height * stride + 0xFFFFFULL) & ~0xFFFFFULL;
            if (end > usableStart && end < 0x20000000ULL) {
                AXELOG("initGgtt: reserving active GOP pipe %c range [0x%x .. 0x%llx)",
                       'A' + p, surf, (unsigned long long)end);
                usableStart = end;
            }
        }
    }

    fGgttUsableStart = usableStart;
    fGgttUsableSize  = (uint64_t)ARC_GUC_GGTT_TOP - fGgttUsableStart;

    fGgttAlloc = IORangeAllocator::withRange((IORangeScalar)fGgttUsableSize, 4096);
    if (!fGgttAlloc) {
        AXELOG("initGgtt: IORangeAllocator creation failed");
        return false;
    }

    AXELOG("GGTT online: usable VA [0x%llx .. 0x%x) (%llu MiB)",
           (unsigned long long)fGgttUsableStart, ARC_GUC_GGTT_TOP,
           (unsigned long long)(fGgttUsableSize >> 20));
    return true;
}

void ArcXeService::teardownGgtt()
{
    for (uint32_t i = 0; i < kMaxScanoutPins; i++) {
        if (fScanoutPins[i].md) {
            freeGgttRange(fScanoutPins[i].ggttOffset, fScanoutPins[i].ggttSize);
            fScanoutPins[i].md->release();
            fScanoutPins[i].md = nullptr;
        }
    }
    if (fScratchPageBo) {
        releaseBo(fScratchPageBo);
        fScratchPageBo = nullptr;
    }
    OSSafeReleaseNULL(fGgttAlloc);
    if (fGgttLock) {
        IOLockFree(fGgttLock);
        fGgttLock = nullptr;
    }
}

bool ArcXeService::allocGgttAndBind(IOMemoryDescriptor *md, uint64_t size, uint16_t patIndex,
                                    uint32_t *outGgttOffset, uint32_t *outGgttSize)
{
    if (!md || !size || !fGgttAlloc || !fGsmPtes) return false;
    const uint32_t alignedSize = (uint32_t)((size + 0xFFFULL) & ~0xFFFULL);
    if (patIndex > 3) patIndex = ARC_MTL_PAT_IDX_WB_1WAY_COH; /* GGTT supports PAT 0..3 */

    IOLockLock(fGgttLock);
    IORangeScalar relOffset = 0;
    if (!fGgttAlloc->allocate((IORangeScalar)alignedSize, &relOffset, 4096)) {
        IOLockUnlock(fGgttLock);
        AXELOG("allocGgttAndBind: out of GGTT space (%u bytes)", alignedSize);
        return false;
    }

    const uint32_t ggttBase = (uint32_t)(fGgttUsableStart + (uint64_t)relOffset);
    uint64_t byteCursor = 0;
    while (byteCursor < alignedSize) {
        IOByteCount segLen = 0;
        addr64_t phys = md->getPhysicalSegment((IOByteCount)byteCursor, &segLen, kIOMemoryMapperNone);
        if (!phys || !segLen) {
            /* Pad trailing page with scratch if descriptor is slightly shorter */
            phys   = fScratchPageBo ? fScratchPageBo->memDesc->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) : 0;
            segLen = 4096;
        }
        uint64_t pageOff = 0;
        while (pageOff < segLen && (byteCursor + pageOff) < alignedSize) {
            const uint32_t pteIdx = (ggttBase + (uint32_t)(byteCursor + pageOff)) >> 12;
            fGsmPtes[pteIdx] = arcEncodeGgttPte(phys + pageOff, patIndex);
            /* Wa_22019338487: Flush GUnit GGTT write queue on MTL via dummy RO write to Media GT GMD_ID */
            if ((++fGgttWriteCount % 63u) == 0) {
                gt1Wr32(ARC_REG_GMD_ID, 0);
            }
            pageOff += 4096;
        }
        byteCursor += pageOff;
    }
    gt1Wr32(ARC_REG_GMD_ID, 0);
    (void)rd32(ARC_REG_GMD_ID); /* Posting read */
    IOLockUnlock(fGgttLock);

    invalidateGlobalTlb();
    *outGgttOffset = ggttBase;
    *outGgttSize   = alignedSize;
    return true;
}

void ArcXeService::freeGgttRange(uint32_t ggttOffset, uint32_t ggttSize)
{
    if (!ggttOffset || !ggttSize || !fGgttAlloc || !fGsmPtes) return;
    if (ggttOffset < fGgttUsableStart) return;

    const uint64_t scratchPhys = fScratchPageBo
        ? fScratchPageBo->memDesc->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone)
        : 0;
    const uint64_t scratchPte = scratchPhys ? arcEncodeGgttPte(scratchPhys, ARC_MTL_PAT_IDX_WB_NONCOH) : 0ULL;

    IOLockLock(fGgttLock);
    const uint32_t numPages = ggttSize >> 12;
    const uint32_t firstIdx = ggttOffset >> 12;
    for (uint32_t i = 0; i < numPages; i++) {
        fGsmPtes[firstIdx + i] = scratchPte;
        if ((++fGgttWriteCount % 63u) == 0)
            gt1Wr32(ARC_REG_GMD_ID, 0);
    }
    gt1Wr32(ARC_REG_GMD_ID, 0);
    fGgttAlloc->deallocate((IORangeScalar)(ggttOffset - fGgttUsableStart), (IORangeScalar)ggttSize);
    IOLockUnlock(fGgttLock);
}

/* ============================================================================
 * 2. UMA Buffer Objects (Kernel Allocated & Zero-Copy Task / IOSurface Imports)
 * ============================================================================ */

ArcBo *ArcXeService::createKernelBo(uint64_t size, uint32_t flags, uint16_t patIndex, bool mapInKernel)
{
    if (!size) return nullptr;
    const uint64_t alignedSize = (size + 0xFFFULL) & ~0xFFFULL;

    /*
     * IMPORTANT XNU RULE:
     *   1. Do NOT pass kIOMemoryKernelUserShared to inTaskWithPhysicalMask(kernel_task, ...):
     *      in XNU, kIOMemoryKernelUserShared sets kInternalFlagAsIfUnmapped, causing
     *      buf->getBytesNoCopy() to return NULL! IOUserClient::clientMemoryForType maps
     *      standard kernel_task IOBufferMemoryDescriptors into owningTask cleanly via map().
     *   2. Do NOT pass kIOMapWriteCombineCache (0x400) in creation options, because 0x400 in
     *      IOMemoryDescriptor creation IOOptionBits is kIOMemoryPageable, which conflicts
     *      with physicalMask! Pass cache flags ONLY to buf->map(mapOpts) / clientMemoryForType.
     */
    IOOptionBits allocOpts = kIODirectionInOut;
    if (alignedSize == 4096)
        allocOpts |= kIOMemoryPhysicallyContiguous;

    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, allocOpts, (mach_vm_size_t)alignedSize, ARC_DMA_PHYS_MASK_46BIT);
    if (!buf) {
        AXELOG("createKernelBo: allocation of %llu bytes failed", (unsigned long long)alignedSize);
        return nullptr;
    }
    if (buf->prepare() != kIOReturnSuccess) {
        buf->release();
        return nullptr;
    }

    ArcBo *bo = IONewZero(ArcBo, 1);
    if (!bo) {
        buf->complete();
        buf->release();
        return nullptr;
    }
    bo->refCount = 1;
    bo->ownedBuf = buf;
    bo->memDesc  = buf;
    bo->prepared = true;
    bo->size     = alignedSize;
    bo->flags    = flags;
    bo->patIndex = (patIndex < ARC_MTL_PAT_NUM_ENTRIES) ? patIndex : ARC_MTL_PAT_IDX_WB_1WAY_COH;

    const IOOptionBits mapOpts = (flags & ARCXE_GEM_FLAG_CPU_WC) ? kIOMapWriteCombineCache : kIOMapDefaultCache;
    if (mapInKernel) {
        bo->kernMap = buf->map(mapOpts);
        if (!bo->kernMap || !bo->kernMap->getVirtualAddress()) {
            AXELOG("createKernelBo: kernel map failed for %llu bytes", (unsigned long long)alignedSize);
            releaseBo(bo);
            return nullptr;
        }
        bzero(reinterpret_cast<void *>(bo->kernMap->getVirtualAddress()), (size_t)alignedSize);
    } else if (void *kva = buf->getBytesNoCopy()) {
        bzero(kva, (size_t)alignedSize);
    } else if (IOMemoryMap *tmpMap = buf->map(kIOMapDefaultCache)) {
        if (tmpMap->getVirtualAddress())
            bzero(reinterpret_cast<void *>(tmpMap->getVirtualAddress()), (size_t)alignedSize);
        tmpMap->release();
    }

    if (flags & ARCXE_GEM_FLAG_SCANOUT) {
        uint16_t ggttPat = (bo->patIndex <= 3) ? bo->patIndex : ARC_MTL_PAT_IDX_WB_1WAY_COH;
        if (!allocGgttAndBind(bo->memDesc, bo->size, ggttPat, &bo->ggttOffset, &bo->ggttSize)) {
            releaseBo(bo);
            return nullptr;
        }
    }
    return bo;
}

ArcBo *ArcXeService::importTaskMemoryBo(task_t task, uint64_t userAddr, uint64_t size,
                                        uint16_t patIndex, uint32_t flags)
{
    if (!task || !userAddr || !size || (userAddr & 0xFFFULL)) return nullptr;
    const uint64_t alignedSize = (size + 0xFFFULL) & ~0xFFFULL;

    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
        (mach_vm_address_t)userAddr, (mach_vm_size_t)alignedSize,
        kIODirectionInOut, task);
    if (!md) return nullptr;
    if (md->prepare() != kIOReturnSuccess) {
        md->release();
        return nullptr;
    }

    ArcBo *bo = IONewZero(ArcBo, 1);
    if (!bo) {
        md->complete();
        md->release();
        return nullptr;
    }
    bo->refCount = 1;
    bo->ownedBuf = nullptr;
    bo->memDesc  = md;
    bo->prepared = true;
    bo->size     = alignedSize;
    bo->flags    = flags;
    bo->patIndex = (patIndex < ARC_MTL_PAT_NUM_ENTRIES) ? patIndex : ARC_MTL_PAT_IDX_WB_2WAY_COH;

    if (flags & ARCXE_GEM_FLAG_SCANOUT) {
        uint16_t ggttPat = (bo->patIndex <= 3) ? bo->patIndex : ARC_MTL_PAT_IDX_WB_1WAY_COH;
        if (!allocGgttAndBind(bo->memDesc, bo->size, ggttPat, &bo->ggttOffset, &bo->ggttSize)) {
            releaseBo(bo);
            return nullptr;
        }
    }
    return bo;
}

void ArcXeService::retainBo(ArcBo *bo)
{
    if (bo) OSIncrementAtomic(&bo->refCount);
}

void ArcXeService::releaseBo(ArcBo *bo)
{
    if (!bo) return;
    if (OSDecrementAtomic(&bo->refCount) != 1) return;

    if (bo->ggttOffset) {
        freeGgttRange(bo->ggttOffset, bo->ggttSize);
        bo->ggttOffset = 0;
        bo->ggttSize   = 0;
    }
    OSSafeReleaseNULL(bo->kernMap);
    if (bo->memDesc) {
        if (bo->prepared) {
            bo->memDesc->complete();
            bo->prepared = false;
        }
        bo->memDesc->release();
        bo->memDesc = nullptr;
        bo->ownedBuf = nullptr;
    }
    IODelete(bo, ArcBo, 1);
}

/* ============================================================================
 * 3. 48-bit 4-Level PPGTT Virtual Memory Manager (PML4 -> PDP -> PD -> PT)
 * ============================================================================ */

ArcPtPage *ArcXeService::allocPtPage()
{
    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        4096, ARC_DMA_PHYS_MASK_46BIT);
    if (!buf) return nullptr;
    if (buf->prepare() != kIOReturnSuccess) {
        buf->release();
        return nullptr;
    }

    ArcPtPage *pt = IONewZero(ArcPtPage, 1);
    if (!pt) {
        buf->complete();
        buf->release();
        return nullptr;
    }
    pt->buf      = buf;
    pt->physAddr = buf->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    void *kva    = buf->getBytesNoCopy();
    if (!kva) {
        pt->kernMap = buf->map(kIOMapDefaultCache);
        if (pt->kernMap)
            kva = reinterpret_cast<void *>(pt->kernMap->getVirtualAddress());
    }
    if (!kva || !pt->physAddr) {
        OSSafeReleaseNULL(pt->kernMap);
        buf->complete();
        buf->release();
        IODelete(pt, ArcPtPage, 1);
        return nullptr;
    }
    pt->entries = reinterpret_cast<volatile uint64_t *>(kva);
    bzero(kva, 4096);
    return pt;
}

void ArcXeService::freePtTree(ArcPtPage *node, int level)
{
    if (!node) return;
    if (level > 0) {
        for (int i = 0; i < 512; i++) {
            if (node->children[i]) {
                freePtTree(node->children[i], level - 1);
                node->children[i] = nullptr;
            }
        }
    }
    OSSafeReleaseNULL(node->kernMap);
    if (node->buf) {
        node->buf->complete();
        node->buf->release();
        node->buf = nullptr;
    }
    IODelete(node, ArcPtPage, 1);
}

ArcVm *ArcXeService::createVm()
{
    ArcVm *vm = IONewZero(ArcVm, 1);
    if (!vm) return nullptr;
    vm->lock = IOLockAlloc();
    vm->pml4 = allocPtPage();
    if (!vm->lock || !vm->pml4) {
        destroyVm(vm);
        return nullptr;
    }
    return vm;
}

void ArcXeService::destroyVm(ArcVm *vm)
{
    if (!vm) return;
    if (vm->pml4) {
        freePtTree(vm->pml4, 3);
        vm->pml4 = nullptr;
    }
    if (vm->lock) {
        IOLockFree(vm->lock);
        vm->lock = nullptr;
    }
    IODelete(vm, ArcVm, 1);
}

bool ArcXeService::vmBindBo(ArcVm *vm, uint64_t gpuVa, ArcBo *bo, uint64_t boOffset,
                            uint64_t rangeSize, uint16_t patIndex, bool isUnmap, bool isNull)
{
    if (!vm || !vm->pml4 || !rangeSize) return false;
    /* Canonicalize to 48-bit address space */
    gpuVa &= 0x0000FFFFFFFFF000ULL;
    const uint64_t alignedSize = (rangeSize + 0xFFFULL) & ~0xFFFULL;
    if (!isUnmap && !isNull && (!bo || boOffset + alignedSize > bo->size))
        return false;
    if (patIndex >= ARC_MTL_PAT_NUM_ENTRIES)
        patIndex = ARC_MTL_PAT_IDX_WB_1WAY_COH;

    const uint64_t scratchPhys = fScratchPageBo
        ? fScratchPageBo->memDesc->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone)
        : 0;

    IOLockLock(vm->lock);

    uint64_t done = 0;
    while (done < alignedSize) {
        uint64_t curVa = gpuVa + done;
        uint64_t physPage = scratchPhys;
        uint64_t chunkBytes = 4096;

        if (!isUnmap && !isNull && bo && bo->memDesc) {
            IOByteCount segLen = 0;
            addr64_t segPhys = bo->memDesc->getPhysicalSegment((IOByteCount)(boOffset + done), &segLen, kIOMemoryMapperNone);
            if (!segPhys || !segLen) {
                IOLockUnlock(vm->lock);
                return false;
            }
            physPage   = segPhys;
            chunkBytes = (segLen < (alignedSize - done)) ? (segLen & ~0xFFFULL) : (alignedSize - done);
            if (chunkBytes < 4096) chunkBytes = 4096;
        } else {
            chunkBytes = alignedSize - done;
        }

        for (uint64_t sub = 0; sub < chunkBytes; sub += 4096) {
            const uint64_t va   = curVa + sub;
            const uint32_t idx3 = (uint32_t)((va >> 39) & 0x1FFULL);
            const uint32_t idx2 = (uint32_t)((va >> 30) & 0x1FFULL);
            const uint32_t idx1 = (uint32_t)((va >> 21) & 0x1FFULL);
            const uint32_t idx0 = (uint32_t)((va >> 12) & 0x1FFULL);

            if (isUnmap) {
                ArcPtPage *pdp = vm->pml4->children[idx3];
                ArcPtPage *pd  = pdp ? pdp->children[idx2] : nullptr;
                ArcPtPage *pt  = pd  ? pd->children[idx1]  : nullptr;
                if (pt) {
                    pt->entries[idx0] = 0ULL;
                }
                continue;
            }

            if (!vm->pml4->children[idx3]) {
                ArcPtPage *pdp = allocPtPage();
                if (!pdp) { IOLockUnlock(vm->lock); return false; }
                vm->pml4->children[idx3] = pdp;
                vm->pml4->entries[idx3]  = arcEncodePpgttPde(pdp->physAddr);
            }
            ArcPtPage *pdp = vm->pml4->children[idx3];

            if (!pdp->children[idx2]) {
                ArcPtPage *pd = allocPtPage();
                if (!pd) { IOLockUnlock(vm->lock); return false; }
                pdp->children[idx2] = pd;
                pdp->entries[idx2]  = arcEncodePpgttPde(pd->physAddr);
            }
            ArcPtPage *pd = pdp->children[idx2];

            if (!pd->children[idx1]) {
                ArcPtPage *pt = allocPtPage();
                if (!pt) { IOLockUnlock(vm->lock); return false; }
                pd->children[idx1] = pt;
                pd->entries[idx1]  = arcEncodePpgttPde(pt->physAddr);
            }
            ArcPtPage *pt = pd->children[idx1];

            const uint64_t targetPhys = isNull ? 0ULL : (physPage + sub);
            pt->entries[idx0] = arcEncodePpgttPte(targetPhys, patIndex, /*readWrite=*/true, isNull);
        }

        done += chunkBytes;
    }

    __asm__ __volatile__("mfence" ::: "memory");
    IOLockUnlock(vm->lock);

    invalidateGlobalTlb();
    return true;
}
