# NativeMaliMetal for twilight (tvOS 16.5 20L563, Mali-G310)

Ken-u/MaliMetal's Metal plug-in (`NativeMaliMetalPlugin.mm` + `frontend/NativeBridgeMetalFrontend.mm`) and its Rust
metal2vulkan bridge, cross-built on Linux for arm64 and retagged as tvOS. Research behind the changes: `../gpure/ca-gpu.md`.

## Build

```sh
./build-metal.sh                                   # fetches sources once, then bridge + plug-in + bundle
ICD=/path/libvulkan_panfrost.dylib ./build-metal.sh # a different PanVK build (default: ../gpubuild/out/libvulkan_panfrost.dylib)
PROFILE_EXTRA= ./build-metal.sh                    # bridge without fat LTO (faster)
```

| Step | What |
|---|---|
| sources | `src/m2v`: steelbrain/metal2vulkan @ `720ef1ce9ae2` (partial clone, the 1.5 GB validation corpus is not checked out), plus the 37 `bridge/patches` of UniXNU/AdrenoMetal @ `d6ca6557039c` (`src/adreno`), plus MaliMetal's 8 MiB pool block and `m2v-patches/0100`, `0101`. `src/MaliMetal`: a clone of `work/src/MaliMetal-ken` with `tvos.patch` applied. |
| bridge | `cargo +1.98.1 --target aarch64-apple-ios`, `IPHONEOS_DEPLOYMENT_TARGET=16.0`, cc crate on system clang 23 with the iPhoneOS16.5 SDK, fat LTO + 1 codegen unit as upstream. Output `build/bridge/target/aarch64-apple-ios/release/libnative_metal_bridge.a`. AIR fixtures are built with `/usr/bin/llvm-as` (LLVM 23). |
| plug-in | clang 23 `-target arm64-apple-ios16.0 -DNMM_TVOS=1` with the SDK's libc++ headers (`-nostdinc++ -isystem $SDK/usr/include/c++/v1`; the host's libc++ 23 headers need `__hash_memory`, which tvOS's libc++ lacks), `ld64.lld -bundle`, then `tools/macho-tvos.py` (LC_BUILD_VERSION platform tvOS, min 16.0, sdk 16.5) and `ldid -S`. `mali_uapi.h` is `hakios/drivers/gpu/hakios.mali_uapi.h` (identical to Ken's). |
| check | Every symbol the plug-in and PanVK bind is looked up in `.tbd`s that `ipsw dyld tbd` makes from the tvOS 16.5 shared cache (`check-imports.py`): currently 380/380 and 196/196 resolve. |

Output: `out/NativeMaliMetal.bundle/`

```
NativeMaliMetal            arm64 Mach-O bundle, platform tvOS 16.0, ad hoc signed (CDHash: ldid -h)
Info.plist                 CFBundleExecutable NativeMaliMetal, NSPrincipalClass NativeMaliMetalDevice,
                           CFBundleSupportedPlatforms [AppleTVOS], MinimumOSVersion 16.0, UIDeviceFamily [3]
libvulkan_panfrost.dylib   copied unchanged from ../gpubuild/out (already tvOS-tagged and signed there)
```

The script needs no network once `src/` exists. It takes about 2 minutes when only the bridge's top crate rebuilds, and about 4 minutes from a clean target directory. Disk use is about 1.4 GB, 1.2 GB of it the cargo target directory.

## What the kernel must publish

Metal (`_MTLRegisterDevices`) matches **`IOAcceleratorES`** and reads, as CFStrings:

