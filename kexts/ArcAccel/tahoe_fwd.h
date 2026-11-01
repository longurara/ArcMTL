/*
 * macOS 26 (Tahoe) vtable forwarders for ArcAccel.kext
 * Adapted for Arc* class hierarchy so ArcAccel.kext and NVAccel.kext never collide in AuxKC.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 */

#pragma once

#ifdef ARC_TAHOE
#define ARC_TAHOE_FWD_ArcResource \
    void pageon(IOAccelEvent* a0, bool a1) override; \
    void pageon(IOAccelEvent* a0, bool a1, unsigned long long a2) override; \
    void pageoff(IOAccelEvent* a0, bool a1, bool* a2) override; \
    uint64_t getMemoryAllocParameter() override;
#define ARC_TAHOE_FWD_ArcSysMemory \
    uint64_t getLength() const override; \
    uint64_t setTag(unsigned int a0) override; \
    uint64_t getTag() override;
#define ARC_TAHOE_FWD_ArcVidMemory \
    uint64_t getLength() const override; \
    uint64_t setTag(unsigned int a0) override; \
    uint64_t getTag() override;
#define ARC_TAHOE_FWD_ArcSurface \
    void set_shape(eIOAccelSurfaceShapeBits a0, unsigned int a1, IOAccelDeviceRegion* a2, unsigned long long a3) override; \
    void set_shape_backing_length_ext(eIOAccelSurfaceShapeBits a0, unsigned int a1, unsigned long long a2, unsigned int a3, unsigned long long a4, IOAccelDeviceRegion* a5, unsigned long long a6) override; \
    void surface_lock_options(eLockType a0, unsigned int a1, IOAccelSurfaceInformation* a2, unsigned long long a3) override; \
    void surface_unlock_options(eLockType a0, unsigned int a1) override; \
    void submitCopyForward(IOAccelEvent* a0, unsigned int a1, IOAccelResource2* a2, IOAccelResource2* a3, IOAccelBounds const* a4, unsigned int a5) override; \
    void submitUpdate(unsigned int a0, IOAccelBounds* a1, unsigned int a2) override;
#define ARC_TAHOE_FWD_ArcDisplayPipe \
    void setStereo(IOAccelStereoMode a0) override;
#define ARC_TAHOE_FWD_ArcGLContext \
    void * getOwningTask() const override; \
    void * getGPUTask() const override; \
    void getOwningTaskPid() const override; \
    void retireCommandBuffer(IOAccelEventFence* a0) override; \
    void setSubmissionError(unsigned int a0) override; \
    bool isOpportunisticWorkload() const override; \
    void postTokenSanityCheck(IOAccelCommandStreamInfo& a0) override; \
    void getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) override; \
    void validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) override; \
    void invalidate() override; \
    void getFrameDelimiter() override; \
    void processSwap(eDoSwap a0) override;
#define ARC_TAHOE_FWD_Arc2DContext \
    void * getOwningTask() const override; \
    void * getGPUTask() const override; \
    void getOwningTaskPid() const override; \
    void retireCommandBuffer(IOAccelEventFence* a0) override; \
    void setSubmissionError(unsigned int a0) override; \
    bool isOpportunisticWorkload() const override; \
    void postTokenSanityCheck(IOAccelCommandStreamInfo& a0) override; \
    void getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) override; \
    void validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) override; \
    void invalidate() override; \
    void getFrameDelimiter() override;
#define ARC_TAHOE_FWD_ArcCLContext \
    void * getOwningTask() const override; \
    void * getGPUTask() const override; \
    void getOwningTaskPid() const override; \
    void retireCommandBuffer(IOAccelEventFence* a0) override; \
    void setSubmissionError(unsigned int a0) override; \
    bool isOpportunisticWorkload() const override; \
    void postTokenSanityCheck(IOAccelCommandStreamInfo& a0) override; \
    void getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) override; \
    void validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) override; \
    void invalidate() override; \
    void getFrameDelimiter() override;
