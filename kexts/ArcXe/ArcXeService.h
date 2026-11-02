/*
 * ArcXe.kext — XNU Kernel Mode Driver for Intel Arc Integrated GPUs (Meteor Lake Xe-LPG 8086:7D55)
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#pragma once

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <libkern/OSAtomic.h>

#include "../../include/arc_regs_mtl.h"
#include "../../include/arc_uapi.h"
#include "../../include/arc_kern_abi.h"

#define AXELOG(fmt, ...) do { kprintf("ArcXe: " fmt "\n", ##__VA_ARGS__); IOLog("ArcXe: " fmt "\n", ##__VA_ARGS__); } while (0)

#define ARC_MASKED_ENABLE(bits)     (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define ARC_MASKED_DISABLE(bits)    ((uint32_t)(bits) << 16)

static constexpr uint32_t kArcMaxClients        = 128;
static constexpr uint32_t kArcMaxBosPerClient   = 4096;
static constexpr uint32_t kArcMaxUserPtrBinds   = 2048;
static constexpr uint32_t kArcMaxVmsPerClient   = 64;
static constexpr uint32_t kArcMaxQueuesPerClient= 64;
static constexpr uint32_t kArcMaxSyncobjs       = 2048;

/* UMA Buffer Object (Backed either by kernel-allocated pages or pinned task/IOSurface pages) */
struct ArcBo {
    uint32_t                    handle;
    volatile SInt32             refCount;
    IOMemoryDescriptor         *memDesc;
    IOBufferMemoryDescriptor   *ownedBuf;       /* Non-null if allocated by kernel */
    IOMemoryMap                *kernMap;        /* Optional kernel virtual mapping */
    bool                        prepared;
    bool                        readOnly;       /* True when pinned from VM_PROT_READ userptr/IOSurface */
    uint64_t                    size;
    uint32_t                    flags;
    uint16_t                    patIndex;
    uint32_t                    ggttOffset;     /* Non-zero if pinned in GGTT */
    uint32_t                    ggttSize;
};

/* 4-Level 48-bit PPGTT Page Table Node (512 entries x 8 bytes = 4 KiB) */
struct ArcPtPage {
    IOBufferMemoryDescriptor   *buf;
    IOMemoryMap                *kernMap;
    uint64_t                    physAddr;
    volatile uint64_t          *entries;
    ArcPtPage                  *children[512];  /* Null at leaf level 0 (PT) */
    bool                        isNullBacked;   /* True if split from a 2 MiB VM_BIND_FLAG_NULL slot */
};

/* Per-Process 48-bit GPU Virtual Address Space */
struct ArcVm {
    uint32_t                    vmId;
    IOLock                     *lock;
    ArcPtPage                  *pml4;           /* Level 3 root page table */
};

/* Hardware Execution Context (LRC + Ring + PPHWSP + Dedicated Fence Page in GGTT) */
struct ArcExecQueue {
    uint32_t                    queueId;
    uint32_t                    vmId;
    uint16_t                    engineClass;
    uint16_t                    engineInstance;
    uint32_t                    mmioBase;
    uint32_t                    swCtxId;
    bool                        isIdleKernelCtx;
    bool                        hasSavedHwState;
    bool                        isLoadedOnHw;
    bool                        holdsForceWake;
    volatile bool               faulted;        /* Reset discarded work; never signal its fences. */
    ArcBo                      *lrcBo;          /* [0..16K): Ring | [16K..20K): PPHWSP | [20K..+56K): Context Image */
    ArcBo                      *fenceBo;        /* Dedicated 4K GGTT page isolated from HW PPHWSP CSB/status writes */
    uint32_t                    ringGgttAddr;
    uint32_t                    pphwspGgttAddr;
    uint32_t                    ctxImgGgttAddr;
    uint32_t                    ringSize;
    uint32_t                    initRingTail;
    uint32_t                    ringTail;
    volatile uint32_t          *ringCpu;
    volatile uint32_t          *ctxRegsCpu;
    volatile uint64_t          *seqnoCpu;       /* Offset 0x00 inside dedicated fenceBo */
    uint32_t                    seqnoGgttAddr;
    uint32_t                    invalScratchGgtt;
    uint64_t                    lastSubmittedSeqno;
    uint64_t                    lastBatchGpuVa;
    uint32_t                    submitRingStart;
    uint32_t                    submitBatchStart;
    uint32_t                    submitBatchEnd;
    uint32_t                    submitFlushEnd;
    uint64_t                    pml4Phys;
    IOLock                     *submitLock;
};

/* DRM Syncobj Emulation Entry */
struct ArcSyncobj {
    bool                        inUse;
    bool                        signaled;
    uint64_t                    timelineValue;
    ArcExecQueue               *boundQueue;
    uint64_t                    waitSeqno;
};

