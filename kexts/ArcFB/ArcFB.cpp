/*
 * ArcFB.kext — macOS IOFramebuffer for Intel Arc Integrated Graphics (Meteor Lake Display IP v14+)
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 *
 * Features:
 *   1. Safe UEFI GOP Scanout Takeover on Display IP Versions 12-20 (Primary target: 8086:7D55 MTL Display v14).
 *   2. Dual Scanout Physical Resolution: BAR2 GMADR aperture OR direct GGTT PTE -> DSM physical address lookup.
 *   3. Hardware PIPE_FRMCOUNT refresh rate measurement (60 Hz - 240 Hz eDP panels) + High-precision VBL pacing timer.
 *   4. Native macOS Brightness Slider & Hotkeys via South Display MTP PCH PWM (0xC8250..0xC8258).
 *   5. Direct Zero-Copy GGTT PLANE_SURF Page-Flipping + Safe Write-Combined Copy Scanout ABI for ArcAccel.kext.
 *
 * Boot arguments:
 *   -arcoff / -arcfboff : Do not attach (leaves firmware VESA / RTX dGPU untouched).
 *   arcfb=1             : Enable ArcFB takeover on the Intel Arc iGPU.
 */

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <mach/kmod.h>

#include "../../include/arc_regs_mtl.h"
#include "../../include/arc_kern_abi.h"

#define AFBLOG(fmt, ...) do { kprintf("ArcFB: " fmt "\n", ##__VA_ARGS__); IOLog("ArcFB: " fmt "\n", ##__VA_ARGS__); } while (0)

extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(com.arc.ArcFB, "0.1.0", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

namespace {

struct DisplayIdEntry { uint16_t id; uint8_t ver; };
const DisplayIdEntry kDisplayIds[] = {
#include "display_ids.inc"
};

uint8_t lookupDisplayVersion(uint16_t dev)
{
    for (const DisplayIdEntry &e : kDisplayIds) {
        if (e.id == dev) return e.ver;
    }
    return 0;
}

static constexpr uint64_t kArcDependentID = 0x4152435800000001ULL; /* 'ARCX' controller group */
static constexpr SInt32   kArcMinBacklightPercent = 5;

IOPMPowerState kArcFbPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static inline uint32_t planeFormatOf(uint32_t ctl) { return (ctl >> ARC_PLANE_CTL_FMT_SHIFT) & ARC_PLANE_CTL_FMT_MASK; }
static inline uint32_t planeTileOf(uint32_t ctl)   { return (ctl >> ARC_PLANE_CTL_TILE_SHIFT) & ARC_PLANE_CTL_TILE_MASK; }

} /* namespace */

class ArcFB;

class ArcBacklight : public IODisplayParameterHandler {
    OSDeclareDefaultStructors(ArcBacklight)
    ArcFB          *fFB      = nullptr;
    thread_call_t   fApply   = nullptr;
    volatile SInt32 fPending = -1;
    SInt32          fLevel   = 100;
    static void applyFire(thread_call_param_t me, thread_call_param_t);
public:
    static ArcBacklight *withFB(ArcFB *fb, SInt32 level);
    bool setDisplay(IODisplay *display) override;
    bool doIntegerSet(OSDictionary *params, const OSSymbol *paramName, UInt32 value) override;
    bool doDataSet(const OSSymbol *, OSData *) override { return false; }
    bool doUpdate() override { return true; }
    void free() override;
};

class ArcFB : public IOFramebuffer {
    OSDeclareDefaultStructors(ArcFB)

private:
    IOPCIDevice        *fPCI          = nullptr;
    IOMemoryMap        *fMMIO         = nullptr;
    IOMemoryMap        *fSurfMap      = nullptr;
    volatile uint8_t   *fRegs         = nullptr;
    IOPhysicalAddress   fScanoutPhys  = 0;
    IOPhysicalLength    fScanoutBytes = 0;

    uint32_t            fWidth        = 0;
    uint32_t            fHeight       = 0;
    uint32_t            fRowBytes     = 0;
    uint32_t            fBootSurfGgtt = 0;
    uint32_t            fBootStrideReg= 0;
    uint32_t            fBootCtlReg   = 0;
    uint32_t            fActiveSurfGgtt = 0;
    uint32_t            fRefresh16    = 60u << 16;
    uint64_t            fFrameNs      = 16666667ULL;

    uint32_t            fHT = 0, fVT = 0, fHSS = 0, fHSE = 0, fVSS = 0, fVSE = 0;
    uint64_t            fPclk         = 0;
    int                 fPipe         = -1;
    uint8_t             fVer          = 0;
    uint32_t            fPwmPeriod    = 0;
    ArcBacklight       *fBacklight    = nullptr;

    struct IrqSlot {
        IOFBInterruptProc proc   = nullptr;
        OSObject         *target = nullptr;
        void             *ref    = nullptr;
    };
    IrqSlot             fVblIrq, fConnectIrq;
    thread_call_t       fVblTimer     = nullptr;
    uint64_t            fVblNext      = 0;
    uint64_t            fVblCount     = 0;
    uint64_t            fFlipCount    = 0;
    volatile bool       fStopping     = false;

    uint32_t rd32(uint32_t off) const { return *(const volatile uint32_t *)(fRegs + off); }
    void     wr32(uint32_t off, uint32_t v) const { *(volatile uint32_t *)(fRegs + off) = v; }
    uint64_t rdGgttPte(uint32_t ggttAddr) const {
        const uint32_t idx = ggttAddr >> 12;
        return *(const volatile uint64_t *)(fRegs + ARC_BAR0_GGTT_GSM_OFFSET + (uint64_t)idx * 8ULL);
    }

    static void vblTimerFired(thread_call_param_t p0, thread_call_param_t)
    {
        ArcFB *me = static_cast<ArcFB *>(p0);
        if (me->fStopping) return;
        if (me->fVblIrq.proc) {
            me->fVblIrq.proc(me->fVblIrq.target, me->fVblIrq.ref);
            me->fVblCount++;
        }
        uint64_t period = 0;
        nanoseconds_to_absolutetime(me->fFrameNs, &period);
        uint64_t now = mach_absolute_time();
        me->fVblNext = (me->fVblNext && me->fVblNext + period > now) ? (me->fVblNext + period) : (now + period);
        thread_call_enter_delayed(me->fVblTimer, me->fVblNext);
    }

    void armVblTimer()
    {
        fVblTimer = thread_call_allocate(vblTimerFired, this);
        if (fVblTimer) {
            uint64_t deadline = 0;
            clock_interval_to_deadline((uint32_t)(fFrameNs / 1000ULL), kMicrosecondScale, &deadline);
            fVblNext = deadline;
            thread_call_enter_delayed(fVblTimer, deadline);
        }
        AFBLOG("VBL timer %s (period %llu us)", fVblTimer ? "armed" : "FAILED", (unsigned long long)(fFrameNs / 1000ULL));
    }

    bool resolveScanoutPhysical(IOPhysicalAddress bar2Base, IOPhysicalLength bar2Len);
    bool adoptFirmwareMode(IOPhysicalAddress bar2Base, IOPhysicalLength bar2Len);
    void publishBacklight();

public:
    bool setPanelPercent(uint32_t percent);

    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOReturn enableController() override { return kIOReturnSuccess; }
    bool isConsoleDevice() override { return true; }
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
    const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
    IOItemCount getDisplayModeCount() override { return 1; }
    IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = 1; return kIOReturnSuccess; }
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = 1; *depth = 0; return kIOReturnSuccess; }
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override { return (mode == 1 && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported; }
    IOItemCount getConnectionCount() override { return 1; }
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;

    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override
    {
        if (type == kIOFBVBLInterruptType) {
            fVblIrq = { proc, target, ref };
            if (iref) *iref = &fVblIrq;
            return kIOReturnSuccess;
        }
        if (type == kIOFBConnectInterruptType) {
            fConnectIrq = { proc, target, ref };
            if (iref) *iref = &fConnectIrq;
            return kIOReturnSuccess;
        }
        return kIOReturnUnsupported;
    }
    IOReturn unregisterInterrupt(void *) override { return kIOReturnSuccess; }
    IOReturn setInterruptState(void *, UInt32) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
    IOReturn setGammaTable(UInt32, UInt32, UInt32, void *, bool) override { return kIOReturnSuccess; }
    IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }

    IOReturn setPowerState(unsigned long ordinal, IOService *) override
    {
        handleEvent(ordinal ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOPMAckImplied;
    }

    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override
    {
        if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
        if (attribute == kIOPowerAttribute) {
            if (value) {
                IOReturn r = IOFramebuffer::setAttribute(attribute, value);
                handleEvent(kIOFBNotifyDidPowerOn);
                return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
            }
            handleEvent(kIOFBNotifyWillPowerOff);
            IOReturn r = IOFramebuffer::setAttribute(attribute, value);
            return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }

    IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override
    {
        switch (attribute) {
        case kConnectionPower:
        case kConnectionColorModesSupported:
        case kConnectionColorDepthsSupported:
        case kConnectionControllerColorDepth:
        case kConnectionControllerDepthsSupported:
        case kConnectionColorMode:
        case kConnectionDisplayFlags:
        case kConnectionFlags:
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
        }
    }

    IOReturn connectFlags(IOIndex, IODisplayModeID, IOOptionBits *flags) override
    {
        if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
        return kIOReturnSuccess;
    }

    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override
    {
        if (attribute == kIOWindowServerActiveAttribute) {
            if (value) *value = 1;
            return kIOReturnSuccess;
        }
        return IOFramebuffer::getAttribute(attribute, value);
    }

    IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override;
    IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) override;
};