#define ARC_TAHOE_FWD_ArcVideoContext \
    void * getOwningTask() const override; \
    void * getGPUTask() const override; \
    void getOwningTaskPid() const override; \
    void retireCommandBuffer(IOAccelEventFence* a0) override; \
    void setSubmissionError(unsigned int a0) override; \
    bool isOpportunisticWorkload() const override; \
    void postTokenSanityCheck(IOAccelCommandStreamInfo& a0) override; \
    void getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) override; \
    void validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) override; \
    void invalidate() override; \
    void getFrameDelimiter() override;
#define ARC_TAHOE_FWD_ArcEventMachine \
    void collectAllLiveEventIfNeeded() override;
#define ARC_TAHOE_FWD_ArcDisplayMachine \
    void setStereo(unsigned int a0, IOAccelStereoMode a1) override;
#define ARC_TAHOE_FWD_ArcTask

#define ARC_TAHOE_FWD_ArcMemoryMap \
    bool matchOptionBits(unsigned int a0) override; \
    unsigned long long getLength() const override;
#define ARC_TAHOE_FWD_ArcAccel \
    uint64_t tmpTotalVRAM() override; \
    uint64_t totalTextureMemory() override;
#define ARC_TAHOE_FWD(cls) ARC_TAHOE_FWD_##cls
#define ARC_TAHOE_FWD_DEFS \
__attribute__((naked)) void ArcResource::pageon(IOAccelEvent* a0, bool a1) { __asm__("movq __ZTV16IOAccelResource2@GOTPCREL(%rip), %r11\n\tjmpq *448(%r11)"); } \
__attribute__((naked)) void ArcResource::pageon(IOAccelEvent* a0, bool a1, unsigned long long a2) { __asm__("movq __ZTV16IOAccelResource2@GOTPCREL(%rip), %r11\n\tjmpq *456(%r11)"); } \
__attribute__((naked)) void ArcResource::pageoff(IOAccelEvent* a0, bool a1, bool* a2) { __asm__("movq __ZTV16IOAccelResource2@GOTPCREL(%rip), %r11\n\tjmpq *464(%r11)"); } \
__attribute__((naked)) uint64_t ArcResource::getMemoryAllocParameter() { __asm__("movq __ZTV16IOAccelResource2@GOTPCREL(%rip), %r11\n\tjmpq *488(%r11)"); } \
__attribute__((naked)) uint64_t ArcSysMemory::getLength() const { __asm__("movq __ZTV16IOAccelSysMemory@GOTPCREL(%rip), %r11\n\tjmpq *304(%r11)"); } \
__attribute__((naked)) uint64_t ArcSysMemory::setTag(unsigned int a0) { __asm__("movq __ZTV16IOAccelSysMemory@GOTPCREL(%rip), %r11\n\tjmpq *376(%r11)"); } \
__attribute__((naked)) uint64_t ArcSysMemory::getTag() { __asm__("movq __ZTV16IOAccelSysMemory@GOTPCREL(%rip), %r11\n\tjmpq *384(%r11)"); } \
__attribute__((naked)) uint64_t ArcVidMemory::getLength() const { __asm__("movq __ZTV16IOAccelVidMemory@GOTPCREL(%rip), %r11\n\tjmpq *304(%r11)"); } \
__attribute__((naked)) uint64_t ArcVidMemory::setTag(unsigned int a0) { __asm__("movq __ZTV16IOAccelVidMemory@GOTPCREL(%rip), %r11\n\tjmpq *376(%r11)"); } \
__attribute__((naked)) uint64_t ArcVidMemory::getTag() { __asm__("movq __ZTV16IOAccelVidMemory@GOTPCREL(%rip), %r11\n\tjmpq *384(%r11)"); } \
__attribute__((naked)) void ArcSurface::set_shape(eIOAccelSurfaceShapeBits a0, unsigned int a1, IOAccelDeviceRegion* a2, unsigned long long a3) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2592(%r11)"); } \
__attribute__((naked)) void ArcSurface::set_shape_backing_length_ext(eIOAccelSurfaceShapeBits a0, unsigned int a1, unsigned long long a2, unsigned int a3, unsigned long long a4, IOAccelDeviceRegion* a5, unsigned long long a6) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2600(%r11)"); } \
__attribute__((naked)) void ArcSurface::surface_lock_options(eLockType a0, unsigned int a1, IOAccelSurfaceInformation* a2, unsigned long long a3) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void ArcSurface::surface_unlock_options(eLockType a0, unsigned int a1) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2616(%r11)"); } \
__attribute__((naked)) void ArcSurface::submitCopyForward(IOAccelEvent* a0, unsigned int a1, IOAccelResource2* a2, IOAccelResource2* a3, IOAccelBounds const* a4, unsigned int a5) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2656(%r11)"); } \
__attribute__((naked)) void ArcSurface::submitUpdate(unsigned int a0, IOAccelBounds* a1, unsigned int a2) { __asm__("movq __ZTV20IOAccelLegacySurface@GOTPCREL(%rip), %r11\n\tjmpq *2664(%r11)"); } \
__attribute__((naked)) void ArcDisplayPipe::setStereo(IOAccelStereoMode a0) { __asm__("movq __ZTV24IOAccelLegacyDisplayPipe@GOTPCREL(%rip), %r11\n\tjmpq *2480(%r11)"); } \
__attribute__((naked)) void * ArcGLContext::getOwningTask() const { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2416(%r11)"); } \
__attribute__((naked)) void * ArcGLContext::getGPUTask() const { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2424(%r11)"); } \
__attribute__((naked)) void ArcGLContext::getOwningTaskPid() const { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2432(%r11)"); } \
__attribute__((naked)) void ArcGLContext::retireCommandBuffer(IOAccelEventFence* a0) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2448(%r11)"); } \
__attribute__((naked)) void ArcGLContext::setSubmissionError(unsigned int a0) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2480(%r11)"); } \
__attribute__((naked)) bool ArcGLContext::isOpportunisticWorkload() const { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2536(%r11)"); } \
__attribute__((naked)) void ArcGLContext::postTokenSanityCheck(IOAccelCommandStreamInfo& a0) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void ArcGLContext::getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2696(%r11)"); } \
__attribute__((naked)) void ArcGLContext::validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2704(%r11)"); } \
__attribute__((naked)) void ArcGLContext::invalidate() { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2744(%r11)"); } \
__attribute__((naked)) void ArcGLContext::getFrameDelimiter() { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2752(%r11)"); } \
__attribute__((naked)) void ArcGLContext::processSwap(eDoSwap a0) { __asm__("movq __ZTV17IOAccelGLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2944(%r11)"); } \
__attribute__((naked)) void * Arc2DContext::getOwningTask() const { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2416(%r11)"); } \
__attribute__((naked)) void * Arc2DContext::getGPUTask() const { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2424(%r11)"); } \
__attribute__((naked)) void Arc2DContext::getOwningTaskPid() const { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2432(%r11)"); } \
__attribute__((naked)) void Arc2DContext::retireCommandBuffer(IOAccelEventFence* a0) { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2448(%r11)"); } \
__attribute__((naked)) void Arc2DContext::setSubmissionError(unsigned int a0) { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2480(%r11)"); } \
__attribute__((naked)) bool Arc2DContext::isOpportunisticWorkload() const { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2536(%r11)"); } \
__attribute__((naked)) void Arc2DContext::postTokenSanityCheck(IOAccelCommandStreamInfo& a0) { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void Arc2DContext::getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2696(%r11)"); } \
__attribute__((naked)) void Arc2DContext::validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2704(%r11)"); } \
__attribute__((naked)) void Arc2DContext::invalidate() { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2744(%r11)"); } \
__attribute__((naked)) void Arc2DContext::getFrameDelimiter() { __asm__("movq __ZTV17IOAccel2DContext2@GOTPCREL(%rip), %r11\n\tjmpq *2752(%r11)"); } \
__attribute__((naked)) void * ArcCLContext::getOwningTask() const { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2416(%r11)"); } \
__attribute__((naked)) void * ArcCLContext::getGPUTask() const { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2424(%r11)"); } \
__attribute__((naked)) void ArcCLContext::getOwningTaskPid() const { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2432(%r11)"); } \
__attribute__((naked)) void ArcCLContext::retireCommandBuffer(IOAccelEventFence* a0) { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2448(%r11)"); } \
__attribute__((naked)) void ArcCLContext::setSubmissionError(unsigned int a0) { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2480(%r11)"); } \
__attribute__((naked)) bool ArcCLContext::isOpportunisticWorkload() const { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2536(%r11)"); } \
__attribute__((naked)) void ArcCLContext::postTokenSanityCheck(IOAccelCommandStreamInfo& a0) { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void ArcCLContext::getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2696(%r11)"); } \
__attribute__((naked)) void ArcCLContext::validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2704(%r11)"); } \
__attribute__((naked)) void ArcCLContext::invalidate() { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2744(%r11)"); } \
__attribute__((naked)) void ArcCLContext::getFrameDelimiter() { __asm__("movq __ZTV17IOAccelCLContext2@GOTPCREL(%rip), %r11\n\tjmpq *2752(%r11)"); } \
__attribute__((naked)) void * ArcVideoContext::getOwningTask() const { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2416(%r11)"); } \
__attribute__((naked)) void * ArcVideoContext::getGPUTask() const { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2424(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::getOwningTaskPid() const { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2432(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::retireCommandBuffer(IOAccelEventFence* a0) { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2448(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::setSubmissionError(unsigned int a0) { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2480(%r11)"); } \
__attribute__((naked)) bool ArcVideoContext::isOpportunisticWorkload() const { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2536(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::postTokenSanityCheck(IOAccelCommandStreamInfo& a0) { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2608(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::getDataBufferPrivate(IOAccelResource2* a0, IOAccelResourcePrivate* a1, unsigned long long a2) { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2696(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::validateDataBuffer(unsigned long long a0, IOBufferMemoryDescriptor* a1, IOAccelResource2* a2) { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2704(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::invalidate() { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2744(%r11)"); } \
__attribute__((naked)) void ArcVideoContext::getFrameDelimiter() { __asm__("movq __ZTV20IOAccelVideoContext2@GOTPCREL(%rip), %r11\n\tjmpq *2752(%r11)"); } \
__attribute__((naked)) void ArcEventMachine::collectAllLiveEventIfNeeded() { __asm__("movq __ZTV24IOAccelEventMachineFast2@GOTPCREL(%rip), %r11\n\tjmpq *576(%r11)"); } \
__attribute__((naked)) void ArcDisplayMachine::setStereo(unsigned int a0, IOAccelStereoMode a1) { __asm__("movq __ZTV27IOAccelLegacyDisplayMachine@GOTPCREL(%rip), %r11\n\tjmpq *2304(%r11)"); } \
__attribute__((naked)) bool ArcMemoryMap::matchOptionBits(unsigned int a0) { __asm__("movq __ZTV16IOAccelMemoryMap@GOTPCREL(%rip), %r11\n\tjmpq *304(%r11)"); } \
__attribute__((naked)) unsigned long long ArcMemoryMap::getLength() const { __asm__("movq __ZTV16IOAccelMemoryMap@GOTPCREL(%rip), %r11\n\tjmpq *376(%r11)"); } \
__attribute__((naked)) uint64_t ArcAccel::tmpTotalVRAM() { __asm__("movq __ZTV22IOGraphicsAccelerator2@GOTPCREL(%rip), %r11\n\tjmpq *2160(%r11)"); } \
__attribute__((naked)) uint64_t ArcAccel::totalTextureMemory() { __asm__("movq __ZTV22IOGraphicsAccelerator2@GOTPCREL(%rip), %r11\n\tjmpq *2168(%r11)"); }
#else
#define ARC_TAHOE_FWD(cls)
#define ARC_TAHOE_FWD_DEFS
#endif
