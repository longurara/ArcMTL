#!/bin/bash
# build_anv.sh — Build Mesa Intel ANV Vulkan 1.3 ICD (libvulkan_intel.dylib) for macOS 15+ (ArcXe IOKit backend)
# License: MIT (Mesa ANV + Darwin/XNU IOKit transport shim)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESA_SRC="${MESA_SRC:-$ROOT/third_party/mesa}"
MESA_BUILD="$ROOT/build/mesa-anv-build"
OUT_DIR="$ROOT/out/arcmtl"

BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || echo "$BREW_PREFIX/opt/llvm")"
export PATH="$LLVM_PREFIX/bin:$BREW_PREFIX/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export PKG_CONFIG_PATH="$LLVM_PREFIX/lib/pkgconfig:$BREW_PREFIX/lib/pkgconfig:$BREW_PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

if [ ! -f "$MESA_SRC/meson.build" ]; then
    echo "[ERROR] Mesa source tree not found at $MESA_SRC"
    echo "        Run 'bash scripts/fetch_deps.sh' first."
    exit 1
fi

for tool in meson ninja pkg-config glslangValidator llvm-config; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "[ERROR] Required build tool '$tool' not found in PATH."
        echo "        Install dependencies on macOS with:"
        echo "          brew install meson ninja pkgconf bison flex glslang llvm spirv-llvm-translator spirv-tools libclc"
        echo "          python3 -m pip install --user --break-system-packages mako pyyaml packaging"
        exit 1
    fi
done

mkdir -p "$OUT_DIR"

# Always sync latest anv_xnu_drm_shim.{c,h}, arc_uapi.h, and arc_regs_mtl.h into Mesa tree
echo "[build_anv] Syncing ArcXe Darwin/XNU IOKit shim into Mesa..."
python3 "$ROOT/mesa-anv/apply_mesa_anv_macos.py" "$MESA_SRC"

MESON_ARGS=(
    -Dbuildtype=release
    -Dvulkan-drivers=intel
    -Dgallium-drivers=
    -Dplatforms=
    -Dopengl=false
    -Dglx=disabled
    -Degl=disabled
    -Dgbm=disabled
    -Dshared-glapi=disabled
    -Dintel-rt=disabled
    -Dzlib=enabled
    -Dzstd=disabled
    -Dxmlconfig=disabled
    -Dvalgrind=disabled
    -Dlibunwind=disabled
    -Dbuild-tests=false
)

if [ ! -f "$MESA_BUILD/build.ninja" ]; then
    echo "[build_anv] Configuring Mesa ANV with Meson..."
    meson setup "$MESA_BUILD" "$MESA_SRC" "${MESON_ARGS[@]}"
else
    echo "[build_anv] Reconfiguring existing Meson build dir ($MESA_BUILD)..."
    meson configure "$MESA_BUILD" "${MESON_ARGS[@]}"
fi

echo "[build_anv] Building libvulkan_intel.dylib..."
ninja -C "$MESA_BUILD" src/intel/vulkan/libvulkan_intel.dylib

BUILT_ICD="$MESA_BUILD/src/intel/vulkan/libvulkan_intel.dylib"
if [ ! -f "$BUILT_ICD" ]; then
    echo "[ERROR] $BUILT_ICD was not produced by ninja!"
    exit 2
fi

DEST="$OUT_DIR/libvulkan_intel.dylib"
cp -f "$BUILT_ICD" "$DEST"
chmod 755 "$DEST"

# Bundle and relocate any non-Apple /usr/local dylib dependency into @loader_path/
# so WindowServer (_windowserver) and sandboxed macOS apps never fail dlopen().
while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    case "$dep" in
        /usr/lib/*|/System/Library/*|@rpath/*|@loader_path/*|"$DEST"|/Library/GPUBundles/*)
            continue
            ;;
    esac
    if [ -f "$dep" ]; then
        base="$(basename "$dep")"
        echo "[build_anv] Bundling external runtime dep $dep -> $OUT_DIR/$base"
        cp -f "$dep" "$OUT_DIR/$base"
        chmod 755 "$OUT_DIR/$base"
        install_name_tool -id "@loader_path/$base" "$OUT_DIR/$base"
        codesign -f -s - "$OUT_DIR/$base"
        install_name_tool -change "$dep" "@loader_path/$base" "$DEST"
    fi
done < <(otool -L "$DEST" | tail -n +2 | awk '{print $1}')

strip -S "$DEST"
install_name_tool -id "/Library/GPUBundles/arcmtl/libvulkan_intel.dylib" "$DEST"
codesign -f -s - "$DEST"

cp -f "$ROOT/mesa-anv/intel_icd.json" "$OUT_DIR/intel_icd.json"

# Verify Vulkan ICD entrypoints exist
if ! nm -gU "$DEST" | awk '{print $3}' | grep -Eq '^(_vk_icdGetInstanceProcAddr|_vkGetInstanceProcAddr)$'; then
    echo "[ERROR] $DEST does not export vk_icdGetInstanceProcAddr / vkGetInstanceProcAddr!"
    exit 3
fi

echo "[build_anv] Built $DEST ($(shasum -a 256 "$DEST" | cut -c1-12))"
echo "[build_anv] Staged ICD manifest $OUT_DIR/intel_icd.json"