OSDefineMetaClassAndStructors(ArcFB, IOFramebuffer)
OSDefineMetaClassAndStructors(ArcBacklight, IODisplayParameterHandler)

IOService *ArcFB::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return nullptr;

    if (PE_parse_boot_argn("-arcoff", nullptr, 0) || PE_parse_boot_argn("-arcfboff", nullptr, 0)) {
        AFBLOG("disabled by -arcoff / -arcfboff boot-arg");
        return nullptr;
    }

    uint32_t enableGate = 0;
    if (!PE_parse_boot_argn("arcfb", &enableGate, sizeof(enableGate)) || !enableGate) {
        AFBLOG("arcfb=1 not set in boot-args; leaving existing display driver untouched");
        return nullptr;
    }

    const uint16_t ven = pci->configRead16(kIOPCIConfigVendorID);
    const uint16_t dev = pci->configRead16(kIOPCIConfigDeviceID);
    if (ven != ARC_PCI_VENDOR_INTEL) return nullptr;

    fVer = lookupDisplayVersion(dev);
    if (!fVer) {
        AFBLOG("8086:%04x not in Intel Display 12-20 table; declining", dev);
        return nullptr;
    }
    AFBLOG("probing Intel Arc iGPU 8086:%04x (Display IP Version %u)", dev, fVer);
    return IOFramebuffer::probe(provider, score);
}

