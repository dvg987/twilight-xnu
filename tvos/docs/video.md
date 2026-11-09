# Hardware video decoding on twilight

## Required result

Playback must work end to end in a player such as Kodi through tvOS APIs. Raw firmware loads,
a decoded test frame, or a true hardware-capability property alone are not acceptance criteria.
The requested scope includes H.264 and HEVC, VP9 where the hardware and tvOS API path permit it,
and both SDR and HDR. Preserve bit depth, range, matrix, primaries, transfer function and HDR
metadata through decode, pixel buffers, rendering and HDMI. Verify seeking, EOF/drain, stop,
reopen, resolution changes and repeated playback on the box.

## Verified infrastructure (2026-11-06)

- Device: fastboot product `s7d_twilight`, port serial `TWL00MIBOXS3`, tvOS 16.5 (20L563).
- OP-TEE 3.18 is resident. The video TA previously failed opening with `0xffff000a` after the
  kernel refused an RPMB RPC. The eMMC driver now handles the OP-TEE read transport, including
  metadata, authenticated reads and counter reads. Key programming and authenticated writes
  are refused. The card lock covers the whole RPMB partition switch and return; a failed
  restoration disables subsequent normal disk transfers.
- The signed video TA opens and accepts the complete 412480-byte firmware payload in four
  chunks. Firmware loading is gated on successful preload, with release/acquire publication.
- DOS/VDEC and HEVC register and scratch tests pass. Reset release and a settle interval now
  precede the first DOS access. The older bring-up image aborted on that access.
- Secure firmware loader calls return zero for H.264 multi, HEVC and VP9 MMU. This does not
  establish HEVC or VP9 frame decoding.
- The H.264 core consumes frame-mode Annex B, handles its SEI/configuration/slice handshakes,
  and produces correct linear NV12. All 6144 active bytes of a 64x64 red baseline picture match
  the software reference. A spatially detailed High-profile CABAC picture also matches all
  bytes, across three power/reset/firmware restarts.
- Kernel return stops the decoder CPUs, bounds the DMA wait and asserts reset before memory
  can be reused. Power-state logging shows VDEC goes off; HEVC can remain powered even after
  the off request, so both its processor and pipeline are held in reset.

## Current implementation limits

`hakios/drivers/gpu/hakios.vdec.c` remains a bring-up implementation. There is no general
H.264 movie path, HEVC/VP9 frame path, production session scheduler, or HDR output path.
The established probe accepts progressive 64x64 H.264 I pictures; a separate experimental
mode adds host reference management at the same resolution. The DMA pool is allocated only
when explicitly requested. Do not advertise general codec support from it.

Boot arguments:

- `optee=2` preloads the firmware (`optee=3` adds diagnostics).
- `vdec=1` tests power, clocks, registers and scratch.
- `vdec=2` additionally checks firmware loads, without starting decoder DMA.
- `vdec=3` additionally feeds `fw/vdec.h264`, stopping at configuration.
- `vdec=4` decodes one 64x64 I picture and checks NV12 against `fw/vdec.nv12` (6144 bytes).
  Without that reference file it checks the original solid-red fixture.
- `vdec=5` repeats that decode across three decoder restarts.
- `vdec=6` instead publishes `/dev/twl-vdec-probe`, the bounded transport used to exercise
  the framework bridge. Its capability flags explicitly identify it as probe-only.
- `vdec=7` enables the experimental reference-picture transport on the same node. It reports
  a separate capability flag and uses PREPARE/FINISH/CANCEL instead of the I-picture ioctl.
- `vdec_delay=N` and `optee_delay=N` delay the independent workers in seconds.

`tvos/src/twlvdec.c` implements the CMBase/VTVideoDecoder vtables recovered from this tvOS
cache. It converts AVCC to Annex B, returns IOSurface-backed NV12 pixel buffers, and passes
through format-description color/HDR attachments. It currently exposes only the bounded
transport. `AVDRegister` registers it system-wide when the kernel publishes the transport (see
"System decoder and Kodi" below). `TwlVDECRegisterProbe` remains an explicit test entry point.

`tvos/src/vtprobe.c` exercises public VTDecompressionSession APIs with hardware required,
checks the actual hardware property, and compares every active pixel byte across a sequence.
The generated test is 15 independent 64x64 CABAC I pictures. On the box, the hardware-required
session now reports hardware decoding and delivers all 15 frames with zero byte mismatches;
finish/drain and asynchronous wait both return zero. Evidence is in
`logs/vdec/vt-reset-mailbox.txt`. This is framework decoding, not yet player playback.

The explicit registration is process-local. `VTApplyRestrictions(1)` makes only the test process
run its decoder in-process; it does not disable hardware decoding. Without this, session
selection goes to the system decode service where the prototype is not registered. Reusing the
core also requires stopping both processors and performing the full DOS reset before reloading
firmware; without that reset the same known-good packet runs to search-buffer exhaustion.
The hardware property must also appear in the decoder's supported-property dictionary.

