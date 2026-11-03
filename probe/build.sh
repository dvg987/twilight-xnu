#!/bin/sh
set -e
cd "$(dirname "$0")"
CF="--target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -fPIC -O2 -mgeneral-regs-only -Wall"
clang $CF $DEFS -c start.S -o start.o
clang $CF $DEFS -c probe.c -o probe.o
ld.lld -T probe.ld -static -o probe.elf start.o probe.o
llvm-objcopy -O binary probe.elf probe.Image
head -c 4096 /dev/zero | gzip > dummy-rd.gz
python3 ../tools/mkbootimg.py --kernel probe.Image --ramdisk dummy-rd.gz --kernel-offset 0x2000000 --cmdline "probe" -o ../out/probe${STAGE:+-$STAGE}.img
