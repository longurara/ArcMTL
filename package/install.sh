#!/bin/bash
# package/install.sh — Safe Co-existing Installer for Intel Arc (Xe-LPG 8086:7D55) on macOS 15+
#
# CRITICAL SAFETY GUARANTEES FOR DUAL-GPU (NullMoth RTX 4070 + Intel Arc 155H):
#   1. Scans and preserves EVERY existing kext in /Library/Extensions (NVRM.kext, NVAccel.kext,
#      NVRMFB.kext, NVRMAGDC.kext, NVPrimarySelector.kext, etc.).
#   2. Never touches /Library/GPUBundles/{NVMTLDriver.bundle, NVIDIAShared.bundle, nvmtl, nvmtl-allow.txt}.
#   3. Pre-flights `kmutil create -n aux` into /var/tmp and verifies BOTH com.arc.* AND any existing
#      com.nullmoth.* bundle IDs are present in the test AuxKC BEFORE modifying /Library/Extensions
#      or /Library/KernelCollections/AuxiliaryKernelExtensions.kc.
#   4. Creates a timestamped backup under /Library/ArcDriver/backups/<timestamp>.
#
# Usage:
#   sudo bash package/install.sh --stage1   # Install ArcXe.kext + firmware + userspace only (Stages 1 & 2)
#   sudo bash package/install.sh            # Install full stack: ArcXe + ArcFB + ArcAccel + Metal 3 (Stages 3 & 4)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/out"
MODE="${1:---full}"

die() {
    echo "" >&2
    echo "[ERROR] $*" >&2
    echo "        No live kernel collection changes were committed (or restore via: sudo bash package/uninstall.sh)." >&2
    exit 1
}

if [ "$(uname -s)" != "Darwin" ]; then
    die "install.sh must be run on macOS 15 Sequoia (or 26 Tahoe)."
fi
if [ "$(id -u)" -ne 0 ]; then
    die "install.sh must be run as root (use: sudo bash package/install.sh)."
fi

if [ "$MODE" = "--stage1" ]; then
    ARC_KEXTS=("ArcXe")
    echo "=================================================================="
    echo " Installing Intel Arc Driver — STAGE 1/2 MODE (ArcXe.kext + ANV)"
    echo " (ArcFB & ArcAccel omitted; RTX 4070 desktop completely untouched)"
    echo "=================================================================="
elif [ "$MODE" = "--full" ]; then
    ARC_KEXTS=("ArcXe" "ArcFB" "ArcAccel")
    echo "=================================================================="
    echo " Installing Intel Arc Driver — FULL STACK (Dual-GPU & Standalone)"
    echo " (Kexts: ArcXe, ArcFB, ArcAccel + Mesa ANV + ArcMTLDriver.bundle)"
    echo "=================================================================="
else
    die "Unknown option '$MODE'. Expected '--stage1' or '--full' (default)."
fi

# 1. Verify all built artifacts exist in out/
for k in "${ARC_KEXTS[@]}"; do
    [ -f "$OUT/kexts/$k.kext/Contents/MacOS/$k" ] || die "Missing built kext $OUT/kexts/$k.kext (run 'bash build/build_all.sh' first)"
    [ -f "$OUT/kexts/$k.kext/Contents/Info.plist" ] || die "Missing Info.plist in $OUT/kexts/$k.kext"
done
[ -f "$OUT/fw/mtl_guc_70.bin" ] || die "Missing $OUT/fw/mtl_guc_70.bin"
[ -f "$OUT/arcmtl/libvulkan_intel.dylib" ] || die "Missing $OUT/arcmtl/libvulkan_intel.dylib"
[ -f "$OUT/arcmtl/libarcmtl_translate.dylib" ] || die "Missing $OUT/arcmtl/libarcmtl_translate.dylib"
[ -f "$OUT/arcmtl/intel_icd.json" ] || die "Missing $OUT/arcmtl/intel_icd.json"
[ -f "$OUT/GPUBundles/ArcMTLDriver.bundle/Contents/MacOS/ArcMTLDriver" ] || die "Missing $OUT/GPUBundles/ArcMTLDriver.bundle"
[ -f "$OUT/GPUBundles/arcmtl-allow.txt" ] || die "Missing $OUT/GPUBundles/arcmtl-allow.txt"

# 2. Locate Boot & System Kernel Collections for kmutil
KB=""
for p in /System/Library/KernelCollections/BootKernelExtensions.kc \
         /Library/Apple/System/Library/KernelCollections/BootKernelExtensions.kc; do
    [ -f "$p" ] && { KB="$p"; break; }
done
KS=""
for p in /System/Library/KernelCollections/SystemKernelExtensions.kc \
         /Library/Apple/System/Library/KernelCollections/SystemKernelExtensions.kc; do
    [ -f "$p" ] && { KS="$p"; break; }