`TWL_VDEC_PLAYER_PROBE` enables a constructor for a separate, explicitly injected player-test
library. It is not enabled by default and must never be installed as general codec support.

## Bounded player result

Kodi now selects the test decoder through its existing FFmpeg/VideoToolbox path. The first
attempt decoded a frame and crashed in CoreVideo's texture import: Apple's implementation
dereferences private GPU sharegroup state absent from the Mesa-backed EAGL context.
`tvos/src/twlcvgl.c` supplies the source-texture operations used by Kodi's NV12 renderer.
It retains each source pixel buffer, uploads the Y and UV planes with their actual strides,
preserves GL unpack state, and defers texture deletion to the owning context. It currently
supports immutable NV12 source textures only. It copies the planes to GPU textures; this is
hardware decoding followed by a CPU upload, not zero-copy rendering or a general CoreVideo
replacement. It is opt-in and is not installed by the normal tvOS build.

With both test libraries injected into Kodi only:

- The 15-frame movie plays to EOF, with 15 successful hardware frame submissions and no
  texture-import crash (`logs/vdec/kodi-cvgl-play.log`).
- The native `kodiprobe` controller drives Kodi's loopback JSON-RPC API, avoiding Wi-Fi timing.
  Two open/pause/seek-to-50-percent/resume/stop cycles pass, followed by a third open and pause
  (`logs/vdec/kodi-long-run.log`). Seeking is asynchronous; the test polls the actual player clock.
- The saved display image shows the decoded test pattern, correctly oriented, pillarboxed
  at 1080x1080 in the 1920x1080 scanout (`logs/vdec/kodi-paused.png`). Its closest reference is
  frame 8 of the short movie; the RGB comparison records scaling/chroma interpolation error
  separately from the bit-exact NV12 decoder test.

These results apply only to 64x64 progressive H.264 I pictures in SDR. They do not establish
ordinary movie playback, P/B pictures, other resolutions/codecs or HDR.

Build the experiment with `bash tvos/build-vdec-probe.sh`. It builds and signs artifacts under
`out/vdec-probe` without installing them. `tools/vdec-probe-fixture.py MOVIE OUTPUT_PREFIX`
creates the public-API harness's packets and software NV12 reference from an all-I 64x64 movie.
`vtprobe PLUGIN PACKETS REFERENCE` performs the byte comparison; `kodiprobe LOCAL_MOVIE` drives
an already running Kodi with its loopback JSON-RPC server enabled and leaves playback paused.
No player network-access settings were changed for these tests.

## Reference-picture work (2026-11-07)

The experimental `vdec=7` transport pauses at the firmware's slice-header handoff. The bridge
passes the decoded parameters to `tvos/video/h264`, an adapter around the Amlogic DPB code.
It then submits logical reference slots, POCs and motion-vector slot indices. The kernel
validates those values and derives all register/DMA addresses itself. A caller cannot submit
arbitrary physical addresses. The probe is exclusive to one open decoder; cancel/close stops
the core and discards its context. It is not a production multi-session scheduler.

The first IDR and following P picture in a moving CABAC clip now match all software-reference
bytes through VideoToolbox (`logs/vdec/dpb-first-native.log`). Repeated testing exposed a
missing canvas-table restore after core reset; rebuilding the table restores the exact P output
(`logs/vdec/restore-canvases-native.log`). This is still only a two-picture result. A separate
I/P-only clip also fails at its third picture (`logs/vdec/p-only-restore-canvases.log`), so the
remaining search failure is not specific to B slices. Firmware state restoration is still under
investigation. General inter-frame playback is not established.

The continuous-GOP diagnostic (`vdecstreamprobe`, NEXT ioctl) reaches the second picture header
but stalls during its decode. Debug-register acknowledgements, picture-cache reset, and
frame/stream input mode comparisons did not resolve it. Adding valid filler NALs also did not
resolve the per-packet search failure. These are diagnostic results, not player acceptance.

`python3 tools/h264-dpb-test.py` compiles the production user-space DPB adapter with ASan and
UBSan. Its captured header vectors exercise 512 I/P pictures across repeated IDR groups and
context lifecycles, checking POC/reference-slot selection and rejecting malformed parameters.
This host test passes; it does not establish B-frame decoding or general movie support.

## Real video (2026-11-07)

Two causes made every stream fail from its third picture on. First, the firmware's own error recovery,
bit 4 of AV_SCRATCH_F, must be set on every run, as vmh264 always sets it. Second, the DMA pool must be
64 KiB aligned. A pool that is only page aligned loses the firmware's context between pictures on some
boots, and after that every later stream in the boot decodes wrong. The pool is now 1 MiB aligned
physically. A per-packet `ml_io_map` that leaked kernel address space (a panic after a few hundred frames)
is now done once.