bool ArcFB::resolveScanoutPhysical(IOPhysicalAddress bar2Base, IOPhysicalLength bar2Len)
{
    const uint64_t needed = (uint64_t)fRowBytes * (uint64_t)fHeight;
    fScanoutBytes = (IOPhysicalLength)((needed + 0xFFFULL) & ~0xFFFULL);

    /* Path 1: Direct BAR2 GMADR Aperture window (fastest and always linear in CPU physical space) */
    if (bar2Base && bar2Len && ((uint64_t)fBootSurfGgtt + fScanoutBytes <= (uint64_t)bar2Len)) {
        fScanoutPhys = bar2Base + fBootSurfGgtt;
        AFBLOG("scanout mapped via BAR2 GMADR: phys 0x%llx (%llu KiB)",
               (unsigned long long)fScanoutPhys, (unsigned long long)(fScanoutBytes >> 10));
        return true;
    }

    /* Path 2: Fallback for platforms/BIOSes placing GOP surface above 256 MiB BAR2 — inspect GGTT PTEs in BAR0+8MB */
    uint64_t pte0 = rdGgttPte(fBootSurfGgtt);
    if (!(pte0 & ARC_PTE_PRESENT)) {
        AFBLOG("GGTT PTE for surface 0x%x is not present (0x%llx)", fBootSurfGgtt, (unsigned long long)pte0);
        return false;
    }
    uint64_t basePhys = pte0 & ARC_PTE_ADDR_MASK;
    const uint32_t numPages = (uint32_t)(fScanoutBytes >> 12);
    for (uint32_t pg = 1; pg < numPages; pg++) {
        uint64_t pte = rdGgttPte(fBootSurfGgtt + (pg << 12));
        if (!(pte & ARC_PTE_PRESENT) || ((pte & ARC_PTE_ADDR_MASK) != basePhys + ((uint64_t)pg << 12))) {
            AFBLOG("GOP GGTT pages non-contiguous at page %u/%u; BAR2 required", pg, numPages);
            return false;
        }
    }
    fScanoutPhys = (IOPhysicalAddress)basePhys;
    AFBLOG("scanout resolved directly from contiguous DSM GGTT PTEs: phys 0x%llx (%llu KiB)",
           (unsigned long long)fScanoutPhys, (unsigned long long)(fScanoutBytes >> 10));
    return true;
}

bool ArcFB::adoptFirmwareMode(IOPhysicalAddress bar2Base, IOPhysicalLength bar2Len)
{
    for (int p = 0; p < 4; p++) {
        const uint32_t conf = rd32(ARC_DISP_TRANSCONF(p));
        const uint32_t ctl  = rd32(ARC_DISP_PLANE_CTL_1(p));
        if (!(conf & ARC_DISP_TRANS_ENABLE) || !(ctl & ARC_PLANE_CTL_ENABLE))
            continue;

        const uint32_t fmt  = planeFormatOf(ctl);
        const uint32_t tile = planeTileOf(ctl);
        const uint32_t fmt4 = (ctl >> 24) & 0xFu;
        if (fmt != ARC_PLANE_CTL_FMT_XRGB8888 && fmt != ARC_PLANE_CTL_FMT_XBGR8888 && fmt4 != 4u) {
            AFBLOG("pipe %c: unsupported GOP format %u (ctl=0x%08x); declining safely", 'A' + p, fmt, ctl);
            return false;
        }
        if (tile != ARC_PLANE_CTL_TILE_LINEAR) {
            AFBLOG("pipe %c: GOP plane is tiled (%u); declining safely", 'A' + p, tile);
            return false;
        }

        const uint32_t sizeReg   = rd32(ARC_DISP_PLANE_SIZE_1(p));
        const uint32_t strideReg = rd32(ARC_DISP_PLANE_STRIDE_1(p)) & 0x7FFu;
        fWidth        = (sizeReg & 0xFFFFu) + 1u;
        fHeight       = ((sizeReg >> 16) & 0xFFFFu) + 1u;
        fRowBytes     = strideReg * 64u;
        fBootSurfGgtt = rd32(ARC_DISP_PLANE_SURF_1(p)) & ~0xFFFu;
        fActiveSurfGgtt = fBootSurfGgtt;
        fBootStrideReg  = strideReg;
        fBootCtlReg     = ctl;

        if (fWidth < 320 || fHeight < 200 || fRowBytes < fWidth * 4u) {
            AFBLOG("pipe %c: invalid dimensions %ux%u pitch %u", 'A' + p, fWidth, fHeight, fRowBytes);
            return false;
        }
        if (!resolveScanoutPhysical(bar2Base, bar2Len))
            return false;

        fHT  = (rd32(ARC_DISP_HTOTAL(p)) >> 16) + 1u;
        fVT  = (rd32(ARC_DISP_VTOTAL(p)) >> 16) + 1u;
        fHSS = (rd32(ARC_DISP_HSYNC(p)) & 0xFFFFu) + 1u;
        fHSE = (rd32(ARC_DISP_HSYNC(p)) >> 16) + 1u;
        fVSS = (rd32(ARC_DISP_VSYNC(p)) & 0xFFFFu) + 1u;
        fVSE = (rd32(ARC_DISP_VSYNC(p)) >> 16) + 1u;

        /* Measure true refresh rate from hardware frame counter over 200 ms */
        const uint32_t f0 = rd32(ARC_DISP_PIPE_FRMCOUNT(p));
        const uint64_t t0 = mach_absolute_time();
        IOSleep(200);
        const uint32_t f1 = rd32(ARC_DISP_PIPE_FRMCOUNT(p));
        const uint64_t t1 = mach_absolute_time();
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        const uint32_t frames = f1 - f0;
        if (frames >= 4 && frames <= 80 && ns > 0) {
            fFrameNs   = ns / frames;
            uint64_t mhz = (uint64_t)frames * 1000000000000ULL / ns;
            fRefresh16 = (uint32_t)((mhz << 16) / 1000ULL);
            fPclk      = (uint64_t)fHT * (uint64_t)fVT * mhz / 1000ULL;
        } else {
            fPclk      = (uint64_t)fHT * (uint64_t)fVT * 60ULL;
        }

        fPipe = p;
        AFBLOG("adopted pipe %c: %ux%u pitch %u GGTT 0x%x phys 0x%llx @ %u.%02u Hz",
               'A' + p, fWidth, fHeight, fRowBytes, fBootSurfGgtt,
               (unsigned long long)fScanoutPhys,
               fRefresh16 >> 16, ((fRefresh16 & 0xFFFFu) * 100u) >> 16);
        return true;
    }

    AFBLOG("no active GOP display pipe found; leaving firmware console intact");
    return false;
}

