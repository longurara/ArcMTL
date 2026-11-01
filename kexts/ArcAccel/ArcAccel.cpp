/*
 * ArcAccel.kext — IOAcceleratorFamily2 Bridge for Intel Arc Integrated GPUs (Meteor Lake Xe-LPG 8086:7D55)
 *
 * Connects macOS WindowServer / CoreDisplay / IOSurface / Metal.framework to:
 *   - ArcXe.kext (UMA/GGTT pin & 48-bit PPGTT engine KMD)
 *   - ArcFB.kext (Display IP v14 zero-copy PLANE_SURF hardware flips & VBL)
 *   - /Library/GPUBundles/ArcMTLDriver.bundle (Metal 3 -> AIR/SPIR-V -> Mesa ANV Vulkan)
 *
 * Supports two runtime modes controlled via boot-args:
 *   1. Dual-GPU / Secondary Metal 3 Mode (`arc_headless=1`):
 *      Runs alongside NullMoth's RTX 4070 (`NVAccel.kext`), leaving WindowServer on the RTX 4070
 *      while exposing `Intel(R) Arc(TM) Graphics` to `MTLCopyAllDevices()` and all Metal apps.
 *   2. Standalone Primary Desktop Mode (`arcaccel=1`, typically paired with `arcfb=1 -nvoff`):
 *      Registers `ArcFB` display pipes and drives full macOS 15 Sequoia WindowServer desktop
 *      compositing on the Intel Arc iGPU via zero-copy GGTT `IOSurface` page flips.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <sys/sysctl.h>

extern "C" void proc_selfname(char *buf, int size);

#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "tahoe_fwd.h"
#include "arcaccel_stubs.h"

#include "../../include/arc_regs_mtl.h"
#include "../../include/arc_uapi.h"
#include "../../include/arc_kern_abi.h"

#define AALOG(fmt, ...) do { kprintf("ArcAccel: " fmt "\n", ##__VA_ARGS__); IOLog("ArcAccel: " fmt "\n", ##__VA_ARGS__); } while (0)

static bool arcAccelVerbose()
{
    static int v = -1;
    if (v < 0) {
        int x = 0;
        v = PE_parse_boot_argn("-arcaccelverbose", &x, sizeof(x)) ? 1 : 0;
    }
    return v == 1;
}
#define AALOGV(fmt, ...) do { if (arcAccelVerbose()) AALOG(fmt, ##__VA_ARGS__); } while (0)

/* Minimal IOSurface & DisplayPipeTransaction2 kernel KPI declarations (resolved from IOSurface & IOAcceleratorFamily2) */
class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};

class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

static constexpr unsigned kArcMaxFB = 4;

/* Runtime Telemetry & Control Sysctls (all prefixed arcaccel_* so they never clash with debug.nvaccel_*) */
static int      gArcAccelArmed      = 0;
static int      gArcHeadless        = 0;
static int      gArcIopEnabled      = 1;
static int      gArcIopFlip         = 0;
static unsigned gArcHeadsPublished  = 0;
static unsigned gArcHeadsRegistered = 0;
static unsigned gArcHeadsPiped      = 0;
static volatile SInt64 gCntArcFlips = 0;
static volatile SInt64 gCntArcCopies= 0;
static volatile SInt64 gCntArcBlank = 0;
static volatile SInt64 gCntArcEmpty = 0;
static volatile SInt64 gCntArcFail  = 0;
static volatile SInt64 gCntArcResNew= 0;
static bool     gArcSysctlInstalled = false;

static void arcAccelWriteStamp(int index, unsigned int value);
static void arcAccelInstallSysctls();

/* ============================================================================
 * 1. EventMachine, DisplayMachine, GPUTask & MemoryMap Subclasses
 * ============================================================================ */

class ArcEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(ArcEventMachine)
    ARC_TAHOE_FWD(ArcEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        arcAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE {}
    void completeBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE {}
    void writeBarrierElement(vendevtBarrierRec *, int, unsigned int) APPLE_KEXT_OVERRIDE {}
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE {
        AALOG("EM eventTimeout(%d) ignored (Metal uses ArcXe PPGTT queues)", stamp);
        return nullptr;
    }
};
OSDefineMetaClassAndStructors(ArcEventMachine, IOAccelEventMachineFast2)

static void (*gArcEnableHook)(const char *why) = nullptr;

class ArcDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(ArcDisplayMachine)
    ARC_TAHOE_FWD(ArcDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE {
        AALOGV("DM displayModeWillChange");
        return true;
    }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        AALOG("DM displayModeDidChange -> enabling accelerator");
        if (gArcEnableHook)
            gArcEnableHook("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(ArcDisplayMachine, IOAccelLegacyDisplayMachine)

class ArcTask : public IOAccelTask {
    OSDeclareDefaultStructors(ArcTask)
    ARC_TAHOE_FWD(ArcTask)
public:
    IORangeAllocator *fTaskVa = nullptr;

    void free() APPLE_KEXT_OVERRIDE {
        OSSafeReleaseNULL(fTaskVa);
        IOAccelTask::free();
    }
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? const_cast<IOAccelMemoryMap *>(map)->getLength() : 4096ULL;
        if (!fTaskVa) {
            fTaskVa = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fTaskVa)
                fTaskVa->allocateRange(0, 0x100000);
        }
        IORangeScalar addr = 0;
        if (fTaskVa && fTaskVa->allocate((IORangeScalar)((len + 0xFFFULL) & ~0xFFFULL), &addr, 4096))
            return (unsigned long long)addr;
        return 0;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? const_cast<IOAccelMemoryMap *>(map)->getLength() : 0;
        if (fTaskVa && addr && len)
            fTaskVa->deallocate((IORangeScalar)addr, (IORangeScalar)((len + 0xFFFULL) & ~0xFFFULL));
    }
};
OSDefineMetaClassAndStructors(ArcTask, IOAccelTask)

class ArcMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(ArcMemoryMap)
    ARC_TAHOE_FWD(ArcMemoryMap)
public:
    IOAccelMemory *fMem    = nullptr;
    bool           fMapped = false;

    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m, unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o))
            return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        OSSafeReleaseNULL(fMem);
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE { fMapped = true; return true; }
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE { fMapped = false; }
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(ArcMemoryMap, IOAccelMemoryMap)

/* ============================================================================
 * 2. ArcAccel (IOGraphicsAccelerator2) & Sandbox UserClient Carriers
 * ============================================================================ */

static constexpr unsigned kArcCarriers = 32;

class IOAccelArcXeCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelArcXeCarrier)
};
OSDefineMetaClassAndStructors(IOAccelArcXeCarrier, IOService)

class ArcAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(ArcAccel)
    ARC_TAHOE_FWD(ArcAccel)

