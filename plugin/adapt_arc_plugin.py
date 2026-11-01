#!/usr/bin/env python3
"""
adapt_arc_plugin.py — Adapt NullMoth's Metal-to-Vulkan Objective-C plugin into ArcMTLDriver.bundle

Transforms third_party/nullmoth/plugin into build/arc-plugin-src so that:
  1. All Objective-C classes and categories are renamed NVMTL* -> ArcMTL*
     (zero Objective-C runtime class collisions when both NVMTLDriver.bundle on
      RTX 4070 and ArcMTLDriver.bundle on Intel Arc 155H load into the same process).
  2. Vendor detection in nvmtl_vk.c is locked to Intel 0x8086 (ArcAccel / ArcXeService),
     preventing accidental matching against NVAccel in Dual-GPU systems.
  3. All dylib and config paths isolate cleanly under /Library/GPUBundles/arcmtl/
     (libvulkan_intel.dylib, libarcmtl_translate.dylib, intel_icd.json, arcmtl-allow.txt),
     with ARCMTL_ICD_LIB / ARC_VULKAN_ICD and ARCMTL_XLATE_LIB env overrides.
  4. Headless & framebuffer gating reads debug.arcaccel_headless and debug.arcaccelfb,
     keeping WindowServer on RTX 4070 in Dual-GPU mode (arc_headless=1) while enabling
     full WindowServer desktop compositing on Intel Arc in standalone mode (arcaccel=1).

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
License: PolyForm Noncommercial License 1.0.0
"""

import os
import re
import shutil
import sys
from pathlib import Path

REQUIRED_NOTICE_BANNER = """/*
 * Adapted for Intel Arc (Xe-LPG / Meteor Lake 8086:7D55) — ArcMTLDriver.bundle
 * Based on NullMoth Metal-to-Vulkan plugin.
 * Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
"""

# Matches NVMTL followed by an uppercase letter (all ObjC classes, categories, and file stems),
# plus optional leading 'k' for associated-object keys like kNVMTLMeshInfo -> kArcMTLMeshInfo.
# Does NOT match NVMTL_ macros/env vars or lowercase nvmtl_ C/dlsym symbols.
OBJC_IDENT_RE = re.compile(r"\b(k?)NVMTL([A-Z][A-Za-z0-9_]*)\b")


def transform_common(text: str) -> str:
    # 1. Rename all Objective-C classes, categories, keys, and NVMTL*.{h,m} includes -> ArcMTL*
    text = OBJC_IDENT_RE.sub(r"\1ArcMTL\2", text)

    # 2. Isolate GPUBundles paths and allow-list files
    text = text.replace(
        "/Library/GPUBundles/nvmtl/libnvmtl_translate.dylib",
        "/Library/GPUBundles/arcmtl/libarcmtl_translate.dylib",
    )
    text = text.replace("/Library/GPUBundles/nvmtl/", "/Library/GPUBundles/arcmtl/")
    text = text.replace("/Library/GPUBundles/nvmtl-allow.txt", "/Library/GPUBundles/arcmtl-allow.txt")
    text = text.replace("/Library/Extensions/nvmtl-allow.txt", "/Library/Extensions/arcmtl-allow.txt")
    text = text.replace("/Library/GPUBundles/nvmtl-gpupass-probe", "/Library/GPUBundles/arcmtl-gpupass-probe")
    text = text.replace("nvmtl-allow.txt", "arcmtl-allow.txt")

    # 3. Point sysctls to ArcAccel's kernel sysctl nodes
    text = text.replace('"debug.nvaccelfb"', '"debug.arcaccelfb"')
    text = text.replace("debug.nvaccelfb", "debug.arcaccelfb")
    text = text.replace('"debug.nvaccel_headless"', '"debug.arcaccel_headless"')
    text = text.replace("debug.nvaccel_headless", "debug.arcaccel_headless")

    # 4. Distinct syslog tag so Dual-GPU logs distinguish RTX 4070 (NullMoth) vs Intel Arc (ArcMTL)
    text = text.replace('"NullMoth:', '"ArcMTL:')

    return text


