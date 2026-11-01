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

echo "[build_plugin] Compiling ArcMTLDriver.bundle (x86_64 Mach-O bundle, ARC + hidden C symbol visibility)..."
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
(
    cd "$PLUGIN_BUILD_SRC"
    xcrun clang -arch x86_64 -mmacosx-version-min=15.0 -bundle -fobjc-arc -fvisibility=hidden -O2 $RDEF \
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

# Bundle self-contained air-runtime/libLLVM.dylib (+ relocated transitive deps such as libzstd)
# so WindowServer (_windowserver sandbox) decodes CoreDisplay/CoreAnimation AIR bitcode in-memory.
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm 2>/dev/null || echo "$BREW_PREFIX/opt/llvm")}"
SRC_LLVM=""
if [ -f "$LLVM_PREFIX/lib/libLLVM.dylib" ]; then
    SRC_LLVM="$LLVM_PREFIX/lib/libLLVM.dylib"
else
    for cand in "$LLVM_PREFIX"/lib/libLLVM-*.dylib; do
        if [ -f "$cand" ]; then
            SRC_LLVM="$cand"
            break
        fi
    done
fi
if [ ! -f "$SRC_LLVM" ]; then
    echo "[ERROR] libLLVM.dylib not found under $LLVM_PREFIX/lib (install Homebrew 'llvm')!"
    exit 4
fi

AIR_RUNTIME="$BUNDLE_MACOS/air-runtime"
AIR_LICENSES="$BUNDLE_CONTENTS/Resources/air-runtime"
mkdir -p "$AIR_RUNTIME" "$AIR_LICENSES"

echo "[build_plugin] Bundling in-memory AIR bitcode reader: $SRC_LLVM -> $AIR_RUNTIME/libLLVM.dylib"
cp -Lf "$SRC_LLVM" "$AIR_RUNTIME/libLLVM.dylib"
chmod 755 "$AIR_RUNTIME/libLLVM.dylib"
install_name_tool -id "@loader_path/libLLVM.dylib" "$AIR_RUNTIME/libLLVM.dylib"

for _pass in 1 2 3 4; do
    added=0
    for target in "$AIR_RUNTIME"/*.dylib; do
        [ -f "$target" ] || continue
        target_base="$(basename "$target")"
        while IFS= read -r dep; do
            [ -z "$dep" ] && continue
            case "$dep" in
                /usr/lib/*|/System/Library/*)
                    continue
                    ;;
            esac
            dep_base="$(basename "$dep")"
            if [ "$dep_base" = "$target_base" ]; then
                continue
            fi
            if [ "$target_base" = "libLLVM.dylib" ]; then
                case "$dep_base" in
                    libLLVM*.dylib)
                        install_name_tool -id "@loader_path/libLLVM.dylib" "$target"
                        continue
                        ;;
                esac
            fi
            resolved=""
            case "$dep" in
                @loader_path/*)
                    if [ ! -f "$AIR_RUNTIME/$dep_base" ]; then
                        for cand in "$LLVM_PREFIX/lib/$dep_base" "$LLVM_PREFIX/lib/c++/$dep_base" "$BREW_PREFIX/lib/$dep_base"; do
                            [ -f "$cand" ] && { resolved="$cand"; break; }
                        done
                    fi
                    ;;
                @rpath/*)
                    for cand in "$LLVM_PREFIX/lib/$dep_base" "$LLVM_PREFIX/lib/c++/$dep_base" "$BREW_PREFIX/lib/$dep_base"; do
                        [ -f "$cand" ] && { resolved="$cand"; break; }
                    done
                    ;;
                /*)
                    [ -f "$dep" ] && resolved="$dep"
                    ;;
            esac
            if [ -n "$resolved" ] && [ ! -f "$AIR_RUNTIME/$dep_base" ]; then
                echo "[build_plugin]   Bundling air-runtime dependency: $resolved -> $AIR_RUNTIME/$dep_base"
                cp -Lf "$resolved" "$AIR_RUNTIME/$dep_base"
                chmod 755 "$AIR_RUNTIME/$dep_base"
                install_name_tool -id "@loader_path/$dep_base" "$AIR_RUNTIME/$dep_base"
                added=1
            fi
            if [ -f "$AIR_RUNTIME/$dep_base" ] && [ "$dep" != "@loader_path/$dep_base" ]; then
                install_name_tool -change "$dep" "@loader_path/$dep_base" "$target"
            fi
        done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
    done
    [ "$added" -eq 0 ] && break
done

for d in "$AIR_RUNTIME"/*.dylib; do
    [ -f "$d" ] || continue
    chmod 755 "$d"
    install_name_tool -id "@loader_path/$(basename "$d")" "$d"
    strip -S "$d"
    codesign -f -s - "$d"
    echo "[build_plugin]   air-runtime/$(basename "$d") linkages:"
    otool -L "$d" | sed 's/^/      /'
    UNRELOCATED="$(otool -L "$d" | tail -n +2 | awk '{print $1}' | grep -E '^(/usr/local/|/opt/homebrew/|@rpath/)' || true)"
    if [ -n "$UNRELOCATED" ]; then
        echo "[ERROR] Unrelocated external path(s) remain in $d:"
        echo "$UNRELOCATED"
        exit 5
    fi
done

LLVM_SYMS="$(nm -gU "$AIR_RUNTIME/libLLVM.dylib")"
for req_sym in \
    _LLVMContextCreate \
    _LLVMParseBitcodeInContext2 \
    _LLVMPrintModuleToString \
    _LLVMRunPasses \
    _LLVMGetArrayLength2 \
    _LLVMConstInBoundsGEP2 \
    _LLVMGetGEPSourceElementType; do
    if ! grep -Fq "$req_sym" <<<"$LLVM_SYMS"; then
        echo "[ERROR] Required LLVM C API symbol '$req_sym' missing from $AIR_RUNTIME/libLLVM.dylib!"
        exit 6
    fi
done

for lf in "$LLVM_PREFIX"/LICENSE* "$LLVM_PREFIX"/share/doc/llvm/LICENSE*; do
    if [ -f "$lf" ]; then
        cp -f "$lf" "$AIR_LICENSES/LICENSE-LLVM.txt"
        break
    fi
done
for zf in "$BREW_PREFIX"/opt/zstd/LICENSE* "$BREW_PREFIX"/Cellar/zstd/*/LICENSE*; do
    if [ -f "$zf" ]; then
        cp -f "$zf" "$AIR_LICENSES/LICENSE-zstd.txt"
        break
    fi
