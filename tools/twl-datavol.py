#!/usr/bin/env python3
"""The data volume for tvOS on the box: an empty journaled HFSX volume for darwin from 4 GiB on, as only the blocks it
uses -- what tools/twl-datavol-write.py puts there from the rescue Linux.

usage: twl-datavol.py [OUT]       (OUT default out/twl-datavol: OUT.bin and OUT.runs are made)

The kernel gives that stretch of darwin out as /dev/emmc_darwin_data (hakios drivers/storage/hakios.emmc.c, emmc=2:
EMMC_DATA_FIRST) and init mounts it on /private/var (var_hfs=emmc_darwin_data). Writing it again empties it.

As hakios/tools/mkdatavol.py does for the phone: mkfs.hfsplus makes the volume in a sparse file, and the blocks the
new volume uses (volume headers, allocation bitmap, catalog and extents trees, journal) are read from its own
allocation bitmap; everything else is free space, which HFS Plus never reads, so it is not sent (6 GB over USB for
a few megabytes). OUT.bin is those blocks one run after the other; OUT.runs has a line per run, "BLOCK COUNT" in
blocks of 4096 from the volume's start, in the order of OUT.bin. The volume's root is root's, 0755 (with another
owner Darwin takes the volume for one to migrate). Case-sensitive (HFSX), as Darwin's own data volume, journaled.

mkfs.hfsplus: from $PATH, or hfsprogs built in work/hfsprogs (Arch's AUR package, built without installing it):
    git clone https://aur.archlinux.org/hfsprogs.git work/hfsprogs/hfsprogs
    cd work/hfsprogs/hfsprogs && makepkg -o && cd src/diskdev_cmds-540.1.linux3 && make -f Makefile
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'hakios', 'tools'))
from mkdatavol import used_blocks  # noqa: E402

BS = 4096
DARWIN_SECTORS = 20971520          # GPT "darwin": sectors 40095744..61067263 (tools/gpt-carve.py)
DATA_FIRST_SECTOR = 8388608        # 4 GiB in: hakios.emmc.c, EMMC_DATA_FIRST
BLOCKS = (DARWIN_SECTORS - DATA_FIRST_SECTOR) * 512 // BS
HFSPROGS = 'work/hfsprogs/hfsprogs/src/diskdev_cmds-540.1.linux3'


def tool(name, built):
    top = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    path = shutil.which(name) or os.path.join(top, HFSPROGS, built)
    if not os.access(path, os.X_OK):
        sys.exit(f'twl-datavol.py: no {name}: see the usage (python3 {sys.argv[0]} -h)')
    return path


def main():
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1].startswith('-')):
        sys.exit(__doc__)
    top = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    out = sys.argv[1] if len(sys.argv) == 2 else os.path.join(top, 'out', 'twl-datavol')
    mkfs = tool('mkfs.hfsplus', 'newfs_hfs.tproj/newfs_hfs')
    fsck = tool('fsck.hfsplus', 'fsck_hfs.tproj/fsck_hfs')
    os.makedirs(os.path.join(top, 'work'), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(top, 'work')) as tmp:     # a sparse 6 GB file: not in RAM
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(BLOCKS * BS)
        subprocess.run([mkfs, '-s', '-J', '-b', str(BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data', path],
                       check=True, stdout=subprocess.DEVNULL)
        # hfsprogs' mkfs leaves the root folder without kHFSHasFolderCountMask, which an HFSX volume has (its own fsck
        # says so); fsck sets it, then has to find the volume clean
        subprocess.run([fsck, '-y', '-f', path], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([fsck, '-n', '-f', path], check=True, stdout=subprocess.DEVNULL)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != BLOCKS:
                sys.exit(f'twl-datavol.py: the volume has {total} blocks, the data stretch of darwin {BLOCKS}')
            runs = []
            for b in range(total):
                if used(b):
                    if runs and runs[-1][0] + runs[-1][1] == b:
                        runs[-1][1] += 1
                    else:
                        runs.append([b, 1])
            with open(out + '.bin', 'wb') as data:
                for first, count in runs:
                    img.seek(first * BS)
                    data.write(img.read(count * BS))
    with open(out + '.runs', 'w') as f:
        for first, count in runs:
            f.write(f'{first} {count}\n')
    written = sum(c for _, c in runs)
    print(f'{out}.bin: {written} blocks ({written * BS >> 10} KB) in {len(runs)} runs; the volume: {BLOCKS} blocks '
          f'({BLOCKS * BS >> 20} MB) from darwin sector {DATA_FIRST_SECTOR}')


if __name__ == '__main__':
    main()