def transform_device_m(text: str) -> str:
    # 1. Sandbox-safe IORegistry fallback for nvmtl_accel_armed() & headless check:
    #    App-sandboxed desktop processes (ControlCenter, NotificationCenter, WallpaperAgent, Spotlight,
    #    Safari/Chrome helpers, and Seatbelt-restricted daemons) may get EPERM on sysctlbyname("debug.*"),
    #    whereas IORegistryEntryCreateCFProperty on ArcAccel ("ArcAccelArmed" / "ArcAccelHeadless") is
    #    globally allowed by Seatbelt iokit-get-properties.
    text = re.sub(
        r"static\s+bool\s+nvmtl_accel_armed\s*\(\s*void\s*\)\s*\{[^}]*\}",
        "static bool arcmtl_accel_registry_flag(CFStringRef key)\n"
        "{\n"
        "    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(\"ArcAccel\"));\n"
        "    if (!s) return false;\n"
        "    CFTypeRef v = IORegistryEntryCreateCFProperty(s, key, kCFAllocatorDefault, 0);\n"
        "    IOObjectRelease(s);\n"
        "    if (!v) return false;\n"
        "    bool ok = (CFGetTypeID(v) == CFBooleanGetTypeID()) && CFBooleanGetValue((CFBooleanRef)v);\n"
        "    CFRelease(v);\n"
        "    return ok;\n"
        "}\n"
        "static bool nvmtl_accel_armed(void)\n"
        "{\n"
        "    int v = 0; size_t n = sizeof v;\n"
        "    if (sysctlbyname(\"debug.arcaccelfb\", &v, &n, NULL, 0) == 0) return v != 0;\n"
        "    return arcmtl_accel_registry_flag(CFSTR(\"ArcAccelArmed\"));\n"
        "}",
        text,
        count=1,
    )

    # 2. Fix WindowServer vendor & headless check: ArcMTLDevice's legitimate vendor is 0x8086, not 0x10de.
    #    When debug.arcaccel_headless == 1 (Dual-GPU headless mode), WindowServer returns false;
    #    when debug.arcaccel_headless == 0 (Primary Arc Desktop mode), WindowServer is permitted
    #    subject to !WindowServer (nvmtl_accel_armed() == true) in arcmtl-allow.txt.
    text = re.sub(
        r'\(\s*sysctlbyname\(\s*"debug\.arcaccel_headless"\s*,\s*&hv\s*,\s*&hn\s*,\s*NULL\s*,\s*0\s*\)\s*==\s*0\s*&&\s*hv\s*\)',
        '(sysctlbyname("debug.arcaccel_headless", &hv, &hn, NULL, 0) == 0 ? (hv != 0) : arcmtl_accel_registry_flag(CFSTR("ArcAccelHeadless")))',
        text,
    )
    text = re.sub(
        r'(!strcmp\(\s*me\s*,\s*"WindowServer"\s*\)\s*&&\s*\(\s*nvmtl_vk_vendor\(\)\s*!=\s*)0x10de',
        r"\g<1>0x8086",
        text,
    )

    # 3. Allow ARCMTL_ENABLE env var and our staged CLI test binaries without allow-list file
    text = text.replace(
        'if (getenv("NVMTL_ENABLE")) return true;',
        'if (getenv("ARCMTL_ENABLE") || getenv("NVMTL_ENABLE")) return true;\n'
        '    if (!strcmp(me, "mtl_offscreen_test") || !strcmp(me, "vk_offscreen_test") || !strcmp(me, "arc_probe")) return true;',
    )

    # 4. Chromium / Electron ANGLE Metal override for Intel GPUs
    text = text.replace('"disableMetalOnNvidia"', '"disableMetalOnIntel"')

    # 5. Export ArcMTLDevice explicitly when compiled with -fvisibility=hidden so internal C symbols
    #    (_nvlog, _gNVMTLMainDevice, _nvmtl_vk_*) stay private to ArcMTLDriver.bundle in Dual-GPU processes
    if "@interface ArcMTLDevice : MTLIOAccelDevice <MTLDevice, MTLDeviceSPI>" not in text:
        raise RuntimeError("Failed to locate @interface ArcMTLDevice declaration in ArcMTLDevice.m")
    text = text.replace(
        "@interface ArcMTLDevice : MTLIOAccelDevice <MTLDevice, MTLDeviceSPI>",
        '__attribute__((visibility("default")))\n@interface ArcMTLDevice : MTLIOAccelDevice <MTLDevice, MTLDeviceSPI>',
        1,
    )

    # 6. Surface key startup / bitcode / translator milestones in Unified Log even under NVMTL_RELEASE=1
    text = text.replace(
        'if (nvmtl_log_failure(t)) syslog(LOG_ERR, "ArcMTL: %s", t);',
        'if (nvmtl_log_failure(t) || strstr(t, "initWithAcceleratorPort:") || strstr(t, "vk: UP") || '
        'strstr(t, "bitcode reader") || strstr(t, "translate:") || strstr(t, "library: metallib")) '
        'syslog(LOG_ERR, "ArcMTL: %s", t);',
    )

    # 7. Never run NVIDIA NVDEC VTDecoderXPCService constructor from ArcMTLDriver.bundle on Dual-GPU systems
    text = text.replace(
        "__attribute__((constructor)) static void nvmtl_nvdec_in_decode_service(void)\n{",
        "__attribute__((constructor)) static void nvmtl_nvdec_in_decode_service(void)\n{\n"
        "    if (nvmtl_vk_vendor() != 0x10de) return;",
    )

    # 8. Intel Arc device & architecture naming
    text = text.replace('(vkname ? vkname : "NVIDIA GPU")', '(vkname ? vkname : "Intel(R) Arc(TM) Graphics")')
    text = text.replace('@" (NVK "', '@" (MTL)"')
    text = text.replace('@"%@ (NVMTL over %@)"', '@"%@ (ArcMTL over %@)"')
    text = text.replace('[NSMutableString stringWithString:@"nvk_"]', '[NSMutableString stringWithString:@"anv_"]')

    # 9. Export end-to-end CoreDisplay / metallib toolchain & Vulkan pipeline self-test entrypoint
    self_test_fn = """

__attribute__((visibility("default")))
int arcmtl_self_test_metallib(const char *metallib_path, int test_vk_pipelines)
{
    @autoreleasepool {
        const char *mpath = (metallib_path && *metallib_path)
            ? metallib_path
            : "/System/Library/Frameworks/CoreDisplay.framework/Versions/A/Resources/default.metallib";
        printf("  [Bundle Self-Test] Verifying ArcMTLDriver toolchain against: %s\\n", mpath);
        if (!nvmtl_bitcode_reader_ready()) {
            fprintf(stderr, "  [FAIL] nvmtl_bitcode_reader_ready() == NO (air-runtime/libLLVM.dylib missing or failed to load)\\n");
            return 1;
        }
        printf("  [OK] In-memory AIR bitcode reader (air-runtime/libLLVM.dylib) READY\\n");
        if (!nvmtl_xlate_ready()) {
            fprintf(stderr, "  [FAIL] nvmtl_xlate_ready() == 0 (libarcmtl_translate.dylib missing or failed to load)\\n");
            return 2;
        }
        printf("  [OK] AIR -> SPIR-V translator (libarcmtl_translate.dylib) READY (tag=%s)\\n", gXlateTag);
        NSData *raw = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:mpath]];
        if (!raw || raw.length < 32) {
            fprintf(stderr, "  [FAIL] Cannot read metallib at %s\\n", mpath);
            return 3;
        }
        NSDictionary *stages = nil, *airs = nil;
        NSDictionary<NSString *, NSData *> *fns = nvmtl_translate_metallib(raw, &stages, &airs);
        if (!fns || fns.count == 0 || !stages || !airs) {
            fprintf(stderr, "  [FAIL] nvmtl_translate_metallib(%s, %lu bytes) returned 0 entries!\\n",
                    mpath, (unsigned long)raw.length);
            return 4;
        }
        printf("  [OK] Indexed %lu shader entry points from metallib (%lu bytes)\\n",
               (unsigned long)fns.count, (unsigned long)raw.length);

        NSMutableDictionary<NSString *, NSData *> *spvMap = [NSMutableDictionary dictionary];
        if (fns[@"ViewportToNDC"]) {
            NSArray<NSString *> *mustCompile = @[@"ViewportToNDC", @"GPUPass", @"ColorFill", @"TextureCopy"];
            NSUInteger compiledReq = 0;
            for (NSString *fnName in mustCompile) {
                NSData *airBlob = fns[fnName];
                if (!airBlob) {
                    if ([fnName isEqualToString:@"ViewportToNDC"] || [fnName isEqualToString:@"GPUPass"]) {
                        fprintf(stderr, "  [FAIL] Required CoreDisplay shader '%s' missing from metallib index!\\n",
                                fnName.UTF8String);
                        return 5;
                    }
                    continue;
                }
                NSString *ll = airs[fnName];
                NSString *st = stages[fnName];
                NSString *errStr = nil;
                NSData *spv = nvmtl_compile_library_entry(airBlob, ll, st, fnName, &errStr);
                if (!spv || !nvmtl_is_spirv(spv)) {
                    fprintf(stderr, "  [FAIL] Failed to translate CoreDisplay '%s' (%s) -> SPIR-V: %s\\n",
                            fnName.UTF8String, st ? st.UTF8String : "?",
                            errStr ? errStr.UTF8String : "invalid SPIR-V");
                    return 5;
                }
                spvMap[fnName] = spv;
                compiledReq++;
                printf("  [OK] Translated CoreDisplay %-14s (%-8s) -> %lu bytes SPIR-V\\n",
                       fnName.UTF8String, st.UTF8String, (unsigned long)spv.length);
            }
            NSUInteger prewarmed = compiledReq;
            for (NSString *k in fns) {
                if (spvMap[k]) continue;
                NSString *st = stages[k];
                if (!st || [st isEqualToString:@"visible"]) continue;
                NSData *spv = nvmtl_compile_library_entry(fns[k], airs[k], st, k, NULL);
                if (spv && nvmtl_is_spirv(spv)) prewarmed++;
            }
            printf("  [OK] Pre-warmed %lu / %lu shader entries into SPIR-V cache\\n",
                   (unsigned long)prewarmed, (unsigned long)fns.count);
        } else {
            NSUInteger okCount = 0;
            for (NSString *k in fns) {
                NSString *st = stages[k];
                if (!st || [st isEqualToString:@"visible"]) continue;
                NSData *spv = nvmtl_compile_library_entry(fns[k], airs[k], st, k, NULL);
                if (spv && nvmtl_is_spirv(spv)) okCount++;
            }
            if (okCount == 0) {
                fprintf(stderr, "  [FAIL] Zero entries in %s compiled to valid SPIR-V!\\n", mpath);
                return 5;
            }
            printf("  [OK] Compiled %lu / %lu entries to SPIR-V\\n",
                   (unsigned long)okCount, (unsigned long)fns.count);
        }

        nvmtl_cache_io_drain();

        if (test_vk_pipelines) {
            if (nvmtl_vk_init() != 0) {
                fprintf(stderr, "  [FAIL] nvmtl_vk_init() failed inside ArcMTLDriver!\\n");
                return 6;
            }
            printf("  [OK] ArcMTLDriver Vulkan backend UP on \\"%s\\"\\n", nvmtl_vk_device_name());

            nvk_buffer tb;
            memset(&tb, 0, sizeof(tb));
            if (nvmtl_vk_buffer_create(65536, 2, &tb) != 0) {
                fprintf(stderr, "  [FAIL] nvmtl_vk_buffer_create(64 KiB shared) failed on UMA!\\n");
                return 7;
            }
            nvmtl_vk_buffer_destroy(&tb);

            nvk_image ti;
            memset(&ti, 0, sizeof(ti));
            if (nvmtl_vk_image_create_fmt(256, 256, 1, &ti) != 0) {
                fprintf(stderr, "  [FAIL] nvmtl_vk_image_create_fmt(256x256 BGRA8) failed on UMA!\\n");
                return 8;
            }
            nvmtl_vk_image_destroy(&ti);
            printf("  [OK] UMA buffer + 256x256 BGRA8 optimal image allocation verified\\n");

            NSData *vsSpv = spvMap[@"ViewportToNDC"];
            if (vsSpv) {
                for (NSString *fsName in @[@"GPUPass", @"ColorFill", @"TextureCopy"]) {
                    NSData *fsSpv = spvMap[fsName];
                    if (!fsSpv) continue;
                    nvk_pipeline pipe;
                    memset(&pipe, 0, sizeof(pipe));
                    int rc = nvmtl_vk_pipeline_create_depth(
                        vsSpv.bytes, vsSpv.length, fsSpv.bytes, fsSpv.length, 1, 0, &pipe);
                    if (rc != 0) {
                        fprintf(stderr, "  [FAIL] vkCreateGraphicsPipelines(ViewportToNDC + %s) FAILED (rc=%d)!\\n",
                                fsName.UTF8String, rc);
                        return 9;
                    }
                    printf("  [OK] Built Xe-LPG VkPipeline: ViewportToNDC + %-12s (%lu + %lu B SPIR-V)\\n",
                           fsName.UTF8String, (unsigned long)vsSpv.length, (unsigned long)fsSpv.length);
                    nvmtl_vk_pipeline_destroy(&pipe);
                }
            }
        }
        return 0;
    }
}
"""
    return REQUIRED_NOTICE_BANNER + text + self_test_fn