class ArcXeService : public IOService {
    OSDeclareDefaultStructors(ArcXeService)

public:
    IOPCIDevice                *fPCI                = nullptr;
    IOMemoryMap                *fBar0Map            = nullptr;
    volatile uint8_t           *fMmio               = nullptr;
    volatile uint64_t          *fGsmPtes            = nullptr;
    uint64_t                    fBar0Phys           = 0;
    uint64_t                    fBar2Phys           = 0;
    uint64_t                    fBar2Size           = 0;
    uint64_t                    fStolenPhys         = 0;
    uint64_t                    fStolenSize         = 0;

    uint16_t                    fVendorId           = 0;
    uint16_t                    fDeviceId           = 0;
    uint16_t                    fSubVendorId        = 0;
    uint16_t                    fSubDeviceId        = 0;
    uint8_t                     fRevId              = 0;
    uint32_t                    fGmdIdGt0           = 0;
    uint32_t                    fGmdIdGt1           = 0;
    uint32_t                    fGraphicsVerX100    = 1270;
    uint32_t                    fMediaVerX100       = 1300;
    uint32_t                    fCrystalClockHz     = 38400000;
    uint32_t                    fTimestampFreqHz    = 19200000;
    uint32_t                    fGeomDssMask        = 0xFF;
    uint32_t                    fComputeDssMask     = 0xFF;
    uint32_t                    fEuPerDssMask       = 0xFFFF;
    uint32_t                    fTotalXeCores       = 8;
    uint32_t                    fTotalEus           = 128;

    /* Forcewake Management */
    IOLock                     *fFwLock             = nullptr;
    int32_t                     fFwRefCount         = 0;
    bool                        fEnginesHoldForceWake = false;

    /* GGTT Manager & Global 48-bit PPGTT Scratch/Null Hierarchy */
    IOLock                     *fGgttLock           = nullptr;
    IORangeAllocator           *fGgttAlloc          = nullptr;
    uint64_t                    fGgttUsableStart    = ARC_GGTT_WOPCM_RESERVED_BASE;
    uint64_t                    fGgttUsableSize     = 0;
    uint32_t                    fGgttWriteCount     = 0;
    ArcBo                      *fScratchPageBo      = nullptr;
    ArcBo                      *fNullPageBo         = nullptr;
    ArcPtPage                  *fScratchPt          = nullptr;
    ArcPtPage                  *fScratchPd          = nullptr;
    ArcPtPage                  *fScratchPdp         = nullptr;
    ArcPtPage                  *fNullPt             = nullptr;
    uint64_t                    fScratchLeafPte     = 0;
    uint64_t                    fNullLeafPte        = 0;

    /* Per-Engine Global Hardware Status Pages (HWSP), Idle Contexts & Submission Locks */
    IOLock                     *fEngineLock         = nullptr;
    ArcBo                      *fRcsHwspBo          = nullptr;
    ArcBo                      *fCcsHwspBo          = nullptr;
    ArcBo                      *fBcsHwspBo          = nullptr;
    ArcVm                      *fKernelIdleVm       = nullptr;
    ArcExecQueue               *fRcsIdleQueue       = nullptr;
    ArcExecQueue               *fBcsIdleQueue       = nullptr;
    ArcExecQueue               *fLastRcsQueue       = nullptr;
    ArcExecQueue               *fLastBcsQueue       = nullptr;
    uint32_t                    fNextSwCtxId        = 1;

    /* GuC v70 State */
    bool                        fUseGuc             = false;
    bool                        fGucLoaded          = false;
    uint32_t                    fGucStatus          = 0;
    ArcBo                      *fGucFwBo            = nullptr;
    ArcBo                      *fGucCtbBo           = nullptr;
    volatile bool               fStopping           = false;

    /* Scanout GGTT Pin Cache for ArcAccel Zero-Copy Page Flips */
    struct PinnedScanoutSlot {
        IOMemoryDescriptor     *md;
        IOMemoryMap            *kernMap;
        uint32_t                ggttOffset;
        uint32_t                ggttSize;
        uint64_t                lastUsedTick;
        bool                    prepared;
    };
    static constexpr uint32_t   kMaxScanoutPins     = 12;
    PinnedScanoutSlot           fScanoutPins[kMaxScanoutPins] = {};

    /* MMIO Helpers */
    inline uint32_t rd32(uint32_t reg) const {
        return *(const volatile uint32_t *)(fMmio + reg);
    }
    inline void wr32(uint32_t reg, uint32_t val) const {
        *(volatile uint32_t *)(fMmio + reg) = val;
    }
    inline uint32_t gt1Rd32(uint32_t reg) const {
        return *(const volatile uint32_t *)(fMmio + ARC_GT1_MEDIA_MMIO_BASE + reg);
    }
    inline void gt1Wr32(uint32_t reg, uint32_t val) const {
        *(volatile uint32_t *)(fMmio + ARC_GT1_MEDIA_MMIO_BASE + reg) = val;
    }

    /* Hardware Bring-up & Forcewake */
    bool forceWakeGet();
    void forceWakePut();
    void readTopologyAndClocks();
    void programMtlPatTable();
    void programMtlMocsTable();
    void applyXeLpgWorkarounds();
    void invalidateGlobalTlb();

