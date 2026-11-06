#!/bin/bash
# OpenGL ES for tvOS on the S905X5M box: Mesa 25.0.7's OSMesa with gallium panfrost (Mali-G310, Valhall v10, CSF) behind HakiOS's EAGL shim.
#
#   out/libOSMesa.8.dylib     everything GL in one library: the gl* entry points, the GLSL compiler, panfrost (and softpipe as the fallback
#                             when there is no MaliService or OSMESA_SOFTWARE=1) and the OSMesa context interface. Install name
#                             /redwood/lib/libOSMesa.8.dylib, arm64, retagged tvOS 16.x, ldid-signed. MaliMetal's darwin-drm (the libdrm
#                             stand-in PanVK uses, Panthor uAPI over the kernel's MaliService) is linked in statically.
#   out/exported-gl.txt       the gl* functions it exports  } what hakios/userland/mkeagl.py needs
#   out/gl.xml                the Khronos registry          }
#   out/libhakios.gl.dylib    hakios/userland/src/eagl.c + gl_osmesa.c (as hakios/userland/build.sh GL=1 builds it), retagged + signed
#   out/glestest              on-box test (tvos/src/glestest.c): GLES context on the GPU, renderer, triangle, read-back, IOSurface texture
#
# Why 25.0.7: the last Mesa series with OSMesa (removed in 25.1). Its gallium panfrost knows the G310 (0xac74, variants 0-4) and drives
# arch 10 through the same src/panfrost/lib/kmod/panthor_kmod.c code PanVK uses, so the kernel side is the one PanVK already exercises.
# Patches: tvos/gpu/gl-patches/ (each one's header says what it changes and why).
#
# Toolchain as build-panvk.sh: system clang 23 (-target arm64-apple-ios16.0) + ld64.lld + llvm-ar against $THEOS/sdks/iPhoneOS16.5.sdk; no LLVM
# and no host tools (25.0.7's panfrost precompiles nothing). The shim is compiled against Mesa 26.1's drm-uapi headers (it serves ioctls
# 25.0.7 does not have); the ones 25.0.7 issues have the same numbers in both (struct drm_panthor_gpu_info only grew at the end).
#
# Stages (each skips itself when its output exists; FORCE=1 starts again from a fresh Mesa tree with the patches):
#   fetch  venv  src  shim  ios  post  eagl  test  check
# Run some: ./build-mesa-gl.sh ios post   (default: all)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tvos/gpu
TW="$(cd "$HERE/../.." && pwd)"                       # twilight-xnu
G="${G:-$TW/work/glbuild}"                           # sources, build trees and out/ (not in the repository)
MM="${MM:-$TW/work/src/MaliMetal-ken}"               # darwin-drm
UAPI="${UAPI:-$TW/hakios/drivers/gpu/hakios.mali_uapi.h}"
MESA26_INC="${MESA26_INC:-$TW/work/gpubuild/src/mesa-26.1.0-rc1/include}"   # drm-uapi headers the shim is written against
THEOS="${THEOS:-$HOME/theos}"
SDK="${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}"
LDID="$THEOS/toolchain/linux/iphone/bin/ldid"
H="$TW/hakios/userland"
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
MESA="$G/src/mesa-$VERSION"
B="$G/build"
OUT="$G/out"
IOS_TARGET=arm64-apple-ios16.0
INSTALL_NAME=/redwood/lib/libOSMesa.8.dylib
DSC="$TW/work/tvos/rootfs/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
IPSW="$TW/work/ipsw/ipsw"
TBD="$G/tvos-tbd"
mkdir -p "$G/src" "$G/dl" "$B" "$OUT"

# a running game gets at most 2 jobs
if ps -eLo comm | grep -qx GameThread; then J=2; else J=${J:-16}; fi
gamecheck() { if ps -eLo comm | grep -qx GameThread; then J=2; fi; }

unset CCC_OVERRIDE_OPTIONS LLVM_CONFIG CLANG_BIN CC CXX PKG_CONFIG_PATH PKG_CONFIG_LIBDIR

stage_fetch() {
	local T="$G/dl/mesa-$VERSION.tar.xz"
	[ -f "$T" ] || curl -sSfL -o "$T" "https://archive.mesa3d.org/mesa-$VERSION.tar.xz"
	echo "$SHA256  $T" | sha256sum -c --quiet
}

stage_venv() {
	[ -x "$G/venv/bin/python" ] || python3 -m venv "$G/venv"
	"$G/venv/bin/python" -c 'import mako, yaml, packaging' 2>/dev/null || "$G/venv/bin/pip" -q install mako pyyaml packaging
}

