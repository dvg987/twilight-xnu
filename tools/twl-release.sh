#!/bin/bash
# The release: tvOS on a USB stick, started by the box's own U-Boot the way CoreELEC is. Nothing on the eMMC is used.
#
#   tools/twl-release.sh [OUT]     OUT: an image file (default out/twilight-usb.img) or the stick itself (/dev/sdX,
#                                  as root). An image is written to a stick with dd or any imaging tool.
#
# Wanted first: the kernel (hakios: make kernel), twloader/twloader.Image, the tvOS system volume out/tvos-system.apfs
# and work/tvos (from your own tvOS 16.5 OTA: INSTALL.md), the W2 firmware in work/w2fw/bin (tools/w2-fw.py).
#
# env: SIZE=8G         the image file's size (a stick's own size is used); /private/var gets what the system leaves
#      NETWORK=FILE    a Wi-Fi network to join at the start: its name, then its passphrase, one a line. Without it
#                      the network is chosen in Settings.
#      SSH=1           sshd for root with your key (SSH_KEY, default ~/.ssh/id_ed25519.pub) and the package
#                      manager's prefix on the stick (tools/twl-jb.sh first)
#      ARGS="..."      more boot-args
set -e
cd "$(dirname "$0")/.."
OUT=${1:-out/twilight-usb.img}
mkdir -p logs
USB=1 VDEC=1 TVAUDIO=1 NET=1 REST=1 CPUS=4 SECS=0 KEYS=/dev/null OSLOG= NETWORK="${NETWORK-}" \
SSH="${SSH-}" JB="${SSH:+1}" OUT=out/tvos-release PAYLOAD=out/payload-tvos-release.bin \
ARGS="fbwidth=1920 fbheight=1080 cec=1 w2=6 w2bt=3 twl_monitor=0 hdmitx=1 mali=8 metal=1 gles=1 mali-pool=192 fb_flip=1 hdmiaudio=3 amlcpu=1 mali-clients=* twl_restart=1 ${ARGS-}" \
bash tvos/build.sh > logs/tvos-release-build.log 2>&1 || { tail -20 logs/tvos-release-build.log; exit 1; }
python3 tools/mkbootimg.py --kernel twloader/twloader.Image --ramdisk out/payload-tvos-release.bin \
	--kernel-offset 0x2000000 --cmdline twloader -o out/twl-usb.img | tail -1
python3 tools/twl-usb.py ${SIZE:+-s $SIZE} --boot out/twl-usb.img "$OUT"
