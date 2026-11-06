#!/bin/sh
set -e
cd "$(dirname "$0")"
CF="--target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -O2 -mgeneral-regs-only -mstrict-align -Wall"
clang $CF -c start.S -o start.o
clang $CF -c emmc.c -o emmc.o
ld.lld -T probe.ld -static -o emmc.elf start.o emmc.o
llvm-objcopy -O binary emmc.elf emmc.Image
head -c 4096 /dev/zero | gzip > dummy-rd.gz
python3 ../../tools/mkbootimg.py --kernel emmc.Image --ramdisk dummy-rd.gz --kernel-offset 0x2000000 --cmdline probe -o ../../out/probe-emmc.img