| Property | Value |
|---|---|
| `MetalPluginName` | `NativeMaliMetal` (Metal loads `/System/Library/Extensions/NativeMaliMetal.bundle`) |
| `MetalPluginClassName` | `NativeMaliMetalDevice` (also the bundle's `NSPrincipalClass`) |

The plug-in opens **`MaliService`** (`IOServiceMatching("MaliService")`) with the `mali_uapi.h` ABI. It returns a device only in
`backboardd`; other processes get nil. Boot-arg `mali-clients=a,b` names more processes, for example `iconservicesagent` once one is seen
to need Metal. On iPadOS the plug-in hard-coded a list of iPad processes.

## Where things must be at run time

| File | Path | Why |
|---|---|---|
| plug-in | `/System/Library/Extensions/NativeMaliMetal.bundle/NativeMaliMetal` + `Info.plist` | Metal's discovery builds this path from `MetalPluginName` |
| PanVK | **`/System/Library/Extensions/NativeMaliMetal.bundle/libvulkan_panfrost.dylib`** | `openDevice()` / `nmm_selftest()` take `dirname(dladdr(plug-in))/libvulkan_panfrost.dylib`. The plug-in `dlopen`s it, and the runtime then opens the same path as its Vulkan "loader" (`ash::Entry::load_from`; PanVK exports `vkGetInstanceProcAddr`). It does not use a Vulkan loader, an ICD json or a search path. If the file is missing, it logs "no PanVK at ..." and returns nil. Its install name `@rpath/...` does not matter, because it is opened by absolute path. |
| darwin-drm | none: linked statically into `libvulkan_panfrost.dylib` | The libdrm stand-in is `libdrm_darwin.a` inside PanVK (exports `drmIoctl`, `drm_darwin_*`, imports only IOKit, libSystem and libc++). The plug-in `dlsym`s `drm_darwin_timing` from the PanVK handle for its status lines. There is no separate shim file to place. If the other build ever makes it a dylib, it must sit next to PanVK and PanVK must reference it by an absolute path or `@loader_path`. |
| IOKit | `/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit` (in the cache) | The plug-in `dlopen`s it. tvOS's rootfs has no `IOKit.framework/IOKit` link (see changes) |
| MTLCompiler | `/System/Library/PrivateFrameworks/MTLCompiler.framework/MTLCompiler` (in the cache) | Only needed for codegen and stitched-library requests |
| QuartzCore library | `/System/Library/Frameworks/QuartzCore.framework/default.metallib` (stock) | CA's `newLibraryWithURL:` and `nmm_selftest(1)` |
| shader cache | First writable of: `~/Library/Caches/com.apple.<prog>/m2v`, `$(getconf DARWIN_USER_CACHE_DIR)m2v`, `$(getconf DARWIN_USER_TEMP_DIR)m2v`, `/tmp/m2v`, `/private/var/tmp/m2v`. Otherwise it runs without a cache. The self-test uses `/private/var/tmp/m2v-selftest`. | Our /var is fresh each boot, so the cache starts cold on every boot unless one of these directories persists |
| console | `MALI_LOG` on MaliService. If the kernel answers `kIOReturnUnsupported` or `kIOReturnBadArgument` (no such selector), the plug-in writes to `/dev/console` (`mali: user: ...`), or to `os_log` if it can't open that. | Diagnostics |

Trust: both Mach-Os are ad hoc signed (ldid). The kernel's AMFI stand-in must accept them, or their CDHashes must go into whatever
trust list the image uses. The CDHashes change with every build: `ldid -h <file> | grep CDHash`.

Prefs for the image: pin `CAEnableDeepFramebuffer=false` and `CAEnableFramebufferCompression=false` for backboardd and globally
(var skeleton). The plug-in also sets both, plus `CAEnableIOSurfaceCompression=false`, in-process when it opens. That may be too late
for QuartzCore's once-only checks, and the plug-in no longer clears the iPad's in-memory flags.

## Source changes for tvOS (`tvos.patch`, against MaliMetal-ken a2b7671)

1. `frontend/NativeBridgeMetalFrontend.mm`: accept a `_MTLDevice` instance size of 528 (tvOS 16.5) as well as 536, and log the size
   when it is neither.
2. `frontend/NativeBridgeMetalFrontend.mm` `quartzCoreCallers()`: under `NMM_TVOS`, skip the iPadOS 17.7.11 QuartzCore offsets
   (MetalContext `+0x143214`, ctx `+0x1a0/+0xb3c`, and the `ca_debug_options` byte, which was read through a raw pointer).
3. `NativeMaliMetalPlugin.mm` `logFramebufferChoice()`: under `NMM_TVOS`, only the preference and MobileGestalt bits. No
   slide from `CARenderServerStart - 0x189651efc`, no reads at 0x1f491ae10 and the other iPad addresses, no display-object walk.
4. `clearDeepFramebuffer()`: does nothing under `NMM_TVOS`. Nothing is `vm_write`n anywhere. `openDevice()` sets the
   `CAEnableDeepFramebuffer=false` preference instead.
5. `mlog()`: if `MALI_LOG` returns `kIOReturnUnsupported` or `kIOReturnBadArgument`, it stops calling it and falls back to
   `/dev/console`, then `os_log`.
6. `presenterThread()`: if `MALI_GOP` returns `kIOReturnUnsupported` or `kIOReturnBadArgument`, it gives up at once instead of retrying 10 times every 2 s.
   `MALI_MAP_SCANOUT` is only tried after a successful `MALI_GOP` mode call, as before.
7. `clientProcess()`: under `NMM_TVOS`, only backboardd, plus the `mali-clients=` boot-arg, instead of the iPad's poster and Settings list.
8. IOKit is `dlopen`ed by its install name `IOKit.framework/Versions/A/IOKit` first, then by the short path as a fallback.
   Without this the plug-in had no IOKit on tvOS: no log, no `IOObjectRetain` of the accelerator port, no GOP.

9. `constantBytes()` qualifies 3-wide vector constants at their packed size (uchar3 3 bytes, float3 12, ...), which is what
   metal2vulkan checks. tvOS 16.5's QuartzCore sets `fc_color_opcodes0`/`1` as `MTLDataTypeUChar3` (51) for its uber shader
   `fixed_frag_lph_cph`/`_cpf`; the frontend refused the specialization ("constant type 51 is not qualified") and backboardd
   aborted with `OS_REASON_COREANIMATION | Metal failed to specialize fragment function` (run-1103-201426).
10. Every refused specialization is reported on the console (`native-metal: specialized function <name>: <why>`, with the constant
   values when the translator refused it). QuartzCore's own message puts the error on a second line.
11. `nmm_selftest(1)` also builds QuartzCore's first real pipeline (fixed_vert_lph_spc + fixed_frag_lph_cph with the spec "A2Xhf"
   constants, two BGRA8 attachments, framebuffer fetch on) through PanVK: `uber made|refused`, the reason in the message line.