def transform_library_load_m(text: str) -> str:
    # 1. Allow ARCMTL_XLATE_LIB env override for pre-install testing from out/arcmtl/libarcmtl_translate.dylib
    old_block = (
        "#ifndef NVMTL_XLATE_LIB\n"
        '#define NVMTL_XLATE_LIB nvmtl_pick_ll("/Library/GPUBundles/arcmtl/libarcmtl_translate.dylib", \\\n'
        '                                      "/Library/GPUBundles/arcmtl/libarcmtl_translate.dylib")\n'
        "#endif"
    )
    new_block = (
        "static inline const char *arcmtl_xlate_lib_path(void) {\n"
        '    const char *e = getenv("ARCMTL_XLATE_LIB");\n'
        "    if (e && *e && access(e, R_OK) == 0) return e;\n"
        '    return "/Library/GPUBundles/arcmtl/libarcmtl_translate.dylib";\n'
        "}\n"
        "#ifndef NVMTL_XLATE_LIB\n"
        "#define NVMTL_XLATE_LIB arcmtl_xlate_lib_path()\n"
        "#endif"
    )
    if old_block in text:
        text = text.replace(old_block, new_block, 1)

    # 2. Isolate bundled translator filename, cache subdirectories, dispatch queue label, and ARCMTL_AIRCACHE env
    text = text.replace('@"libnvmtl_translate.dylib"', '@"libarcmtl_translate.dylib"')
    text = text.replace('@"nvmtl/aircache"', '@"arcmtl/aircache"')
    text = text.replace('@"nvmtl/spvcache"', '@"arcmtl/spvcache"')
    text = text.replace('"nvmtl.cache-io"', '"arcmtl.cache-io"')
    text = text.replace(
        'const char *e = getenv("NVMTL_AIRCACHE");',
        'const char *e = getenv("ARCMTL_AIRCACHE"); if (!e) e = getenv("NVMTL_AIRCACHE");',
    )

    # 3. Resilient multi-path dlopen for air-runtime/libLLVM.dylib inside nvmtl_bitcode_reader_ready()
    old_llvm_block = (
        '        NSString *path = [[@(info.dli_fname) stringByDeletingLastPathComponent]\n'
        '                         stringByAppendingPathComponent:@"air-runtime/libLLVM.dylib"];\n'
        '        void *library = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);\n'
        '        if (!library) { nvlog("library: in-memory bitcode reader unavailable: %s", dlerror()); return; }'
    )
    new_llvm_block = (
        '        NSString *bundledPath = [[@(info.dli_fname) stringByDeletingLastPathComponent]\n'
        '                                stringByAppendingPathComponent:@"air-runtime/libLLVM.dylib"];\n'
        '        const char *envLlvm = getenv("ARCMTL_LLVM_LIB");\n'
        '        const char *candidates[5] = {\n'
        '            (envLlvm && *envLlvm) ? envLlvm : NULL,\n'
        '            bundledPath.fileSystemRepresentation,\n'
        '            "/Library/GPUBundles/arcmtl/air-runtime/libLLVM.dylib",\n'
        '            "/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib",\n'
        '            NULL\n'
        '        };\n'
        '        void *library = NULL;\n'
        '        for (int ci = 0; ci < 4 && !library; ci++) {\n'
        '            if (candidates[ci] && access(candidates[ci], R_OK) == 0)\n'
        '                library = dlopen(candidates[ci], RTLD_NOW | RTLD_LOCAL);\n'
        '        }\n'
        '        if (!library) library = dlopen(bundledPath.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);\n'
        '        if (!library) { nvlog("library: in-memory bitcode reader unavailable: %s", dlerror()); return; }'
    )
    if old_llvm_block not in text:
        raise RuntimeError("Failed to locate air-runtime/libLLVM.dylib dlopen block in ArcMTLLibraryLoad.m")
    text = text.replace(old_llvm_block, new_llvm_block, 1)

    return text