# A pristine tree (the release tarball, committed once into a local git so it can be reset) with gl-patches/ applied to the work tree.
# A tree patched with other patches than the current ones is reset and rebuilt.
stage_src() {
	local sum
	sum=$(cat "$HERE"/gl-patches/*.patch | sha256sum | cut -d' ' -f1)
	if [ -f "$B/.src-done" ] && [ "$(cat "$B/.src-done")" = "$sum" ] && [ "${FORCE:-0}" != 1 ]; then return; fi
	if [ ! -d "$MESA/.git" ]; then
		rm -rf "$MESA"
		tar -xJf "$G/dl/mesa-$VERSION.tar.xz" -C "$G/src"
		git -C "$MESA" init -q
		git -C "$MESA" add -A
		git -C "$MESA" -c user.name=build -c user.email=build@localhost commit -qm "mesa-$VERSION"
	fi
	git -C "$MESA" reset -q --hard
	git -C "$MESA" clean -fdxq
	for p in "$HERE"/gl-patches/*.patch; do
		git -C "$MESA" apply --whitespace=nowarn "$p" || { echo "error: $(basename "$p") does not apply" >&2; exit 1; }
		echo "applied $(basename "$p")"
	done
	rm -rf "$B/mesa-ios"
	echo "$sum" > "$B/.src-done"
}

# darwin-drm, the same source and flags as build-panvk.sh's shim stage (static archive + libdrm.pc for the cross build's pkg-config)
stage_shim() {
	local S="$MM/darwin-drm" D="$B/darwin-drm"
	[ -f "$MESA26_INC/drm-uapi/panthor_drm.h" ] || { echo "error: no Mesa 26.1 headers at $MESA26_INC (run build-panvk.sh fetch)" >&2; exit 1; }
	mkdir -p "$D/inc" "$D/pkgconfig"
	cp "$UAPI" "$D/inc/mali_uapi.h"
	# the GL build's own copy of the shim, with darwin-drm-gl.patch (GROUP_GET_STATE from the kernel's hang watchdog); the checkout stays pristine
	cp "$S/drm_darwin.c" "$D/drm_darwin.c"
	patch -s -d "$D" -p1 < "$TW/tvos/gpu/darwin-drm-gl.patch" || { echo "error: darwin-drm-gl.patch does not apply" >&2; exit 1; }
	clang -target $IOS_TARGET -isysroot "$SDK" -O2 -Wall -Wextra -fPIC -I"$S/include" -I"$MESA26_INC" -I"$D/inc" \
		-c "$D/drm_darwin.c" -o "$D/drm_darwin.o"
	rm -f "$D/libdrm_darwin.a"
	llvm-ar rcs "$D/libdrm_darwin.a" "$D/drm_darwin.o"
	sed -e "s|@PREFIX@|$S|" -e "s|@LIBDIR@|$D|" "$S/pkgconfig/libdrm.pc.in" > "$D/pkgconfig/libdrm.pc"
}

stage_ios() {
	gamecheck
	local D="$B/darwin-drm" X="$B/mesa-ios"
	export PATH="$G/venv/bin:$PATH"
	# (see build-panvk.sh: the SDK's libc++ headers, not the host's; Mesa's C fallback for the GCC-only *_overflow_p builtins)
	local F="'-target', '$IOS_TARGET', '-isysroot', '$SDK', '-I$MM/darwin-drm/include', '-UHAVE___BUILTIN_ADD_OVERFLOW_P', '-UHAVE___BUILTIN_SUB_OVERFLOW_P'"
	local CXXF="'-nostdinc++', '-isystem', '$SDK/usr/include/c++/v1', '-stdlib=libc++'"
	local LF="$F, '-fuse-ld=lld', '-Wl,-adhoc_codesign', '-Wl,-x', '-Wl,-map,$OUT/osmesa.map'"
	cat > "$B/mesa-ios.cross" <<-EOF
	[binaries]
	c = ['/usr/bin/clang']
	cpp = ['/usr/bin/clang++']
	objc = ['/usr/bin/clang']
	ar = '/usr/bin/llvm-ar'
	ranlib = '/usr/bin/llvm-ranlib'
	strip = '/usr/bin/llvm-strip'
	pkg-config = '/usr/bin/pkg-config'
	[host_machine]
	system = 'darwin'
	subsystem = 'ios'
	cpu_family = 'aarch64'
	cpu = 'arm64'
	endian = 'little'
	[properties]
	needs_exe_wrapper = true
	pkg_config_libdir = ['$D/pkgconfig']
	[built-in options]
	c_args = [$F]
	cpp_args = [$F, $CXXF]
	objc_args = [$F]
	c_link_args = [$LF]
	cpp_link_args = [$LF, '-stdlib=libc++']
	objc_link_args = [$LF]
	EOF
	[ -f "$X/build.ninja" ] || meson setup "$X" "$MESA" --cross-file "$B/mesa-ios.cross" --wrap-mode=nodownload \
		--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
		-Dplatforms= -Dgallium-drivers=panfrost,softpipe -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
		-Dosmesa=true -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dshared-glapi=enabled -Dglvnd=disabled \
		-Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
		-Dlibunwind=disabled -Dlmsensors=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
		-Dgallium-xa=disabled -Dbuild-tests=false > "$B/meson-setup.log" || { tail -40 "$B/meson-setup.log"; exit 1; }
	# the shim comes in as -ldrm_darwin, which ninja does not track: relink when it is newer than the library
	local DY="$X/src/gallium/targets/osmesa/libOSMesa.8.dylib"
	[ -f "$DY" ] && [ "$D/libdrm_darwin.a" -nt "$DY" ] && rm -f "$DY"
	ninja -C "$X" -j$J src/gallium/targets/osmesa/libOSMesa.8.dylib
}

stage_post() {
	local X="$B/mesa-ios/src/gallium/targets/osmesa/libOSMesa.8.dylib" O="$OUT/libOSMesa.8.dylib"
	cp "$X" "$O"
	llvm-install-name-tool -id "$INSTALL_NAME" "$O"
	llvm-nm -gU "$O" | awk '$3 ~ /^_gl/ { print substr($3, 2) }' | sort -u > "$OUT/exported-gl.txt"
	cp "$MESA/src/mapi/glapi/registry/gl.xml" "$OUT/gl.xml"
	python3 "$TW/tools/macho-tvos.py" "$O"
	"$LDID" -S "$O"
	echo "libOSMesa.8.dylib: $(stat -c %s "$O") bytes, $(wc -l < "$OUT/exported-gl.txt") gl* functions"
}

# libhakios.gl.dylib, as hakios/userland/build.sh GL=1 makes it (xnu-clang, the iOS 16.5 SDK), then made a tvOS library
stage_eagl() {
	# shellcheck disable=SC1091
	source "$TW/hakios/xnu-env.sh"
	local DYN="xnu-clang -arch arm64 -isysroot $SDK -O1" O="$B/eagl"
	mkdir -p "$O"
	python3 "$H/mkeagl.py" "$H/src/eagl_slots.txt" "$OUT/gl.xml" "$OUT/exported-gl.txt" "$H/src/eagl.c" > "$O/eagl_gen.h"
	$DYN -dynamiclib -install_name /redwood/lib/libhakios.gl.dylib -I"$O" -framework CoreFoundation -framework IOKit -framework IOSurface \
		-lobjc "$OUT/libOSMesa.8.dylib" -o "$OUT/libhakios.gl.dylib" "$H/src/eagl.c" "$H/src/gl_osmesa.c"
	python3 "$TW/tools/macho-tvos.py" "$OUT/libhakios.gl.dylib"
	"$LDID" -S "$OUT/libhakios.gl.dylib"
}

# the on-box test: a tvOS program (ad hoc: what the kernel's AMFI stand-in takes for a platform binary) that dlopen()s libOSMesa.8.dylib
# from its own directory, else from /redwood/lib
stage_test() {
	# shellcheck disable=SC1091
	source "$TW/hakios/xnu-env.sh"
	local DYN="xnu-clang -arch arm64 -isysroot $SDK -O1"
	$DYN -Wall -framework CoreFoundation -framework IOSurface -o "$OUT/glestest" "$TW/tvos/src/glestest.c"
	python3 "$TW/tools/macho-tvos.py" "$OUT/glestest"
	"$LDID" -S -Cadhoc "$OUT/glestest"
}

# every import must exist in tvOS 16.5's shared cache (dyld refuses the image otherwise)
stage_check() {
	if [ ! -f "$DSC" ] || [ ! -x "$IPSW" ]; then echo "== (no tvOS shared cache or ipsw here: imports not checked)"; return; fi
	mkdir -p "$TBD"
	for l in libSystem.B.dylib libc++.1.dylib libc++abi.dylib libobjc.A.dylib libz.1.dylib CoreFoundation IOSurface IOKit; do
		[ -f "$TBD/$l.tbd" ] || [ -f "$TBD/$l.dylib.tbd" ] || (cd "$TBD" && "$IPSW" dyld tbd "$DSC" "$l" -o . >/dev/null 2>&1) || echo "warning: no tbd for $l"
	done
	# libhakios.gl.dylib's imports from libOSMesa (not in the cache: the library we just built) are checked against its exports
	{ printf 'symbols: [ '; llvm-nm -gU "$OUT/libOSMesa.8.dylib" | awk '{ printf "%s'\''%s'\''", n++ ? ", " : "", $3 }'; printf ' ]\n'; } > "$TBD/libOSMesa.tbd"
	for f in "$OUT/libOSMesa.8.dylib" "$OUT/libhakios.gl.dylib" "$OUT/glestest"; do
		[ -f "$f" ] || continue
		echo "== imports of ${f##*/}: $(/usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD" | tail -1)"
		/usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD" >/dev/null || { /usr/bin/python3 -I "$HERE/check-imports.py" "$f" "$TBD"; exit 1; }
	done
	ls -l "$OUT"
}

stages=("$@")
[ ${#stages[@]} = 0 ] && stages=(fetch venv src shim ios post eagl test check)
for s in "${stages[@]}"; do echo "=== $s"; "stage_$s"; done
