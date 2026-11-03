#!/usr/bin/env python3
"""The Amlogic W2's Wi-Fi firmware as the kernel sends it: three binary pieces out of Amlogic's text file.

  w2-fw.py wifi_w2_fw_usb.bin OUTDIR

The file has one 32-bit word per 9-byte line ("%08x\\n"): 512 KiB of ICCM (the first half is the chip's ROM and is
not sent), then 256 KiB of DCCM, of which 8 KiB (USB ROM data and the stack) are not sent either. Each word goes
byte-swapped, as Linux's wifi_fw_download() (w2_usb.c) does. Written:

  w2_iccm.bin    256 KiB  to 0x00040000
  w2_dccm1.bin   152 KiB  to 0x00d00000
  w2_dccm2.bin    96 KiB  to 0x00d28000
"""
import os, struct, sys

LINE = 9
K = 1024

def words(text, first_word, count):
    out = bytearray()
    for i in range(first_word, first_word + count):
        out += struct.pack('>I', int(text[i * LINE:i * LINE + 8], 16))
    return bytes(out)

def main():
    src, outdir = sys.argv[1], sys.argv[2]
    text = open(src, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    iccm = words(text, 256 * K // 4, 256 * K // 4)
    dccm_base = 512 * K // 4
    dccm1 = words(text, dccm_base, 152 * K // 4)
    dccm2 = words(text, dccm_base + 160 * K // 4, 96 * K // 4)
    for name, data in (('w2_iccm.bin', iccm), ('w2_dccm1.bin', dccm1), ('w2_dccm2.bin', dccm2)):
        open(os.path.join(outdir, name), 'wb').write(data)
        print('%s: %d bytes' % (name, len(data)))

if __name__ == '__main__':
    main()
