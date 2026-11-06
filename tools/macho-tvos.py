#!/usr/bin/env python3
"""Make Mach-Os built for iOS tvOS ones: LC_BUILD_VERSION platform 2 -> 3 (min OS 16.0, SDK 16.5),
LC_VERSION_MIN_IPHONEOS -> LC_VERSION_MIN_TVOS. Thin or fat (every 64-bit slice, arm64e as well).

usage: macho-tvos.py FILE...    (in place; the code signature is then stale: sign again with ldid)
"""
import struct
import sys


def retag(d, base):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', d, base)
    if magic != 0xfeedfacf:
        return 0
    off, changed = base + 32, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', d, off)
        if cmd == 0x32:                                 # LC_BUILD_VERSION
            struct.pack_into('<III', d, off + 8, 3, 0x100000, 0x100500)
            changed += 1
        elif cmd == 0x25:                               # LC_VERSION_MIN_IPHONEOS -> LC_VERSION_MIN_TVOS
            struct.pack_into('<I', d, off, 0x2f)
            changed += 1
        off += size
    return changed


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        d = bytearray(open(p, 'rb').read())
        magic = struct.unpack_from('>I', d, 0)[0]
        if magic in (0xcafebabe, 0xcafebabf):           # fat (32- or 64-bit offsets)
            n = struct.unpack_from('>I', d, 4)[0]
            wide = magic == 0xcafebabf
            offsets = [struct.unpack_from('>Q' if wide else '>I', d, 8 + i * (32 if wide else 20) + 8)[0]
                       for i in range(n)]
        else:
            offsets = [0]
        if not any([retag(d, o) for o in offsets]):     # (a list: every slice, not the first that changes)
            print(f'macho-tvos.py: {p}: no platform load command in any slice', file=sys.stderr)
        open(p, 'wb').write(d)


if __name__ == '__main__':
    main()