bool ArcFB::start(IOService *provider)
{
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) return false;
    fPCI->setMemoryEnable(true);

    fMMIO = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!fMMIO || fMMIO->getLength() < ARC_BAR0_TOTAL_SIZE) {
        AFBLOG("BAR0 map failed or smaller than 16 MiB");
        OSSafeReleaseNULL(fMMIO);
        return false;
    }
    fRegs = reinterpret_cast<volatile uint8_t *>(fMMIO->getVirtualAddress());

    IOPhysicalAddress bar2Base = 0;
    IOPhysicalLength  bar2Len  = 0;
    if (IODeviceMemory *bar2 = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2)) {
        bar2Base = bar2->getPhysicalAddress();
        bar2Len  = bar2->getLength();
    }

    if (!adoptFirmwareMode(bar2Base, bar2Len)) {
        OSSafeReleaseNULL(fMMIO);
        fRegs = nullptr;
        return false;
    }

    publishBacklight();

    if (!IOFramebuffer::start(provider)) {
        if (fBacklight) {
            fBacklight->detach(this);
            OSSafeReleaseNULL(fBacklight);
        }
        OSSafeReleaseNULL(fMMIO);
        fRegs = nullptr;
        return false;
    }

    if (IODeviceMemory *sm = IODeviceMemory::withRange(fScanoutPhys, fScanoutBytes)) {
        fSurfMap = sm->map(kIOMapWriteCombineCache);
        sm->release();
    }

    /* Bind head 0 to ArcAccel.kext */
    setProperty("IOFBDependentID", kArcDependentID, 64);
    setProperty("IOFBDependentIndex", 0ULL, 32);
    setProperty("ArcFBPipe", (uint64_t)fPipe, 32);
    setProperty("ArcFBDisplayVersion", (uint64_t)fVer, 32);
    setProperty("ArcFBBootGgttOffset", (uint64_t)fBootSurfGgtt, 32);
    setProperty("built-in", kOSBooleanTrue);

    /* Publish GOP reserved GGTT range on IOPCIDevice so ArcXe.kext never overwrites the boot scanout */
    fPCI->setProperty("arc-gop-ggtt-base", (uint64_t)fBootSurfGgtt, 64);
    fPCI->setProperty("arc-gop-ggtt-size", (uint64_t)fScanoutBytes, 64);

    armVblTimer();
    registerPowerDriver(this, kArcFbPowerStates, 2);
    AFBLOG("Intel Arc Display v%u framebuffer live (%ux%u)", fVer, fWidth, fHeight);
    return true;
}

