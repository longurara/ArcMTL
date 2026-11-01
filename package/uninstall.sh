#!/bin/bash
# package/uninstall.sh — Cleanly remove Intel Arc driver components while preserving NullMoth RTX 4070
#
# Usage:
#   sudo bash package/uninstall.sh                   # Remove Arc kexts/bundles & rebuild AuxKC with remaining kexts
#   sudo bash package/uninstall.sh --restore-backup  # Remove Arc files & restore pre-install AuxKC backup directly

set -euo pipefail

MODE="${1:-}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "[ERROR] uninstall.sh must be run on macOS." >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ]; then
    echo "[ERROR] uninstall.sh must be run as root (sudo bash package/uninstall.sh)." >&2
    exit 1
fi

LE="/Library/Extensions"
GB="/Library/GPUBundles"
AUX_KC="/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
LATEST_BK="/Library/ArcDriver/backups/latest"

echo "=================================================================="
echo " Uninstalling Intel Arc Driver Stack (Preserving NullMoth NVIDIA)"
echo "=================================================================="

echo "[1/3] Removing Arc kexts from $LE..."
rm -rf "$LE/ArcXe.kext" "$LE/ArcFB.kext" "$LE/ArcAccel.kext"

echo "[2/3] Removing Arc GPUBundles & firmware..."
rm -rf "$GB/ArcMTLDriver.bundle" "$GB/arcmtl" "$GB/arcmtl-allow.txt" "/Library/ArcDriver/fw"

if [ "$MODE" = "--restore-backup" ] && [ -f "$LATEST_BK/AuxiliaryKernelExtensions.kc" ]; then
    echo "[3/3] Restoring backed-up AuxiliaryKernelExtensions.kc from $LATEST_BK..."
    cp -pf "$LATEST_BK/AuxiliaryKernelExtensions.kc" "$AUX_KC"
    if [ -f "$LATEST_BK/AuxiliaryKernelExtensions.kc.elides" ]; then
        cp -pf "$LATEST_BK/AuxiliaryKernelExtensions.kc.elides" "$AUX_KC.elides"
    fi
    echo "[DONE] Backup AuxKC restored. Reboot to complete uninstall."
    exit 0
fi

echo "[3/3] Rebuilding $AUX_KC with all remaining kexts in $LE..."
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

REMAINING_KARG=()
for k in "$LE"/*.kext; do
    [ -d "$k" ] || continue
    echo "      Keeping: $(basename "$k")"
    REMAINING_KARG+=(--bundle-path "$k")
done

if [ "${#REMAINING_KARG[@]}" -eq 0 ]; then
    rm -f "$AUX_KC" "$AUX_KC.elides"
    echo "      No third-party kexts remain in $LE; removed $AUX_KC."
else
    TMP_DIR="$(mktemp -d /var/tmp/arc_uninstall.XXXXXX)"
    trap 'rm -rf "$TMP_DIR"' EXIT
    kmutil create -n aux \
        --volume-root / \
        "${REMAINING_KARG[@]}" \
        -B "$KB" -S "$KS" \
        --repository "$LE" \
        -A "$TMP_DIR/aux.kc" -z
    cp -f "$TMP_DIR/aux.kc" "$AUX_KC.new"
    chown root:wheel "$AUX_KC.new"
    chmod 644 "$AUX_KC.new"
    mv -f "$AUX_KC.new" "$AUX_KC"
    if [ -f "$TMP_DIR/aux.kc.elides" ]; then
        cp -f "$TMP_DIR/aux.kc.elides" "$AUX_KC.elides"
        chown root:wheel "$AUX_KC.elides"
        chmod 644 "$AUX_KC.elides"
    fi
fi

echo ""
echo "[DONE] Intel Arc driver removed cleanly; existing kexts preserved."
echo "       Reboot macOS to complete."
