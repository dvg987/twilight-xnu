#!/bin/bash
# Bring the box back into an image after it hung: the kernel's watchdog (hakios.wdt.c, wdt=20) resets it, U-Boot
# starts Android, and from there adb takes it to fastboot.  usage: tools/twl-recover.sh [IMAGE] [WAIT_SECONDS]
# (IMAGE default out/twl-live-vdec.img; waits up to WAIT_SECONDS, default 150, for the box to show up on USB)
cd "$(dirname "$0")/.."
image=${1:-out/twl-live-vdec.img}
wait=${2:-150}
serial=${TWL_SERIAL:-$(cat work/serial)}       # the box's adb serial
for i in $(seq "$wait"); do
	fastboot devices | grep -q $serial && break
	if adb devices | grep -q "$serial.*device$\|device$"; then
		timeout 15 adb reboot bootloader
		for j in $(seq 40); do fastboot devices | grep -q $serial && break; sleep 1; done
		break
	fi
	sleep 1
done
fastboot devices | grep -q $serial || { echo "twl-recover: the box did not come back on USB in $wait s"; exit 1; }
timeout 15 fastboot -s $serial getvar product 2>&1 | grep -q s7d_twilight || { echo "twl-recover: not the Mi Box"; exit 1; }
timeout 30 fastboot -s $serial boot "$image" 2>&1 | tail -1
