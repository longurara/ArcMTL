#!/bin/bash
# Build ArcFB.kext on macOS 15 Sequoia / 26 Tahoe (Xcode 16+)
set -euo pipefail

SRC=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SRC/../.." && pwd)
OUT=${1:-"$ROOT/out/kexts"}
mkdir -p "$OUT"

SDK=$(xcrun --show-sdk-path)
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"

CFLAGS=(
  -arch x86_64 -fapple-kext -mkernel -nostdinc
  -I"$KHDR" -I"$SRC" -I"$ROOT/include"
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -mmacosx-version-min=15.0 -g0
  -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common
  -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2
)

echo "=== Building ArcFB.kext ==="
xcrun clang++ "${CFLAGS[@]}" -c "$SRC/ArcFB.cpp" -o "$OUT/ArcFB.o"

KEXT="$OUT/ArcFB.kext"
rm -rf "$KEXT"
mkdir -p "$KEXT/Contents/MacOS"

xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib \
  -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "$OUT/ArcFB.o" -o "$KEXT/Contents/MacOS/ArcFB"

cp "$SRC/Info.plist" "$KEXT/Contents/Info.plist"

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
stamp_dep_ver "com.apple.iokit.IOGraphicsFamily" "/System/Library/Extensions/IOGraphicsFamily.kext/Contents/Info.plist"
stamp_dep_ver "com.apple.iokit.IOPCIFamily"      "/System/Library/Extensions/IOPCIFamily.kext/Contents/Info.plist"

rm -f "$OUT/ArcFB.o"
codesign --force --sign - "$KEXT"
plutil -lint "$KEXT/Contents/Info.plist"
echo "Built $KEXT ($(stat -f %z "$KEXT/Contents/MacOS/ArcFB") bytes, kmod_info=$(nm "$KEXT/Contents/MacOS/ArcFB" | grep -c "_kmod_info"))"
