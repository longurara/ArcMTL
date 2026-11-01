#!/bin/bash
# Build ArcXe.kext on macOS 15 Sequoia / 26 Tahoe (Xcode 16+)
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

echo "=== Building ArcXe.kext ==="
OBJS=()
for f in ArcXeService.cpp ArcXeMemory.cpp ArcXeExec.cpp ArcXeUserClient.cpp; do
  obj="$OUT/${f%.cpp}.o"
  xcrun clang++ "${CFLAGS[@]}" -c "$SRC/$f" -o "$obj"
  OBJS+=("$obj")
done

KEXT="$OUT/ArcXe.kext"
rm -rf "$KEXT"
mkdir -p "$KEXT/Contents/MacOS"

xcrun clang++ -arch x86_64 -mmacosx-version-min=15.0 -fapple-kext -nostdlib \
  -Xlinker -kext -lkmodc++ -lkmod -lcc_kext \
  "${OBJS[@]}" -o "$KEXT/Contents/MacOS/ArcXe"

cp "$SRC/Info.plist" "$KEXT/Contents/Info.plist"

if [ -f "/System/Library/Extensions/IOPCIFamily.kext/Contents/Info.plist" ]; then
  VER=$(/usr/libexec/PlistBuddy -c "Print :OSBundleCompatibleVersion" "/System/Library/Extensions/IOPCIFamily.kext/Contents/Info.plist" 2>/dev/null || true)
  if [ -n "$VER" ]; then
    /usr/libexec/PlistBuddy -c "Set :OSBundleLibraries:com.apple.iokit.IOPCIFamily $VER" "$KEXT/Contents/Info.plist"
  fi
fi

rm -f "${OBJS[@]}"
codesign --force --sign - "$KEXT"
plutil -lint "$KEXT/Contents/Info.plist"
echo "Built $KEXT ($(stat -f %z "$KEXT/Contents/MacOS/ArcXe") bytes, kmod_info=$(nm "$KEXT/Contents/MacOS/ArcXe" | grep -c "_kmod_info"))"
