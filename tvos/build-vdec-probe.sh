#!/bin/bash
# Build bounded decoder experiments. This does not install or advertise codec support.
set -eo pipefail
TOP="$(cd "$(dirname "$0")/.." && pwd)"
cd "$TOP"
source hakios/xnu-env.sh
set -u
OUT="${OUT:-$TOP/out/vdec-probe}"
mkdir -p "$OUT/tmp"
export TMPDIR="$OUT/tmp"
SDK="${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}"
LDID="$THEOS/toolchain/linux/iphone/bin/ldid"
COMMON=(-arch arm64 -isysroot "$SDK" -O1 -Wall -Wextra)
FRAMEWORKS=(-framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOSurface)
DPB=(tvos/video/h264/dpb.c tvos/video/h264/h264_dpb.c tvos/video/hevc/hevc_dpb.c tvos/video/vp9/vp9_dpb.c)
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -Wno-pointer-sign \
    -o "$OUT/vdecstreamprobe" tvos/src/vdecstreamprobe.c "${DPB[@]}"
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -dynamiclib \
    -install_name /redwood/lib/AVD.videodecoder "${FRAMEWORKS[@]}" \
    -Wno-pointer-sign -o "$OUT/AVD.videodecoder" tvos/src/twlvdec.c "${DPB[@]}"
xnu-clang "${COMMON[@]}" "${FRAMEWORKS[@]}" -o "$OUT/vtprobe" tvos/src/vtprobe.c
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -o "$OUT/vdecdbg" tvos/src/vdecdbg.c
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -o "$OUT/vp9probe" tvos/src/vp9probe.c tvos/video/vp9/vp9_dpb.c
xnu-clang "${COMMON[@]}" -fobjc-arc -framework Foundation -o "$OUT/kodiprobe" tvos/src/kodiprobe.m
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -DTWL_VDEC_PLAYER_PROBE -dynamiclib \
    -install_name /redwood/lib/libtwlvdec-probe.dylib "${FRAMEWORKS[@]}" \
    -Wno-pointer-sign -o "$OUT/libtwlvdec-probe.dylib" tvos/src/twlvdec.c "${DPB[@]}"
xnu-clang "${COMMON[@]}" -Ihakios/xnu/files/bsd -Wno-deprecated-declarations -dynamiclib \
    -install_name /redwood/lib/libtwlcvgl.dylib -framework CoreFoundation -framework IOSurface \
    -framework CoreVideo -framework OpenGLES -lobjc \
    -o "$OUT/libtwlcvgl.dylib" tvos/src/twlcvgl.c
for file in AVD.videodecoder vtprobe vp9probe vdecdbg vdecstreamprobe kodiprobe libtwlvdec-probe.dylib libtwlcvgl.dylib; do
    python3 tools/macho-tvos.py "$OUT/$file"
    "$LDID" -S -Cadhoc "$OUT/$file"
done
printf 'Decoder probe artifacts: %s\n' "$OUT"
