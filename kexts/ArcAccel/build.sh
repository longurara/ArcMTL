#!/bin/bash
# Build ArcAccel.kext on macOS 15 Sequoia / 26 Tahoe (Xcode 16+)
set -euo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SRC/../.." && pwd)
OUT=${1:-"$ROOT/out/kexts"}
mkdir -p "$OUT"

python3 "$SRC/gen_arc_stubs.py"

SDK=$(xcrun --show-sdk-path)
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

TAHOE_FLAG=()
DARWIN_MAJOR=$(uname -r | cut -d. -f1)
if [ "${ARC_TAHOE:-auto}" = "1" ]; then
  TAHOE_FLAG=(-DARC_TAHOE=1)
elif [ "${ARC_TAHOE:-auto}" = "0" ]; then
  TAHOE_FLAG=()
elif [ "${DARWIN_MAJOR:-24}" -ge 25 ]; then
  TAHOE_FLAG=(-DARC_TAHOE=1)
fi

CFLAGS=(
  -arch x86_64 -fapple-kext -mkernel -nostdinc
  -I"$KHDR" -I"$SRC" -I"$SRC/iofam" -I"$SRC/re" -I"$ROOT/include"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  ${TAHOE_FLAG[@]+"${TAHOE_FLAG[@]}"}
  -mmacosx-version-min=15.0 -g0
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -Wno-deprecated-declarations -O2
)

echo "=== Building ArcAccel.kext ==="
xcrun clang++ "${CFLAGS[@]}" -c "$SRC/ArcAccel.cpp" -o "$OUT/ArcAccel.o"

KEXT="$OUT/ArcAccel.kext"
rm -rf "$KEXT"
mkdir -p "$KEXT/Contents/MacOS"

xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib \
  -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/ArcAccel.o" -o "$KEXT/Contents/MacOS/ArcAccel"

cp "$SRC/Info.plist" "$KEXT/Contents/Info.plist"

# Stamp exact OSBundleCompatibleVersion from the running macOS /System/Library/Extensions so kmutil never mismatches
stamp_dep_ver() {
  local bundle_id="$1"
  local sle_plist="$2"
  if [ -f "$sle_plist" ]; then
    local ver
    ver=$(/usr/libexec/PlistBuddy -c "Print :OSBundleCompatibleVersion" "$sle_plist" 2>/dev/null || \
          /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$sle_plist" 2>/dev/null || true)
    if [ -n "$ver" ]; then
      /usr/libexec/PlistBuddy -c "Set :OSBundleLibraries:$bundle_id $ver" "$KEXT/Contents/Info.plist"
    fi
  fi
}

stamp_dep_ver "com.apple.iokit.IOAcceleratorFamily2" "/System/Library/Extensions/IOAcceleratorFamily2.kext/Contents/Info.plist"
stamp_dep_ver "com.apple.iokit.IOGraphicsFamily"     "/System/Library/Extensions/IOGraphicsFamily.kext/Contents/Info.plist"
stamp_dep_ver "com.apple.iokit.IOPCIFamily"          "/System/Library/Extensions/IOPCIFamily.kext/Contents/Info.plist"
stamp_dep_ver "com.apple.iokit.IOSurface"            "/System/Library/Extensions/IOSurface.kext/Contents/Info.plist"

rm -f "$OUT/ArcAccel.o"
codesign --force --sign - "$KEXT"
plutil -lint "$KEXT/Contents/Info.plist"
echo "Built $KEXT ($(stat -f %z "$KEXT/Contents/MacOS/ArcAccel") bytes, kmod_info=$(nm "$KEXT/Contents/MacOS/ArcAccel" | grep -c "_kmod_info"))"
