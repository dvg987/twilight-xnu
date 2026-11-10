#!/usr/bin/env python3
"""The USB stick tvOS boots from on the box, as CoreELEC's does: the box's own U-Boot reads the first partition.

usage: twl-usb.py [-s SIZE] [--boot IMG] [--system APFS] [--force] OUT

  OUT       an image file (made sparse, SIZE long: 8G by default; "16G", "32000M" ...) or the stick itself
            (/dev/sdX, as root: its size is the stick's). Everything on it is lost.
  --boot    the Android boot image of twloader and the payload (tools/twl-release.sh: out/twl-usb.img)
  --system  the tvOS system volume (out/tvos-system.apfs)

The stick (MBR; hakios/drivers/storage/hakios.usbdisk.c takes the partitions by their place in the table):

  1  FAT16  256 MiB   aml_autoscript, cfgload, twl.img        U-Boot
  2  APFS             the tvOS system volume, read-only       /dev/usb_darwin
  3  HFSX   the rest  /private/var, journaled                 /dev/usb_darwin_data

aml_autoscript is what U-Boot runs from a stick when the box starts in update mode ("adb reboot update", or the
reset button held while power is applied): it makes U-Boot look for cfgload on a stick at every start, before the
eMMC, under the names CoreELEC uses -- so a box that already starts CoreELEC from a stick starts this one with no
change, and the other way round. cfgload loads twl.img and starts it. Without a stick the box starts Android.

mkfs.hfsplus and fsck.hfsplus: as tools/twl-datavol.py says.
"""
import argparse
import fcntl
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import time
import zlib

TOP = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(TOP, 'hakios', 'tools'))
from mkdatavol import used_blocks  # noqa: E402

SECTOR = 512
MIB = 1 << 20
FAT_FIRST = 4 * MIB // SECTOR
FAT_SECTORS = 256 * MIB // SECTOR
HFS_BS = 4096
HFSPROGS = 'work/hfsprogs/hfsprogs/src/diskdev_cmds-540.1.linux3'

AUTOSCRIPT = r'''setenv cfgloadusb 'if fatload usb 0:1 ${loadaddr} cfgload; then setenv device usb; setenv devnr 0; setenv partnr 1; autoscr ${loadaddr}; fi'
setenv bootfromusb 'usb start; if usb storage; then run cfgloadusb; fi'
setenv bootcmd 'run bootfromusb; run storeboot'
saveenv
run storeargs
run bootfromusb
'''

CFGLOAD = r'''echo "tvOS from ${device} ${devnr}:${partnr}"
if fdt addr ${dtb_mem_addr}; then echo "dtb in place"; else run common_dtb_load; fi
setenv twl_addr 0x08000000
if fatload ${device} ${devnr}:${partnr} ${twl_addr} twl.img; then bootm ${twl_addr}; bootm start; bootm loados; bootm prep; bootm go; fi
'''


def die(msg):
    sys.exit('twl-usb.py: ' + msg)


def script_image(text, name):
    """What mkimage -A arm64 -T script makes."""
    body = text.encode()
    data = struct.pack('>II', len(body), 0) + body
    head = struct.pack('>7I4B32s', 0x27051956, 0, int(time.time()), len(data), 0, 0, zlib.crc32(data), 5, 22, 6, 0,
                       name.encode())
    return head[:4] + struct.pack('>I', zlib.crc32(head)) + head[8:] + data


def short_name(name, n):
    base, _, ext = name.upper().partition('.')
    if len(base) <= 8 and len(ext) <= 3:
        return (base.ljust(8) + ext.ljust(3)).encode()
    return (base[:6] + '~' + str(n)).ljust(8).encode() + ext[:3].ljust(3).encode()


def dir_entries(name, n, cluster, size):
    short = short_name(name, n)
    total = 0
    for c in short:
        total = (((total & 1) << 7) + (total >> 1) + c) & 0xff
    units = [ord(c) for c in name] + [0]
    units += [0xffff] * (-len(units) % 13)
    out = b''
    count = len(units) // 13
    for i in range(count, 0, -1):
        u = units[(i - 1) * 13:i * 13]
        out += struct.pack('<B10sBBB12sH4s', i | (0x40 if i == count else 0), struct.pack('<5H', *u[:5]), 0x0f, 0,
                           total, struct.pack('<6H', *u[5:11]), 0, struct.pack('<2H', *u[11:]))
    out += struct.pack('<11sBBBHHHHHHHI', short, 0x20, 0, 0, 0, 0, 0, cluster >> 16, 0, 0x21, cluster & 0xffff, size)
    return out


