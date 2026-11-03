#!/usr/bin/env python3
"""Pack an Android boot image (header v0 or v2) for the Amlogic U-Boot's fastboot boot."""
import argparse, hashlib, struct

def pad(b, ps):
    return b + b'\0' * ((ps - len(b) % ps) % ps)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('--second')
    ap.add_argument('--dtb')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--header-version', type=int, default=0)
    ap.add_argument('--base', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--kernel-offset', type=lambda x: int(x, 0), default=0x01080000)
    ap.add_argument('--ramdisk-offset', type=lambda x: int(x, 0), default=0x01000000)
    ap.add_argument('--second-offset', type=lambda x: int(x, 0), default=0x00f00000)
    ap.add_argument('--tags-offset', type=lambda x: int(x, 0), default=0x00000100)
    ap.add_argument('--dtb-offset', type=lambda x: int(x, 0), default=0x01f00000)
    ap.add_argument('--pagesize', type=int, default=2048)
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()
    rd = lambda p: open(p, 'rb').read() if p else b''
    k, r, s, d = rd(a.kernel), rd(a.ramdisk), rd(a.second), rd(a.dtb)
    cmd = a.cmdline.encode()
    assert len(cmd) < 512 + 1024
    sha = hashlib.sha1()
    for blob in (k, r, s):
        sha.update(blob); sha.update(struct.pack('<I', len(blob)))
    if a.header_version >= 1:
        sha.update(b''); sha.update(struct.pack('<I', 0))
    if a.header_version >= 2:
        sha.update(d); sha.update(struct.pack('<I', len(d)))
    hdr = struct.pack('<8s10I', b'ANDROID!', len(k), a.base + a.kernel_offset, len(r), a.base + a.ramdisk_offset,
                      len(s), a.base + a.second_offset, a.base + a.tags_offset, a.pagesize, a.header_version, 0)
    hdr += b'\0' * 16 + cmd[:512].ljust(512, b'\0') + sha.digest().ljust(32, b'\0') + cmd[512:].ljust(1024, b'\0')
    if a.header_version >= 1:
        hdr += struct.pack('<IQI', 0, 0, 0)  # recovery dtbo size/offset, header size (patched below)
    if a.header_version >= 2:
        hdr += struct.pack('<IQ', len(d), a.base + a.dtb_offset)
    if a.header_version >= 1:
        hsz = len(hdr)
        off = 1632 + 4 + 8
        hdr = hdr[:off] + struct.pack('<I', hsz) + hdr[off + 4:]
    out = pad(hdr, a.pagesize) + pad(k, a.pagesize) + pad(r, a.pagesize) + pad(s, a.pagesize)
    if a.header_version >= 2:
        out += pad(d, a.pagesize)
    open(a.output, 'wb').write(out)
    print(f'{a.output}: {len(out)} bytes, header v{a.header_version}')

if __name__ == '__main__':
    main()
