#!/usr/bin/env python3
"""Make an arm64 Mach-O built with the iOS SDK a tvOS one: LC_BUILD_VERSION platform 2 -> 3 (and min OS 16.0)."""
import struct, sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
magic, cputype, _, ftype, ncmds, sizeofcmds, _, _ = struct.unpack_from('<IiiIIIII', d, 0)
assert magic == 0xfeedfacf
off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', d, off)
    if cmd == 0x32:                                 # LC_BUILD_VERSION
        struct.pack_into('<III', d, off + 8, 3, 0x100000, 0x100500)
    elif cmd == 0x25:                               # LC_VERSION_MIN_IPHONEOS -> LC_VERSION_MIN_TVOS
        struct.pack_into('<I', d, off, 0x2f)
    off += size
open(p, 'wb').write(d)