def transform_vendor_compiler_m(text: str) -> str:
    # Never load NVIDIA's NVIDIAShared.bundle SASS compiler inside ArcMTLDriver.bundle,
    # even if NVMTL_VENDOR_COMPILER=1 is exported globally for the RTX 4070.
    text = re.sub(
        r"static\s+bool\s+nvmtl_vendor_enabled\s*\(\s*void\s*\)\s*\{[^}]*\}",
        "static bool nvmtl_vendor_enabled(void) { return false; }",
        text,
        count=1,
    )
    return text


def transform_objects_m(text: str) -> str:
    # 1. Isolate constructor nvmtl_conf_knobs() so ArcMTLDriver reads /Library/GPUBundles/arcmtl/arcmtl.conf
    #    for ARCMTL_* keys only and NEVER calls unsetenv() on NullMoth's NVMTL_*/NVK_*/NAK_*/NVRM_* variables
    #    in Dual-GPU processes where both ArcMTLDriver.bundle and NVMTLDriver.bundle are loaded!
    text, n0 = re.subn(
        r"__attribute__\(\(constructor\)\)\s*static\s+void\s+nvmtl_conf_knobs\s*\(\s*void\s*\)\s*\{.*?\n\}\n(?=static\s+int\s+nvmtl_dontcare_clears)",
        lambda _m: (
            "__attribute__((constructor)) static void nvmtl_conf_knobs(void)\n"
            "{\n"
            '    FILE *f = fopen("/Library/GPUBundles/arcmtl/arcmtl.conf", "r");\n'
            "    if (!f) return;\n"
            "    char line[512];\n"
            "    while (fgets(line, sizeof line, f)) {\n"
            "        char *p = line;\n"
            "        while (*p == ' ' || *p == '\\t') p++;\n"
            "        if (*p == '#' || *p == '\\n' || *p == '\\r' || !*p) continue;\n"
            "        char *eq = strchr(p, '=');\n"
            "        if (!eq) continue;\n"
            "        *eq = 0;\n"
            "        char *key = p, *val = eq + 1;\n"
            "        char *end = key + strlen(key);\n"
            "        while (end > key && (end[-1] == ' ' || end[-1] == '\\t')) *--end = 0;\n"
            "        end = val + strlen(val);\n"
            "        while (end > val && (end[-1] == '\\n' || end[-1] == '\\r' || end[-1] == ' ' || end[-1] == '\\t')) *--end = 0;\n"
            '        if (strncmp(key, "ARCMTL_", 7) != 0) continue;\n'
            "        if (!getenv(key)) setenv(key, val, 1);\n"
            "    }\n"
            "    fclose(f);\n"
            "}\n"
        ),
        text,
        count=1,
        flags=re.DOTALL,
    )
    if n0 == 0:
        raise RuntimeError("Failed to patch nvmtl_conf_knobs in ArcMTLObjects.m")

    # 2. Default IOSurface upload (nvmtlSurfaceIn) and readback (nvmtlSurfaceOutDirtied:) on Intel Arc UMA
    # to the 100%-coherent kIOMemoryKernelUserShared staging buffer path (nvmtl_vk_image_write /
    # nvmtl_vk_image_read) unless ARCMTL_SURFIN_UPLOAD=gpu / ARCMTL_SURFOUT_READBACK=gpu is explicitly
    # requested. XNU IOSurface mappings use Write-Combining CPU PTEs and unwire across IOSurfaceUnlock,
    # which can otherwise mismatch GPU L4 WB (PAT[4]) userptr imports without failing VkResult.
    text, n1 = re.subn(
        r"static\s+bool\s+nvmtl_surfin_import\s*\(\s*void\s*\)\s*\{.*?return\s+on\s*;\s*\}",
        "static bool nvmtl_surfin_import(void) {\n"
        "    static int on = -1;\n"
        "    if (on < 0) {\n"
        '        const char *u = getenv("ARCMTL_SURFIN_UPLOAD");\n'
        '        if (!u) u = getenv("NVMTL_SURFIN_UPLOAD");\n'
        '        on = (u && !strcmp(u, "gpu")) ? 1 : 0;\n'
        "    }\n"
        "    return on != 0;\n"
        "}",
        text,
        count=1,
        flags=re.DOTALL,
    )
    text, n2 = re.subn(
        r"static\s+bool\s+nvmtl_surfout_import\s*\(\s*void\s*\)\s*\{.*?return\s+on\s*;\s*\}",
        "static bool nvmtl_surfout_import(void) {\n"
        "    static int on = -1;\n"
        "    if (on < 0) {\n"
        '        const char *u = getenv("ARCMTL_SURFOUT_READBACK");\n'
        '        if (!u) u = getenv("NVMTL_SURFOUT_READBACK");\n'
        '        on = (u && !strcmp(u, "gpu")) ? 1 : 0;\n'
        "    }\n"
        "    return on != 0;\n"
        "}",
        text,
        count=1,
        flags=re.DOTALL,
    )
    if n1 == 0 or n2 == 0:
        raise RuntimeError(
            f"Failed to patch nvmtl_surfin_import / nvmtl_surfout_import in ArcMTLObjects.m (n1={n1}, n2={n2})"
        )
    return text


