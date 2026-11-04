#!/bin/bash
# PanVK (Mesa 26.1.0-rc1 + MaliMetal's patches) for the S905X5M box: libvulkan_panfrost.dylib, arm64, retagged tvOS 16.x, ldid-signed,
# with MaliMetal's darwin-drm libdrm stand-in linked in statically (it talks to the kernel's MaliService, hakios.mali_uapi.h).
#
#   out/libvulkan_panfrost.dylib   the ICD (what NativeMaliMetal.bundle carries next to its executable)
#   out/libdrm_darwin.a            the shim on its own (already inside the dylib; for other consumers)
#   out/panvk.map                  link map (vk_test.c / the plugin's profiler turn offsets into functions with it)
#
# Host side (no root): system clang/LLVM 21 (/usr/lib/llvm21, Arch's llvm21/clang21) for mesa_clc + libclc + SPIRV-LLVM-Translator,
# system clang 23 + ld64.lld + llvm-ar for the iOS cross build, the iPhoneOS 16.5 SDK of theos.
#
# Stages (each skips itself when its output exists; FORCE=1 redoes the Mesa source + both Mesa builds):
#   fetch  venv  deps (SPIRV-Headers, llvm-spirv, libclc)  src (fresh Mesa + patches/)  host (mesa_clc, vtn_bindgen2, panfrost_compile)
#   shim (darwin-drm)  ios (cross Mesa)  post (fix weak imports, tvOS retag, ldid)
# Run one stage: ./build-panvk.sh ios   (default: all)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tvos/gpu: this script and its Mesa patches (panvk-patches/)
TW="$(cd "$HERE/../.." && pwd)"                       # twilight-xnu
G="${G:-$TW/work/gpubuild}"                          # sources, tools, build trees and out/ (not in the repository)
mkdir -p "$G"
MM="${MM:-$TW/work/src/MaliMetal-ken}"            # Ken-u/MaliMetal checkout (darwin-drm, fix-weak-imports.py)
UAPI="${UAPI:-$TW/hakios/drivers/gpu/hakios.mali_uapi.h}"
THEOS="${THEOS:-$HOME/theos}"
SDK="${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}"
LDID="$THEOS/toolchain/linux/iphone/bin/ldid"
L21=/usr/lib/llvm21
MESA_TAG=mesa-26.1.0-rc1
MESA="$G/src/$MESA_TAG"
P="$G/prefix"
B="$G/build"
OUT="$G/out"
IOS_TARGET=arm64-apple-ios16.0
mkdir -p "$G/src" "$G/dl" "$B" "$OUT" "$P"

# a running game gets at most 2 jobs
if ps -eLo comm | grep -qx GameThread; then J=2; else J=${J:-16}; fi
gamecheck() { if ps -eLo comm | grep -qx GameThread; then J=2; fi; }

# the UniXNU/kernel environments leak these into clang/meson (see MaliMetal mesa-build/README.md)
unset CCC_OVERRIDE_OPTIONS LLVM_CONFIG CLANG_BIN CC CXX PKG_CONFIG_PATH PKG_CONFIG_LIBDIR

stage_fetch() {
	cd "$G/src"
	[ -d "$MESA/.git" ] || git clone -q --depth 1 --branch $MESA_TAG https://gitlab.freedesktop.org/mesa/mesa.git "$MESA"
	[ -d SPIRV-Headers/.git ] || git clone -q --depth 1 https://github.com/KhronosGroup/SPIRV-Headers.git
	[ -d SPIRV-LLVM-Translator/.git ] || git clone -q --depth 1 --branch llvm_release_210 https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git
	for t in libclc cmake; do
		[ -f "$G/dl/$t-21.1.8.src.tar.xz" ] || curl -sSfL -o "$G/dl/$t-21.1.8.src.tar.xz" \
			https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/$t-21.1.8.src.tar.xz
	done
	[ -d libclc-21.1.8.src ] || tar -xJf "$G/dl/libclc-21.1.8.src.tar.xz"
	# libclc's standalone build looks for ../cmake/Modules
	[ -d cmake ] || { tar -xJf "$G/dl/cmake-21.1.8.src.tar.xz" && mv cmake-21.1.8.src cmake; }
}