void ArcFB::stop(IOService *provider)
{
    fStopping = true;
    if (fRegs && fPipe >= 0 && fActiveSurfGgtt != fBootSurfGgtt) {
        wr32(ARC_DISP_PLANE_STRIDE_1(fPipe), fBootStrideReg);
        wr32(ARC_DISP_PLANE_SURF_1(fPipe), fBootSurfGgtt);
        fActiveSurfGgtt = fBootSurfGgtt;
    }
    if (fVblTimer) {
        thread_call_cancel(fVblTimer);
        while (!thread_call_free(fVblTimer)) {
            thread_call_cancel(fVblTimer);
            IOSleep(5);
        }
        fVblTimer = nullptr;
    }
    if (fBacklight) {
        fBacklight->terminate(kIOServiceSynchronous);
        fBacklight->detach(this);
        OSSafeReleaseNULL(fBacklight);
    }
    fPwmPeriod = 0;
    OSSafeReleaseNULL(fSurfMap);
    OSSafeReleaseNULL(fMMIO);
    fRegs = nullptr;
    IOFramebuffer::stop(provider);
}

IODeviceMemory *ArcFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    return IODeviceMemory::withRange(fScanoutPhys, fScanoutBytes);
}

IOReturn ArcFB::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (mode != 1 || !info) return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->maxDepthIndex = 0;
    info->nominalWidth  = fWidth;
    info->nominalHeight = fHeight;
    info->refreshRate   = fRefresh16;
    info->flags         = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    return kIOReturnSuccess;
}

IOReturn ArcFB::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi)
{
    if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi)
        return kIOReturnUnsupportedMode;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow       = fRowBytes;
    pi->bytesPerPlane     = 0;
    pi->bitsPerPixel      = 32;
    pi->pixelType         = kIORGBDirectPixels;
    pi->componentCount    = 3;
    pi->bitsPerComponent  = 8;
    pi->componentMasks[0] = 0x00FF0000;
    pi->componentMasks[1] = 0x0000FF00;
    pi->componentMasks[2] = 0x000000FF;
    strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    pi->activeWidth       = fWidth;
    pi->activeHeight      = fHeight;
    return kIOReturnSuccess;
}

IOReturn ArcFB::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    if (mode != 1 || !info) return kIOReturnUnsupportedMode;
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    if (!fPclk || fHT <= fWidth || fVT <= fHeight) return kIOReturnSuccess;

    IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
    d.pixelClock = d.minPixelClock = d.maxPixelClock = fPclk;
    d.horizontalActive          = fWidth;
    d.horizontalBlanking        = fHT - fWidth;
    d.horizontalSyncOffset      = fHSS > fWidth ? fHSS - fWidth : 0;
    d.horizontalSyncPulseWidth  = fHSE > fHSS   ? fHSE - fHSS   : 0;
    d.verticalActive            = fHeight;
    d.verticalBlanking          = fVT - fHeight;
    d.verticalSyncOffset        = fVSS > fHeight ? fVSS - fHeight : 0;
    d.verticalSyncPulseWidth    = fVSE > fVSS    ? fVSE - fVSS    : 0;
    info->flags = kIODetailedTimingValid;
    return kIOReturnSuccess;
}

