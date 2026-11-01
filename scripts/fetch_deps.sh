#!/bin/bash
# fetch_deps.sh — Fetch and patch third-party dependencies for the Intel Arc macOS 15+ driver stack
# Run on macOS 15 Sequoia (or 26 Tahoe) from anywhere; paths resolve relative to repo root.
#
# Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
# License: PolyForm Noncommercial License 1.0.0

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
MESA_TAG="${MESA_TAG:-mesa-25.0.7}"
FORCE="${1:-}"

mkdir -p "$TP" "$ROOT/build"

echo "=================================================================="
echo " Intel Arc (Xe-LPG 8086:7D55) macOS Driver — Dependency Fetcher"
echo " Workspace: $ROOT"
echo "=================================================================="

# 1. Host build toolchain preflight (non-fatal during fetch, explicit instructions if missing)
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /usr/local)"
export PATH="$BREW_PREFIX/opt/llvm/bin:$BREW_PREFIX/bin:$HOME/.cargo/bin:$PATH"

MISSING=()
for cmd in git python3 cargo meson ninja pkg-config bison flex glslangValidator llvm-config; do
    command -v "$cmd" >/dev/null 2>&1 || MISSING+=("$cmd")
done
if [ "${#MISSING[@]}" -gt 0 ]; then
    echo "[WARN] Missing build tool(s): ${MISSING[*]}"
    echo "       Before running build/build_all.sh on macOS, install them via Homebrew:"
    echo "         brew install rust meson ninja pkgconf bison flex glslang llvm spirv-llvm-translator spirv-tools libclc"
    echo "         python3 -m pip install --user --break-system-packages mako pyyaml packaging"
    echo ""
fi

# 2. Khronos Vulkan-Headers
if [ "$FORCE" = "--force" ]; then
    rm -rf "$TP/Vulkan-Headers" "$TP/mesa" "$TP/nullmoth"
fi

if [ ! -d "$TP/Vulkan-Headers/.git" ]; then
    echo "[1/3] Cloning KhronosGroup/Vulkan-Headers..."
    git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$TP/Vulkan-Headers"
else
    echo "[1/3] Vulkan-Headers already present ($TP/Vulkan-Headers)"
fi

# 3. Upstream Mesa (tag $MESA_TAG) for Intel ANV (Xe-LPG 12.70)
if [ ! -d "$TP/mesa/.git" ]; then
    echo "[2/3] Cloning Mesa ($MESA_TAG)..."
    git clone --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$TP/mesa" \
      || git clone --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git "$TP/mesa"
else
    echo "[2/3] Mesa already present ($TP/mesa)"
fi

# 4. NullMoth sparse checkout (LICENSE, NOTICE, translator/, plugin/)
if [ ! -d "$TP/nullmoth/.git" ]; then
    echo "[3/3] Sparse-cloning nullmoth/nvidia-macos-driver (translator + plugin + LICENSE)..."
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/nullmoth/nvidia-macos-driver.git "$TP/nullmoth"
    (
        cd "$TP/nullmoth"
        git sparse-checkout set --no-cone /LICENSE /NOTICE '/translator/**' '/plugin/**'
    )
else
    echo "[3/3] NullMoth tree already present ($TP/nullmoth)"
fi

# 5. Apply Darwin/XNU IOKit shim to Mesa ANV & generate ArcMTLDriver ObjC sources
echo ""
echo "[Patch 1/2] Applying Darwin/XNU ArcXe IOKit transport to Mesa ANV..."
python3 "$ROOT/mesa-anv/apply_mesa_anv_macos.py" "$TP/mesa"

echo "[Patch 2/2] Adapting NullMoth plugin -> ArcMTLDriver (build/arc-plugin-src)..."
python3 "$ROOT/plugin/adapt_arc_plugin.py" "$TP/nullmoth/plugin" "$ROOT/build/arc-plugin-src"

echo ""
echo "[OK] All dependencies fetched and patched."
echo "     Next step on macOS 15+:  bash build/build_all.sh"
