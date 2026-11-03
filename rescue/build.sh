#!/bin/sh
# Build out/rescue.img: CoreELEC's 5.15 kernel + the Xiaomi 3rd-gen DTB (patched) + our initramfs.
set -e
cd "$(dirname "$0")/.."
mkdir -p out out/rescue-root/bin
cp work/busybox/busybox-1.37.0/busybox out/rescue-root/bin/busybox
cp rescue/init out/rescue-root/init
aarch64-linux-gnu-gcc -static -Os -s -o out/rescue-root/bin/reboot2 rescue/reboot2.c
(cd out/rescue-root && find . | cpio -o -H newc --owner 0:0 --quiet | gzip -9) > out/rescue-initrd.gz
dtc -I dtb -O dts -o out/rescue.dts device/s7d_s905x5m_xiaomi_3rd_gen.dtb 2>/dev/null
python3 tools/dtpatch-rescue.py out/rescue.dts
dtc -I dts -O dtb -o out/rescue.dtb out/rescue.dts 2>/dev/null
CMD="console=tty0 otg_device=1 loglevel=7 ignore_loglevel rdinit=/init panic=10"
python3 tools/mkbootimg.py --kernel work/ce-kernel/kernel --ramdisk out/rescue-initrd.gz --second out/rescue.dtb \
    --cmdline "$CMD" --kernel-offset 0x2000000 -o out/rescue.img
python3 tools/mkbootimg.py --kernel work/ce-kernel/kernel --ramdisk out/rescue-initrd.gz --dtb out/rescue.dtb \
    --cmdline "$CMD" --kernel-offset 0x2000000 --header-version 2 -o out/rescue-v2.img
