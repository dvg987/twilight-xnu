#!/bin/sh
# tvOS on QEMU virt (GICv2, EL2, Cortex-A55, 2 GB like the box): twloader + payload, the tvOS volume as virtio-blk,
# ramfb as the screen. Monitor on 127.0.0.1:4444 (screendump), serial log to out/qemu/serial.log.
cd "$(dirname "$0")/.."
SECS=${1:-900}
exec timeout "$SECS" qemu-system-aarch64 -M virt,gic-version=2,virtualization=on -cpu cortex-a55 -smp 4 -m 2G \
	-accel tcg,thread=multi,tb-size=512 \
	-kernel twloader/twloader.Image -initrd ${PAYLOAD:-out/payload-tvos-qemu.bin} \
	-global virtio-mmio.force-legacy=false \
	-drive file=out/tvos-system.apfs,format=raw,if=none,id=sys,readonly=on -device virtio-blk-device,drive=sys \
	-device ramfb -display none -serial file:out/qemu/serial.log \
	-monitor tcp:127.0.0.1:4444,server,nowait -no-reboot