done
[ -n "$KB" ] && [ -n "$KS" ] || die "Could not locate BootKernelExtensions.kc / SystemKernelExtensions.kc"

LE="/Library/Extensions"
GB="/Library/GPUBundles"
KC_DIR="/Library/KernelCollections"
AUX_KC="$KC_DIR/AuxiliaryKernelExtensions.kc"

STAGE_DIR="$(mktemp -d /var/tmp/arc_kmutil_precheck.XXXXXX)"
trap 'rm -rf "$STAGE_DIR"' EXIT
STAGE_EXT="$STAGE_DIR/Extensions"
mkdir -p "$STAGE_EXT"

# 3. Stage EVERY existing non-Arc kext from /Library/Extensions into $STAGE_EXT
PRESERVED_IDS=()
echo ""
echo "[1/5] Scanning existing kexts in $LE to preserve alongside Arc..."
if [ -d "$LE" ]; then
    for existing in "$LE"/*.kext; do
        [ -d "$existing" ] || continue
        bname="$(basename "$existing")"
        case "$bname" in
            ArcXe.kext|ArcFB.kext|ArcAccel.kext)
                continue
                ;;
        esac
        if [ -f "$existing/Contents/Info.plist" ]; then
            bid="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$existing/Contents/Info.plist" 2>/dev/null || true)"
            echo "      Preserving: $bname ($bid)"
            ditto "$existing" "$STAGE_EXT/$bname"
            chown -R root:wheel "$STAGE_EXT/$bname"
            chmod -R 755 "$STAGE_EXT/$bname"
            if [ -n "$bid" ]; then
                case "$bid" in
                    com.nullmoth.*)
                        PRESERVED_IDS+=("$bid")
                        ;;
                esac
            fi
        fi
    done
fi

# Copy selected Arc*.kext bundles into staging repository
for k in "${ARC_KEXTS[@]}"; do
    ditto "$OUT/kexts/$k.kext" "$STAGE_EXT/$k.kext"
    chown -R root:wheel "$STAGE_EXT/$k.kext"
    chmod -R 755 "$STAGE_EXT/$k.kext"
done

# Build explicit --bundle-path arguments for every kext in $STAGE_EXT
KARG=()
for staged in "$STAGE_EXT"/*.kext; do
    [ -d "$staged" ] || continue
    KARG+=(--bundle-path "$staged")
done

# 4. Pre-flight kmutil create -n aux in /var/tmp BEFORE touching /Library/Extensions or AuxKC
echo ""
echo "[2/5] Running pre-flight kmutil AuxiliaryKernelExtensions validation..."
if ! kmutil create -n aux \
        --volume-root / \
        ${KARG[@]+"${KARG[@]}"} \
        -B "$KB" -S "$KS" \
        --repository "$STAGE_EXT" \
        -A "$STAGE_DIR/test_aux.kc" -z >"$STAGE_DIR/kmutil_pre.log" 2>&1; then
    tail -n 30 "$STAGE_DIR/kmutil_pre.log" >&2
    die "Pre-flight kmutil test build failed (see log above)."
fi

INSPECT_OUT="$(kmutil inspect -a x86_64 -A "$STAGE_DIR/test_aux.kc" 2>/dev/null)" \
    || die "kmutil inspect failed on pre-flight test_aux.kc"

for k in "${ARC_KEXTS[@]}"; do
    if ! printf '%s\n' "$INSPECT_OUT" | grep -q "com\.arc\.$k"; then
        tail -n 25 "$STAGE_DIR/kmutil_pre.log" >&2
        die "Pre-flight AuxKC is missing com.arc.$k!"
    fi
done

for nid in ${PRESERVED_IDS[@]+"${PRESERVED_IDS[@]}"}; do
    if ! printf '%s\n' "$INSPECT_OUT" | grep -Fq "$nid"; then
        tail -n 25 "$STAGE_DIR/kmutil_pre.log" >&2
        die "SAFETY ABORT: Pre-flight AuxKC dropped existing NullMoth kext '$nid'!"
    fi
done
echo "      Pre-flight AuxKC verified (all com.arc.* and existing com.nullmoth.* kexts intact)."

# 5. Timestamped Backup
TS="$(date +%Y%m%d-%H%M%S)"
BK="/Library/ArcDriver/backups/$TS"
mkdir -p "$BK" "/Library/ArcDriver/fw" "$GB" "$LE" "$KC_DIR"

echo ""
echo "[3/5] Backing up current AuxKC & previous Arc bundles to $BK..."
if [ -f "$AUX_KC" ]; then
    cp -p "$AUX_KC" "$BK/AuxiliaryKernelExtensions.kc"
fi
if [ -f "$AUX_KC.elides" ]; then
    cp -p "$AUX_KC.elides" "$BK/AuxiliaryKernelExtensions.kc.elides"
fi
for oldk in ArcXe.kext ArcFB.kext ArcAccel.kext; do
    [ ! -d "$LE/$oldk" ] || ditto "$LE/$oldk" "$BK/$oldk"
done
for oldb in ArcMTLDriver.bundle arcmtl arcmtl-allow.txt; do
    [ ! -e "$GB/$oldb" ] || ditto "$GB/$oldb" "$BK/$oldb"
done
ln -sfn "$BK" "/Library/ArcDriver/backups/latest"

# 6. Install Firmware, Userspace GPUBundles, and Kexts
echo ""
echo "[4/5] Installing GuC firmware, GPUBundles, and Kernel Extensions..."
cp -f "$OUT/fw/mtl_guc_70.bin" "/Library/ArcDriver/fw/mtl_guc_70.bin"
chown root:wheel "/Library/ArcDriver/fw/mtl_guc_70.bin"
chmod 644 "/Library/ArcDriver/fw/mtl_guc_70.bin"

rm -rf "$GB/ArcMTLDriver.bundle" "$GB/arcmtl"
mkdir -p "$GB/arcmtl"
ditto "$OUT/GPUBundles/ArcMTLDriver.bundle" "$GB/ArcMTLDriver.bundle"
cp -f "$OUT/arcmtl/"* "$GB/arcmtl/"
cp -f "$OUT/GPUBundles/arcmtl-allow.txt" "$GB/arcmtl-allow.txt"

chown -R root:wheel "$GB/ArcMTLDriver.bundle" "$GB/arcmtl" "$GB/arcmtl-allow.txt"
chmod -R go-w,a+rX "$GB/ArcMTLDriver.bundle" "$GB/arcmtl"
chmod 644 "$GB/arcmtl-allow.txt"

if id _windowserver >/dev/null 2>&1; then
    sudo -u _windowserver /bin/test -r "$GB/arcmtl-allow.txt" \
        || die "_windowserver cannot read $GB/arcmtl-allow.txt!"
    sudo -u _windowserver /bin/test -r "$GB/arcmtl/libvulkan_intel.dylib" \
        || die "_windowserver cannot read $GB/arcmtl/libvulkan_intel.dylib!"
fi

# Remove old Arc*.kext first (ensures --stage1 removes ArcFB/ArcAccel if previously installed)
rm -rf "$LE/ArcXe.kext" "$LE/ArcFB.kext" "$LE/ArcAccel.kext"
for k in "${ARC_KEXTS[@]}"; do
    ditto "$OUT/kexts/$k.kext" "$LE/$k.kext"
    chown -R root:wheel "$LE/$k.kext"
    chmod -R 755 "$LE/$k.kext"
done

# 7. Rebuild Live AuxiliaryKernelExtensions.kc atomically
echo ""
echo "[5/5] Rebuilding live $AUX_KC..."
LIVE_KARG=()
for livek in "$LE"/*.kext; do
    [ -d "$livek" ] || continue
    LIVE_KARG+=(--bundle-path "$livek")
done

NEW_KC="$STAGE_DIR/live_aux.kc"
if ! kmutil create -n aux \
        --volume-root / \
        ${LIVE_KARG[@]+"${LIVE_KARG[@]}"} \
        -B "$KB" -S "$KS" \
        --repository "$LE" \
        -A "$NEW_KC" -z >"$STAGE_DIR/kmutil_live.log" 2>&1; then
    tail -n 30 "$STAGE_DIR/kmutil_live.log" >&2
    die "Live kmutil build failed! Restore backup from $BK if needed."
fi

cp -f "$NEW_KC" "$AUX_KC.arcnew"
chown root:wheel "$AUX_KC.arcnew"
chmod 644 "$AUX_KC.arcnew"
mv -f "$AUX_KC.arcnew" "$AUX_KC"
if [ -f "$NEW_KC.elides" ]; then
    cp -f "$NEW_KC.elides" "$AUX_KC.elides"
    chown root:wheel "$AUX_KC.elides"
    chmod 644 "$AUX_KC.elides"
fi

echo ""
echo "=================================================================="
echo " [INSTALLED] Intel Arc macOS Driver Stack Ready!"
echo " Backup saved at: $BK"
echo "=================================================================="
echo "  1. First install: If macOS prompts in System Settings -> Privacy & Security,"
echo "     click 'Allow' for ArcXe / ArcFB / ArcAccel and reboot."
echo "  2. Recommended boot-args by testing stage:"
echo "     - Stage 1 & 2 (ArcXe + Vulkan offscreen, RTX 4070 desktop):  (no extra boot-args needed!)"
echo "     - Stage 3     (Dual-GPU Metal 3: RTX 4070 + Intel Arc):      arc_headless=1"
echo "     - Stage 4     (Standalone Intel Arc Laptop/Battery Mode):    arcfb=1 arcaccel=1 -nvoff"
echo "     - Emergency disable all Arc kexts at OpenCore picker:        -arcoff"
echo "=================================================================="
