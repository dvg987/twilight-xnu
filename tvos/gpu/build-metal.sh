#!/bin/bash
# NativeMaliMetal.bundle for twilight (tvOS 16.5 20L563, AppleTV6,2 identity, Amlogic S905X5M Mali-G310):
# Ken-u/MaliMetal's Metal plug-in + native Metal front end + Rust metal2vulkan bridge, cross-built on Linux.
#
#   ./build-metal.sh            sources (fetched once), bridge (cargo), plug-in, bundle
#   ICD=/path/libvulkan_panfrost.dylib ./build-metal.sh   also put that PanVK build into the bundle
#   PROFILE_EXTRA= ./build-metal.sh                        quick bridge build (no fat LTO)
#
# Inputs:  work/src/MaliMetal-ken (pristine, + tvos.patch next to this script), hakios/drivers/gpu/hakios.mali_uapi.h,
#          metal2vulkan @ 720ef1ce + UniXNU/AdrenoMetal bridge/patches @ d6ca655 + MaliMetal m2v-patches (fetched into src/),
#          $THEOS/sdks/iPhoneOS16.5.sdk, system clang/lld/llvm-as (LLVM 23), rustup toolchain 1.98.1 with aarch64-apple-ios.
# Output:  out/NativeMaliMetal.bundle/{NativeMaliMetal, Info.plist[, libvulkan_panfrost.dylib]}
# Everything lives under this directory (cargo target dir included); nothing goes to /tmp.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tvos/gpu: this script, metal-tvos.patch, m2v-patches/, check-imports.py
TW="$(cd "$HERE/../.." && pwd)"                       # ~/clones/twilight-xnu
W="${W:-$TW/work/metalbuild}"                        # sources, build trees and out/ (not in the repository)
mkdir -p "$W"
THEOS="${THEOS:-$HOME/theos}"
SDK="${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}"
LDID="${LDID:-$THEOS/toolchain/linux/iphone/bin/ldid}"
CLANG="${CLANG:-clang}"
LLVM_AS="${LLVM_AS:-/usr/bin/llvm-as}"
LLVM_AR="${LLVM_AR:-llvm-ar}"
RUST="${RUST:-1.98.1}"
UAPI="${UAPI:-$TW/hakios/drivers/gpu/hakios.mali_uapi.h}"
KEN="${KEN:-$TW/work/src/MaliMetal-ken}"
GPUOUT="$TW/work/gpubuild/out"                       # the PanVK build of the other job (read only)
SRC="$W/src"; BUILD="$W/build"; OUT="$W/out"
M2V="$SRC/m2v"; ADRENO="$SRC/adreno"; MM="$SRC/MaliMetal"
M2V_REV=720ef1ce9ae2a2068999a8675717418b9416283e
ADRENO_REV=d6ca6557039cf9e0c3a1d1a58c560f2e55e8e9bb
TARGET_OS=16.0
mkdir -p "$SRC" "$BUILD" "$OUT"

JOBS="${JOBS:-$(nproc)}"
if ps -eLo comm | grep -qx GameThread; then JOBS=2; echo "== a game is running: $JOBS jobs"; fi

# ---- 1. sources --------------------------------------------------------------------------------------------------------------------------
if [ ! -d "$M2V/.git" ]; then
	# Partial clone at the pinned revision; the 1.5 GB validation corpus is not checked out (nothing here builds or patches it).
	git init -q "$M2V"
	git -C "$M2V" remote add origin https://github.com/steelbrain/metal2vulkan
	git -C "$M2V" config remote.origin.promisor true
	git -C "$M2V" config remote.origin.partialclonefilter blob:none
	git -C "$M2V" fetch -q --depth 1 --filter=blob:none origin "$M2V_REV"
	git -C "$M2V" update-ref refs/heads/pin "$M2V_REV"
	git -C "$M2V" sparse-checkout init --no-cone
	printf '/*\n!/crates/metal2vulkan-validation/corpus/\n' > "$M2V/.git/info/sparse-checkout"
	git -C "$M2V" checkout -q pin
