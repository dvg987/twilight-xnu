#!/usr/bin/env python3
"""Read memory of a dyld shared cache (and its subcaches) by unslid address.

  dsc.py CACHE ADDR [COUNT]        COUNT 64-bit words at ADDR (pointers decoded: slide info v2 / v3)
  dsc.py CACHE --vtable ADDR N SYMS   N entries of a vtable (after its 16-byte header), named from a symbol list
"""
import os, struct, sys

class Cache:
    def __init__(self, path):
        self.maps = []          # (address, size, fileoff, file)
        self.files = []
        self._load(path)
        base = path
        with open(path, 'rb') as f:
            h = f.read(0x200)
        self.slide_value_add = 0
        n = struct.unpack_from('<I', h, 0x18c)[0] if len(h) > 0x190 else 0   # subCacheArrayCount
        off = struct.unpack_from('<I', h, 0x188)[0]
        # subcache file names: CACHE.01, .02 ... (new style entries carry a suffix)
        d, b = os.path.split(base)
        for name in sorted(os.listdir(d or '.')):
            if name.startswith(b + '.') and not name.endswith(('.symbols', '.a2s', '.map')):
                self._load(os.path.join(d, name))

    def _load(self, path):
        f = open(path, 'rb')
        self.files.append(f)
        h = f.read(0x200)
        moff, mcount = struct.unpack_from('<II', h, 0x10)
        f.seek(moff)
        for i in range(mcount):
            addr, size, fileoff, maxp, initp = struct.unpack('<QQQII', f.read(32))
            self.maps.append((addr, size, fileoff, f))

    def read(self, addr, n):
        for a, s, o, f in self.maps:
            if a <= addr < a + s:
                f.seek(o + addr - a)
                return f.read(n)
        raise KeyError(hex(addr))

    def ptr(self, addr):
        raw = struct.unpack('<Q', self.read(addr, 8))[0]
        # arm64 v2: high bits hold the delta to the next rebase; the target is in the low 36-40 bits
        return raw & 0xFFFFFFFFF if raw >> 40 else raw

def load_syms(path):
    syms = {}
    for line in open(path):
        p = line.split()
        if len(p) >= 2:
            syms[int(p[0], 16)] = p[1]
    return syms

if __name__ == '__main__':
    c = Cache(sys.argv[1])
    if sys.argv[2] == '--vtable':
        vt, n = int(sys.argv[3], 16), int(sys.argv[4])
        syms = load_syms(sys.argv[5]) if len(sys.argv) > 5 else {}
        for i in range(n):
            p = c.ptr(vt + 16 + 8 * i)
            print(f'slot {8 * i:#05x}  {p:#x}  {syms.get(p, "")}')
    else:
        a, n = int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 8
        for i in range(n):
            print(f'{a + 8 * i:#x}: {c.ptr(a + 8 * i):#x}')