IOReturn ArcFB::getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType)
{
    if (connectIndex != 0 || !fBacklight) return kIOReturnUnsupported;
    if (senseType)   *senseType   = 0;
    if (primary)     *primary     = 0;
    if (extended)    *extended    = 0;
    if (displayType) *displayType = kGenericLCD;
    return kIOReturnSuccess;
}

IOReturn ArcFB::getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value)
{
    if (idx != 0 || !value) return kIOReturnBadArgument;
    switch (attr) {
    case kConnectionEnable:
    case kConnectionCheckEnable:
        *value = 1;
        return kIOReturnSuccess;
    case kConnectionFlags:
        *value = kIOConnectionBuiltIn;
        return kIOReturnSuccess;
    case kConnectionColorModesSupported:
    case kConnectionColorMode:
        *value = 0x00000001;
        return kIOReturnSuccess;
    case kConnectionColorDepthsSupported:
    case kConnectionControllerColorDepth:
    case kConnectionControllerDepthsSupported:
        *value = 0x00000002;
        return kIOReturnSuccess;
    default:
        return IOFramebuffer::getAttributeForConnection(idx, attr, value);
    }
}

IOReturn ArcFB::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo(ARC_FB_FN_GET_SCANOUT)) {
        ArcScanoutInfoReq *req = static_cast<ArcScanoutInfoReq *>(p1);
        if (!req || req->version != ARC_KERN_ABI_VERSION) return kIOReturnBadArgument;
        if (!fSurfMap) return kIOReturnNotReady;
        req->pipe             = (uint32_t)fPipe;
        req->width            = fWidth;
        req->height           = fHeight;
        req->pitch            = fRowBytes;
        req->ggtt_boot_offset = fBootSurfGgtt;
        req->phys_addr        = (uint64_t)fScanoutPhys;
        req->kva              = reinterpret_cast<void *>(fSurfMap->getVirtualAddress());
        return kIOReturnSuccess;
    }

    if (fn && fn->isEqualTo(ARC_FB_FN_FLIP_SURFACE)) {
        ArcFlipSurfaceReq *req = static_cast<ArcFlipSurfaceReq *>(p1);
        if (!req || req->version != ARC_KERN_ABI_VERSION || fPipe < 0 || !fRegs || fStopping)
            return kIOReturnBadArgument;

        if (req->flags & 1u) {
            /* Restore boot GOP scanout surface */
            wr32(ARC_DISP_PLANE_STRIDE_1(fPipe), fBootStrideReg);
            wr32(ARC_DISP_PLANE_SURF_1(fPipe), fBootSurfGgtt);
            fActiveSurfGgtt = fBootSurfGgtt;
            req->status = 0;
            return kIOReturnSuccess;
        }

        if ((req->ggtt_offset & 0xFFFu) || (req->pitch & 63u) ||
            req->width != fWidth || req->height != fHeight) {
            req->status = -1;
            return kIOReturnBadArgument;
        }

        const uint32_t strideUnits = req->pitch / 64u;
        if (rd32(ARC_DISP_PLANE_STRIDE_1(fPipe)) != strideUnits)
            wr32(ARC_DISP_PLANE_STRIDE_1(fPipe), strideUnits);

        /* Writing PLANE_SURF arms the atomic flip at the next hardware VBlank on Display 14 */
        wr32(ARC_DISP_PLANE_SURF_1(fPipe), req->ggtt_offset);
        fActiveSurfGgtt = req->ggtt_offset;
        fFlipCount++;
        req->status = 0;
        return kIOReturnSuccess;
    }

    return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

/* ---- South Display Backlight PWM Handler ---- */

