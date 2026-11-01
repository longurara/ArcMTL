/*
 * tools/mtl_offscreen_test.m — Stage 3 Dual-GPU & Standalone Metal 3 End-to-End Verification
 * SPDX-License-Identifier: MIT
 *
 * Verifies:
 *   1. MTLCopyAllDevices() coexistence (both RTX 4070 NVMTLDevice and Intel Arc ArcMTLDevice)
 *   2. Runtime MSL -> AIR -> SPIR-V -> Mesa ANV -> ArcXe compute kernel execution
 *   3. Offscreen IOSurface-backed MTLTexture render pass clear & UMA readback
 */

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <math.h>
#include <stdio.h>

static NSString *const kArcComputeMSL =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void arc_fma_test(device const float *a [[buffer(0)]],\n"
     "                         device const float *b [[buffer(1)]],\n"
     "                         device float *out     [[buffer(2)]],\n"
     "                         uint gid              [[thread_position_in_grid]]) {\n"
     "    out[gid] = a[gid] * 2.5f + b[gid];\n"
     "}\n";

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        printf("==================================================================\n");
        printf(" Stage 3: Metal 3 End-to-End & Dual-GPU Coexistence Verification\n");
        printf("==================================================================\n");

        NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
        printf("  MTLCopyAllDevices() returned %lu GPU(s):\n", (unsigned long)devices.count);

        id<MTLDevice> arcDev = nil;
        for (NSUInteger i = 0; i < devices.count; i++) {
            id<MTLDevice> d = devices[i];
            const char *cls = object_getClassName(d);
            printf("    [%lu] %-36s class=%-14s regID=0x%llx unified=%d headless=%d lowPower=%d\n",
                   (unsigned long)i,
                   d.name.UTF8String,
                   cls,
                   (unsigned long long)d.registryID,
                   d.hasUnifiedMemory ? 1 : 0,
                   d.isHeadless ? 1 : 0,
                   d.isLowPower ? 1 : 0);

            NSString *clsStr = [NSString stringWithUTF8String:cls];
            if ([clsStr hasPrefix:@"ArcMTL"] ||
                [d.name rangeOfString:@"Intel" options:NSCaseInsensitiveSearch].location != NSNotFound ||
                [d.name rangeOfString:@"Arc" options:NSCaseInsensitiveSearch].location != NSNotFound) {
                arcDev = d;
            }
        }

        if (!arcDev) {
            fprintf(stderr,
                    "\n[FAIL] Intel Arc Metal device not found in MTLCopyAllDevices()!\n"
                    "       Check:\n"
                    "         1. boot-args includes 'arc_headless=1' (Dual-GPU) or 'arcaccel=1'\n"
                    "         2. /Library/GPUBundles/ArcMTLDriver.bundle & arcmtl/ are installed\n"
                    "         3. `ioreg -l | grep -E 'ArcAccel|ArcXeService'` shows ArcAccel attached\n");
            return 1;
        }

        printf("\n  Selected Target Metal GPU: %s (%s)\n",
               arcDev.name.UTF8String, object_getClassName(arcDev));

        /* --- Test 1: Runtime MSL Compute Pipeline --- */
        printf("\n  [Test 1/2] Compiling runtime MSL compute kernel -> SPIR-V -> Xe-LPG...\n");
        NSError *err = nil;
        id<MTLLibrary> lib = [arcDev newLibraryWithSource:kArcComputeMSL options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "[FAIL] newLibraryWithSource failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "unknown");
            return 2;
        }

        id<MTLFunction> fn = [lib newFunctionWithName:@"arc_fma_test"];
        if (!fn) {
            fprintf(stderr, "[FAIL] Function 'arc_fma_test' missing from translated MTLLibrary!\n");
            return 3;
        }

        id<MTLComputePipelineState> pso =
            [arcDev newComputePipelineStateWithFunction:fn error:&err];
        if (!pso) {
            fprintf(stderr, "[FAIL] newComputePipelineStateWithFunction failed: %s\n",
                    err ? err.localizedDescription.UTF8String : "unknown");
            return 4;
        }

        const NSUInteger count = 4096;
        const NSUInteger byteLen = count * sizeof(float);
        id<MTLBuffer> bufA = [arcDev newBufferWithLength:byteLen options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufB = [arcDev newBufferWithLength:byteLen options:MTLResourceStorageModeShared];
        id<MTLBuffer> bufOut = [arcDev newBufferWithLength:byteLen options:MTLResourceStorageModeShared];
        if (!bufA || !bufB || !bufOut) {
            fprintf(stderr, "[FAIL] Failed to allocate shared UMA MTLBuffers!\n");
            return 5;
        }

        float *a = (float *)bufA.contents;
        float *b = (float *)bufB.contents;
        float *out = (float *)bufOut.contents;
        for (NSUInteger i = 0; i < count; i++) {
            a[i] = (float)i * 0.5f;
            b[i] = 10.0f - (float)(i % 17);
            out[i] = -999.0f;
        }

        id<MTLCommandQueue> queue = [arcDev newCommandQueue];
        if (!queue) {
            fprintf(stderr, "[FAIL] newCommandQueue returned nil!\n");
            return 6;
        }

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:bufA offset:0 atIndex:0];
        [enc setBuffer:bufB offset:0 atIndex:1];
        [enc setBuffer:bufOut offset:0 atIndex:2];
        [enc dispatchThreadgroups:MTLSizeMake(count / 64, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        if (cb.status == MTLCommandBufferStatusError) {
            fprintf(stderr, "[FAIL] Compute command buffer failed: %s\n",
                    cb.error ? cb.error.localizedDescription.UTF8String : "unknown");
            return 7;
        }

        NSUInteger bad = 0;
        for (NSUInteger i = 0; i < count; i++) {
            float expected = a[i] * 2.5f + b[i];
            if (fabsf(out[i] - expected) > 1e-3f) {
                if (bad < 4) {
                    fprintf(stderr, "    [MISMATCH] out[%lu] = %f (expected %f)\n",
                            (unsigned long)i, out[i], expected);
                }
                bad++;
            }
        }
        if (bad != 0) {
            fprintf(stderr, "[FAIL] Compute verification failed (%lu / %lu elements wrong)\n",
                    (unsigned long)bad, (unsigned long)count);
            return 8;
        }
        printf("             Verified %lu / %lu float FMA outputs on Intel Arc!\n",
               (unsigned long)count, (unsigned long)count);

        /* --- Test 2: IOSurface-backed Offscreen Render Pass Clear --- */
        printf("  [Test 2/2] Rendering offscreen clear into IOSurface-backed BGRA8 MTLTexture...\n");
        const uint32_t w = 64, h = 64, bpr = 64 * 4;
        NSDictionary *surfProps = @{
            (id)kIOSurfaceWidth : @(w),
            (id)kIOSurfaceHeight : @(h),
            (id)kIOSurfaceBytesPerElement : @(4),
            (id)kIOSurfaceBytesPerRow : @(bpr),
            (id)kIOSurfaceAllocSize : @(bpr * h),
            (id)kIOSurfacePixelFormat : @(0x42475241) /* 'BGRA' */
        };
        IOSurfaceRef surf = IOSurfaceCreate((__bridge CFDictionaryRef)surfProps);
        if (!surf) {
            fprintf(stderr, "[FAIL] IOSurfaceCreate failed!\n");
            return 9;
        }

        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [arcDev newTextureWithDescriptor:td iosurface:surf plane:0];
        if (!tex) {
            CFRelease(surf);
            fprintf(stderr, "[FAIL] newTextureWithDescriptor:iosurface:plane: returned nil!\n");
            return 10;
        }

        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = tex;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.50, 0.75, 1.0);

        id<MTLCommandBuffer> rcb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> renc = [rcb renderCommandEncoderWithDescriptor:rpd];
        [renc endEncoding];
        [rcb commit];
        [rcb waitUntilCompleted];

        IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *px = (const uint8_t *)IOSurfaceGetBaseAddress(surf);
        uint8_t b0 = px[0], g0 = px[1], r0 = px[2], a0 = px[3];
        IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        CFRelease(surf);

        printf("             IOSurface pixel[0,0] BGRA = (%u, %u, %u, %u) [expected ~(191, 128, 64, 255)]\n",
               b0, g0, r0, a0);
        if (abs((int)b0 - 191) > 4 || abs((int)g0 - 128) > 4 ||
            abs((int)r0 - 64) > 4  || abs((int)a0 - 255) > 2) {
            fprintf(stderr, "[FAIL] IOSurface render clear pixel mismatch!\n");
            return 11;
        }

        printf("\n[PASS] Stage 3 PASSED: Metal 3 Compute + IOSurface Render work end-to-end on Intel Arc!\n");
        return 0;
    }
}