done

cp -f "$ROOT/plugin/Info.plist" "$BUNDLE_CONTENTS/Info.plist"
cp -f "$ROOT/plugin/arcmtl-allow.txt" "$OUT_GPUBUNDLES/arcmtl-allow.txt"

# Verify Dual-GPU Objective-C runtime class & C symbol isolation
PLUGIN_SYMS="$(nm -gU "$BUNDLE_MACOS/ArcMTLDriver")"
if ! grep -Fq '_OBJC_CLASS_$_ArcMTLDevice' <<<"$PLUGIN_SYMS"; then
    echo "[ERROR] _OBJC_CLASS_\$_ArcMTLDevice missing from $BUNDLE_MACOS/ArcMTLDriver!"
    exit 2
fi
if ! grep -Fq '_arcmtl_self_test_metallib' <<<"$PLUGIN_SYMS"; then
    echo "[ERROR] _arcmtl_self_test_metallib missing from $BUNDLE_MACOS/ArcMTLDriver!"
    exit 2
fi

COLLISIONS="$(grep -F '_OBJC_CLASS_$_NVMTL' <<<"$PLUGIN_SYMS" || true)"
if [ -n "$COLLISIONS" ]; then
    echo "[ERROR] Found colliding NVMTL* ObjC class symbols in ArcMTLDriver:"
    echo "$COLLISIONS"
    exit 3
fi

LEAKED_C="$(grep -E ' (_nvmtl_vk_init|_nvlog|_gNVMTLMainDevice)$' <<<"$PLUGIN_SYMS" || true)"
if [ -n "$LEAKED_C" ]; then
    echo "[ERROR] Internal C symbols leaked into global export table of ArcMTLDriver:"
    echo "$LEAKED_C"
    exit 7
fi

codesign --force --deep --sign - "$BUNDLE_DIR"

echo "[build_plugin] Built $BUNDLE_DIR ($(shasum -a 256 "$BUNDLE_MACOS/ArcMTLDriver" | cut -c1-12))"
echo "[build_plugin] Verified bundled air-runtime/libLLVM.dylib + zero NVMTL* class or C symbol collisions."