def fat16(sectors, files, label):
    """A FAT16 volume of that many sectors with the files (name, bytes) in its root: [(offset, bytes)] to write."""
    spc, reserved, root_entries = 16, 1, 512
    root_sectors = root_entries * 32 // SECTOR
    fat_sectors = 1
    while True:
        clusters = (sectors - reserved - root_sectors - 2 * fat_sectors) // spc
        need = -(-(clusters + 2) * 2 // SECTOR)
        if need <= fat_sectors:
            break
        fat_sectors = need
    if not 4085 <= clusters < 65525:
        die('the first partition is the wrong size for FAT16')
    bs = bytearray(SECTOR)
    struct.pack_into('<3s8sHBHBHHBHHHII', bs, 0, b'\xeb\x3c\x90', b'TWILIGHT', SECTOR, spc, reserved, 2, root_entries,
                     0, 0xf8, fat_sectors, 63, 255, FAT_FIRST, sectors)
    struct.pack_into('<BBBI11s8s', bs, 36, 0x80, 0, 0x29, 0x54574c31, label.ljust(11).encode(), b'FAT16   ')
    bs[510:512] = b'\x55\xaa'
    fat = [0xfff8, 0xffff]
    root = struct.pack('<11sB20x', label.ljust(11).encode(), 0x08)
    data = []
    csize = spc * SECTOR
    for n, (name, body) in enumerate(files, 1):
        first = len(fat)
        count = max(1, -(-len(body) // csize))
        fat += list(range(first + 1, first + count)) + [0xffff]
        root += dir_entries(name, n, first, len(body))
        data.append((first, body))
    if len(fat) - 2 > clusters:
        die('the files do not fit in the first partition')
    if len(root) > root_sectors * SECTOR:
        die('too many files for the root directory')
    table = struct.pack(f'<{len(fat)}H', *fat).ljust(fat_sectors * SECTOR, b'\0')
    first_data = reserved + 2 * fat_sectors + root_sectors
    out = [(0, bytes(bs)), (reserved * SECTOR, table), ((reserved + fat_sectors) * SECTOR, table),
           ((reserved + 2 * fat_sectors) * SECTOR, root.ljust(root_sectors * SECTOR, b'\0'))]
    for first, body in data:
        out.append(((first_data + (first - 2) * spc) * SECTOR, body))
    return out


def tool(name, built):
    path = shutil.which(name) or os.path.join(TOP, HFSPROGS, built)
    if not os.access(path, os.X_OK):
        die(f'no {name}: see tools/twl-datavol.py -h')
    return path


def size_arg(s):
    unit = {'K': 1 << 10, 'M': MIB, 'G': 1 << 30}.get(s[-1:].upper())
    return int(s[:-1]) * unit if unit else int(s)


def copy_file(out, offset, path, skip_zeros):
    zeros = bytes(MIB)
    with open(path, 'rb') as f:
        pos = 0
        while True:
            chunk = f.read(MIB)
            if not chunk:
                break
            if not (skip_zeros and chunk == zeros[:len(chunk)]):
                os.pwrite(out, chunk, offset + pos)
            pos += len(chunk)


def main():
    ap = argparse.ArgumentParser(usage=__doc__)
    ap.add_argument('-s', '--size', type=size_arg, default=8 << 30)
    ap.add_argument('--boot', default=os.path.join(TOP, 'out', 'twl-usb.img'))
    ap.add_argument('--system', default=os.path.join(TOP, 'out', 'tvos-system.apfs'))
    ap.add_argument('--force', action='store_true')
    ap.add_argument('out')
    a = ap.parse_args()
    for path in (a.boot, a.system):
        if not os.path.isfile(path):
            die(f'no {path}')
    mkfs = tool('mkfs.hfsplus', 'newfs_hfs.tproj/newfs_hfs')
    fsck = tool('fsck.hfsplus', 'fsck_hfs.tproj/fsck_hfs')

    device = os.path.exists(a.out) and stat.S_ISBLK(os.stat(a.out).st_mode)
    if device:
        name = os.path.basename(os.path.realpath(a.out))
        removable = os.path.join('/sys/block', name, 'removable')
        if not os.path.exists(removable):
            die(f'{a.out} is not a whole disk')
        if open(removable).read().strip() != '1' and not a.force:
            die(f'{a.out} is not a removable disk (--force if it is the right one)')
        with open('/proc/mounts') as m:
            if any(line.split()[0].startswith('/dev/' + name) for line in m):
                die(f'{a.out} has mounted partitions: unmount them first')
        out = os.open(a.out, os.O_RDWR | os.O_EXCL)
        size = struct.unpack('Q', fcntl.ioctl(out, 0x80081272, bytes(8)))[0]      # BLKGETSIZE64
    else:
        out = os.open(a.out, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
        size = a.size
        os.ftruncate(out, size)

    system_bytes = os.path.getsize(a.system)
    sys_first = FAT_FIRST + FAT_SECTORS
    sys_sectors = -(-system_bytes // MIB) * MIB // SECTOR
    data_first = sys_first + sys_sectors
    data_blocks = (size // SECTOR - data_first) * SECTOR // HFS_BS
    if data_blocks * HFS_BS < 1 << 30:
        die(f'{size >> 20} MB is too small: the system volume takes {system_bytes >> 20} MB and /private/var wants '
            f'1024 MB or more')
    if size // SECTOR > 0xffffffff:
        die('more than 2 TiB')
    data_sectors = data_blocks * HFS_BS // SECTOR

    os.pwrite(out, bytes(MIB), 0)
    boot = open(a.boot, 'rb').read()
    files = [('aml_autoscript', script_image(AUTOSCRIPT, 'twilight autoscript')),
             ('cfgload', script_image(CFGLOAD, 'twilight cfgload')), ('twl.img', boot)]
    for offset, data in fat16(FAT_SECTORS, files, 'TWILIGHT'):
        os.pwrite(out, data, FAT_FIRST * SECTOR + offset)

    print(f'the system volume, {system_bytes >> 20} MB ...', flush=True)
    copy_file(out, sys_first * SECTOR, a.system, not device)

    os.makedirs(os.path.join(TOP, 'work'), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(TOP, 'work')) as tmp:
        path = os.path.join(tmp, 'data.hfs')
        with open(path, 'wb') as f:
            f.truncate(data_blocks * HFS_BS)
        subprocess.run([mkfs, '-s', '-J', '-b', str(HFS_BS), '-U', '0', '-G', '0', '-M', '755', '-v', 'Data', path],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([fsck, '-y', '-f', path], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([fsck, '-n', '-f', path], check=True, stdout=subprocess.DEVNULL)
        with open(path, 'rb') as img:
            total, used = used_blocks(img)
            if total != data_blocks:
                die(f'the data volume has {total} blocks, not {data_blocks}')
            b = 0
            while b < total:
                if not used(b):
                    b += 1
                    continue
                e = b
                while e < total and e - b < 256 and used(e):
                    e += 1
                img.seek(b * HFS_BS)
                os.pwrite(out, img.read((e - b) * HFS_BS), data_first * SECTOR + b * HFS_BS)
                b = e

    mbr = bytearray(SECTOR)
    for i, (kind, first, count) in enumerate(((0x0e, FAT_FIRST, FAT_SECTORS), (0xaf, sys_first, sys_sectors),
                                              (0xaf, data_first, data_sectors))):
        struct.pack_into("<B3sB3sII", mbr, 446 + 16 * i, 0x80 if i == 0 else 0, b"\xfe\xff\xff", kind,
                         b"\xfe\xff\xff", first, count)
    struct.pack_into('<I', mbr, 440, 0x54574c31)
    mbr[510:512] = b'\x55\xaa'
    os.pwrite(out, bytes(mbr), 0)
    os.fsync(out)
    os.close(out)
    print(f'{a.out}: {size >> 20} MB; U-Boot\'s partition 256 MB, the system volume {sys_sectors * SECTOR >> 20} MB, '
          f'/private/var {data_sectors * SECTOR >> 20} MB')


if __name__ == '__main__':
    main()