def transform_vk_c(text: str) -> str:
    # 1. Hard-lock nvmtl_vk_set_accelerator and nvmtl_vk_vendor to Intel 0x8086.
    # CRITICAL on Dual-GPU systems: upstream nvmtl_vk_vendor() probes IOServiceMatching("NVAccel")
    # first, which would match the active RTX 4070 and mistakenly select 0x10de inside ArcMTLDriver!
    text, n1 = re.subn(
        r"uint16_t\s+nvmtl_vk_set_accelerator\s*\(\s*mach_port_t\s+accel\s*\)\s*\{.*?\n\}",
        "uint16_t nvmtl_vk_set_accelerator(mach_port_t accel)\n"
        "{\n"
        "    (void)accel;\n"
        "    g_vendor = 0x8086;\n"
        "    return 0x8086;\n"
        "}",
        text,
        count=1,
        flags=re.DOTALL,
    )
    text, n2 = re.subn(
        r"uint16_t\s+nvmtl_vk_vendor\s*\(\s*void\s*\)\s*\{.*?\n\}",
        "uint16_t nvmtl_vk_vendor(void)\n"
        "{\n"
        "    g_vendor = 0x8086;\n"
        "    return 0x8086;\n"
        "}",
        text,
        count=1,
        flags=re.DOTALL,
    )
    if n1 == 0 or n2 == 0:
        raise RuntimeError(f"Failed to patch nvmtl_vk_vendor in nvmtl_vk.c (n1={n1}, n2={n2})")

    # 2. Support ARCMTL_ICD_LIB / ARC_VULKAN_ICD env overrides before NVMTL_ICD_LIB
    text = text.replace(
        'const char *e = getenv("NVMTL_ICD_LIB"); if (e && access(e, R_OK) == 0) return e;',
        'const char *ea = getenv("ARCMTL_ICD_LIB"); if (ea && access(ea, R_OK) == 0) return ea;\n'
        '    const char *eb = getenv("ARC_VULKAN_ICD"); if (eb && access(eb, R_OK) == 0) return eb;\n'
        '    const char *e = getenv("NVMTL_ICD_LIB"); if (e && access(e, R_OK) == 0) return e;',
    )

    # 3. Enable sandbox-safe _open -> nvmtl_icd_open rebind on Intel 0x8086 as well as 0x10de
    text = text.replace(
        "if (nvmtl_vk_vendor() == 0x10de) { int rb = nvmtl_rebind_import",
        "if (nvmtl_vk_vendor() == 0x8086) { int rb = nvmtl_rebind_import",
    )

    # 4. Fix nvmtl_gpu_headless(): do not force h = 1 solely because vendor != 0x10de;
    # let debug.arcaccel_headless (with sandbox-safe IORegistry ArcAccelHeadless fallback)
    # control Dual-GPU headless vs Primary Arc display mode.
    text = text.replace(
        "if (h < 0 && nvmtl_vk_vendor() != 0x10de)",
        "if (h < 0 && nvmtl_vk_vendor() != 0x8086)",
    )
    text = text.replace(
        'h = sysctlbyname("debug.arcaccel_headless", &v, &n, NULL, 0) == 0 && v;',
        'if (sysctlbyname("debug.arcaccel_headless", &v, &n, NULL, 0) == 0) { h = (v != 0); } '
        'else { io_service_t _as = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("ArcAccel")); '
        'CFTypeRef _av = _as ? IORegistryEntryCreateCFProperty(_as, CFSTR("ArcAccelHeadless"), kCFAllocatorDefault, 0) : NULL; '
        'if (_as) IOObjectRelease(_as); '
        'h = (_av && CFGetTypeID(_av) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)_av)) ? 1 : 0; '
        'if (_av) CFRelease(_av); }',
    )

    # 5. Isolate accelerator surface-wait query & Mesa disk cache folder from NVAccel/nvmtl -> ArcAccel/arcmtl
    text = text.replace('IOServiceMatching("NVAccel")', 'IOServiceMatching("ArcAccel")')
    text = text.replace('"NVAccelSurfaceWait"', '"ArcAccelSurfaceWait"')
    text = text.replace('"%snvmtl-mesa"', '"%sarcmtl-mesa"')

    # 6. CRITICAL UMA FIX: Disable discrete-GPU soft VRAM spill gate on Intel Arc UMA (0x8086).
    # On Intel Arc ANV, all memory types on the 12 GiB UMA heap carry VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    # (there is no separate non-DEVICE_LOCAL spill heap), so gate == 1 would skip vkAllocateMemory and fail!
    vram_wall_anchor = "    if (freev_out) *freev_out = freev;"
    if vram_wall_anchor not in text:
        raise RuntimeError("Failed to locate nvmtl_vk_vram_wall0 anchor in nvmtl_vk.c")
    text = text.replace(
        vram_wall_anchor,
        "    if (nvmtl_vk_vendor() == 0x8086) gate = 0;\n    if (freev_out) *freev_out = freev;",
        1,
    )

    return text


