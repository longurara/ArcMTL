#!/bin/bash
# build_plugin.sh — Build ArcMTLDriver.bundle (Metal 3 userspace plugin for Intel Arc Xe-LPG)
#
# Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
# License: PolyForm Noncommercial License 1.0.0

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NULLMOTH_PLUGIN_SRC="${NULLMOTH_PLUGIN_SRC:-$ROOT/third_party/nullmoth/plugin}"
VULKAN_INC="${VULKAN_INC:-$ROOT/third_party/Vulkan-Headers/include}"
PLUGIN_BUILD_SRC="$ROOT/build/arc-plugin-src"

OUT_GPUBUNDLES="$ROOT/out/GPUBundles"
BUNDLE_DIR="$OUT_GPUBUNDLES/ArcMTLDriver.bundle"
BUNDLE_CONTENTS="$BUNDLE_DIR/Contents"
BUNDLE_MACOS="$BUNDLE_CONTENTS/MacOS"

if [ ! -f "$NULLMOTH_PLUGIN_SRC/NVMTLDevice.m" ]; then
    echo "[ERROR] NullMoth plugin source not found at $NULLMOTH_PLUGIN_SRC"
    echo "        Run 'bash scripts/fetch_deps.sh' first."
    exit 1
fi
if [ ! -f "$VULKAN_INC/vulkan/vulkan_core.h" ]; then
    echo "[ERROR] Vulkan-Headers not found at $VULKAN_INC"
    echo "        Run 'bash scripts/fetch_deps.sh' first."
    exit 1
fi

echo "[build_plugin] Adapting NullMoth plugin sources -> $PLUGIN_BUILD_SRC..."
python3 "$ROOT/plugin/adapt_arc_plugin.py" "$NULLMOTH_PLUGIN_SRC" "$PLUGIN_BUILD_SRC"

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_MACOS" "$OUT_GPUBUNDLES"

RDEF=""
if [ "${RELEASE:-1}" = "1" ]; then
    RDEF="-DNVMTL_RELEASE=1"
fi

echo "[build_plugin] Compiling ArcMTLDriver.bundle (x86_64 Mach-O bundle, ARC enabled)..."
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
(
    cd "$PLUGIN_BUILD_SRC"
    xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -bundle -fobjc-arc -O2 $RDEF \
        -Wno-protocol -Wno-incomplete-implementation -Wno-deprecated-declarations \
        -Wno-objc-protocol-method-implementation \
        -I"$VULKAN_INC" -I"$PLUGIN_BUILD_SRC" \
        -o "$BUNDLE_MACOS/ArcMTLDriver" \
        "$PLUGIN_BUILD_SRC/ArcMTLDevice.m" \
        -framework Foundation \
        -framework Metal \
        -framework IOSurface \
        -framework IOKit \
        -framework QuartzCore
)

cp -f "$ROOT/plugin/Info.plist" "$BUNDLE_CONTENTS/Info.plist"
cp -f "$ROOT/plugin/arcmtl-allow.txt" "$OUT_GPUBUNDLES/arcmtl-allow.txt"

# Verify Dual-GPU Objective-C runtime class isolation (ArcMTLDevice present, zero NVMTL* classes)
PLUGIN_SYMS="$(nm -gU "$BUNDLE_MACOS/ArcMTLDriver")"
if ! grep -Fq '_OBJC_CLASS_$_ArcMTLDevice' <<<"$PLUGIN_SYMS"; then
    echo "[ERROR] _OBJC_CLASS_\$_ArcMTLDevice missing from $BUNDLE_MACOS/ArcMTLDriver!"
    exit 2
fi

COLLISIONS="$(grep -F '_OBJC_CLASS_$_NVMTL' <<<"$PLUGIN_SYMS" || true)"
if [ -n "$COLLISIONS" ]; then
    echo "[ERROR] Found colliding NVMTL* ObjC class symbols in ArcMTLDriver:"
    echo "$COLLISIONS"
    exit 3
fi

codesign -f -s - "$BUNDLE_DIR"

echo "[build_plugin] Built $BUNDLE_DIR ($(shasum -a 256 "$BUNDLE_MACOS/ArcMTLDriver" | cut -c1-12))"
echo "[build_plugin] Verified zero NVMTL* Objective-C class collisions (safe for RTX 4070 + Intel Arc Dual-GPU)."
