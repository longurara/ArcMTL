#!/bin/bash
# package/install_opencore_userspace.sh — Hybrid OpenCore (ArcXe) + AuxKC (ArcFB/ArcAccel) + Userspace Installer
#
# Architecture on macOS 15 Sequoia & macOS 26 Tahoe with OpenCore + NullMoth RTX 4070:
#   - ArcXe.kext      : Injected at boot by OpenCore (EFI/OC/Kexts/ArcXe.kext) — links only BootKC (IOPCIFamily).
#   - ArcFB.kext      : Linked into /Library/KernelCollections/AuxiliaryKernelExtensions.kc alongside NullMoth
#   - ArcAccel.kext   : Linked into /Library/KernelCollections/AuxiliaryKernelExtensions.kc alongside NullMoth
#                       (required because IOGraphicsFamily, IOAcceleratorFamily2 & IOSurface live in SystemKC, not BootKC).
#   - GPUBundles      : /Library/GPUBundles/{ArcMTLDriver.bundle, arcmtl/*, arcmtl-allow.txt} + /Library/ArcDriver/fw.
#
# Modes:
#   sudo bash install_opencore_userspace.sh                      # Userspace only + live arc_probe & Vulkan 1.3 test
#   sudo bash install_opencore_userspace.sh --enable-dual-metal  # + Link ArcFB/ArcAccel into AuxKC (preserving 100% NullMoth)
#                                                                #   & auto-detect Optimus (arcfb=1 arcaccel=1 when Arc owns panel,
#                                                                #   or arc_headless=1 when dGPU owns panel; IONDRVSupport unblocked)
#   sudo bash install_opencore_userspace.sh --enable-arc-desktop # + Link ArcFB/ArcAccel into AuxKC & set arcfb=1 arcaccel=1
#   sudo bash install_opencore_userspace.sh --disable-arc-accel  # Strip arc_headless/arcfb/arcaccel from config.plist (safe Stage 1)

set -euo pipefail