public:
    static ArcAccel            *gAccel;
    IOPCIDevice                *fPCI                = nullptr;
    IOService                  *fArcXe              = nullptr;
    ArcDisplayMachine          *fDM                 = nullptr;
    IOBufferMemoryDescriptor   *fStamps             = nullptr;
    IORangeAllocator           *fRanges[32]         = {};
    IOAccelArcXeCarrier        *fCarriers[kArcCarriers] = {};
    volatile SInt32             fCarrierNext        = 0;
    ArcDisplayPipe             *fPipes[kArcMaxFB]   = {};
    unsigned                    fPipeCount          = 0;
    IOLock                     *fRegLock            = nullptr;
    IONotifier                 *fFBNote             = nullptr;
    OSObject                   *fSavedMetalPlugin   = nullptr;
    IOService                  *fFB                 = nullptr;
    IOService *volatile         fFBs[kArcMaxFB]     = {};
    unsigned                    fFBRegistered       = 0;
    bool                        fManualFbArm        = false;
    bool                        fFlippedFromBoot[kArcMaxFB] = {};

    const OSSymbol             *fSymGetScanout      = nullptr;
    const OSSymbol             *fSymFlipSurface     = nullptr;
    const OSSymbol             *fSymPinGgtt         = nullptr;
    const OSSymbol             *fSymNewClient       = nullptr;

    /* 17 Required Pure-Virtual Vendor Overrides on IOGraphicsAccelerator2 */
    uint64_t getStampMemory(unsigned int *countOut) APPLE_KEXT_OVERRIDE {
        if (!fStamps) {
            fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(
                kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096);
            if (fStamps) {
                fStamps->prepare();
                if (void *sp = fStamps->getBytesNoCopy())
                    bzero(sp, 16384);
            }
        }
        if (countOut) *countOut = fStamps ? 4096u : 0u;
        return reinterpret_cast<uint64_t>(fStamps);
    }

    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE {
        return OSTypeAlloc(ArcEventMachine);
    }

    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) {
            if (!fRanges[i]) {
                fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
                if (fRanges[i])
                    fRanges[i]->allocateRange(0, 0x100000);
            }
        }
        ArcTask *t = OSTypeAlloc(ArcTask);
        bool ok = t && t->init(this, 1, fRanges);
        AALOGV("%s -> %p init=%d", who, t, ok);
        if (t && !ok) {
            t->release();
            t = nullptr;
        }
        return t;
    }

    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE   { return newGPUTask("createUserGPUTask"); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }

    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) return;
        if (((uintptr_t)p & 0xFFFu) + 0x70u > 0x1000u)
            return;

        const uint32_t maxFbCfg = gArcHeadless ? 0u : kArcMaxFB;
        *(uint64_t *)(p + 0x00) = reinterpret_cast<uint64_t>("IntelArcAccelerator");
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;          /* AccelCaps = 15 (Metal + DisplayPipe) */
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x28) = maxFbCfg;
        *(uint32_t *)(p + 0x30) = 0x40004000u;  /* Max 2D texture 16384 x 16384 */
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 1;            /* Integrated / Unified Memory Architecture (UMA) */
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        AALOG("populateAccelConfig(%p) -> IntelArcAccelerator (UMA=1, maxFB=%u, maxTex=16384)", p, maxFbCfg);
    }

    bool configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE {
        AALOG("configureDevice(%04x:%04x) — owned by ArcXe.kext",
              pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0);
        return true;
    }

    void teardownDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE {}

    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        ArcDisplayPipe *p = OSTypeAlloc(ArcDisplayPipe);
        if (p && fPipeCount < kArcMaxFB)
            fPipes[fPipeCount++] = p;
        AALOG("newDisplayPipe() -> %p (slot %u)", p, fPipeCount ? (fPipeCount - 1u) : 0u);
        return p;
    }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE {
        ArcDisplayMachine *dm = OSTypeAlloc(ArcDisplayMachine);
        fDM = dm;
        return dm;
    }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(ArcGLContext); }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(ArcCLContext); }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE        { return OSTypeAlloc(ArcSurface); }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(Arc2DContext); }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE   { return OSTypeAlloc(ArcVideoContext); }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(ArcSysMemory); }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(ArcVidMemory); }
    OSObject *newResource() APPLE_KEXT_OVERRIDE {
        OSAddAtomic64(1, &gCntArcResNew);
        return OSTypeAlloc(ArcResource);
    }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE      { return OSTypeAlloc(ArcMemoryMap); }

    /* Helper: Locate sibling ArcXeService attached to the same IOPCIDevice */
    IOService *getArcXeService() {
        if (fArcXe && !fArcXe->isInactive())
            return fArcXe;
        OSSafeReleaseNULL(fArcXe);
        if (fPCI) {
            if (OSIterator *it = fPCI->getClientIterator()) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *s = OSDynamicCast(IOService, o);
                    if (s && s->metaCast("ArcXeService") && !s->isInactive()) {
                        s->retain();
                        fArcXe = s;
                        break;
                    }
                }
                it->release();
            }
        }
        if (!fArcXe) {
            if (OSDictionary *m = serviceMatching(ARCXE_IOKIT_SERVICE_NAME)) {
                fArcXe = waitForMatchingService(m, 50ULL * 1000ULL * 1000ULL);
                m->release();
            }
        }
        return fArcXe;
    }

    /*
     * Sandbox-Safe UserClient Bridge:
     * WindowServer (com.apple.WindowServer.sb) and App Sandbox allow opening IOAccel* user clients
     * on an IOGraphicsAccelerator2 / IOAccelerator service, whereas opening ArcXeService directly is
     * denied by Seatbelt. Opens with ARC_XE_ACCEL_OPEN_TYPE ('AR' << 16) instantiate IOAccelArcXeBridge
     * via ArcXeService and attach it to one of our 32 IOAccelArcXeCarrier child services.
     */
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        char caller[32] = {};
        proc_selfname(caller, sizeof(caller));
        static volatile SInt32 sUcLogCnt = 0;
        const SInt32 logSeq = OSIncrementAtomic(&sUcLogCnt) + 1;

        if ((type & 0xFFFF0000u) != ARC_XE_ACCEL_OPEN_TYPE) {
            IOReturn famRc = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (logSeq <= 24 || famRc != kIOReturnSuccess) {
                AALOG("newUserClient(type=0x%x, proc=%s) -> family rc=0x%x uc=%s",
                      (unsigned)type, caller[0] ? caller : "?", (unsigned)famRc,
                      (famRc == kIOReturnSuccess && handler && *handler)
                          ? (*handler)->getMetaClass()->getClassName() : "none");
            }
            return famRc;
        }
        if (!handler)
            return kIOReturnBadArgument;

        IOService *xe = getArcXeService();
        if (!xe) {
            AALOG("newUserClient(0x%x, proc=%s): ArcXeService not found", (unsigned)type, caller);
            return kIOReturnNotAttached;
        }
        if (!fSymNewClient)
            fSymNewClient = OSSymbol::withCStringNoCopy(ARC_XE_FN_NEW_ACCEL_CLIENT);
        if (!fSymNewClient)
            return kIOReturnNoMemory;

        IOUserClient *uc = nullptr;
        IOReturn rc = xe->callPlatformFunction(
            fSymNewClient, false,
            reinterpret_cast<void *>(owningTask),
            reinterpret_cast<void *>(static_cast<uintptr_t>(type & 0xFFFFu)),
            &uc, nullptr);
        if (rc != kIOReturnSuccess || !uc) {
            AALOG("newUserClient(0x%x, proc=%s): ArcXeService refused bridge client (0x%x)",
                  (unsigned)type, caller, (unsigned)rc);
            return (rc != kIOReturnSuccess) ? rc : kIOReturnNoResources;
        }

        IOService *carrier = nullptr;
        const unsigned startIdx = static_cast<unsigned>(OSIncrementAtomic(&fCarrierNext));
        for (unsigned i = 0; i < kArcCarriers && !carrier; i++)
            carrier = fCarriers[(startIdx + i) % kArcCarriers];
        if (!carrier) {
            AALOG("newUserClient(0x%x, proc=%s): no IOAccelArcXeCarrier available", (unsigned)type, caller);
            uc->release();
            return kIOReturnNoResources;
        }
        if (!uc->attach(carrier)) {
            AALOG("newUserClient(0x%x, proc=%s): attach to carrier failed", (unsigned)type, caller);
            uc->release();
            return kIOReturnInternalError;
        }
        if (!uc->start(carrier)) {
            AALOG("newUserClient(0x%x, proc=%s): start on carrier failed", (unsigned)type, caller);
            uc->detach(carrier);
            uc->release();
            return kIOReturnInternalError;
        }

        if (logSeq <= 24) {
            AALOG("newUserClient(0x%x, proc=%s) -> %s attached under ArcAccel carrier",
                  (unsigned)type, caller[0] ? caller : "?", uc->getMetaClass()->getClassName());
        }
        *handler = uc;
        return kIOReturnSuccess;
    }

    IOService *pipeFramebuffer(const ArcDisplayPipe *pipe, unsigned *idxOut) {
        if (!pipe) return nullptr;
        /* 1. Exact pipe object match from newDisplayPipe() creation order (zero offset assumptions) */
        for (unsigned i = 0; i < fPipeCount && i < kArcMaxFB; i++) {
            if (fPipes[i] == pipe && fFBs[i]) {
                if (idxOut) *idxOut = i;
                return fFBs[i];
            }
        }
        /* 2. IOAccelDisplayPipe internal IOFramebuffer* slot (+0x98): pointer-equality only (never dereference raw slot) */
        IOService *fb = *reinterpret_cast<IOService *const volatile *>(
                            reinterpret_cast<const uint8_t *>(pipe) + 0x98);
        if (fb) {
            for (unsigned i = 0; i < kArcMaxFB; i++) {
                if (fFBs[i] == fb) {
                    if (idxOut) *idxOut = i;
                    return fb;
                }
            }
        }
        /* 3. Deterministic single-head fallback */
        if (fFBs[0] && !fFBs[1]) {
            if (idxOut) *idxOut = 0;
            return fFBs[0];
        }
        if (fFB && !fFBs[1]) {
            if (idxOut) *idxOut = 0;
            return fFB;
        }
        return nullptr;
    }

    IOReturn queryFbScanout(IOService *fb, ArcScanoutInfoReq *outReq) {
        if (!fb || !outReq) return kIOReturnBadArgument;
        if (!fSymGetScanout)
            fSymGetScanout = OSSymbol::withCStringNoCopy(ARC_FB_FN_GET_SCANOUT);
        if (!fSymGetScanout) return kIOReturnNoMemory;
        bzero(outReq, sizeof(*outReq));
        outReq->version = ARC_KERN_ABI_VERSION;
        return fb->callPlatformFunction(fSymGetScanout, false, outReq, nullptr, nullptr, nullptr);
    }

    IOReturn flipFbSurface(IOService *fb, ArcFlipSurfaceReq *req) {
        if (!fb || !req) return kIOReturnBadArgument;
        if (!fSymFlipSurface)
            fSymFlipSurface = OSSymbol::withCStringNoCopy(ARC_FB_FN_FLIP_SURFACE);
        if (!fSymFlipSurface) return kIOReturnNoMemory;
        req->version = ARC_KERN_ABI_VERSION;
        return fb->callPlatformFunction(fSymFlipSurface, false, req, nullptr, nullptr, nullptr);
    }

    IOReturn pinSurfaceInGgtt(IOMemoryDescriptor *md, uint64_t byteLen, uint32_t *outGgttOffset) {
        IOService *xe = getArcXeService();
        if (!xe || !md || !byteLen || !outGgttOffset)
            return kIOReturnNotAttached;
        if (!fSymPinGgtt)
            fSymPinGgtt = OSSymbol::withCStringNoCopy(ARC_XE_FN_PIN_SURFACE_GGTT);
        if (!fSymPinGgtt) return kIOReturnNoMemory;

        ArcPinGgttReq req = {};
        req.version    = ARC_KERN_ABI_VERSION;
        req.iomem_desc = md;
        req.size       = byteLen;
        IOReturn rc = xe->callPlatformFunction(fSymPinGgtt, false, &req, nullptr, nullptr, nullptr);
        if (rc == kIOReturnSuccess && req.status == 0) {
            *outGgttOffset = req.out_ggtt_offset;
            return kIOReturnSuccess;
        }
        return rc != kIOReturnSuccess ? rc : kIOReturnNoResources;
    }

    void publishDisplayPipeCapabilities(bool enable) {
        if (enable) {
            if (OSDictionary *caps = OSDictionary::withCapacity(2)) {
                caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
                caps->setObject("TransactionsSupported", kOSBooleanTrue);
                setProperty("IOAccelDisplayPipeCapabilities", caps);
                caps->release();
            }
            gArcIopEnabled = 1;
        } else {
            gArcIopEnabled = 0;
            removeProperty("IOAccelDisplayPipeCapabilities");
        }
    }

    void deliverModeChange() {
        if (!fDM) return;
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
    }

    void recordPublishedFb(IOService *newService) {
        if (!newService) return;
        unsigned idx = 0;
        if (OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex")))
            idx = num->unsigned32BitValue();

        if (idx < kArcMaxFB) {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, reinterpret_cast<void *volatile *>(&fFBs[idx]))) {
                OSBitOrAtomic(1u << idx, &gArcHeadsPublished);
                if (idx == 0) {
                    newService->retain();
                    if (!OSCompareAndSwapPtr(nullptr, newService, reinterpret_cast<void *volatile *>(&fFB)))
                        newService->release();
                }
            } else {
                newService->release();
            }
        }
    }

    unsigned registerPendingLocked(const char *why) {
        if (!fDM) return 0;
        unsigned added = 0;
        for (unsigned i = 0; i < kArcMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) break;
            if (fFBRegistered & (1u << i)) continue;

            const unsigned before = fDM->getFramebufferCount();
            /*
             * Because ArcFB (IOProbeScore=65000) attaches before ArcAccel (IOProbeScore=40000),
             * IOGraphicsAccelerator2::start() normally sweeps ArcFB into fDM automatically
             * (before > i). Recognize family-swept heads directly without relying on private
             * IOAccelLegacyDisplayMachine ivar offsets that differ between Sequoia and Tahoe.
             */
            if (i < before) {
                fFBRegistered |= (1u << i);
                gArcHeadsRegistered = fFBRegistered;
                gArcHeadsPiped |= (1u << i);
                added++;
                AALOG("%s: head %u (%s) already bound by family sweep (dm_count=%u, pipes=%u)",
                      why, i, fb->getName(), before, fPipeCount);
                continue;
            }

            AALOG("%s: calling found_framebuffer(%s head %u) count_before=%u", why, fb->getName(), i, before);
            fDM->found_framebuffer(reinterpret_cast<IOFramebuffer *>(fb));
            const unsigned after = fDM->getFramebufferCount();
            fFBRegistered |= (1u << i);
            gArcHeadsRegistered = fFBRegistered;

            if (after <= before) {
                AALOG("%s: head %u not added by family (%u -> %u)", why, i, before, after);
                break;
            }
            gArcHeadsPiped |= (1u << i);
            added++;
            AALOG("%s: head %u bound via found_framebuffer (%u -> %u, pipes=%u)",
                  why, i, before, after, fPipeCount);
        }
        return added;
    }

    void armDesktopAcceleratorLocked() {
        if (gArcIopEnabled)
            publishDisplayPipeCapabilities(true);
        deliverModeChange();
        gArcAccelArmed = 1;
        setProperty("ArcAccelArmed", kOSBooleanTrue);
        if (fSavedMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fSavedMetalPlugin);
            AALOG("MetalPluginName restored -> WindowServer & apps can now load ArcMTLDriver.bundle");
        }
        if (!getProperty("IOGLBundleName"))
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
    }

    static bool fbPublished(void *target, void *, IOService *newService, IONotifier *) {
        ArcAccel *me = static_cast<ArcAccel *>(target);
        if (!me || !newService) return true;

        me->recordPublishedFb(newService);
        AALOG("ArcFB (%s) published notification (dm_count=%u)",
              newService->getName(), me->fDM ? me->fDM->getFramebufferCount() : 0u);
        if (!me->fManualFbArm) {
            if (me->fRegLock) IOLockLock(me->fRegLock);
            const unsigned added = me->registerPendingLocked("fbPublished");
            if (added > 0 || (!gArcAccelArmed && me->fFBs[0] && me->fDM && me->fDM->getFramebufferCount() > 0))
                me->armDesktopAcceleratorLocked();
            if (me->fRegLock) IOLockUnlock(me->fRegLock);
        } else {
            AALOG("Manual arm mode (-arcmanualfb): run `sudo sysctl -w debug.arcaccelfb=1` to arm display pipe");
        }
        return true;
    }

    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        AALOG("enableAccelerator() called from %s", why);
    }

    /* IOService Lifecycle */
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        uint32_t off = 0;
        if (PE_parse_boot_argn("-arcoff", &off, sizeof(off)))
            return nullptr;

        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
        if (!pci) return nullptr;

        const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID);
        const uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
        if (!arcIsSupportedXeLpgDevice(ven, dev))
            return nullptr;

        return IOGraphicsAccelerator2::probe(provider, score);
    }

    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        uint32_t off = 0;
        if (PE_parse_boot_argn("-arcoff", &off, sizeof(off)))
            return false;

        uint32_t headlessArg = 0;
        uint32_t accelArg    = 0;
        const bool wantHeadless = PE_parse_boot_argn("arc_headless", &headlessArg, sizeof(headlessArg)) && headlessArg != 0;
        const bool wantDesktop  = PE_parse_boot_argn("arcaccel", &accelArg, sizeof(accelArg)) && accelArg != 0;

        if (!wantHeadless && !wantDesktop) {
            AALOG("Neither arc_headless=1 nor arcaccel=1 set in boot-args — staying idle (safe boot guard)");
            return false;
        }

        IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
        if (!pci) return false;

        /* Do not block boot waiting for ArcXe: ArcXe calls fPCI->registerService() once ready! */
        if (pci->getProperty("arcxe-claimed") != kOSBooleanTrue) {
            AALOG("ArcXe has not claimed %04x:%04x yet — deferring until ArcXe re-triggers matching",
                  pci->configRead16(0), pci->configRead16(2));
            return false;
        }

        uint32_t flipArg = 0;
        if (PE_parse_boot_argn("arcflip", &flipArg, sizeof(flipArg)))
            gArcIopFlip = flipArg ? 1 : 0;
        uint32_t noFlip = 0;
        if (PE_parse_boot_argn("-arcnoflip", &noFlip, sizeof(noFlip)))
            gArcIopFlip = 0;
        uint32_t noIop = 0;
        if (PE_parse_boot_argn("-arcnoiop", &noIop, sizeof(noIop)))
            gArcIopEnabled = 0;
        uint32_t manualFb = 0;
        if (PE_parse_boot_argn("-arcmanualfb", &manualFb, sizeof(manualFb)))
            fManualFbArm = true;

        const bool isHeadlessMode = (wantHeadless && !wantDesktop);
        gArcHeadless = isHeadlessMode ? 1 : 0;
        if (isHeadlessMode) {
            /* Keep IOAcceleratorFamily2 completely detached from IONDRVFramebuffer display pipes on 00:02.0 */
            gArcIopEnabled = 0;
        } else if (gArcIopEnabled) {
            /* Publish DisplayPipe capabilities before IOGraphicsAccelerator2::start so family-swept pipes see them */
            publishDisplayPipeCapabilities(true);
        }

        fPCI = pci;
        AALOG("Starting IOGraphicsAccelerator2 on Intel Arc %04x:%04x (mode=%s)",
              pci->configRead16(0), pci->configRead16(2),
              isHeadlessMode ? "Dual-GPU Headless Metal 3" : "Primary Desktop Display+Metal 3");

        if (!IOGraphicsAccelerator2::start(provider)) {
            AALOG("IOGraphicsAccelerator2::start failed");
            return false;
        }

        gAccel         = this;
        gArcEnableHook = &ArcAccel::enableFromHook;
        fRegLock       = IOLockAlloc();

        /* Create 32 IOAccelArcXeCarrier child nodes for sandbox-safe IOAccelArcXeBridge user clients */
        unsigned carriersReady = 0;
        for (unsigned i = 0; i < kArcCarriers; i++) {
            IOAccelArcXeCarrier *c = OSTypeAlloc(IOAccelArcXeCarrier);
            if (!c) break;
            if (!c->init()) { c->release(); break; }
            if (!c->attach(this)) { c->release(); break; }
            if (!c->start(this)) { c->detach(this); c->release(); break; }
            fCarriers[i] = c;
            carriersReady++;
        }
        AALOG("DisplayMachine fb count after family sweep = %u (pipes=%u, carriers=%u/%u)",
              fDM ? fDM->getFramebufferCount() : 0u, fPipeCount, carriersReady, kArcCarriers);

        enableFromHook("start");
        arcAccelInstallSysctls();

        if (isHeadlessMode) {
            /*
             * Optimus Boot-Display Safety Guard:
             * If 00:02.0 (Intel Arc) physically owns the active UEFI GOP boot display (AAPL,boot-display)
             * while ArcFB/arcaccel=1 is NOT active, publishing MetalPluginName in headless mode would make
             * CoreDisplay demand a MetalDevice on 00:02.0 for WindowServer — which headless mode refuses,
             * causing WindowServer to abort at the Apple logo ("Failed to create MetalDevice").
             * Withhold MetalPluginName in that topology so WindowServer boots cleanly on IONDRVFramebuffer!
             */
            bool ownsUnaccelBootDisplay = (pci->getProperty("AAPL,boot-display") == kOSBooleanTrue);
            if (!ownsUnaccelBootDisplay) {
                if (OSIterator *cit = pci->getClientIterator()) {
                    while (OSObject *co = cit->getNextObject()) {
                        IOService *cs = OSDynamicCast(IOService, co);
                        if (cs && cs->metaCast("IOFramebuffer") && !cs->metaCast("ArcFB")) {
                            ownsUnaccelBootDisplay = true;
                            break;
                        }
                    }
                    cit->release();
                }
            }
            setProperty("ArcAccelHeadless", kOSBooleanTrue);
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            if (ownsUnaccelBootDisplay) {
                if (OSObject *mp = getProperty("MetalPluginName")) {
                    mp->retain();
                    fSavedMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                }
                registerService();
                AALOG("headless guard: %04x:%04x owns the boot display without ArcFB — MetalPluginName withheld "
                      "so WindowServer boots cleanly on IONDRVFramebuffer (use arcfb=1 arcaccel=1 for Metal desktop)",
                      pci->configRead16(0), pci->configRead16(2));
                return true;
            }
            registerService();
            AALOG("READY in Dual-GPU Headless Mode (WindowServer on dGPU; Metal apps see Intel Arc)");
            return true;
        }

        /* Primary Desktop Mode (`arcaccel=1`): check for already-attached ArcFB children first */
        if (OSIterator *cit = pci->getClientIterator()) {
            while (OSObject *co = cit->getNextObject()) {
                IOService *cs = OSDynamicCast(IOService, co);
                if (cs && cs->metaCast("ArcFB") && !cs->isInactive())
                    recordPublishedFb(cs);
            }
            cit->release();
        }

        if (OSObject *mp = getProperty("MetalPluginName")) {
            mp->retain();
            fSavedMetalPlugin = mp;
            if (fManualFbArm || !fFBs[0])
                removeProperty("MetalPluginName");
        }

        if (!fManualFbArm && fFBs[0]) {
            if (fRegLock) IOLockLock(fRegLock);
            registerPendingLocked("start");
            armDesktopAcceleratorLocked();
            if (fRegLock) IOLockUnlock(fRegLock);
        }

        registerService();

        if (OSDictionary *fbMatch = serviceMatching("ArcFB")) {
            fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbMatch,
                                              &ArcAccel::fbPublished, this, nullptr, 0);
            fbMatch->release();
        }
        AALOG("READY in Primary Desktop Mode (armed=%d, fbs_published=0x%x, pipes=%u)",
              gArcAccelArmed, gArcHeadsPublished, fPipeCount);
        return true;
    }

    void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (fFBNote) {
            fFBNote->remove();
            fFBNote = nullptr;
        }
        for (unsigned i = 0; i < kArcCarriers; i++) {
            if (fCarriers[i]) {
                fCarriers[i]->stop(this);
                fCarriers[i]->detach(this);
                fCarriers[i]->release();
                fCarriers[i] = nullptr;
            }
        }
        OSSafeReleaseNULL(fArcXe);
        OSSafeReleaseNULL(fSavedMetalPlugin);
        if (fSymGetScanout)  { fSymGetScanout->release();  fSymGetScanout  = nullptr; }
        if (fSymFlipSurface) { fSymFlipSurface->release(); fSymFlipSurface = nullptr; }
        if (fSymPinGgtt)     { fSymPinGgtt->release();     fSymPinGgtt     = nullptr; }
        if (fSymNewClient)   { fSymNewClient->release();   fSymNewClient   = nullptr; }
        for (unsigned i = 0; i < kArcMaxFB; i++) {
            if (fFBs[i]) {
                fFBs[i]->release();
                fFBs[i] = nullptr;
            }
        }
        OSSafeReleaseNULL(fFB);
        for (unsigned i = 0; i < 32; i++)
            OSSafeReleaseNULL(fRanges[i]);
        if (fStamps) {
            fStamps->complete();
            fStamps->release();
            fStamps = nullptr;
        }
        if (fRegLock) {
            IOLockFree(fRegLock);
            fRegLock = nullptr;
        }
        if (gAccel == this)
            gAccel = nullptr;
        IOGraphicsAccelerator2::stop(provider);
    }
};