stage_venv() {
	[ -x "$G/venv/bin/python" ] || python3 -m venv "$G/venv"
	"$G/venv/bin/python" -c 'import mako, yaml, ply, packaging' 2>/dev/null || "$G/venv/bin/pip" -q install mako pyyaml ply packaging
}

stage_deps() {
	[ -f "$P/share/clc/spirv64-mesa3d-.spv" ] && [ -x "$P/bin/llvm-spirv" ] && return
	gamecheck
	cmake -G Ninja -S "$G/src/SPIRV-Headers" -B "$B/spirv-headers" -DCMAKE_INSTALL_PREFIX="$P" -DSPIRV_HEADERS_ENABLE_TESTS=OFF
	ninja -C "$B/spirv-headers" install
	cmake -G Ninja -S "$G/src/SPIRV-LLVM-Translator" -B "$B/llvm-spirv" -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=$L21/bin/clang -DCMAKE_CXX_COMPILER=$L21/bin/clang++ -DCMAKE_INSTALL_PREFIX="$P" \
		-DLLVM_DIR=$L21/lib/cmake/llvm -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$G/src/SPIRV-Headers" \
		-DLLVM_SPIRV_INCLUDE_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF
	ninja -C "$B/llvm-spirv" -j$J install
	cmake -G Ninja -S "$G/src/libclc-21.1.8.src" -B "$B/libclc" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
		-DLLVM_DIR=$L21/lib/cmake/llvm -DLLVM_CMAKE_DIR=$L21/lib/cmake/llvm -DLLVM_SPIRV="$P/bin/llvm-spirv" \
		-DCMAKE_C_COMPILER=$L21/bin/clang -DCMAKE_CXX_COMPILER=$L21/bin/clang++ \
		-DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-"
	ninja -C "$B/libclc" -j$J install
}

