#!/usr/bin/env python3
"""Shrink userdata and add a 'darwin' partition at the end of the eMMC, in a copy of the GPT.

usage: gpt-carve.py gpt-primary.bin gpt-backup.bin DISK_SECTORS DARWIN_SECTORS OUTDIR
Writes OUTDIR/new-primary.bin (LBA 0..33) and OUTDIR/new-backup.bin (the last 33 LBAs). The header grows
from its entry count to 128 entries, which the 32-sector entry array already has room for; nothing moves.
"""
import struct, sys, uuid, zlib

APFS_TYPE = uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')

def hdr_fields(h):
    (sig, rev, hsz, crc, _, my, alt, first, last, guid, ent_lba, n, esz, ecrc) = struct.unpack_from('<8sIIIIQQQQ16sQIII', h, 0)
    return dict(sig=sig, rev=rev, hsz=hsz, crc=crc, my=my, alt=alt, first=first, last=last, guid=guid, ent_lba=ent_lba, n=n, esz=esz, ecrc=ecrc)

def pack_hdr(f, entries_crc, my, alt, ent_lba):
    h = struct.pack('<8sIIIIQQQQ16sQIII', f['sig'], f['rev'], f['hsz'], 0, 0, my, alt, f['first'], f['last'], f['guid'], ent_lba, 128, f['esz'], entries_crc)
    crc = zlib.crc32(h[:f['hsz']])
    h = h[:16] + struct.pack('<I', crc) + h[20:]
    return h.ljust(512, b'\0')

def main():
    prim, back = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
    disk, dsz, out = int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    f = hdr_fields(prim[512:1024])
    assert f['sig'] == b'EFI PART' and f['esz'] == 128 and f['ent_lba'] == 2 and f['first'] >= 34, f
    assert zlib.crc32(prim[512:512 + f['hsz']][:16] + b'\0\0\0\0' + prim[512 + 20:512 + f['hsz']]) == f['crc'], 'primary header crc'
    ents = bytearray(prim[1024:1024 + 128 * 128])
    assert zlib.crc32(bytes(ents[:f['n'] * 128])) == f['ecrc'], 'entries crc'
    # the backup must describe the same table
    bf = hdr_fields(back[-512:])
    assert bf['sig'] == b'EFI PART' and bf['my'] == disk - 1 and bf['ecrc'] == f['ecrc'] and back[:f['n'] * 128] == bytes(ents[:f['n'] * 128]), 'backup differs'
    names = {}
    for i in range(128):
        e = ents[i * 128:(i + 1) * 128]
        if e[:16] != b'\0' * 16:
            names[e[56:128].decode('utf-16le').rstrip('\0')] = i
    print(f"header: {f['n']} entries, first {f['first']} last {f['last']}, {len(names)} used")
    assert 'darwin' not in names, 'already carved'
    ui = names['userdata']
    ufirst, ulast = struct.unpack_from('<QQ', ents, ui * 128 + 32)
    assert ulast <= f['last'], (ulast, f['last'])
    dlast = ulast                       # darwin takes the tail of userdata: it ends where userdata ends now
    dfirst = dlast + 1 - dsz
    assert dfirst % 2048 == 0 and dfirst > ufirst
    struct.pack_into('<Q', ents, ui * 128 + 40, dfirst - 1)
    free = [i for i in range(128) if ents[i * 128:i * 128 + 16] == b'\0' * 16 and i >= f['n']][0]
    e = APFS_TYPE.bytes_le + uuid.uuid4().bytes_le + struct.pack('<QQQ', dfirst, dlast, 0) + 'darwin'.encode('utf-16le').ljust(72, b'\0')
    ents[free * 128:(free + 1) * 128] = e
    ecrc = zlib.crc32(bytes(ents))
    newp = prim[:512] + pack_hdr(f, ecrc, 1, disk - 1, 2) + bytes(ents)
    newb = bytes(ents) + pack_hdr(f, ecrc, disk - 1, 1, disk - 33)
    assert len(newp) == 34 * 512 and len(newb) == 33 * 512
    open(out + '/new-primary.bin', 'wb').write(newp)
    open(out + '/new-backup.bin', 'wb').write(newb)
    print(f'userdata {ufirst}..{dfirst - 1} ({(dfirst - ufirst) / 2097152:.2f} GiB), darwin {dfirst}..{dlast} ({dsz / 2097152:.2f} GiB) in entry {free}')

if __name__ == '__main__':
    main()