OSDefineMetaClassAndStructors(ArcAccel, IOGraphicsAccelerator2)
ArcAccel *ArcAccel::gAccel = nullptr;

static void arcAccelWriteStamp(int index, unsigned int value)
{
    ArcAccel *a = ArcAccel::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096)
        return;
    auto *base = static_cast<volatile unsigned int *>(a->fStamps->getBytesNoCopy());
    if (base)
        base[index] = value;
}

/* ============================================================================
 * 3. ArcResource & ArcVidMemory Implementations (UMA System RAM Backed)
 * ============================================================================ */

static ArcVidMemory *arcResVidMem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *reinterpret_cast<OSObject **>(reinterpret_cast<uint8_t *>(r) + 0x88);
    return OSDynamicCast(ArcVidMemory, o);
}

bool ArcResource::addToAperture()
{
    ArcVidMemory *m = arcResVidMem(this);
    return m && m->fPhys && m->fBytes;
}

void ArcResource::removeFromAperture() {}

void *ArcResource::getApertureMemoryDescriptor(unsigned long long *offsetOut)
{
    if (offsetOut) *offsetOut = 0;
    if (fApDesc) return fApDesc;
    ArcVidMemory *m = arcResVidMem(this);
    /* Only contiguous scanout ranges may wrap a physical range; UMA buffers use scattered pages in fUmaBuf */
    if (!m || !m->fScanout || !m->fPhys || !m->fBytes)
        return nullptr;
    fApDesc = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    return fApDesc;
}