die() {
    echo "" >&2
    echo "[ERROR] $*" >&2
    exit 1
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP_EXTRACT=""
cleanup() {
    [ -z "$TMP_EXTRACT" ] || rm -rf "$TMP_EXTRACT"
}
trap cleanup EXIT

if [ -d "$SCRIPT_DIR/out" ]; then
    OUT="$SCRIPT_DIR/out"
elif [ -d "$SCRIPT_DIR/../out" ] && [ -d "$SCRIPT_DIR/../out/arcmtl" ]; then
    OUT="$(cd "$SCRIPT_DIR/../out" && pwd)"
elif [ -f "$SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz" ]; then
    TMP_EXTRACT="$(mktemp -d /var/tmp/arcmtl_extract.XXXXXX)"
    echo "[*] Extracting $SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz ..."
    tar -xzf "$SCRIPT_DIR/ArcMTL-macos26-15-x86_64.tar.gz" -C "$TMP_EXTRACT"
    if [ -d "$TMP_EXTRACT/ArcMTL-macos26-15-x86_64/out" ]; then
        OUT="$TMP_EXTRACT/ArcMTL-macos26-15-x86_64/out"
    else
        OUT="$TMP_EXTRACT/out"
    fi
    # Overlay latest fast-built kexts if ArcKexts-macos26-15-x86_64.tar.gz is present alongside main bundle
    if [ -f "$SCRIPT_DIR/ArcKexts-macos26-15-x86_64.tar.gz" ]; then
        echo "[*] Overlaying latest kexts from $SCRIPT_DIR/ArcKexts-macos26-15-x86_64.tar.gz ..."
        tar -xzf "$SCRIPT_DIR/ArcKexts-macos26-15-x86_64.tar.gz" -C "$OUT"
    fi
else
    die "Cannot locate out/ directory or ArcMTL-macos26-15-x86_64.tar.gz next to script."
fi

MODE="${1:-}"

if [ "$(uname -s)" != "Darwin" ]; then
    die "Must be run inside macOS."
fi
if [ "$(id -u)" -ne 0 ]; then
    die "Run with sudo: sudo bash $0 $MODE"
fi

OS_VER="$(sw_vers -productVersion)"
OS_MAJ="${OS_VER%%.*}"
DARWIN_MAJ="$(uname -r | cut -d. -f1)"
if [ "$OS_MAJ" != "15" ] && [ "$OS_MAJ" != "26" ]; then
    if [ "$DARWIN_MAJ" = "24" ]; then
        OS_MAJ="15"
    elif [ "$DARWIN_MAJ" = "25" ]; then
        OS_MAJ="26"
    else
        die "Unsupported macOS version '$OS_VER' (Darwin $DARWIN_MAJ). Expected macOS 15 or 26."
    fi
fi

echo "=================================================================="
echo " ArcMTL Installer for OpenCore + NullMoth Dual-GPU Optimus"
echo " macOS $OS_VER (ABI $OS_MAJ / Darwin $DARWIN_MAJ)"
echo "=================================================================="

echo ""
echo "[1/5] Checking running ArcXe & NullMoth kexts in kernel..."
kmutil showloaded --list-only 2>/dev/null | grep -E "com\.arc\.|com\.nullmoth\." || \
    kextstat 2>/dev/null | grep -E "com\.arc\.|com\.nullmoth\." || true
LATEST_WS_CRASH="$(ls -1t /Library/Logs/DiagnosticReports/WindowServer*.ips 2>/dev/null | head -n 1 || true)"
if [ -n "$LATEST_WS_CRASH" ]; then
    echo "      [diag] Most recent WindowServer report: $LATEST_WS_CRASH"
    grep -E '"termination"|"exception"|"asi"|"faultingThread"' "$LATEST_WS_CRASH" 2>/dev/null | head -n 4 || true
fi
sysctl debug.arcaccelfb debug.arcaccel_headless debug.arcaccel_copies debug.arcaccel_blank debug.arcaccel_fail 2>/dev/null || true

echo ""
echo "[2/5] Installing GuC v70 firmware & diagnostic tools..."
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
echo "[3/5] Installing Mesa 26.2.6 ANV (/Library/GPUBundles/arcmtl) & ArcMTLDriver.bundle..."
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

rm -rf "$GB/ArcMTLDriver.bundle"
ditto "$OUT/GPUBundles/ArcMTLDriver.bundle" "$GB/ArcMTLDriver.bundle"
codesign --force --deep --sign - "$GB/ArcMTLDriver.bundle"
install -m 644 "$OUT/GPUBundles/arcmtl-allow.txt" "$GB/arcmtl-allow.txt"

chown -R root:wheel "$GB/ArcMTLDriver.bundle" "$GB/arcmtl" "$GB/arcmtl-allow.txt"
chmod -R go-w,a+rX "$GB/ArcMTLDriver.bundle" "$GB/arcmtl"
chmod 644 "$GB/arcmtl-allow.txt"
xattr -cr "$GB/ArcMTLDriver.bundle" "$GB/arcmtl" 2>/dev/null || true

echo ""
echo "[4/5] Running LIVE Intel Arc Xe-LPG hardware & Vulkan 1.3 tests..."
PROBE_OK=0
VK_OK=0
if [ -x /usr/local/bin/arc_probe ]; then
    echo "--- [Test A] arc_probe (48-bit PPGTT + RCS0 128-EU Command Streamer) ---"
    if /usr/local/bin/arc_probe; then
        PROBE_OK=1
    else
        echo "[WARN] arc_probe returned non-zero"
        log show --last 2m --style compact --predicate 'eventMessage CONTAINS "ArcXe:"' 2>/dev/null | tail -n 25 || true
    fi
fi
if [ -x /usr/local/bin/vk_offscreen_test ]; then
    echo "--- [Test B] vk_offscreen_test (Mesa 26.2.6 ANV Vulkan 1.3 Offscreen GPU Test) ---"
    if VK_ICD_FILENAMES=/Library/GPUBundles/arcmtl/intel_icd.json /usr/local/bin/vk_offscreen_test; then
        VK_OK=1
        echo "[OK] Mesa ANV Vulkan 1.3 offscreen GPU verification PASSED!"
    else
        echo "[WARN] vk_offscreen_test returned non-zero"
        log show --last 2m --style compact --predicate 'eventMessage CONTAINS "ArcXe:"' 2>/dev/null | tail -n 25 || true
    fi
fi
if (kmutil showloaded --list-only 2>/dev/null || kextstat 2>/dev/null) | grep -q "com\.arc\.ArcAccel"; then
    if [ -x /usr/local/bin/mtl_offscreen_test ]; then
        echo "--- [Test C] mtl_offscreen_test (Metal 3 MTLCopyAllDevices + Render/Compute) ---"
        /usr/local/bin/mtl_offscreen_test \
            && echo "[OK] Metal 3 end-to-end render & compute verification PASSED!" \
            || echo "[WARN] mtl_offscreen_test returned non-zero"
    fi
fi

stamp_host_deps() {
    local kext_dir="$1"
    local plist="$kext_dir/Contents/Info.plist"
    local pairs=(
        "com.apple.iokit.IOPCIFamily:/System/Library/Extensions/IOPCIFamily.kext/Contents/Info.plist"
        "com.apple.iokit.IOGraphicsFamily:/System/Library/Extensions/IOGraphicsFamily.kext/Contents/Info.plist"
        "com.apple.iokit.IOAcceleratorFamily2:/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/Info.plist"
        "com.apple.iokit.IOSurface:/System/Library/Extensions/IOSurface.kext/Contents/Info.plist"
    )
    for item in "${pairs[@]}"; do
        local bid="${item%%:*}"
        local sle="${item#*:}"
        if /usr/libexec/PlistBuddy -c "Print :OSBundleLibraries:$bid" "$plist" >/dev/null 2>&1 && [ -f "$sle" ]; then
            local ver
            ver="$(/usr/libexec/PlistBuddy -c 'Print :OSBundleCompatibleVersion' "$sle" 2>/dev/null || \
                   /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$sle" 2>/dev/null || true)"
            if [ -n "$ver" ]; then
                /usr/libexec/PlistBuddy -c "Set :OSBundleLibraries:$bid $ver" "$plist"
            fi
        fi
    done
    codesign --force --sign - "$kext_dir"
}