fi
if [ ! -d "$ADRENO/.git" ]; then
	git init -q "$ADRENO"
	git -C "$ADRENO" remote add origin https://github.com/UniXNU/AdrenoMetal
	git -C "$ADRENO" fetch -q --depth 1 origin "$ADRENO_REV"
	git -C "$ADRENO" update-ref refs/heads/pin "$ADRENO_REV"
fi
if [ ! -d "$MM/.git" ]; then
	git clone -q "$KEN" "$MM"
	git -C "$MM" apply "$HERE/metal-tvos.patch"
elif git -C "$MM" diff --quiet; then
	git -C "$MM" apply "$HERE/metal-tvos.patch"
fi
[ "$(git -C "$M2V" rev-parse HEAD)" = "$M2V_REV" ] || { echo "error: $M2V is not at $M2V_REV" >&2; exit 1; }

# metal2vulkan patches (as MaliMetal's m2v-patches/apply.sh, with the AdrenoMetal patches from the pinned commit): only redone when a patch changed,
# so cargo keeps its incremental state.
# Our own metal2vulkan fixes (patches-local/*.patch, applied last, in name order; none needed so far).
LOCAL_PATCHES=("$HERE"/m2v-patches/*.patch); [ -e "${LOCAL_PATCHES[0]}" ] || LOCAL_PATCHES=()
STAMP="$( (git -C "$ADRENO" rev-parse "$ADRENO_REV"; cat "$MM"/metal-plugin/m2v-patches/apply.sh "$MM"/metal-plugin/m2v-patches/0*.patch ${LOCAL_PATCHES[@]+"${LOCAL_PATCHES[@]}"}) | sha256sum | cut -c1-16)"
if [ "$(cat "$BUILD/m2v.stamp" 2>/dev/null)" != "$STAMP" ]; then
	echo "== patching metal2vulkan"
	git -C "$M2V" checkout -q . && git -C "$M2V" clean -fdq
	for f in $(git -C "$ADRENO" ls-tree --name-only "$ADRENO_REV" bridge/patches/ | sort); do
		git -C "$ADRENO" show "$ADRENO_REV:$f" | git -C "$M2V" apply - || { echo "error: $f does not apply" >&2; exit 1; }
	done
	# The 8 MiB pool block of MaliMetal's apply.sh (the kernel driver hands out physically contiguous memory).
	sed -n '/^\/usr\/bin\/python3 - <<.PYEOF./,/^PYEOF$/p' "$MM/metal-plugin/m2v-patches/apply.sh" | sed '1d;$d' > "$BUILD/m2v-block.py"
	(cd "$M2V" && /usr/bin/python3 -I "$BUILD/m2v-block.py")
	for f in "$MM"/metal-plugin/m2v-patches/0*.patch ${LOCAL_PATCHES[@]+"${LOCAL_PATCHES[@]}"}; do git -C "$M2V" apply "$f" || { echo "error: $f does not apply" >&2; exit 1; }; done
	echo "$STAMP" > "$BUILD/m2v.stamp"
	echo "== $(git -C "$M2V" diff --stat | tail -1)"
fi

# ---- 2. the Rust bridge (staticlib) ------------------------------------------------------------------------------------------------------
B="$BUILD/bridge"; mkdir -p "$B"
air() { "$LLVM_AS" -o "$2.new" "$1"; cmp -s "$2.new" "$2" && rm "$2.new" || mv "$2.new" "$2"; }   # (include_bytes!: keep the mtime when unchanged)
air "$M2V/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll" "$B/runtime_examples.air"
air "$M2V/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll" "$B/function_constant.air"
air "$MM/metal-plugin/bridge/scanout_convert.ll" "$B/scanout_convert.air"
RUNTIME_ID="$( (git -C "$M2V" rev-parse HEAD; cd "$M2V" && find crates Cargo.toml -type f -not -path '*/target/*' | sort | xargs sha256sum) | sha256sum | cut -c1-32)"
if [ -z "${PROFILE_EXTRA+x}" ]; then PROFILE_EXTRA=$'lto = "fat"\ncodegen-units = 1'; fi
cat > "$B/Cargo.toml.new" <<TOML
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$MM/metal-plugin/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$M2V/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$M2V/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
${PROFILE_EXTRA}
TOML
cmp -s "$B/Cargo.toml.new" "$B/Cargo.toml" && rm "$B/Cargo.toml.new" || mv "$B/Cargo.toml.new" "$B/Cargo.toml"   # unchanged: cargo keeps its build
unset CCC_OVERRIDE_OPTIONS CC CXX CFLAGS CXXFLAGS LDFLAGS
export NMB_FIXTURE_AIR="$B/runtime_examples.air" NMB_FC_AIR="$B/function_constant.air" NMB_SCANOUT_AIR="$B/scanout_convert.air" NMB_RUNTIME_ID="$RUNTIME_ID"
export IPHONEOS_DEPLOYMENT_TARGET=$TARGET_OS SDKROOT="$SDK"
export CC_aarch64_apple_ios="$CLANG" AR_aarch64_apple_ios="$LLVM_AR"
export CFLAGS_aarch64_apple_ios="-target arm64-apple-ios$TARGET_OS -isysroot $SDK"
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER="$CLANG"
export CARGO_HOME="${CARGO_HOME:-$HOME/.cargo}"
echo "== bridge (cargo +$RUST, $JOBS jobs)"
(cd "$B" && cargo "+$RUST" build --release -j "$JOBS" --manifest-path "$B/Cargo.toml" --target aarch64-apple-ios --target-dir "$B/target" 2>&1 | tail -30)
BRIDGE="$B/target/aarch64-apple-ios/release/libnative_metal_bridge.a"
[ -f "$BRIDGE" ] || { echo "error: no $BRIDGE" >&2; exit 1; }

# ---- 3. the plug-in --------------------------------------------------------------------------------------------------------------------
INC="$BUILD/include"; mkdir -p "$INC"
cp "$UAPI" "$INC/mali_uapi.h"                         # the plug-in includes "mali_uapi.h": our kernel's copy of Ken's header
P="$MM/metal-plugin"
BUNDLE="$OUT/NativeMaliMetal.bundle"
rm -rf "$BUNDLE"; mkdir -p "$BUNDLE"
echo "== plug-in"
OBJ="$BUILD/obj"; mkdir -p "$OBJ"
CXXFLAGS_PLUGIN=(-target arm64-apple-ios$TARGET_OS -isysroot "$SDK" -DNMM_TVOS=1 -O2 -g0 -std=c++17 -fobjc-arc -Wall -Wextra
	-Wno-deprecated-declarations -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
	-Wno-unguarded-availability-new -Wno-unknown-warning-option -Wno-vla-cxx-extension -Wno-nontrivial-memcall
	-Wno-missing-designated-field-initializers -nostdinc++ -isystem "$SDK/usr/include/c++/v1" -I"$P/frontend" -I"$INC")
"$CLANG" "${CXXFLAGS_PLUGIN[@]}" -c -x objective-c++ "$P/NativeMaliMetalPlugin.mm" -o "$OBJ/NativeMaliMetalPlugin.o" & PID1=$!
"$CLANG" "${CXXFLAGS_PLUGIN[@]}" -c -x objective-c++ "$P/frontend/NativeBridgeMetalFrontend.mm" -o "$OBJ/NativeBridgeMetalFrontend.o" & PID2=$!
wait $PID1 && wait $PID2 || { echo "error: the plug-in did not compile" >&2; exit 1; }
LINK_IN=("$OBJ/NativeMaliMetalPlugin.o" "$OBJ/NativeBridgeMetalFrontend.o" "$BRIDGE")
LINK_LIBS=(-lobjc -lc++ -framework Foundation -framework Metal -framework IOSurface)
"$CLANG" -target arm64-apple-ios$TARGET_OS -isysroot "$SDK" -bundle -fuse-ld=lld -Wl,-dead_strip "${LINK_IN[@]}" "${LINK_LIBS[@]}" \
	-o "$BUNDLE/NativeMaliMetal" 2> >(grep -v "directory not found" >&2) || { echo "error: the plug-in did not link" >&2; exit 1; }
python3 "$TW/tools/macho-tvos.py" "$BUNDLE/NativeMaliMetal"   # LC_BUILD_VERSION platform 3 (tvOS), min 16.0, sdk 16.5
"$LDID" -S "$BUNDLE/NativeMaliMetal"

/usr/bin/python3 - "$BUNDLE/Info.plist" <<'PYEOF'
import plistlib, sys
open(sys.argv[1], 'wb').write(plistlib.dumps(dict(
    CFBundleIdentifier='io.github.ken-u.NativeMaliMetal', CFBundleName='NativeMaliMetal', CFBundleExecutable='NativeMaliMetal',
    CFBundlePackageType='BNDL', CFBundleInfoDictionaryVersion='6.0', CFBundleDevelopmentRegion='en',
    CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeMaliMetalDevice',
    CFBundleSupportedPlatforms=['AppleTVOS'], DTPlatformName='appletvos', DTSDKName='appletvos16.5', DTPlatformVersion='16.5',
    MinimumOSVersion='16.0', UIDeviceFamily=[3]), fmt=plistlib.FMT_XML))
PYEOF

# ---- 4. PanVK into the bundle (the plug-in loads <bundle>/libvulkan_panfrost.dylib) ---------------------------------------------------
ICD="${ICD:-}"
if [ -z "$ICD" ]; then ICD="$(ls "$GPUOUT"/libvulkan_panfrost.dylib "$GPUOUT"/*/libvulkan_panfrost.dylib 2>/dev/null | head -1 || true)"; fi
if [ -n "$ICD" ] && [ -f "$ICD" ]; then
	cp "$ICD" "$BUNDLE/libvulkan_panfrost.dylib"
	echo "== PanVK from $ICD (copied as is: retag/sign it in its own build)"