void ArcResource::free()
{
    OSSafeReleaseNULL(fApDesc);
    IOAccelResource2::free();
}

uint64_t ArcVidMemory::getPhysicalSegment(unsigned long long offset, unsigned long long *lengthOut)
{
    if (offset >= fBytes) {
        if (lengthOut) *lengthOut = 0;
        return 0;
    }
    if (fUmaBuf) {
        IOByteCount segLen = 0;
        addr64_t pa = fUmaBuf->getPhysicalSegment((IOByteCount)offset, &segLen, kIOMemoryMapperNone);
        if (lengthOut) *lengthOut = segLen;
        return pa;
    }
    if (fPhys) {
        if (lengthOut) *lengthOut = fBytes - offset;
        return fPhys + offset;
    }
    if (lengthOut) *lengthOut = 0;
    return 0;
}

bool ArcVidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) return false;
        *reinterpret_cast<volatile uint64_t *>(reinterpret_cast<uint8_t *>(this) + 0xc8) = fPhys;
        return true;
    }
    if (fUmaBuf) return false;

    const uint64_t want = *reinterpret_cast<const volatile uint64_t *>(
                              reinterpret_cast<const uint8_t *>(this) + 0x40);
    if (!want) return false;

    const uint64_t aligned = (want + 0xFFFULL) & ~0xFFFULL;
    fUmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut, aligned, ARC_DMA_PHYS_MASK_46BIT);
    if (!fUmaBuf) return false;
    if (fUmaBuf->prepare() != kIOReturnSuccess) {
        fUmaBuf->release();
        fUmaBuf = nullptr;
        return false;
    }

    IOByteCount segLen = 0;
    fPhys  = fUmaBuf->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    fKva   = fUmaBuf->getBytesNoCopy();
    fBytes = aligned;
    *reinterpret_cast<volatile uint64_t *>(reinterpret_cast<uint8_t *>(this) + 0xc8) = fPhys;
    return true;
}

