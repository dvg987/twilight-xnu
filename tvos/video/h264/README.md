# H.264 reference-picture manager

`h264_dpb.c` and `h264_dpb.h` are from CoreELEC's Amlogic media_modules,
commit `c15aebb1ac93028d0aceadf8f35dda2204d33424`,
`drivers/frame_provider/decoder/h264_multi`. Their GPL-2.0-or-later notices are retained.
The C file's Linux-only includes are replaced by `compat.h`; the reference algorithms
remain upstream's. The two-word signed-value helper assembles unsigned words before
conversion to avoid signed-shift undefined behavior. `dpb.c` supplies buffer ownership
and the bounded transport adapter.

This experimental adapter currently accepts progressive 64x64, single-slice pictures.
It is not a general H.264 capability. Decoder calls are serialized; the imported code
contains process-global dummy-picture state and must not be used concurrently.
