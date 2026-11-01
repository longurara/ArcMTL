#!/bin/bash
# build_all.sh — Master build orchestrator for the Intel Arc (Xe-LPG 8086:7D55) macOS 15+ driver stack
#
# Builds:
#   1. Kernel Extensions : out/kexts/{ArcXe.kext, ArcFB.kext, ArcAccel.kext}
#   2. GuC v70 Firmware  : out/fw/mtl_guc_70.bin
#   3. AIR->SPIR-V Xlate : out/arcmtl/libarcmtl_translate.dylib
#   4. Mesa ANV Vulkan   : out/arcmtl/{libvulkan_intel.dylib, intel_icd.json}
#   5. Metal 3 Bundle    : out/GPUBundles/{ArcMTLDriver.bundle, arcmtl-allow.txt}
#   6. Diagnostic Tools  : out/tools/{arc_probe, vk_offscreen_test, mtl_offscreen_test}
#
# Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
# License: PolyForm Noncommercial License 1.0.0

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[ERROR] build_all.sh must be executed on macOS 15 Sequoia (or 26 Tahoe)."
    exit 1
fi

echo "=================================================================="
echo " Building Intel Arc (Xe-LPG 8086:7D55) Driver Stack for macOS"
echo " Host OS: $(sw_vers -productName) $(sw_vers -productVersion) ($(uname -r))"
echo "=================================================================="

if [ ! -d "$ROOT/third_party/Vulkan-Headers" ] || \
   [ ! -d "$ROOT/third_party/mesa" ] || \
   [ ! -d "$ROOT/third_party/nullmoth" ]; then
    echo "[Init] Third-party sources missing — running scripts/fetch_deps.sh..."
    bash "$ROOT/scripts/fetch_deps.sh"
fi

mkdir -p "$OUT/kexts" "$OUT/fw" "$OUT/arcmtl" "$OUT/GPUBundles" "$OUT/tools"

# 1. Kernel Extensions (ArcXe, ArcFB + ArcAccel for BOTH macOS 15 Sequoia & macOS 26 Tahoe)
echo ""
echo "[Step 1/5] Building Kernel Extensions (ArcXe, ArcFB, ArcAccel [macOS 15 & macOS 26])..."
bash "$ROOT/kexts/ArcXe/build.sh" "$OUT/kexts"
bash "$ROOT/kexts/ArcFB/build.sh" "$OUT/kexts"
ARC_TAHOE=0 bash "$ROOT/kexts/ArcAccel/build.sh" "$OUT/kexts/15"
ARC_TAHOE=1 bash "$ROOT/kexts/ArcAccel/build.sh" "$OUT/kexts/26"

HOST_VER="$(sw_vers -productVersion)"
HOST_MAJ="${HOST_VER%%.*}"
if [ "$HOST_MAJ" = "26" ]; then
    ditto "$OUT/kexts/26/ArcAccel.kext" "$OUT/kexts/ArcAccel.kext"
else
    ditto "$OUT/kexts/15/ArcAccel.kext" "$OUT/kexts/ArcAccel.kext"
fi

# 2. GuC v70 Firmware
echo ""
echo "[Step 2/5] Staging Intel Meteor Lake GuC v70 firmware..."
cp -f "$ROOT/firmware/mtl_guc_70.bin" "$OUT/fw/mtl_guc_70.bin"
echo "  Staged $OUT/fw/mtl_guc_70.bin ($(stat -f %z "$OUT/fw/mtl_guc_70.bin") bytes)"

# 3. Rust AIR -> SPIR-V Translator
echo ""
echo "[Step 3/5] Building Rust AIR -> SPIR-V translator (libarcmtl_translate.dylib)..."
bash "$ROOT/build/build_xlate.sh"

# 4. Mesa ANV Vulkan 1.3 ICD
echo ""
echo "[Step 4/5] Building Mesa Intel ANV Vulkan 1.3 ICD (libvulkan_intel.dylib)..."
bash "$ROOT/build/build_anv.sh"

# 5. ArcMTLDriver.bundle + Staged Verification Tools
echo ""
echo "[Step 5/5] Building ArcMTLDriver.bundle & Staged Verification CLI Tools..."
bash "$ROOT/build/build_plugin.sh"

VULKAN_INC="$ROOT/third_party/Vulkan-Headers/include"

xcrun clang -arch x86_64 -O2 -Wall \
    -I"$ROOT/include" \
    "$ROOT/tools/arc_probe.c" \
    -o "$OUT/tools/arc_probe" \
    -framework IOKit -framework CoreFoundation

xcrun clang -arch x86_64 -O2 -Wall \
    -I"$VULKAN_INC" \
    "$ROOT/tools/vk_offscreen_test.c" \
    -o "$OUT/tools/vk_offscreen_test"

xcrun clang -arch x86_64 -fobjc-arc -O2 -Wall \
    "$ROOT/tools/mtl_offscreen_test.m" \
    -o "$OUT/tools/mtl_offscreen_test" \
    -framework Foundation -framework Metal -framework IOSurface

codesign -f -s - "$OUT/tools/arc_probe" "$OUT/tools/vk_offscreen_test" "$OUT/tools/mtl_offscreen_test"

echo ""
echo "=================================================================="
echo " [SUCCESS] Complete Intel Arc macOS Driver Stack Built!"
echo "=================================================================="
echo "  Kexts      : $OUT/kexts/{ArcXe.kext, ArcFB.kext, ArcAccel.kext}"
echo "  Firmware   : $OUT/fw/mtl_guc_70.bin"
echo "  Vulkan/AIR : $OUT/arcmtl/{libvulkan_intel.dylib, libarcmtl_translate.dylib, intel_icd.json}"
echo "  GPUBundle  : $OUT/GPUBundles/{ArcMTLDriver.bundle, arcmtl-allow.txt}"
echo "  CLI Tools  : $OUT/tools/{arc_probe, vk_offscreen_test, mtl_offscreen_test}"
echo ""
echo "  Next step (safe co-existing install preserving NullMoth RTX 4070):"
echo "    sudo bash package/install.sh --stage1     # Stage 1/2: ArcXe.kext + userspace only"
echo "    sudo bash package/install.sh              # Full stack: ArcXe + ArcFB + ArcAccel + Metal 3"
echo "=================================================================="
