# PanVK for the S905X5M box (tvOS 16.5)

`build-panvk.sh` builds `out/libvulkan_panfrost.dylib`: Mesa PanVK, arm64, retagged as tvOS (platform 3, minos 16.0, sdk 16.5) and ad hoc
signed with ldid. The darwin-drm libdrm stand-in is linked into it statically. Build output only; nothing has been run on the box.

## Outputs (`out/`)

| File | What |
| --- | --- |
| `libvulkan_panfrost.dylib` | The ICD, about 16 MB. Exports `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`, plus the shim's `drmIoctl`, `drm_darwin_timing`, `drm_darwin_open_render`, `drm_darwin_mmap`/`munmap` and `drm_darwin_bo_create_host`. Links only IOKit, libSystem and libc++ (SDK 16.5). |
| `libdrm_darwin.a` | The shim on its own. It is already inside the dylib, so you only need this to link something else against it. |
| `panvk.map` | ld64.lld link map. `usertest/vk_test.c` and the plug-in's profiler use it to turn offsets into function names. |

How MaliMetal uses them: `metal-plugin/build-plugin.sh` copies the ICD into `NativeMaliMetal.bundle/libvulkan_panfrost.dylib`. The plug-in
`dlopen()`s it from the bundle directory and `dlsym()`s `drm_darwin_timing` from that same handle. So libdrm is not a separate library, and the
dylib is the whole deliverable. `build-plugin.sh` reads `ICD=`, so set `ICD=$PWD/out/libvulkan_panfrost.dylib`. That script, and the plug-in
itself, still target iOS 17 with AOSP clang-20 and have not been adapted here.

## Versions

- Mesa: upstream tag `mesa-26.1.0-rc1`, a shallow clone in `src/`. The work tree has the patches applied; nothing is committed.
- Host LLVM: Arch's `llvm21`/`clang21` 21.1.8 in `/usr/lib/llvm21`. It builds mesa_clc, libclc and llvm-spirv. LLVM 23 was not used because
  Mesa 26.1's `clc_helpers.cpp` only knows LLVM versions up to 22. No LLVM was built.
- SPIRV-LLVM-Translator: branch `llvm_release_210`, static, installed to `prefix/`.
- SPIRV-Headers: main branch (shallow).
- libclc 21.1.8: release tarball, targets `spirv-mesa3d-` and `spirv64-mesa3d-`. Needs the `cmake-21.1.8.src` tarball beside it.
- SPIRV-Tools: the system copy (2026.4).
- Python: `venv/` with mako, pyyaml, ply and packaging. meson 1.12.1 and ninja come from the system.
- Cross compiler: system clang 23.1.3 (`-target arm64-apple-ios16.0`), ld64.lld, llvm-ar, and `$THEOS/sdks/iPhoneOS16.5.sdk`.
- Post-link steps:
  1. MaliMetal `mesa-build/fix-weak-imports.py`
  2. `tools/macho-tvos.py`
  3. `$THEOS/toolchain/linux/iphone/bin/ldid -S`
- Host tools (`prefix/host-tools/bin`): `mesa_clc`, `vtn_bindgen2` and `panfrost_compile`, built from the same patched tree. They are not
  installed; only these three targets of `build/mesa-host` are built.

## Patches (`patches/`)

MaliMetal-ken `mesa-patches/0001..0011` were made on AOSP's 26.1-devel. Against 26.1.0-rc1:

- 0001-0003, 0005, 0007-0011 apply as they are. Some need fuzz or offsets, and have been regenerated as plain `git diff` against rc1.
- 0004 (host-pointer import): one hunk is ported. Its context, the `minTexelBufferOffsetAlignment` lines, differs in rc1, so
  `.minImportedHostPointerAlignment = 4096` now goes after `.minMemoryMapAlignment`.
- 0006 (rasterization-order attachment access): one hunk is ported. rc1 has no `.EXT_rasterization_order_attachment_access` neighbour line, so
  the extension line now goes after `.EXT_queue_family_foreign`.

The changes in `patches/` are identical in content to the originals; only the context differs. `build-panvk.sh src` resets the clone to the
tag and applies them with `git apply`.

## darwin-drm check

`darwin-drm/drm_darwin.c` (1307 lines) is the full implementation, not the skeleton that `mesa-build/README.md` still describes.

- **Connection:** one IOKit connection per process: `IOServiceGetMatchingService(IOServiceMatching(MALI_SERVICE = "MaliService"))`,
  `IOServiceOpen`, then `MALI_DEV_QUERY`.