void ArcVidMemory::deallocPhysical()
{
    if (fScanout) {
        fPhys    = 0;
        fBytes   = 0;
        fScanout = false;
        *reinterpret_cast<volatile uint64_t *>(reinterpret_cast<uint8_t *>(this) + 0xc8) = 0;
        return;
    }
    if (fUmaBuf) {
        fUmaBuf->complete();
        fUmaBuf->release();
        fUmaBuf = nullptr;
    }
    fKva   = nullptr;
    fPhys  = 0;
    fBytes = 0;
    *reinterpret_cast<volatile uint64_t *>(reinterpret_cast<uint8_t *>(this) + 0xc8) = 0;
}

void ArcVidMemory::free()
{
    deallocPhysical();
    IOAccelVidMemory::free();
}

/* ============================================================================
 * 4. ArcDisplayPipe — Zero-Copy UMA IOSurface -> GGTT PLANE_SURF Page Flips
 * ============================================================================ */

void ArcDisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        ArcIopSrc &e = fIopSrc[i];
        if (e.map) { e.map->release(); e.map = nullptr; }
        if (e.md)  { e.md->complete(); e.md->release(); e.md = nullptr; }
        e.id = 0;
    }
}

void ArcDisplayPipe::free()
{
    iopDropSources();
    IOAccelLegacyDisplayPipe::free();
}