def adapt_plugin_tree(src_dir: Path, dst_dir: Path) -> None:
    if not src_dir.is_dir():
        raise FileNotFoundError(f"Source plugin directory not found: {src_dir}")

    if dst_dir.exists():
        shutil.rmtree(dst_dir)
    dst_dir.mkdir(parents=True, exist_ok=True)

    text_exts = {".m", ".c", ".h", ".metal"}
    converted_count = 0

    for entry in sorted(src_dir.iterdir()):
        if entry.name.startswith("."):
            continue
        if entry.is_dir():
            continue

        out_name = OBJC_IDENT_RE.sub(r"\1ArcMTL\2", entry.name)
        out_path = dst_dir / out_name

        if entry.suffix.lower() in text_exts:
            raw = entry.read_text(encoding="utf-8")
            out = transform_common(raw)

            if out_name == "ArcMTLDevice.m":
                out = transform_device_m(out)
            elif out_name == "ArcMTLObjects.m":
                out = transform_objects_m(out)
            elif out_name == "ArcMTLLibraryLoad.m":
                out = transform_library_load_m(out)
            elif out_name == "ArcMTLVendorCompiler.m":
                out = transform_vendor_compiler_m(out)
            elif out_name == "nvmtl_vk.c":
                out = transform_vk_c(out)

            # Sanity-check: zero residual @interface NVMTL or @implementation NVMTL
            residual = re.findall(r"@(?:interface|implementation)\s+NVMTL\w+", out)
            if residual:
                raise RuntimeError(f"Residual NVMTL ObjC declarations in {out_name}: {residual}")

            out_path.write_text(out, encoding="utf-8", newline="\n")
            converted_count += 1
        else:
            shutil.copy2(entry, out_path)

    unity_entry = dst_dir / "ArcMTLDevice.m"
    if not unity_entry.is_file():
        raise RuntimeError(f"Expected unity build entry {unity_entry} was not generated!")

    print(f"[adapt_arc_plugin] Adapted {converted_count} source files -> {dst_dir}")
    print(f"[adapt_arc_plugin] Unity build entry ready: {unity_entry}")