    /* Memory & Page Table Subsystem (ArcXeMemory.cpp) */
    bool initGgtt();
    void teardownGgtt();
    bool allocGgttAndBind(IOMemoryDescriptor *md, uint64_t size, uint16_t patIndex,
                          uint32_t *outGgttOffset, uint32_t *outGgttSize);
    void freeGgttRange(uint32_t ggttOffset, uint32_t ggttSize);

    ArcBo *createKernelBo(uint64_t size, uint32_t flags, uint16_t patIndex, bool mapInKernel);
    ArcBo *importTaskMemoryBo(task_t task, uint64_t userAddr, uint64_t size, uint16_t patIndex, uint32_t flags);
    void   retainBo(ArcBo *bo);
    void   releaseBo(ArcBo *bo);

    ArcPtPage *allocPtPage(uint64_t fillEntry = 0ULL);
    void       freePtTree(ArcPtPage *node, int level);
    ArcVm     *createVm();
    void       destroyVm(ArcVm *vm);
    bool       vmBindBo(ArcVm *vm, uint64_t gpuVa, ArcBo *bo, uint64_t boOffset,
                        uint64_t rangeSize, uint16_t patIndex, bool isUnmap, bool isNull);

    /* Execution & GuC Subsystem (ArcXeExec.cpp) */
    bool initEngines();
    void teardownEngines();
    void armEngineHw(uint32_t base, ArcBo *hwsp);
    bool resetEngineHw(uint32_t base, ArcBo *hwsp);
    uint32_t engineMmioBase(uint16_t engineClass, uint16_t engineInstance, ArcBo **outHwsp) const;
    ArcExecQueue *createExecQueue(ArcVm *vm, uint16_t engineClass, uint16_t engineInstance, uint32_t ringSize,
                                  bool isIdleKernelCtx = false);
    void          destroyExecQueue(ArcExecQueue *q);
    bool          parkEngineOnIdleContextLocked(uint32_t base, ArcExecQueue *prevQueue);
    IOReturn      submitBatch(ArcExecQueue *q, uint64_t batchGpuVa, uint64_t *outSeqno);
    IOReturn      waitQueueSeqno(ArcExecQueue *q, uint64_t targetSeqno, uint64_t timeoutNs,
                                 uint64_t *outCompleted, bool *outTimedOut,
                                 ArcXeWaitQueueOut *outDiag = nullptr,
                                 bool resetOnTimeout = true,
                                 uint32_t *outSubunitDiag5 = nullptr,
                                 uint32_t *outProgress = nullptr);
    bool          tryLoadGucFirmwareFromDisk(const char *fwPath);
    static void   gucLoaderThreadEntry(void *arg, wait_result_t);

    /* IOService Overrides */
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool       start(IOService *provider) override;
    void       stop(IOService *provider) override;
    IOReturn   newUserClient(task_t owningTask, void *securityID, UInt32 type,
                             OSDictionary *properties, IOUserClient **handler) override;
    IOReturn   callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                    void *param1, void *param2, void *param3, void *param4) override;
};

struct ArcUserPtrBind {
    uint32_t                    vmId;
    uint64_t                    gpuVa;
    uint64_t                    size;
    ArcBo                      *bo;
};

class ArcXeUserClient : public IOUserClient {
    OSDeclareDefaultStructors(ArcXeUserClient)

protected:
    ArcXeService               *fOwner              = nullptr;

private:
    task_t                      fTask               = nullptr;
    IOLock                     *fClientLock         = nullptr;

    ArcBo                      *fBos[kArcMaxBosPerClient]       = {};
    ArcVm                      *fVms[kArcMaxVmsPerClient]       = {};
    ArcExecQueue               *fQueues[kArcMaxQueuesPerClient] = {};
    ArcSyncobj                  fSyncobjs[kArcMaxSyncobjs]      = {};
    ArcUserPtrBind             *fUserPtrBinds                   = nullptr;

    ArcBo        *lookupBoLocked(uint32_t handle) const;
    ArcVm        *lookupVmLocked(uint32_t vmId) const;
    ArcExecQueue *lookupQueueLocked(uint32_t queueId) const;
    void          releaseUserPtrBinds(uint32_t vmId, uint64_t gpuVa, uint64_t rangeSize, bool allInVm);

public:
    void     setOwner(ArcXeService *owner) { fOwner = owner; }
    bool     initWithTask(task_t owningTask, void *securityToken, UInt32 type, OSDictionary *properties) override;
    bool     start(IOService *provider) override;
    IOReturn clientClose() override;
    void     stop(IOService *provider) override;
    void     free() override;

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
};

/*
 * IOAccel*-prefixed user client bridge subclass for WindowServer & macOS Seatbelt sandboxes:
 * Seatbelt iokit-open rules in com.apple.WindowServer.sb and App Sandbox permit opening user
 * clients matching ^IOAccel.* attached under an IOAccelerator / IOGraphicsAccelerator2 service.
 */
class IOAccelArcXeBridge : public ArcXeUserClient {
    OSDeclareDefaultStructors(IOAccelArcXeBridge)
public:
    bool start(IOService *provider) override;
};
