#!/bin/bash
# package/install_opencore_userspace.sh — Userspace-Only Installer when Arc*.kext are injected via OpenCore
#
# Use this script inside macOS (15 Sequoia or 26 Tahoe) AFTER OpenCore (EFI/OC/Kexts) has injected:
#   - ArcXe.kext
#   - ArcFB.kext
#   - ArcAccel15.kext (macOS 15) / ArcAccel26.kext (macOS 26)
#
# Why userspace-only?
#   - Does NOT touch /Library/Extensions or AuxiliaryKernelExtensions.kc (zero kmutil rebuild, zero risk to NullMoth!).
#   - Installs GuC v70 firmware, Mesa 26.2.6 ANV Vulkan ICD, SPIR-V translator, ArcMTLDriver.bundle, and test tools.
#   - Immediately runs live GPU hardware verification (`arc_probe` + `vk_offscreen_test`).
#
# Optional flags:
#   sudo bash install_opencore_userspace.sh                     # Install userspace + run live GPU & Vulkan tests
#   sudo bash install_opencore_userspace.sh --enable-dual-metal # Also add `arc_headless=1` to mounted EFI config.plist
#   sudo bash install_opencore_userspace.sh --enable-arc-desktop# Also add `arcaccel=1` to mounted EFI config.plist

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$SCRIPT_DIR/out" ]; then
    OUT="$SCRIPT_DIR/out"
elif [ -d "$SCRIPT_DIR/../out" ]; then
    OUT="$(cd "$SCRIPT_DIR/../out" && pwd)"
elif [ -f "$SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz" ]; then
    TMP_EXTRACT="$(mktemp -d /var/tmp/arcmtl_extract.XXXXXX)"
    trap 'rm -rf "$TMP_EXTRACT"' EXIT
    echo "[*] Extracting $SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz ..."
    tar -xzf "$SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz" -C "$TMP_EXTRACT"
    if [ -d "$TMP_EXTRACT/ArcMTL-macos26-15-x86_64/out" ]; then
        OUT="$TMP_EXTRACT/ArcMTL-macos26-15-x86_64/out"
    else
        OUT="$TMP_EXTRACT/out"
    fi
else
    echo "[ERROR] Cannot locate out/ directory or ArcMTL-macos26-15-x86_64.tar.gz next to script." >&2
    exit 1
fi

MODE="${1:-}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[ERROR] Must be run inside macOS." >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] Run with sudo: sudo bash $0 $MODE" >&2
    exit 1
fi

OS_VER="$(sw_vers -productVersion)"
echo "=================================================================="
echo " ArcMTL Userspace Installer (OpenCore Kext Injection Mode)"
echo " macOS $OS_VER | Preserving NullMoth RTX 4070 AuxKC untouched"
echo "=================================================================="

echo ""
echo "[1/4] Checking OpenCore-injected Arc kexts in running kernel..."
kextstat | grep -E "com\.arc\.|com\.nullmoth\." || true

echo ""
echo "[2/4] Installing GuC v70 firmware & diagnostic tools..."
mkdir -p /Library/ArcDriver/fw /usr/local/bin
install -m 644 "$OUT/fw/mtl_guc_70.bin" /Library/ArcDriver/fw/mtl_guc_70.bin
chown -R root:wheel /Library/ArcDriver

for tool in arc_probe vk_offscreen_test mtl_offscreen_test; do
    if [ -f "$OUT/tools/$tool" ]; then
        install -m 755 "$OUT/tools/$tool" "/usr/local/bin/$tool"
        codesign --force --sign - "/usr/local/bin/$tool" 2>/dev/null || true
    fi
done

echo ""
echo "[3/4] Installing Mesa 26.2.6 ANV (/Library/GPUBundles/arcmtl) & ArcMTLDriver.bundle..."
GB="/Library/GPUBundles"
mkdir -p "$GB/arcmtl"

rm -rf "$GB/arcmtl/libvulkan_intel.dylib" \
       "$GB/arcmtl/libarcmtl_translate.dylib" \
       "$GB/arcmtl/intel_icd.json"