# A fresh Mesa tree with patches/ (MaliMetal's 0001..0011, 0004 and 0006 ported to 26.1.0-rc1). Nothing is committed: the shallow
# clone is reset to the tag and the patches are applied to the work tree.
stage_src() {
	if [ -f "$B/.src-done" ] && [ "${FORCE:-0}" != 1 ]; then return; fi
	git -C "$MESA" reset -q --hard
	git -C "$MESA" clean -fdxq
	for p in "$HERE"/panvk-patches/*.patch; do
		git -C "$MESA" apply --whitespace=nowarn "$p" || { echo "error: $(basename "$p") does not apply" >&2; exit 1; }
		echo "applied $(basename "$p")"
	done
	rm -rf "$B/mesa-host" "$B/mesa-ios"
	touch "$B/.src-done"
}

# Host Mesa: only the three build tools the cross build runs (-Dmesa-clc=system -Dprecomp-compiler=system), against LLVM 21.
stage_host() {
	local T="$P/host-tools/bin"
	if [ -x "$T/mesa_clc" ] && [ -x "$T/vtn_bindgen2" ] && [ -x "$T/panfrost_compile" ] && [ -f "$B/mesa-host/build.ninja" ]; then return; fi
	gamecheck
	export PATH="$P/bin:$G/venv/bin:$PATH" PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/share/pkgconfig"
	cat > "$B/host-native.ini" <<-EOF
	[binaries]
	c = '$L21/bin/clang'
	cpp = '$L21/bin/clang++'
	llvm-config = '$L21/bin/llvm-config'
	EOF
	[ -f "$B/mesa-host/build.ninja" ] || meson setup "$B/mesa-host" "$MESA" --native-file "$B/host-native.ini" \
		--prefix="$P/mesa-host" --wrap-mode=nodownload \
		-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dllvm=enabled -Dshared-llvm=enabled \
		-Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dtools= -Dvideo-codecs= -Dvulkan-layers= -Dxmlconfig=disabled -Dshader-cache=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dbuild-tests=false -Dbuildtype=release
	ninja -C "$B/mesa-host" -j$J src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile
	mkdir -p "$T"
	cp "$B/mesa-host/src/compiler/clc/mesa_clc" "$B/mesa-host/src/compiler/spirv/vtn_bindgen2" \
		"$B/mesa-host/src/panfrost/clc/panfrost_compile" "$T/"
}

# darwin-drm: drm_darwin.c against the kernel's uAPI header (hakios.mali_uapi.h == Ken-u's mali_uapi.h), static archive + libdrm.pc
# that the cross build finds through pkg-config.
stage_shim() {
	local S="$MM/darwin-drm" D="$B/darwin-drm"
	mkdir -p "$D/inc" "$D/pkgconfig"
	cp "$UAPI" "$D/inc/mali_uapi.h"
	clang -target $IOS_TARGET -isysroot "$SDK" -O2 -Wall -Wextra -fPIC -I"$S/include" -I"$MESA/include" -I"$D/inc" \
		-c "$S/drm_darwin.c" -o "$D/drm_darwin.o"
	rm -f "$D/libdrm_darwin.a"
	llvm-ar rcs "$D/libdrm_darwin.a" "$D/drm_darwin.o"
	sed -e "s|@PREFIX@|$S|" -e "s|@LIBDIR@|$D|" "$S/pkgconfig/libdrm.pc.in" > "$D/pkgconfig/libdrm.pc"
}

stage_ios() {
	gamecheck
	local D="$B/darwin-drm" X="$B/mesa-ios"
	export PATH="$P/host-tools/bin:$P/bin:$G/venv/bin:$PATH"
	# -nostdinc++: the host's /usr/include/c++/v1 (libc++ 23) would otherwise win over the SDK's (iOS 16.5 libc++.dylib).
	# clang's __has_builtin says yes to the GCC-only __builtin_*_overflow_p: undefine Mesa's HAVE_ so it takes its C fallback.
	local F="'-target', '$IOS_TARGET', '-isysroot', '$SDK', '-I$MM/darwin-drm/include', '-UHAVE___BUILTIN_ADD_OVERFLOW_P', '-UHAVE___BUILTIN_SUB_OVERFLOW_P'"
	local CXXF="'-nostdinc++', '-isystem', '$SDK/usr/include/c++/v1', '-stdlib=libc++'"
	local LF="$F, '-fuse-ld=lld', '-Wl,-adhoc_codesign', '-Wl,-S', '-Wl,-x', '-Wl,-map,$OUT/panvk.map'"
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
		-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= \
		-Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
		-Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
		-Dshared-glapi=disabled -Dtools= -Dvideo-codecs= -Dvulkan-layers= -Dxmlconfig=disabled -Dshader-cache=disabled \
		-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dexpat=disabled -Dbuild-tests=false \
		-Dbuildtype=release -Db_ndebug=true
	# the shim comes in as -ldrm_darwin, which ninja does not track: relink when it is newer than the dylib
	local DY="$X/src/panfrost/vulkan/libvulkan_panfrost.dylib"
	[ -f "$DY" ] && [ "$D/libdrm_darwin.a" -nt "$DY" ] && rm -f "$DY"
	# PanVK's weak entrypoints (no weak aliases in Mach-O) link through -undefined dynamic_lookup. It must not be in the cross file:
	# meson's has_function probes would all "succeed". Only the ninja step gets it, through the clang driver.
	CCC_OVERRIDE_OPTIONS='# +-Wl,-undefined,dynamic_lookup' ninja -C "$X" -j$J src/panfrost/vulkan/libvulkan_panfrost.dylib
}

stage_post() {
	local X="$B/mesa-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib" O="$OUT/libvulkan_panfrost.dylib"
	cp "$X" "$O"
	# the flat-namespace weak imports (unimplemented entrypoints) become lookups in the image itself (MaliMetal gpu45/gpu46)
	python3 "$MM/mesa-build/fix-weak-imports.py" "$O"
	python3 "$TW/tools/macho-tvos.py" "$O"
	"$LDID" -S "$O"
	cp "$B/darwin-drm/libdrm_darwin.a" "$OUT/"
	ls -l "$OUT"
}

stages=("$@")
[ ${#stages[@]} = 0 ] && stages=(fetch venv deps src host shim ios post)
for s in "${stages[@]}"; do echo "=== $s"; "stage_$s"; done