install_auxkc_arcfb_arcaccel() {
    echo ""
    echo "[5/5] Linking ArcFB.kext & ArcAccel.kext (macOS $OS_MAJ) into AuxKC alongside NullMoth..."
    local KB="" KS=""
    for p in /System/Library/KernelCollections/BootKernelExtensions.kc \
             /Library/Apple/System/Library/KernelCollections/BootKernelExtensions.kc; do
        [ -f "$p" ] && { KB="$p"; break; }
    done
    for p in /System/Library/KernelCollections/SystemKernelExtensions.kc \
             /Library/Apple/System/Library/KernelCollections/SystemKernelExtensions.kc; do
        [ -f "$p" ] && { KS="$p"; break; }
    done
    [ -n "$KB" ] && [ -n "$KS" ] || die "Could not locate BootKernelExtensions.kc / SystemKernelExtensions.kc"

    local KM_ARGS=(--allow-missing-kdk)
    if [ -f "/System/Library/Kernels/kernel" ]; then
        KM_ARGS+=(--kernel "/System/Library/Kernels/kernel")
    fi

    local LE="/Library/Extensions"
    local KC_DIR="/Library/KernelCollections"
    local AUX_KC="$KC_DIR/AuxiliaryKernelExtensions.kc"
    local STAGE_DIR
    STAGE_DIR="$(mktemp -d /var/tmp/arc_auxkc_stage.XXXXXX)"
    local STAGE_EXT="$STAGE_DIR/Extensions"
    mkdir -p "$STAGE_EXT"

    local PRESERVED_IDS=()
    if [ -d "$LE" ]; then
        for existing in "$LE"/*.kext; do
            [ -d "$existing" ] || continue
            local bname
            bname="$(basename "$existing")"
            case "$bname" in
                ArcXe.kext|ArcFB.kext|ArcAccel.kext) continue ;;
            esac
            if [ -f "$existing/Contents/Info.plist" ]; then
                local bid
                bid="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$existing/Contents/Info.plist" 2>/dev/null || true)"
                echo "      Preserving existing kext: $bname ($bid)"
                ditto "$existing" "$STAGE_EXT/$bname"
                chown -R root:wheel "$STAGE_EXT/$bname"
                chmod -R go-w "$STAGE_EXT/$bname"
                case "$bid" in
                    com.nullmoth.*) PRESERVED_IDS+=("$bid") ;;
                esac
            fi
        done
    fi

    local ACCEL_SRC="$OUT/kexts/$OS_MAJ/ArcAccel.kext"
    [ -d "$ACCEL_SRC" ] || ACCEL_SRC="$OUT/kexts/ArcAccel.kext"
    [ -f "$OUT/kexts/ArcFB.kext/Contents/MacOS/ArcFB" ] || die "Missing $OUT/kexts/ArcFB.kext"
    [ -f "$ACCEL_SRC/Contents/MacOS/ArcAccel" ] || die "Missing $ACCEL_SRC"

    # Note: ArcXe.kext stays injected via OpenCore (EFI/OC/Kexts/ArcXe.kext); only ArcFB & ArcAccel go into AuxKC
    ditto "$OUT/kexts/ArcFB.kext" "$STAGE_EXT/ArcFB.kext"
    ditto "$ACCEL_SRC" "$STAGE_EXT/ArcAccel.kext"
    stamp_host_deps "$STAGE_EXT/ArcFB.kext"
    stamp_host_deps "$STAGE_EXT/ArcAccel.kext"
    chown -R root:wheel "$STAGE_EXT/ArcFB.kext" "$STAGE_EXT/ArcAccel.kext"
    chmod -R 755 "$STAGE_EXT/ArcFB.kext" "$STAGE_EXT/ArcAccel.kext"

    echo "      Running pre-flight kmutil validation in /var/tmp..."
    if ! kmutil create -n aux \
            --volume-root / \
            "${KM_ARGS[@]}" \
            -B "$KB" -S "$KS" \
            --repository "$STAGE_EXT" \
            -A "$STAGE_DIR/test_aux.kc" -z >"$STAGE_DIR/kmutil_pre.log" 2>&1; then
        tail -n 30 "$STAGE_DIR/kmutil_pre.log" >&2
        rm -rf "$STAGE_DIR"
        die "Pre-flight kmutil failed! Live /Library/Extensions & AuxKC were NOT modified."
    fi

    local INSPECT_OUT
    INSPECT_OUT="$(kmutil inspect -a x86_64 -A "$STAGE_DIR/test_aux.kc" 2>/dev/null)" || {
        rm -rf "$STAGE_DIR"
        die "kmutil inspect failed on pre-flight test_aux.kc"
    }
    for req in com.arc.ArcFB com.arc.ArcAccel; do
        if ! printf '%s\n' "$INSPECT_OUT" | grep -Fq "$req"; then
            tail -n 25 "$STAGE_DIR/kmutil_pre.log" >&2
            rm -rf "$STAGE_DIR"
            die "Pre-flight AuxKC missing $req!"
        fi
    done
    for nid in ${PRESERVED_IDS[@]+"${PRESERVED_IDS[@]}"}; do
        if ! printf '%s\n' "$INSPECT_OUT" | grep -Fq "$nid"; then
            tail -n 25 "$STAGE_DIR/kmutil_pre.log" >&2
            rm -rf "$STAGE_DIR"
            die "SAFETY ABORT: Pre-flight AuxKC dropped NullMoth kext '$nid'!"
        fi
    done
    echo "      [OK] Pre-flight AuxKC verified (com.arc.ArcFB + com.arc.ArcAccel + all NullMoth kexts present)."

    local TS BK
    TS="$(date +%Y%m%d-%H%M%S)"
    BK="/Library/ArcDriver/backups/$TS"
    mkdir -p "$BK" "$LE" "$KC_DIR"
    [ ! -f "$AUX_KC" ] || cp -p "$AUX_KC" "$BK/AuxiliaryKernelExtensions.kc"
    [ ! -f "$AUX_KC.elides" ] || cp -p "$AUX_KC.elides" "$BK/AuxiliaryKernelExtensions.kc.elides"
    ln -sfn "$BK" "/Library/ArcDriver/backups/latest"

    rm -rf "$LE/ArcXe.kext" "$LE/ArcFB.kext" "$LE/ArcAccel.kext"
    ditto "$STAGE_EXT/ArcFB.kext" "$LE/ArcFB.kext"
    ditto "$STAGE_EXT/ArcAccel.kext" "$LE/ArcAccel.kext"
    chown -R root:wheel "$LE/ArcFB.kext" "$LE/ArcAccel.kext"
    chmod -R 755 "$LE/ArcFB.kext" "$LE/ArcAccel.kext"

    echo "      Building live $AUX_KC..."
    local NEW_KC="$STAGE_DIR/live_aux.kc"
    if ! kmutil create -n aux \
            --volume-root / \
            "${KM_ARGS[@]}" \
            -B "$KB" -S "$KS" \
            --repository "$LE" \
            -A "$NEW_KC" -z >"$STAGE_DIR/kmutil_live.log" 2>&1; then
        tail -n 30 "$STAGE_DIR/kmutil_live.log" >&2
        rm -rf "$STAGE_DIR"
        die "Live kmutil build failed! Backup preserved at $BK."
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
    if [ -x /usr/sbin/kcditto ]; then
        echo "      Syncing Preboot KernelCollections via /usr/sbin/kcditto..."
        /usr/sbin/kcditto || echo "[WARN] kcditto returned non-zero (non-fatal)"
    fi
    rm -rf "$STAGE_DIR"
    echo "      [OK] Live AuxKC updated and synced (backup: $BK)."
}

update_oc_config() {
    local target_mode="$1"
    local plist=""
    for candidate in /Volumes/OPENCORE/EFI/OC/config.plist /Volumes/EFI/EFI/OC/config.plist /Volumes/ESP/EFI/OC/config.plist "$SCRIPT_DIR/../EFI/OC/config.plist"; do
        if [ -f "$candidate" ]; then
            plist="$candidate"
            break
        fi
    done
    if [ -z "$plist" ]; then
        echo "[WARN] EFI config.plist not found at /Volumes/OPENCORE/EFI/OC/config.plist or /Volumes/EFI/EFI/OC/config.plist."
        echo "       Run 'sudo diskutil mount OPENCORE' (or 'sudo diskutil mount EFI') and re-run with $MODE."
        return 1
    fi

    local oc_dir
    oc_dir="$(dirname "$plist")"
    if [ -d "$OUT/kexts/ArcXe.kext" ] && [ -d "$oc_dir/Kexts" ]; then
        echo "[*] Syncing latest ArcXe.kext -> $oc_dir/Kexts/ArcXe.kext ..."
        rm -rf "$oc_dir/Kexts/ArcXe.kext"
        cp -R "$OUT/kexts/ArcXe.kext" "$oc_dir/Kexts/ArcXe.kext"
    fi

    local nv_headless="0"
    nv_headless="$(sysctl -n debug.nvaccel_headless 2>/dev/null || echo 0)"
    local arc_owns_boot_display="0"
    if ioreg -r -n IGPU -d 1 2>/dev/null | grep -Fq '"AAPL,boot-display" = Yes'; then
        arc_owns_boot_display="1"
    elif ioreg -l 2>/dev/null | grep -B 5 -A 15 '"arcxe-claimed" = Yes' | grep -Fq '"AAPL,boot-display" = Yes'; then
        arc_owns_boot_display="1"
    fi

    cp "$plist" "${plist}.bak-userspace-$(date +%Y%m%d-%H%M%S)"
    python3 - "$plist" "$target_mode" "$nv_headless" "$arc_owns_boot_display" <<'PY'
import sys, plistlib
path, mode, nv_headless, arc_boot_disp = sys.argv[1], sys.argv[2], sys.argv[3] == '1', sys.argv[4] == '1'
with open(path, 'rb') as f:
    cfg = plistlib.load(f)

nv = cfg['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
raw_tokens = nv.get('boot-args', '').split()
has_nvrmheadless = ('nvrmheadless=1' in raw_tokens)
optimus_arc_display = nv_headless or arc_boot_disp or has_nvrmheadless

remove_set = {'arc_headless=1', 'arcaccel=1', 'arcfb=1', 'arcflip=1', '-arcnoflip'}
tokens = [t for t in raw_tokens if t not in remove_set]

# Never block IONDRVSupport: ArcFB (IOProbeScore=65000) wins matching automatically when arcfb=1 is set,
# while keeping IONDRVFramebuffer available as an instant fail-safe if -arcfboff or -arcoff is passed!
block_iondrv = False
effective_desc = mode

if mode == 'dual-metal':
    if optimus_arc_display:
        tokens.extend(['arcfb=1', 'arcaccel=1'])
        effective_desc = 'dual-metal (Optimus: Intel Arc owns display -> arcfb=1 arcaccel=1 + RTX 4070 headless Metal)'
    else:
        tokens.append('arc_headless=1')
        effective_desc = 'dual-metal (dGPU owns display -> arc_headless=1 secondary Intel Arc Metal)'
elif mode == 'arc-desktop':
    tokens.extend(['arcfb=1', 'arcaccel=1'])
    effective_desc = 'arc-desktop (arcfb=1 arcaccel=1, IONDRVSupport unblocked fallback)'
elif mode == 'safe-stage1':
    effective_desc = 'safe-stage1 (ArcXe only, IONDRVFramebuffer desktop)'

nv['boot-args'] = ' '.join(tokens)
nv_del = cfg.setdefault('NVRAM', {}).setdefault('Delete', {}).setdefault('7C436110-AB2A-4BBB-A880-FE41995C9F82', [])
if 'boot-args' not in nv_del:
    nv_del.append('boot-args')

for entry in cfg.get('Kernel', {}).get('Block', []):
    if entry.get('Identifier') == 'com.apple.iokit.IONDRVSupport':
        entry['Enabled'] = block_iondrv

for entry in cfg.get('Kernel', {}).get('Add', []):
    bp = entry.get('BundlePath', '')
    if bp == 'ArcXe.kext':
        entry['Enabled'] = True
    elif bp in ('ArcFB.kext', 'ArcAccel15.kext', 'ArcAccel26.kext'):
        entry['Enabled'] = False

with open(path, 'wb') as f:
    plistlib.dump(cfg, f, sort_keys=False)
arc_flags = ' '.join(t for t in tokens if t.startswith('arc') or t.startswith('-arc')) or '(none)'
print(f"[OK] Updated OpenCore config.plist [{effective_desc}]: arc_flags='{arc_flags}', IONDRVSupport blocked={block_iondrv}")
PY
}

FORCE_ARG="${2:-}"

if [ "$MODE" = "--disable-arc-accel" ] || [ "$MODE" = "--safe-stage1" ]; then
    update_oc_config "safe-stage1"
elif [ "$MODE" = "--enable-dual-metal" ]; then
    install_auxkc_arcfb_arcaccel
    if [ "$PROBE_OK" = "1" ] && [ "$VK_OK" = "1" ] || [ "$FORCE_ARG" = "--force" ]; then
        update_oc_config "dual-metal"
    else
        echo ""
        echo "[SAFETY GUARD] arc_probe ($PROBE_OK) or vk_offscreen_test ($VK_OK) did NOT pass!"
        echo "               Keeping ArcAccel DISABLED in config.plist so your desktop stays 100% clean on reboot."
        update_oc_config "safe-stage1" || true
    fi
elif [ "$MODE" = "--enable-arc-desktop" ]; then
    install_auxkc_arcfb_arcaccel
    if [ "$PROBE_OK" = "1" ] && [ "$VK_OK" = "1" ] || [ "$FORCE_ARG" = "--force" ]; then
        update_oc_config "arc-desktop"
    else
        echo ""
        echo "[SAFETY GUARD] arc_probe ($PROBE_OK) or vk_offscreen_test ($VK_OK) did NOT pass!"
        echo "               Keeping ArcFB/ArcAccel DISABLED in config.plist so your desktop stays 100% bootable."
        update_oc_config "safe-stage1" || true
    fi
fi

echo ""
echo "=================================================================="
echo " ArcMTL Setup COMPLETE!"
if [ "$MODE" = "--disable-arc-accel" ] || [ "$MODE" = "--safe-stage1" ]; then
    echo " -> Reset OpenCore boot-args to safe Stage 1 (ArcXe only)."
elif [ "$MODE" = "--enable-dual-metal" ]; then
    if [ "$PROBE_OK" = "1" ] && [ "$VK_OK" = "1" ] || [ "$FORCE_ARG" = "--force" ]; then
        echo " -> Stage 1 (ArcXe BCS0+RCS0) & Stage 2 (Mesa ANV Vulkan 1.3) PASSED!"
        echo " -> Next: Run 'sudo reboot' to activate Dual-GPU Metal 3 (Intel Arc Display+Metal 3 + RTX 4070 Metal 3)!"
        echo " -> After reboot, verify both GPUs in Metal 3 via: /usr/local/bin/mtl_offscreen_test"
    else
        echo " -> SAFETY GUARD kept ArcAccel off so your desktop stays clean. Share the output above to debug!"
    fi
elif [ "$MODE" = "--enable-arc-desktop" ]; then
    echo " -> Next: Run 'sudo reboot' to boot full Intel Arc Metal 3 Desktop (ArcFB + ArcAccel)!"
else
    echo " -> To activate Dual-GPU Metal 3 (Intel Arc + RTX 4070): sudo bash $0 --enable-dual-metal && sudo reboot"
    echo " -> To reset to Stage 1 safe mode anytime:               sudo bash $0 --disable-arc-accel && sudo reboot"
fi
echo "=================================================================="