ditto "$OUT/arcmtl" "$GB/arcmtl"
for dylib in "$GB/arcmtl"/*.dylib; do
    [ -f "$dylib" ] || continue
    codesign --force --sign - "$dylib"
done
chown -R root:wheel "$GB/arcmtl"
chmod -R 755 "$GB/arcmtl"

rm -rf "$GB/ArcMTLDriver.bundle"
ditto "$OUT/GPUBundles/ArcMTLDriver.bundle" "$GB/ArcMTLDriver.bundle"
codesign --force --deep --sign - "$GB/ArcMTLDriver.bundle"
chown -R root:wheel "$GB/ArcMTLDriver.bundle"
chmod -R 755 "$GB/ArcMTLDriver.bundle"
install -m 644 "$OUT/GPUBundles/arcmtl-allow.txt" "$GB/arcmtl-allow.txt"
chown root:wheel "$GB/arcmtl-allow.txt"
xattr -cr "$GB/ArcMTLDriver.bundle" "$GB/arcmtl" 2>/dev/null || true

echo ""
echo "[4/4] Running live Intel Arc Xe-LPG hardware & Vulkan tests..."
if [ -x /usr/local/bin/arc_probe ]; then
    echo "--- Running arc_probe ---"
    /usr/local/bin/arc_probe || echo "[WARN] arc_probe returned non-zero (check if BIOS is in Optimus mode and ArcXe.kext attached)"
fi
if [ -x /usr/local/bin/vk_offscreen_test ]; then
    echo "--- Running vk_offscreen_test (Mesa 26.2.6 ANV on XNU) ---"
    VK_ICD_FILENAMES=/Library/GPUBundles/arcmtl/intel_icd.json /usr/local/bin/vk_offscreen_test /var/tmp/arc_vk_test.ppm \
        && echo "[OK] Vulkan offscreen render saved to /var/tmp/arc_vk_test.ppm" \
        || echo "[WARN] vk_offscreen_test returned non-zero"
fi

update_oc_boot_arg() {
    local add_arg="$1"
    local remove_arg="$2"
    local plist=""
    for candidate in /Volumes/EFI/EFI/OC/config.plist /Volumes/ESP/EFI/OC/config.plist "$SCRIPT_DIR/../EFI/OC/config.plist"; do
        if [ -f "$candidate" ]; then
            plist="$candidate"
            break
        fi
    done
    if [ -z "$plist" ]; then
        echo "[INFO] EFI partition not currently mounted at /Volumes/EFI."
        echo "       Run 'sudo diskutil mount EFI' and re-run with $MODE, or add '$add_arg' to boot-args manually."
        return 0
    fi
    cp "$plist" "${plist}.bak-userspace-$(date +%Y%m%d-%H%M%S)"
    python3 - "$plist" "$add_arg" "$remove_arg" <<'PY'
import sys, plistlib
path, add_arg, rem_arg = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, 'rb') as f:
    cfg = plistlib.load(f)
nv = cfg['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
tokens = [t for t in nv.get('boot-args', '').split() if t != rem_arg and t != add_arg]
tokens.append(add_arg)
nv['boot-args'] = ' '.join(tokens)
with open(path, 'wb') as f:
    plistlib.dump(cfg, f, sort_keys=False)
print(f"[OK] Updated {path} boot-args -> {nv['boot-args']}")
PY
}

if [ "$MODE" = "--enable-dual-metal" ]; then
    echo ""
    update_oc_boot_arg "arc_headless=1" "arcaccel=1"
elif [ "$MODE" = "--enable-arc-desktop" ]; then
    echo ""
    update_oc_boot_arg "arcaccel=1" "arc_headless=1"
fi

echo ""
echo "=================================================================="
echo " Userspace installation COMPLETE!"
echo " - To enable Intel Arc as 2nd Metal 3 GPU alongside RTX 4070:"
echo "     sudo diskutil mount EFI"
echo "     sudo bash $0 --enable-dual-metal && sudo reboot"
echo " - To enable Intel Arc full Metal 3 Desktop DisplayPipe on ArcFB:"
echo "     sudo diskutil mount EFI"
echo "     sudo bash $0 --enable-arc-desktop && sudo reboot"
echo " - After reboot, test Metal 3:"
echo "     /usr/local/bin/mtl_offscreen_test /var/tmp/arc_mtl_test.ppm"
echo "=================================================================="