else
	echo "== no libvulkan_panfrost.dylib yet: place it at /System/Library/Extensions/NativeMaliMetal.bundle/libvulkan_panfrost.dylib"
fi

# ---- 5. every import must exist in tvOS 16.5's shared cache (dyld refuses the bundle otherwise) ----------------------------------------
DSC="$TW/work/tvos/rootfs/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
IPSW="$TW/work/ipsw/ipsw"
TBD="$BUILD/tvos-tbd"
if [ -f "$DSC" ] && [ -x "$IPSW" ]; then
	mkdir -p "$TBD"
	for l in libSystem.B.dylib libc++.1.dylib libc++abi.dylib libobjc.A.dylib Foundation CoreFoundation Metal IOSurface IOKit; do
		[ -f "$TBD/$l.tbd" ] || [ -f "$TBD/$l.dylib.tbd" ] || (cd "$TBD" && "$IPSW" dyld tbd "$DSC" "$l" -o . >/dev/null 2>&1) || echo "warning: no tbd for $l"
	done
	for f in "$BUNDLE"/NativeMaliMetal "$BUNDLE"/libvulkan_panfrost.dylib; do
		[ -f "$f" ] || continue
		echo "== imports of ${f##*/}: $(/usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD" | tail -1)"
		/usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD" >/dev/null || { /usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD"; exit 1; }
	done
else
	echo "== (no tvOS shared cache or ipsw here: imports not checked)"
fi
ls -l "$BUNDLE"