ArcBacklight *ArcBacklight::withFB(ArcFB *fb, SInt32 level)
{
    ArcBacklight *h = OSTypeAlloc(ArcBacklight);
    if (!h || !h->init()) { OSSafeReleaseNULL(h); return nullptr; }
    h->fFB    = fb;
    h->fApply = thread_call_allocate(applyFire, h);
    h->fLevel = level < 0 ? 0 : (level > 100 ? 100 : level);
    if (!h->fApply) { h->release(); return nullptr; }
    return h;
}

void ArcBacklight::free()
{
    if (fApply) {
        thread_call_cancel(fApply);
        while (!thread_call_free(fApply)) {
            thread_call_cancel(fApply);
            IOSleep(1);
        }
        fApply = nullptr;
    }
    IODisplayParameterHandler::free();
}

bool ArcBacklight::setDisplay(IODisplay *display)
{
    if (!display) return false;
    OSObject *old = display->copyProperty(gIODisplayParametersKey);
    OSDictionary *cur = OSDynamicCast(OSDictionary, old);
    OSDictionary *params = cur ? OSDictionary::withDictionary(cur) : OSDictionary::withCapacity(2);
    OSSafeReleaseNULL(old);
    if (!params) return false;

    IODisplay::addParameter(params, gIODisplayBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayBrightnessKey, fLevel);
    IODisplay::addParameter(params, gIODisplayLinearBrightnessKey, 0, 100);
    IODisplay::setParameter(params, gIODisplayLinearBrightnessKey, fLevel);
    display->setProperty(gIODisplayParametersKey, params);
    params->release();
    return true;
}

bool ArcBacklight::doIntegerSet(OSDictionary *, const OSSymbol *paramName, UInt32 value)
{
    if (paramName != gIODisplayBrightnessKey && paramName != gIODisplayLinearBrightnessKey)
        return false;
    fPending = value > 100 ? 100 : (SInt32)value;
    thread_call_enter(fApply);
    return true;
}

void ArcBacklight::applyFire(thread_call_param_t p, thread_call_param_t)
{
    ArcBacklight *me = static_cast<ArcBacklight *>(p);
    SInt32 v;
    do { v = me->fPending; } while (!OSCompareAndSwap((UInt32)v, (UInt32)-1, (volatile UInt32 *)&me->fPending));
    if (v < 0) return;
    uint32_t pct = (uint32_t)(v < kArcMinBacklightPercent ? kArcMinBacklightPercent : v);
    if (me->fFB->setPanelPercent(pct))
        me->fLevel = v;
}

bool ArcFB::setPanelPercent(uint32_t percent)
{
    if (!fRegs || !fPwmPeriod || fStopping) return false;
    const uint32_t duty = (uint32_t)((uint64_t)fPwmPeriod * percent / 100ULL);
    wr32(ARC_DISP_BLC_PWM_DUTY, duty);
    return rd32(ARC_DISP_BLC_PWM_DUTY) == duty;
}

void ArcFB::publishBacklight()
{
    const uint32_t ctl    = rd32(ARC_DISP_BLC_PWM_CTL);
    const uint32_t period = rd32(ARC_DISP_BLC_PWM_FREQ);
    const uint32_t duty   = rd32(ARC_DISP_BLC_PWM_DUTY);
    if (ctl == ~0u || period == ~0u || !(ctl & ARC_BLC_PWM_ENABLE) || !period || duty > period) {
        AFBLOG("backlight PWM not active in GOP (ctl=0x%08x period=%u); keeping firmware panel brightness", ctl, period);
        return;
    }
    fPwmPeriod = period;
    const SInt32 level = (SInt32)((uint64_t)duty * 100ULL / period);
    fBacklight = ArcBacklight::withFB(this, level);
    if (!fBacklight) { fPwmPeriod = 0; return; }
    fBacklight->setName("backlight");
    if (!fBacklight->attach(this)) {
        OSSafeReleaseNULL(fBacklight);
        fPwmPeriod = 0;
        return;
    }
    fBacklight->registerService(kIOServiceSynchronous);
    AFBLOG("backlight PWM attached (period=%u, current=%d%%)", period, (int)level);
}
