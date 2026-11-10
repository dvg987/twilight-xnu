#!/bin/bash
# The development tvOS image: the hardware video decoder (VDEC=1), GPU, audio, SSH, and on top of what a release
# carries the crash logger in every application, the loader of diagnostic libraries and PineBoard's diagnostics. Builds out/twl-live-vdec.img; with "boot" also sends the box there (it must be in fastboot or tvOS).
set -e
cd "$(dirname "$0")/.."
VDEC=1 \
TVLIBS="${TVLIBS-twlhook:backboardd twlpb:PineBoard twlremote:PineBoard,HeadBoard twlbt:TVSettings twlcloud:@apps twlload:backboardd,mediaserverd,PineBoard,IdleScreen,@apps twlcrash:@apps twlpbdiag:PineBoard}" \
VAR=1 SSH=1 CPUS=4 SECS=86400 KEYS=work/keys-none.list REST=1 TVAUDIO=1 \
OSLOG="com.apple.wifid com.apple.Pineboard com.apple.bluetooth com.apple.mediaserverd" \
ARGS="fbwidth=1920 fbheight=1080 usbkeys=1 cec=1 w2=6 w2bt=3 twl_monitor=0 hdmitx=1 mali=8 metal=1 gles=1 mali-pool=192 fb_flip=1 hdmiaudio=3 amlcpu=1 mali-clients=* msgbuf=4194304 ${EXTRA_ARGS-}" \
bash tvos/build.sh > logs/tvos-vdec-build.log 2>&1 || { tail -20 logs/tvos-vdec-build.log; exit 1; }
python3 tools/mkbootimg.py --kernel twloader/twloader.Image --ramdisk out/payload-tvos.bin --kernel-offset 0x2000000 \
	--cmdline twloader -o out/twl-live-vdec.img | tail -1
[ "${1-}" = boot ] || exit 0
serial=${TWL_SERIAL:-$(cat work/serial)}       # the box's adb serial
# from tvOS (its key gadget's STOP record) or from Android after a power cycle (adb), to fastboot
if ! fastboot devices | grep -q $serial; then
	if adb devices | grep -q 'device$'; then
		adb reboot bootloader
	else
		timeout 10 work/venv/bin/python tools/twl-keys.py stop || true
	fi
	for i in $(seq 40); do fastboot devices | grep -q $serial && break; sleep 1; done
	fastboot devices | grep -q $serial || { echo "the Mi Box did not come to fastboot (power-cycle it?)"; exit 1; }
fi
timeout 15 fastboot -s $serial getvar product 2>&1 | grep -q s7d_twilight || { echo "not the Mi Box in fastboot"; exit 1; }
fastboot -s $serial boot out/twl-live-vdec.img 2>&1 | tail -1
