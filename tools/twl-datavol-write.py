#!/usr/bin/env python3
"""Put the empty data volume (tools/twl-datavol.py: out/twl-datavol.bin + .runs) into darwin from 4 GiB on, from the
box's rescue Linux (tools/rescue-up.sh first): each run streamed with nc into dd at its place, then read back.

usage: twl-datavol-write.py [--check] [OUT]      (OUT default out/twl-datavol)
    --check   only find darwin and check it, write nothing

Before writing it makes sure the partition is the one the port made (tools/gpt-carve.py): named darwin, sectors
40095744 for 20971520, not mounted, and with the tvOS system volume (APFS, "NXSB") at its start -- the first
4 GiB, which nothing here touches. Everything of the data volume that was there before is gone afterwards.
"""
import hashlib
import io
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rx import rx  # noqa: E402

HOST, PORT = '192.168.77.1', 5004
BS = 4096
DARWIN_START, DARWIN_SECTORS = 40095744, 20971520
DATA_FIRST = (4 << 30) // BS                    # in blocks of 4096 from darwin's start: hakios.emmc.c EMMC_DATA_FIRST
DATA_BLOCKS = DARWIN_SECTORS * 512 // BS - DATA_FIRST


def run(cmd):
    out = io.BytesIO()
    rx(cmd, out=out)
    return out.getvalue().decode('utf-8', 'replace').strip()


def find_darwin():
    """The partition named darwin: (device, start, size) as the rescue kernel has it."""
    found = run('for d in /sys/class/block/*; do grep -qx PARTNAME=darwin $d/uevent 2>/dev/null && '
                'echo $(sed -n "s/^DEVNAME=//p" $d/uevent) $(cat $d/start) $(cat $d/size); done')
    lines = [line.split() for line in found.splitlines() if len(line.split()) == 3]
    if len(lines) != 1:
        sys.exit(f'twl-datavol-write.py: not exactly one partition named darwin: {found!r}')
    name, start, size = lines[0]
    return '/dev/' + name, int(start), int(size)


def main():
    args = [a for a in sys.argv[1:] if a != '--check']
    check_only = len(args) != len(sys.argv) - 1
    if len(args) > 1 or (args and args[0].startswith('-')):
        sys.exit(__doc__)
    top = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    out = args[0] if args else os.path.join(top, 'out', 'twl-datavol')

    dev, start, size = find_darwin()
    print(f'darwin: {dev}, sectors {start} for {size}')
    if (start, size) != (DARWIN_START, DARWIN_SECTORS):
        sys.exit(f'twl-datavol-write.py: darwin is not where the port made it ({DARWIN_START} for {DARWIN_SECTORS})')
    if run(f'ls -l {dev} | cut -c1') != 'b':
        sys.exit(f'twl-datavol-write.py: {dev} is not a block device')
    if run(f'grep -c "^{dev} " /proc/mounts') != '0':
        sys.exit(f'twl-datavol-write.py: {dev} is mounted')
    magic = run(f'dd if={dev} bs=4096 count=1 2>/dev/null | dd bs=1 skip=32 count=4 2>/dev/null')
    if magic != 'NXSB':
        sys.exit(f'twl-datavol-write.py: no APFS container at the start of darwin ({magic!r}): not writing')
    print('darwin checks out: the tvOS system volume at its start')

    runs = [tuple(map(int, line.split())) for line in open(out + '.runs') if line.strip()]
    data = open(out + '.bin', 'rb').read()
    if sum(c for _, c in runs) * BS != len(data) or any(f + c > DATA_BLOCKS for f, c in runs):
        sys.exit(f'twl-datavol-write.py: {out}.runs does not fit {out}.bin or the data stretch')
    if check_only:
        print(f'would write {len(runs)} runs, {len(data) >> 10} KB, from darwin block {DATA_FIRST} (4096-byte blocks)')
        return

    at = 0
    for first, count in runs:
        piece = data[at:at + count * BS]
        at += count * BS
        seek = DATA_FIRST + first
        said = []
        t = threading.Thread(target=lambda: said.append(
            run(f'nc -l -p {PORT} | dd of={dev} bs={BS} seek={seek} conv=fsync 2>&1 | tail -1')))
        t.start()
        for attempt in range(50):                       # until nc listens
            try:
                s = socket.create_connection((HOST, PORT), timeout=10)
                break
            except OSError:
                if attempt == 49:
                    raise
                time.sleep(0.2)
        s.sendall(piece)
        s.shutdown(socket.SHUT_WR)
        t.join()
        s.close()
        print(f'  blocks {seek}..{seek + count - 1}: {said[0] if said else "?"}')

    run('sync; echo 3 > /proc/sys/vm/drop_caches')
    at, bad = 0, 0
    for first, count in runs:
        want = hashlib.md5(data[at:at + count * BS]).hexdigest()
        at += count * BS
        got = run(f'dd if={dev} bs={BS} skip={DATA_FIRST + first} count={count} 2>/dev/null | md5sum').split()[0]
        if got != want:
            print(f'  blocks {DATA_FIRST + first}+{count}: read back {got}, written {want}')
            bad += 1
    if bad:
        sys.exit(f'twl-datavol-write.py: {bad} of {len(runs)} runs did not read back')
    print(f'the data volume is in darwin: {len(runs)} runs, {len(data) >> 10} KB, read back and checked')


if __name__ == '__main__':
    main()