const void *ArcDisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map) {
            return (fIopSrc[i].map->getLength() >= need)
                 ? reinterpret_cast<const void *>(fIopSrc[i].map->getVirtualAddress())
                 : nullptr;
        }
    }
    if (!md || md->getLength() < need)
        return nullptr;

    ArcIopSrc &e = fIopSrc[fIopNext % kIopSrcSlots];
    fIopNext++;
    if (e.map) { e.map->release(); e.map = nullptr; }
    if (e.md)  { e.md->complete(); e.md->release(); e.md = nullptr; }
    e.id = 0;

    if (md->prepare() != kIOReturnSuccess)
        return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) {
        md->complete();
        return nullptr;
    }
    md->retain();
    e.md  = md;
    e.map = m;
    e.id  = id;
    return reinterpret_cast<const void *>(m->getVirtualAddress());
}

IOAccelMemory *ArcDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res)
{
    (void)index;
    (void)res;
    /* Returning nullptr lets IOAcceleratorFamily2 use pure DisplayPipe transactions without BAR aperture pinning */
    return nullptr;
}

IOReturn ArcDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}

bool ArcDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    return gArcIopEnabled != 0;
}

IOReturn ArcDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!gArcIopEnabled) {
        const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
        if (fn <= 4)
            AALOG("iop fail #%lld: gArcIopEnabled == 0", (long long)fn);
        return (IOReturn)0xe00002c7; /* kIOReturnUnsupported */
    }
    if (!txn) {
        OSAddAtomic64(1, &gCntArcFail);
        return kIOReturnBadArgument;
    }

    ArcAccel *a = ArcAccel::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    if (!fb) {
        const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
        if (fn <= 4)
            AALOG("iop fail #%lld: pipeFramebuffer(%p) == nullptr (pipes=%u fbs_pub=0x%x)",
                  (long long)fn, this, a ? a->fPipeCount : 0u, gArcHeadsPublished);
        return kIOReturnNotAttached;
    }

    IOSurface *s = txn->getPlaneIOSurface(0, 0);
    OSObject *ro = txn->getPlaneResource(0, 0);
    if (!s && !ro) {
        OSAddAtomic64(1, &gCntArcEmpty);
        return kIOReturnSuccess;
    }

    ArcScanoutInfoReq scan = {};
    if (a->queryFbScanout(fb, &scan) != kIOReturnSuccess || !scan.width || !scan.height) {
        const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
        if (fn <= 4)
            AALOG("iop fail #%lld: queryFbScanout(%s) failed (%ux%u)",
                  (long long)fn, fb->getName(), scan.width, scan.height);
        return kIOReturnNotReady;
    }

    /*
     * FAST PATH (Zero-Copy UMA Page Flip):
     * On Intel Arc UMA, the IOSurface's IOMemoryDescriptor already holds the coherent rendered pixels in RAM!
     * Pinning its pages into GGTT and programming Display v14 PLANE_SURF_1 flips at VBlank with 0 bytes copied.
     */
    if (s && gArcIopFlip) {
        const uint32_t fmt   = s->getPixelFormat();
        const uint64_t surfW = s->getWidth();
        const uint64_t surfH = s->getHeight();
        const uint64_t surfP = s->getBytesPerRow();
        IOMemoryDescriptor *md = s->getMemoryDescriptor();

        if (fmt == 0x42475241u /* 'BGRA' */ &&
            surfW == scan.width && surfH == scan.height &&
            surfP >= 4ULL * surfW && (surfP % 64ULL) == 0 && md) {
            uint32_t ggttOff = 0;
            if (a->pinSurfaceInGgtt(md, surfH * surfP, &ggttOff) == kIOReturnSuccess && ggttOff != 0) {
                ArcFlipSurfaceReq flip = {};
                flip.flags       = 0;
                flip.ggtt_offset = ggttOff;
                flip.width       = (uint32_t)surfW;
                flip.height      = (uint32_t)surfH;
                flip.pitch       = (uint32_t)surfP;
                flip.tile_mode   = ARC_PLANE_CTL_TILE_LINEAR;
                if (a->flipFbSurface(fb, &flip) == kIOReturnSuccess && flip.status == 0) {
                    if (head < kArcMaxFB)
                        a->fFlippedFromBoot[head] = true;
                    OSAddAtomic64(1, &gCntArcFlips);
                    return kIOReturnSuccess;
                }
            }
        }
    }

    /*
     * FALLBACK PATH (Write-Combined Scanout Copy):
     * Used if -arcnoflip is passed, or pitch is not 64-byte aligned, or source is an ArcResource.
     */
    if (!scan.kva || scan.pitch < 4) {
        const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
        if (fn <= 6)
            AALOG("iop fail #%lld: scanout kva not ready (kva=%p pitch=%u)", (long long)fn, scan.kva, scan.pitch);
        return kIOReturnNotReady;
    }

    const uint8_t *srcBytes = nullptr;
    uint32_t w = 0, h = 0, pitch = 0;
    uint32_t pixFmt = 0x42475241u; /* 'BGRA' */
    uint32_t surfId = 0;

    if (s) {
        pixFmt = s->getPixelFormat();
        surfId = s->getSurfaceID();
        const bool supportedFmt =
            (pixFmt == 0x42475241u) || /* 'BGRA' (8-bit BGRA8888 LE) */
            (pixFmt == 0x6C313072u) || /* 'l10r' (10-bit BGR10A2Unorm / A2R10G10B10 LE) */
            (pixFmt == 0x7231306Bu) || /* 'r10k' (10-bit RGB10A2Unorm / A2B10G10R10 LE) */
            (pixFmt == 0x00000020u) || /* kCVPixelFormatType_32ARGB (big-endian ARGB) */
            (pixFmt == 0x52474241u) || /* 'RGBA' */
            (pixFmt == 0x41424752u);   /* 'ABGR' */
        if (!supportedFmt) {
            const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
            if (fn <= 6) {
                AALOG("iop fail #%lld: unsupported IOSurface#%u fmt=0x%08x (%llux%llu bpr=%llu)",
                      (long long)fn, surfId, pixFmt,
                      s->getWidth(), s->getHeight(), s->getBytesPerRow());
            }
            return kIOReturnUnsupported;
        }
        const uint64_t W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!W || !H || P < 4ULL * W || !md) {
            const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
            if (fn <= 6) {
                AALOG("iop fail #%lld: invalid IOSurface#%u geometry (%llux%llu bpr=%llu md=%p)",
                      (long long)fn, surfId, W, H, P, md);
            }
            return kIOReturnBadArgument;
        }
        srcBytes = static_cast<const uint8_t *>(iopMapSurface(surfId, md, H * P));
        w = (uint32_t)W;
        h = (uint32_t)H;
        pitch = (uint32_t)P;
    } else if (ArcResource *res = OSDynamicCast(ArcResource, ro)) {
        ArcVidMemory *vm = arcResVidMem(res);
        if (vm && vm->fKva) {
            const uint8_t *rr = reinterpret_cast<const uint8_t *>(res);
            w     = *reinterpret_cast<const volatile uint16_t *>(rr + 0xb0);
            h     = *reinterpret_cast<const volatile uint16_t *>(rr + 0xb2);
            pitch = *reinterpret_cast<const volatile uint32_t *>(rr + 0xb8);
            if ((uint64_t)h * pitch <= vm->fBytes)
                srcBytes = static_cast<const uint8_t *>(vm->fKva);
        }
    }

    if (!srcBytes || !w || !h || pitch < 4) {
        const SInt64 fn = OSAddAtomic64(1, &gCntArcFail) + 1;
        if (fn <= 6) {
            AALOG("iop fail #%lld: unmapped source (surf=%u fmt=0x%08x %ux%u pitch=%u)",
                  (long long)fn, surfId, pixFmt, w, h, pitch);
        }
        return kIOReturnBadArgument;
    }

    /* Restore boot GOP scanout surface before writing to scan.kva if previously flipped */
    if (head < kArcMaxFB && a->fFlippedFromBoot[head]) {
        ArcFlipSurfaceReq home = {};
        home.flags = 1u; /* Restore boot GOP surface */
        a->flipFbSurface(fb, &home);
        a->fFlippedFromBoot[head] = false;
    }

    const uint32_t copyRows = (h < scan.height) ? h : scan.height;
    const uint32_t copyCols = (w < scan.width) ? w : scan.width;
    const uint32_t copyBytes = ((pitch < scan.pitch) ? pitch : scan.pitch) & ~3u;
    uint8_t *dstBytes = static_cast<uint8_t *>(scan.kva);

    /* Sample 64 pixels across the incoming surface to distinguish rendered content vs solid black */
    const uint32_t totalPx = copyRows * copyCols;
    const uint32_t stepPx  = (totalPx > 64u) ? (totalPx / 64u) : 1u;
    uint32_t firstSample = 0;
    bool haveFirst = false, anyNonZero = false, anyVaried = false;
    for (uint32_t idx = 0; idx < totalPx; idx += stepPx) {
        const uint32_t sy = idx / (copyCols ? copyCols : 1u);
        const uint32_t sx = idx % (copyCols ? copyCols : 1u);
        const uint32_t px = *reinterpret_cast<const uint32_t *>(srcBytes + (size_t)sy * pitch + (size_t)sx * 4u);
        const uint32_t rgbMask = (pixFmt == 0x6C313072u || pixFmt == 0x7231306Bu) ? 0x3FFFFFFFu
                               : (pixFmt == 0x00000020u) ? 0xFFFFFF00u : 0x00FFFFFFu;
        if ((px & rgbMask) != 0u)
            anyNonZero = true;
        if (!haveFirst) {
            firstSample = px;
            haveFirst   = true;
        } else if (px != firstSample) {
            anyVaried = true;
        }
    }
    if (!anyVaried)
        OSAddAtomic64(1, &gCntArcBlank);

    if (pixFmt == 0x42475241u) {
        for (uint32_t y = 0; y < copyRows; y++) {
            bcopy(srcBytes + (size_t)y * pitch, dstBytes + (size_t)y * scan.pitch, copyBytes);
        }
    } else if (pixFmt == 0x6C313072u) {
        /* 'l10r': 10-bit BGR10A2Unorm (bits [9:0]=B, [19:10]=G, [29:20]=R) -> 8-bit XRGB8888 LE */
        for (uint32_t y = 0; y < copyRows; y++) {
            const uint32_t *sp = reinterpret_cast<const uint32_t *>(srcBytes + (size_t)y * pitch);
            uint32_t *dp       = reinterpret_cast<uint32_t *>(dstBytes + (size_t)y * scan.pitch);
            for (uint32_t x = 0; x < copyCols; x++) {
                const uint32_t v = sp[x];
                const uint32_t b = (v >> 2)  & 0xFFu;
                const uint32_t g = (v >> 12) & 0xFFu;
                const uint32_t r = (v >> 22) & 0xFFu;
                dp[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    } else if (pixFmt == 0x7231306Bu) {
        /* 'r10k': 10-bit RGB10A2Unorm (bits [9:0]=R, [19:10]=G, [29:20]=B) -> 8-bit XRGB8888 LE */
        for (uint32_t y = 0; y < copyRows; y++) {
            const uint32_t *sp = reinterpret_cast<const uint32_t *>(srcBytes + (size_t)y * pitch);
            uint32_t *dp       = reinterpret_cast<uint32_t *>(dstBytes + (size_t)y * scan.pitch);
            for (uint32_t x = 0; x < copyCols; x++) {
                const uint32_t v = sp[x];
                const uint32_t r = (v >> 2)  & 0xFFu;
                const uint32_t g = (v >> 12) & 0xFFu;
                const uint32_t b = (v >> 22) & 0xFFu;
                dp[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    } else if (pixFmt == 0x00000020u) {
        /* 32-bit big-endian ARGB [A, R, G, B] -> 8-bit XRGB8888 LE [B, G, R, 0xFF] */
        for (uint32_t y = 0; y < copyRows; y++) {
            const uint8_t *sp = srcBytes + (size_t)y * pitch;
            uint32_t *dp      = reinterpret_cast<uint32_t *>(dstBytes + (size_t)y * scan.pitch);
            for (uint32_t x = 0; x < copyCols; x++) {
                const uint8_t *p = sp + (size_t)x * 4u;
                dp[x] = 0xFF000000u | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            }
        }
    } else {
        /* 'RGBA' / 'ABGR' [R, G, B, A] -> 8-bit XRGB8888 LE [B, G, R, 0xFF] */
        for (uint32_t y = 0; y < copyRows; y++) {
            const uint8_t *sp = srcBytes + (size_t)y * pitch;
            uint32_t *dp      = reinterpret_cast<uint32_t *>(dstBytes + (size_t)y * scan.pitch);
            for (uint32_t x = 0; x < copyCols; x++) {
                const uint8_t *p = sp + (size_t)x * 4u;
                dp[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
            }
        }
    }

    const SInt64 cn = OSAddAtomic64(1, &gCntArcCopies) + 1;
    if (cn <= 6 || (cn == 60) || (cn == 300)) {
        AALOG("iop copy #%lld: surf=%u fmt=0x%08x %ux%u pitch=%u nonzero=%d varied=%d first=0x%08x",
              (long long)cn, surfId, pixFmt, w, h, pitch,
              anyNonZero ? 1 : 0, anyVaried ? 1 : 0, firstSample);
    }
    return kIOReturnSuccess;
}

/* ============================================================================
 * 5. Debug & Telemetry Sysctls (debug.arcaccel*)
 * ============================================================================ */

static int arcaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gArcAccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0)
        return err;
    ArcAccel *a = ArcAccel::gAccel;
    if (!a) return ENXIO;
    if (v >= 1) {
        if (a->fRegLock) IOLockLock(a->fRegLock);
        a->registerPendingLocked("sysctl");
        a->armDesktopAcceleratorLocked();
        if (a->fRegLock) IOLockUnlock(a->fRegLock);
    }
    return 0;
}

static int arcaccel_metal_sysctl SYSCTL_HANDLER_ARGS
{
    ArcAccel *a = ArcAccel::gAccel;
    int v = (a && a->getProperty("MetalPluginName") != nullptr) ? 1 : 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0)
        return err;
    if (!a) return ENXIO;
    if (v >= 1) {
        if (a->fSavedMetalPlugin) {
            a->setProperty("MetalPluginName", a->fSavedMetalPlugin);
        } else {
            a->setProperty("MetalPluginName", "../../../Library/GPUBundles/ArcMTLDriver");
        }
        AALOG("sysctl debug.arcaccel_metal=1 -> MetalPluginName published for CLI testing");
    } else {
        a->removeProperty("MetalPluginName");
        AALOG("sysctl debug.arcaccel_metal=0 -> MetalPluginName withheld");
    }
    return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, arcaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            nullptr, 0, arcaccelfb_sysctl, "I", "Arm ArcFB display pipe with ArcAccel");
SYSCTL_PROC(_debug, OID_AUTO, arcaccel_metal, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            nullptr, 0, arcaccel_metal_sysctl, "I", "Publish MetalPluginName on ArcAccel without arming display pipes");
SYSCTL_INT(_debug, OID_AUTO, arcaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
           &gArcHeadless, 0, "1 = Dual-GPU headless Metal 3 mode alongside RTX 4070");
SYSCTL_INT(_debug, OID_AUTO, arcaccel_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
           &gArcIopFlip, 0, "1 = Zero-copy GGTT PLANE_SURF page flips; 0 = WC scanout copy");
SYSCTL_QUAD(_debug, OID_AUTO, arcaccel_flips, CTLFLAG_RD | CTLFLAG_LOCKED,
            (SInt64 *)&gCntArcFlips, "Zero-copy UMA IOSurface GGTT page flips completed");
SYSCTL_QUAD(_debug, OID_AUTO, arcaccel_copies, CTLFLAG_RD | CTLFLAG_LOCKED,
            (SInt64 *)&gCntArcCopies, "Fallback WC scanout frame copies completed");
SYSCTL_QUAD(_debug, OID_AUTO, arcaccel_blank, CTLFLAG_RD | CTLFLAG_LOCKED,
            (SInt64 *)&gCntArcBlank, "DisplayPipe frames with uniform/blank pixel sample");
SYSCTL_QUAD(_debug, OID_AUTO, arcaccel_fail, CTLFLAG_RD | CTLFLAG_LOCKED,
            (SInt64 *)&gCntArcFail, "DisplayPipe transactions refused");

static void arcAccelInstallSysctls()
{
    if (gArcSysctlInstalled) return;
    sysctl_register_oid(&sysctl__debug_arcaccelfb);
    sysctl_register_oid(&sysctl__debug_arcaccel_metal);
    sysctl_register_oid(&sysctl__debug_arcaccel_headless);
    sysctl_register_oid(&sysctl__debug_arcaccel_flip);
    sysctl_register_oid(&sysctl__debug_arcaccel_flips);
    sysctl_register_oid(&sysctl__debug_arcaccel_copies);
    sysctl_register_oid(&sysctl__debug_arcaccel_blank);
    sysctl_register_oid(&sysctl__debug_arcaccel_fail);
    gArcSysctlInstalled = true;
}

ARC_TAHOE_FWD_DEFS

extern "C" {
static kern_return_t _arcaccel_start(kmod_info_t *, void *) { return KERN_SUCCESS; }
static kern_return_t _arcaccel_stop(kmod_info_t *, void *)  { return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(com.arc.ArcAccel, "0.1.0", _arcaccel_start, _arcaccel_stop)
}
