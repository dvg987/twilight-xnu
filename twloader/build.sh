#!/bin/sh
# twloader.Image: the loader as a Linux arm64 Image (U-Boot's booti, QEMU's -kernel)
set -e
cd "$(dirname "$0")"
CF="--target=aarch64-none-elf -ffreestanding -fno-builtin -nostdlib -fPIE -fvisibility=hidden -mcmodel=small -O2
    -mgeneral-regs-only -mstrict-align -Wall -Wextra -Wno-unused-parameter -I../hakios/xnuloader"
clang $CF -c start.S -o start.o
clang $CF -c loader.c -o loader.o
ld.lld -T link.ld -pie --no-dynamic-linker -z notext -o twloader.elf start.o loader.o
llvm-objcopy -O binary twloader.elf twloader.Image
ls -la twloader.Image