def run_self_test() -> None:
    sample_dev = (
        '@interface NVMTLDevice : MTLIOAccelDevice <MTLDevice, MTLDeviceSPI>\n@end\n'
        '#import "NVMTLObjects.h"\n'
        'static bool nvmtl_accel_armed(void)\n'
        '{\n'
        '    int v = 0; size_t n = sizeof v;\n'
        '    if (sysctlbyname("debug.nvaccelfb", &v, &n, NULL, 0) != 0) return false;\n'
        '    return v != 0;\n'
        '}\n'
        '__attribute__((constructor)) static void nvmtl_nvdec_in_decode_service(void)\n{\n    int x = 1; (void)x;\n}\n'
        'if (!strcmp(me, "WindowServer") && (nvmtl_vk_vendor() != 0x10de || '
        '(sysctlbyname("debug.nvaccel_headless", &hv, &hn, NULL, 0) == 0 && hv))) return false;\n'
        'if (getenv("NVMTL_ENABLE")) return true;\n'
        'FILE *f = fopen("/Library/GPUBundles/nvmtl-allow.txt", "r");\n'
        'const char *k = "NVMTLCommandBuffer";\n'
        'id x = objc_getAssociatedObject(p, &kNVMTLMeshInfo);\n'
        'void *fn = dlsym(h, "nvmtl_translate");\n'
    )
    out_dev = transform_device_m(transform_common(sample_dev))
    assert '__attribute__((visibility("default")))\n@interface ArcMTLDevice' in out_dev
    assert "arcmtl_self_test_metallib" in out_dev
    assert '#import "ArcMTLObjects.h"' in out_dev
    assert 'CFSTR("ArcAccelArmed")' in out_dev
    assert 'CFSTR("ArcAccelHeadless")' in out_dev
    assert "if (nvmtl_vk_vendor() != 0x10de) return;" in out_dev
    assert "nvmtl_vk_vendor() != 0x8086" in out_dev
    assert '"debug.arcaccel_headless"' in out_dev
    assert '"/Library/GPUBundles/arcmtl-allow.txt"' in out_dev
    assert '"ArcMTLCommandBuffer"' in out_dev
    assert "&kArcMTLMeshInfo" in out_dev
    assert 'dlsym(h, "nvmtl_translate")' in out_dev

    sample_vk = (
        'uint16_t nvmtl_vk_set_accelerator(mach_port_t accel)\n{\n    return 0x10de;\n}\n'
        'uint16_t nvmtl_vk_vendor(void)\n{\n    return 0x10de;\n}\n'
        'const char *e = getenv("NVMTL_ICD_LIB"); if (e && access(e, R_OK) == 0) return e;\n'
        'case 0x8086: return "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib";\n'
        'if (nvmtl_vk_vendor() == 0x10de) { int rb = nvmtl_rebind_import(p, "_open", f); }\n'
        'if (h < 0 && nvmtl_vk_vendor() != 0x10de) { h = 1; }\n'
        'h = sysctlbyname("debug.nvaccel_headless", &v, &n, NULL, 0) == 0 && v;\n'
        'snprintf(dir, sizeof dir, "%snvmtl-mesa", ucd);\n'
        '    if (freev_out) *freev_out = freev;\n'
    )
    out_vk = transform_vk_c(transform_common(sample_vk))
    assert "return 0x8086;" in out_vk
    assert '"/Library/GPUBundles/arcmtl/libvulkan_intel.dylib"' in out_vk
    assert 'getenv("ARCMTL_ICD_LIB")' in out_vk
    assert "if (nvmtl_vk_vendor() == 0x8086) { int rb = nvmtl_rebind_import" in out_vk
    assert "nvmtl_vk_vendor() != 0x8086" in out_vk
    assert 'CFSTR("ArcAccelHeadless")' in out_vk
    assert '"%sarcmtl-mesa"' in out_vk
    assert "if (nvmtl_vk_vendor() == 0x8086) gate = 0;" in out_vk

    sample_obj = (
        '__attribute__((constructor)) static void nvmtl_conf_knobs(void)\n{\n    unsetenv("NVMTL_X");\n}\n'
        'static int nvmtl_dontcare_clears(void) { return 0; }\n'
        'static bool nvmtl_surfin_import(void) {\n'
        '    static int on = -1;\n'
        '    if (on < 0) { const char *u = getenv("NVMTL_SURFIN_UPLOAD"); on = !(u && !strcmp(u, "cpu")); }\n'
        '    return on;\n'
        '}\n'
        'static bool nvmtl_surfout_import(void) {\n'
        '    static int on = -1;\n'
        '    if (on < 0) { const char *u = getenv("NVMTL_SURFOUT_READBACK"); on = !(u && !strcmp(u, "cpu")); }\n'
        '    return on;\n'
        '}\n'
    )
    out_obj = transform_objects_m(transform_common(sample_obj))
    assert "arcmtl.conf" in out_obj
    assert "unsetenv" not in out_obj
    assert 'getenv("ARCMTL_SURFIN_UPLOAD")' in out_obj
    assert 'getenv("ARCMTL_SURFOUT_READBACK")' in out_obj
    assert '!strcmp(u, "gpu")' in out_obj

    sample_lib = (
        'bundled = [x stringByAppendingPathComponent:@"libnvmtl_translate.dylib"];\n'
        'const char *e = getenv("NVMTL_AIRCACHE");\n'
        'dir = [base stringByAppendingPathComponent:@"nvmtl/aircache"];\n'
        'tmp = [base stringByAppendingPathComponent:@"nvmtl/spvcache"];\n'
        'gCacheIoQueue = dispatch_queue_create("nvmtl.cache-io", 0);\n'
        '        NSString *path = [[@(info.dli_fname) stringByDeletingLastPathComponent]\n'
        '                         stringByAppendingPathComponent:@"air-runtime/libLLVM.dylib"];\n'
        '        void *library = dlopen(path.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);\n'
        '        if (!library) { nvlog("library: in-memory bitcode reader unavailable: %s", dlerror()); return; }\n'
    )
    out_lib = transform_library_load_m(transform_common(sample_lib))
    assert '@"libarcmtl_translate.dylib"' in out_lib
    assert '@"arcmtl/aircache"' in out_lib
    assert '@"arcmtl/spvcache"' in out_lib
    assert '"arcmtl.cache-io"' in out_lib
    assert 'getenv("ARCMTL_AIRCACHE")' in out_lib
    assert 'getenv("ARCMTL_LLVM_LIB")' in out_lib
    print("[adapt_arc_plugin] Self-test PASSED.")


def main() -> int:
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        run_self_test()
        return 0
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <nullmoth_plugin_dir> <output_dir>", file=sys.stderr)
        print(f"       {sys.argv[0]} --self-test", file=sys.stderr)
        return 2
    adapt_plugin_tree(Path(sys.argv[1]), Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
