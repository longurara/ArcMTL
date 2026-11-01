#!/bin/bash
# build_xlate.sh — Build the AIR/Bitcode -> SPIR-V Rust translator (libarcmtl_translate.dylib)
# Derived from metal2vulkan / NullMoth translator (LGPL-3.0).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XLATE_SRC="${XLATE_SRC:-$ROOT/third_party/nullmoth/translator}"
TARGET_DIR="$ROOT/build/xlate-target"
OUT_DIR="$ROOT/out/arcmtl"

BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
export PATH="$HOME/.cargo/bin:$BREW_PREFIX/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"

if [ ! -f "$XLATE_SRC/wrapper/Cargo.toml" ]; then
    echo "[ERROR] Translator source not found at $XLATE_SRC/wrapper/Cargo.toml"
    echo "        Run 'bash scripts/fetch_deps.sh' first."
    exit 1
fi

mkdir -p "$TARGET_DIR" "$OUT_DIR"

echo "[build_xlate] Compiling AIR -> SPIR-V translator (x86_64-apple-darwin, release)..."
(
    cd "$XLATE_SRC"
    cargo build --release \
        --target x86_64-apple-darwin \
        --manifest-path wrapper/Cargo.toml \
        --target-dir "$TARGET_DIR"
)

BUILT_DYLIB="$TARGET_DIR/x86_64-apple-darwin/release/libnvmtl_translate.dylib"
if [ ! -f "$BUILT_DYLIB" ]; then
    BUILT_DYLIB="$TARGET_DIR/release/libnvmtl_translate.dylib"
fi
if [ ! -f "$BUILT_DYLIB" ]; then
    echo "[ERROR] Expected libnvmtl_translate.dylib not produced under $TARGET_DIR"
    exit 2
fi

DEST="$OUT_DIR/libarcmtl_translate.dylib"
cp -f "$BUILT_DYLIB" "$DEST"
strip -S "$DEST"
install_name_tool -id "/Library/GPUBundles/arcmtl/libarcmtl_translate.dylib" "$DEST"
codesign -f -s - "$DEST"

# Verify essential translator C entrypoints expected by ArcMTLLibraryLoad.m
for sym in _nvmtl_translate _nvmtl_translate_free _nvmtl_reflect _nvmtl_lower_buffer_addresses; do
    if ! nm -gU "$DEST" | awk '{print $3}' | grep -Fxq "$sym"; then
        echo "[ERROR] Missing required exported symbol '$sym' in $DEST"
        exit 3
    fi
done

echo "[build_xlate] Built $DEST ($(shasum -a 256 "$DEST" | cut -c1-12))"
