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

    return REQUIRED_NOTICE_BANNER + text


def transform_library_load_m(text: str) -> str:
    # Allow ARCMTL_XLATE_LIB env override for pre-install testing from out/arcmtl/libarcmtl_translate.dylib
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
    # Default IOSurface upload (nvmtlSurfaceIn) and readback (nvmtlSurfaceOutDirtied:) on Intel Arc UMA
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

    # 5. Isolate accelerator surface-wait query from NVAccel -> ArcAccel
    text = text.replace('IOServiceMatching("NVAccel")', 'IOServiceMatching("ArcAccel")')
    text = text.replace('"NVAccelSurfaceWait"', '"ArcAccelSurfaceWait"')

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
        '@interface NVMTLDevice : MTLIOAccelDevice\n@end\n'
        '#import "NVMTLObjects.h"\n'
        'static bool nvmtl_accel_armed(void)\n'
        '{\n'
        '    int v = 0; size_t n = sizeof v;\n'
        '    if (sysctlbyname("debug.nvaccelfb", &v, &n, NULL, 0) != 0) return false;\n'
        '    return v != 0;\n'
        '}\n'
        'if (!strcmp(me, "WindowServer") && (nvmtl_vk_vendor() != 0x10de || '
        '(sysctlbyname("debug.nvaccel_headless", &hv, &hn, NULL, 0) == 0 && hv))) return false;\n'
        'if (getenv("NVMTL_ENABLE")) return true;\n'
        'FILE *f = fopen("/Library/GPUBundles/nvmtl-allow.txt", "r");\n'
        'const char *k = "NVMTLCommandBuffer";\n'
        'id x = objc_getAssociatedObject(p, &kNVMTLMeshInfo);\n'
        'void *fn = dlsym(h, "nvmtl_translate");\n'
    )
    out_dev = transform_device_m(transform_common(sample_dev))
    assert "@interface ArcMTLDevice" in out_dev
    assert '#import "ArcMTLObjects.h"' in out_dev
    assert 'CFSTR("ArcAccelArmed")' in out_dev
    assert 'CFSTR("ArcAccelHeadless")' in out_dev
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
    )
    out_vk = transform_vk_c(transform_common(sample_vk))
    assert "return 0x8086;" in out_vk
    assert '"/Library/GPUBundles/arcmtl/libvulkan_intel.dylib"' in out_vk
    assert 'getenv("ARCMTL_ICD_LIB")' in out_vk
    assert "if (nvmtl_vk_vendor() == 0x8086) { int rb = nvmtl_rebind_import" in out_vk
    assert "nvmtl_vk_vendor() != 0x8086" in out_vk
    assert 'CFSTR("ArcAccelHeadless")' in out_vk

    sample_obj = (
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
    assert 'getenv("ARCMTL_SURFIN_UPLOAD")' in out_obj
    assert 'getenv("ARCMTL_SURFOUT_READBACK")' in out_obj
    assert '!strcmp(u, "gpu")' in out_obj
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