- **Panthor ioctls:** `drmIoctl` decodes every Panthor ioctl that PanVK 26.1 issues: DEV_QUERY, VM_CREATE/DESTROY/BIND/GET_STATE,
  BO_CREATE/MMAP_OFFSET/SET_LABEL/SYNC/QUERY_INFO, GROUP_CREATE/DESTROY/SUBMIT/GET_STATE, TILER_HEAP_CREATE/DESTROY and
  SET_USER_MMIO_OFFSET. It walks the arrays itself and makes one `IOConnectCallStructMethod` per element. The selectors it uses are
  DEV_QUERY, VM_CREATE/DESTROY, BO_CREATE/DESTROY, VM_BIND, GROUP_CREATE/DESTROY/SUBMIT, HEAP_CREATE/DESTROY, BO_CREATE_HOST and LOG.
- **Mapping:** BO mmap goes through `IOConnectMapMemory64` (BO handle as the type, or `MALI_MAP_SYNCOBJ | group`).
- **Syncobjs:** binary and timeline syncobjs are emulated in the process on top of the group syncobj pages.
- **Not supported, by design:** PRIME, sync files and sharing syncobjs between processes return -ENOSYS. They are also not needed: the
  raw `DRM_IOCTL_SYNCOBJ_*` users are only in the JM (v6/v7) backend.

The ABI matches the kernel: `build/darwin-drm/inc/mali_uapi.h` is a copy of `hakios/drivers/gpu/hakios.mali_uapi.h`, and that file is
byte-identical to `work/src/Mali-ken/mali_uapi.h`. The selector enum and struct layouts are compiled in from it, so the selectors are
`hakios.mali_uapi.h`'s by construction. `llvm-nm -u` shows no unresolved `drm*` symbols.

Mesa 26.1 knows the G310: `PAN_PROD_ID(10, 12, 4)`, variants 0-4 picked by `gpu_variant` (`core_features & 0xff`), so GPU_ID 0xac740001 →
G310vN depending on what the kernel reports in `core_features`. arch 10 is not behind `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`.

## Problems and notes

- The MaliMetal README pitfalls all apply and are handled in the script:
  - `CCC_OVERRIDE_OPTIONS` is unset.
  - `-undefined dynamic_lookup` is given only at ninja time, through `CCC_OVERRIDE_OPTIONS='# +-Wl,-undefined,dynamic_lookup'`.
  - `-UHAVE___BUILTIN_{ADD,SUB}_OVERFLOW_P`.
  - No host `PKG_CONFIG_PATH`; the cross build's pkg-config sees only the shim's `libdrm.pc`.
- System clang 23 for a Darwin target would pick up the host's `/usr/include/c++/v1` (libc++ 23) ahead of the SDK's. The script passes
  `-nostdinc++ -isystem $SDK/usr/include/c++/v1` so the C++ parts are compiled against iOS 16.5's libc++.
- Configure probes look right for Darwin: `secure_getenv`, `sched_getaffinity`, `*_overflow_p`, `alias` and similar all come out NO.
- `fix-weak-imports.py` rebinds the unimplemented entrypoints: 6692 of 6887 imports were flat-namespace weak lookups and are now lookups in
  the image itself. The rest are two-level imports from IOKit (8), libSystem (171) and libc++ (14), plus 2 weak-coalesce imports. No
  non-weak flat-namespace imports are left.
- Order matters: fix-weak-imports, then the tvOS retag, then `ldid -S`. fix-weak-imports and the retag both change bytes, and ldid signs last.
- Every PanVK arch (v6/v7/v9/v10/v12/v13) is built into the one dylib, the same as MaliMetal's build. Only v10 matters on the box.
- Disk: about 1 GB in total. The biggest parts are `src/mesa-26.1.0-rc1` (≈500 MB), `build/mesa-ios` (≈160 MB) and `build/mesa-host`
  (≈120 MB). All of `build/` can be deleted after a build.
- Timing: deps take about 1 min, the host tools about 2 min and the iOS build about 2 min at -j16. A running game (`GameThread`) drops
  this to -j2.

## Reproduce

```
./build-panvk.sh            # fetch venv deps src host shim ios post
FORCE=1 ./build-panvk.sh    # also start again from a fresh Mesa tree with patches/ (redoes both Mesa builds)
./build-panvk.sh shim ios post   # rebuild the shim and relink (the dylib is relinked when the shim archive is newer)
```

Environment: `THEOS` (default `~/theos`), `MM` (MaliMetal checkout, default `work/src/MaliMetal-ken`), `UAPI` (default
`hakios/drivers/gpu/hakios.mali_uapi.h`) and `J`.