Build-only differences from `build-plugin.sh` / `build-bridge.sh`: target iOS 16.0 instead of 17.0, then retagged as tvOS. The NGPUADD2
add-on (`make-addon.py`) is dropped. The bundle goes straight into the rootfs. There are no metal2vulkan changes beyond MaliMetal's own set;
`patches-local/*.patch` (empty so far) would be applied after them.

## QuartzCore's library on the host

`build/re/` holds the host checks (not part of the build): `mtlb.py` splits default.metallib into `qc/<function>.air` (+ `llvm-dis`
`.ll`), `host/release/metal2vulkan` is the translator's CLI built from `src/m2v` as patched (`cargo +1.98.1 build --release -p metal2vulkan
--features cli --bin metal2vulkan --target-dir build/host`), `panvk-approx.json` is the Roadmap 2022 profile plus
rasterizationOrderColorAttachmentAccess (our PanVK exposes it, patch 0006). `tools/sweep.py` (every function, all constants zero,
fragments paired with a vertex function that writes their inputs), `tools/fixed.py` (CA's A2Xhf pipeline) and `tools/ubersweep.py`
(each uber-shader constant varied alone; specialized and generic, with and without an image) drive the CLI; translation and spirv-val
only, no driver.

How CA builds a fragment function (`CA::OGL::MetalContext::create_fragment_shader`, `load_shader_function`): a
`MTLFunctionDescriptor` with name and `MTLFunctionConstantValues` (named constants only: bool 53, uchar 49, uchar3 51), then
`newFunctionWithDescriptor:error:`; a failure is retried once and then `abort_with_payload`. The spec string (`Spec::name`) is
`T<tex fn>` `B<blend>` `D<dest>` `A<attachments>` `S<..>` `L<layout>` and `X` + flags (h half, f framebuffer fetch, z fb sample,
c, x, u, p, e, s, n, l, r, g generic), then image segments: "A2Xhf" is no texture function, no blend, two attachments, half
precision, framebuffer fetch.

Results (2026-11-03): 96 of 102 functions/pairs translate; all 15960 uber-shader variations do. Still refused, as expected:
`tile_downsample_1/2/4/8` (tile functions; the device offers no tile shaders) and `inplace_copy_lpf/lph` (raster order groups need
fragmentShaderPixelInterlock, which PanVK lacks; `areRasterOrderGroupsSupported` answers no). The self-test's own pair count over-reports
refusals: it gives no constants, no vertex descriptor and BGRA8 for single-channel outputs.

## Open risks

- **EL0 cache maintenance.** The frontend replaces `sys_dcache_flush` with its own `mrs ctr_el0` plus a `dc civac` loop. Our kernel
  must set SCTLR_EL1.UCT and SCTLR_EL1.UCI (Ken's `evb10-sctlr-uct.patch`). Otherwise backboardd gets SIGILL on its first commit.
  `TPIDR_EL0`'s low 12 bits are read as a CPU number only for diagnostics.
- **The kernel accelerator.** hakios.mali.cpp still has Ken's `publishAccelerator()`, which needs Apple's IOGPUFamily kext (there is
  none here). The two properties above must go on our own `IOAcceleratorES` (MiniIOMobileFramebuffer) instead. The values are strings
  (OSString).
- **PanVK side of QuartzCore's pipelines.** The host checks stop at SPIR-V. Whether PanVK makes the pipelines (framebuffer fetch on
  raster-order attachment access) shows in `nmm_selftest(1)`'s `uber` field and in backboardd's `render pipeline refused` lines.
- **Selectors tvOS sends that iPadOS 17 did not.** These are answered with zero through `forwardInvocation` and logged as
  `metal: unimplemented ...`. The first boot's log is the to-do list.
- **The deep framebuffer.** It now depends only on preferences. If CA still picks `'RGhA'`, the display driver gets 8-byte layers. The
  tvOS addresses for a retargeted clear are in ca-gpu.md (`enable_cif10` 0x1da245710 and others), but they are deliberately not used.
- **All other processes.** The bundle's constructor still swizzles `CAMetalLayer setPixelFormat:` (XR10 to 8-bit) and installs a
  SIGSEGV/SIGBUS reporter in every non-backboardd process that loads it. That is harmless but global.
- **PanVK.** G310 support and the CSF firmware belong to the other build. This bundle copies whatever is in
  `gpubuild/out/libvulkan_panfrost.dylib` when the script runs, so rerun the script after PanVK changes. PanVK's own disk cache or
  driconf lookups, if enabled in that build, may try paths under `$HOME`.
- **Size.** The plug-in is 19 MB (fat-LTO bridge, symbols kept for the fault handler's `dladdr`) and PanVK is 16 MB.
