#!/bin/zsh
# Thor port: build the shader translator with its Vulkan service mode (totk_xlate --service-vk, VkService.cs), compiled
# ahead of time with NativeAOT into one program that needs no .NET runtime:
#   android  linux-bionic-arm64, linked with the NDK's clang (cross-compiled from the Mac): for the APK
#   mac      osx-arm64: for checks on the Mac (check_parity.py)
# The sources are the Mac port's translator (games/totk/tools/totk_xlate/*.cs) and vendor/ryujinx with the translator's
# patches 0001 to 0004. Not 0005 (unwritten vertex outputs start as (0, 0, 0, 1) in the translator): Scarlet's Mac
# translator (prebuilt/macos-arm64, from the Bayonetta 3 port) was built before it, and VkService.cs does the same thing
# itself, as the bridge did; with 0001 to 0004 the translator's SPIR-V matches that binary byte for byte (351 of 351
# variants of the Mac's shader cache). Needs the .NET 10 SDK, the Android NDK (android) and network access the first
# time (NuGet: the NativeAOT compiler and the linux-bionic runtime pack).
# usage: games/scarlet/android/xlate/build.sh [android|mac] [dotnet]  -> SCARLET_build/build-android-tools/xlate-<target>/totk_xlate
set -e
HERE=${0:A:h}
SCARLET=${HERE:h:h}
REPO=${SCARLET:h:h}
TARGET=${1:-android}
DOTNET=${2:-$HOME/.dotnet/dotnet}
NDK=${NDK:-/opt/homebrew/share/android-ndk}
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
case $TARGET in
  android) RID=linux-bionic-arm64
           NDKBIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin
           EXTRA=(-p:DisableUnsupportedError=true -p:PublishAotUsingRuntimePack=true
                  -p:CppCompilerAndLinker=$NDKBIN/aarch64-linux-android33-clang -p:ObjCopyName=$NDKBIN/llvm-objcopy) ;;
  mac)     RID=osx-arm64; EXTRA=() ;;
  *) echo "usage: build.sh [android|mac] [dotnet]" >&2; exit 2 ;;
esac
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM HUP
mkdir -p "$WORK/ryujinx-src" "$WORK/xlate"
cp -R "$REPO/vendor/ryujinx/." "$WORK/ryujinx-src/"
find "$WORK/ryujinx-src/src" -maxdepth 2 \( -name obj -o -name bin \) -type d -exec rm -rf {} +
for p in "$REPO"/games/totk/tools/totk_xlate/patches/000[1-4]-*.patch; do patch -s -d "$WORK/ryujinx-src" -p1 --forward < "$p"; done
cp "$REPO"/games/totk/tools/totk_xlate/*.cs "$HERE"/*.cs "$HERE"/scarlet_xlate.csproj "$WORK/xlate/"
OUT=$SCARLET/SCARLET_build/build-android-tools/xlate-$TARGET
"$DOTNET" publish "$WORK/xlate/scarlet_xlate.csproj" -c Release -r $RID -p:PublishAot=true -p:RyuSrc="$WORK/ryujinx-src" \
  $EXTRA -o "$WORK/out" | grep -vE "warning IL|^\s*$" || true
[ -f "$WORK/out/totk_xlate" ] || { echo "build failed" >&2; exit 1; }
mkdir -p "$OUT"
cp "$WORK/out/totk_xlate" "$OUT/totk_xlate"
echo "built $OUT/totk_xlate ($(wc -c < "$OUT/totk_xlate" | tr -d ' ') bytes)"