The transport takes any progressive 8-bit 4:2:0 picture up to 1920x1088 (or 1088x1920), in 24 slots.
Pictures with several slices use ABI 2: FINISH returns EINPROGRESS with the next slice's parameters.
`tools/vdec-real-fixture.py MOVIE PREFIX [FRAMES]` makes timestamped packets and per-frame MD5s from
FFmpeg's decode, and `vtprobe` checks each output frame against them:

| clip | size | frames | rate (with two copies and MD5) |
| --- | --- | --- | --- |
| phone video, High | 720x1280 | 1478/1478 exact | 85 a second |
| High with B frames | 1280x720 | 240/240 exact | 91 a second |
| High with B frames | 1080x1920 | 300/300 exact | 48 a second |
| High, 8 slices a picture, 14.5 Mbit/s | 1080x1920 | 301/301 exact | 40 a second |

## System decoder and Kodi (2026-11-07)

`tvos/build.sh VDEC=1` installs `twlvdec.c` as `/System/Library/VideoDecoders/AVD.videodecoder` (the image's
`vdecoders/`, union-mounted over the volume's directory by init), injects `libtwlcvgl.dylib` into Kodi and adds
`optee=2 optee_delay=10 vdec=7 vdec_delay=25`. `AVDRegister` registers the decoder in every process when
`/dev/twl-vdec-probe` exists, so the image falls back to software without the transport. The kernel takes one
decoder at a time; another session gets `kVTVideoDecoderNotAvailableNowErr` and the software decoder.
`vtprobe - PACKETS REFERENCE` uses the system registration instead of an explicit plug-in.

Results on the normal image (GPU, audio, Kodi):

- Public VideoToolbox, hardware required, no explicit registration: 1280x720 B 240/240, 1920x1080 Constrained
  Baseline 833/833 (40 a second), 1080x1920 8 slices 301/301, all exact.
- Kodi 21.3 plays a 1920x1080 30 fps H.264 movie with AAC audio through `ff-h264-vtb`. The session runs in
  mediaserverd (out of process, IOSurface pixel buffers into Kodi), 1920x1080 buffers; Kodi's `videoheight` 1088 is
  its own coded size. The player clock keeps real time (14.84 s in 15.03 s). Three open / seek 60% / seek 10% /
  pause / resume / stop cycles: 11 sessions, no kernel errors.
- Kodi and backboardd each use about one core during playback: the NV12 planes are copied into GL textures.

`touch /var/tmp/twlvdec-debug` logs each session to `/var/tmp/twlvdec.log`.

Still not done: zero-copy display (the Amlogic video plane, which would also give real 4K video under a 1080p UI),
HEVC and VP9, interlaced/MBAFF, 10-bit H.264, 1080p60 (decode measures about 40 pictures a second), several
simultaneous hardware sessions.

## Player and HDR integration findings

The installed Kodi is 21.3, build `20251031-a3a448d26b`. Its binary includes H.264, HEVC and
VP9 VideoToolbox paths. The corresponding `VTB.cpp` uses FFmpeg's VideoToolbox hardware frame
context and hands CVPixelBuffer objects to its renderer. Its tvOS GLES renderer imports the
Y and UV planes with CVOpenGLESTextureCacheCreateTextureFromImage. This is part of the end-to-end
path that still needs validation; kernel decode is not sufficient.

MiniIOSurface already recognizes NV12 and 10-bit biplanar `x420`/`xf20` layouts. The HDMI helper
has HDR color-mode descriptions, but MiniIOMobileFramebuffer currently publishes a single
8-bit SDR color mode. Publishing HDR capabilities without implementing correct mode changes,
rendering and HDMI signaling would be incorrect. Kodi's current GLES renderer also explicitly
uses 8-bit NV12 imports, which needs attention for 10-bit HDR playback.

The private tvOS decoder-list hook is `/System/Library/VideoDecoders/AVD.videodecoder` with
`AVDRegister`. Production integration must cover both application and decode-service processes.
VP9 also has a tvOS decoder-specification/entitlement gate, so its usability must be tested
through the actual player rather than inferred from the presence of firmware.

## Evidence and host checks

Local, ignored evidence: `logs/vdec/` contains the firmware preload, register tests, successful
red-frame decode, detailed CABAC/restart results, build logs and VideoToolbox session diagnostics.
`work/vdecre/` holds generated fixtures, references and reverse-engineering material. No firmware,
Apple binaries, generated libraries or test media are committed.

Run the transport host checks with `python3 hakios/tools/emmc-rpmb-test.py`. The harness compiles
the production request handler against a mock controller with AddressSanitizer and UBSan. It
checks malformed inputs, write rejection, command ordering, recovery after command failures and
quarantine after a failed partition restoration. These checks do not substitute for device tests.
