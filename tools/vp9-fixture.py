#!/usr/bin/env python3
"""Make VP9 packets and per-frame MD5 references from the start of a VP9 movie (WebM, MP4 or IVF).

Usage: vp9-fixture.py MOVIE OUTPUT_PREFIX [FRAMES]

PREFIX.packets: "TWV9", packet count, timescale, bit depth (little-endian u32), then per packet (decode order) its
size (u32), its presentation time (s64) and its bytes as stored (a superframe whole, its index included).
PREFIX.md5: one line per packet, "PTS MD5": the frame FFmpeg shows for it, cropped 8-bit NV12 (luma rows, then
interleaved chroma rows; 10-bit streams rounded to 8 bits, as the decoder's 8-bit copy does).
"""
import hashlib
import os
import struct
import subprocess
import sys

import numpy


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    movie, prefix = sys.argv[1:3]
    frames = int(sys.argv[3]) if len(sys.argv) == 4 else 300
    probe = subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                                     'stream=codec_name,width,height,pix_fmt', '-of', 'default=nw=1', movie], text=True)
    info = dict(line.split('=', 1) for line in probe.split())
    codec, width, height, pix_fmt = info['codec_name'], info['width'], info['height'], info['pix_fmt']
    depth = {'yuv420p': 8, 'yuv420p10le': 10}.get(pix_fmt)
    if codec != 'vp9' or not depth:
        raise SystemExit(f'Expected 8- or 10-bit 4:2:0 VP9, not {codec} {pix_fmt}')
    # the packets through IVF (one packet a frame record, superframes kept whole)
    ivf = subprocess.run(['ffmpeg', '-v', 'error', '-i', movie, '-map', '0:v:0', '-c', 'copy', '-frames:v', str(frames),
                          '-f', 'ivf', '-'], check=True, stdout=subprocess.PIPE).stdout
    if ivf[:4] != b'DKIF':
        raise SystemExit('ffmpeg wrote no IVF')
    rate, scale = struct.unpack_from('<II', ivf, 16)
    at, packets = struct.unpack_from('<H', ivf, 6)[0], []
    while at + 12 <= len(ivf):
        size, pts = struct.unpack_from('<Iq', ivf, at)
        packets.append((pts, ivf[at + 12:at + 12 + size]))
        at += 12 + size
    body = bytearray(b'TWV9' + struct.pack('<III', len(packets), rate, depth))
    for pts, data in packets:
        body += struct.pack('<Iq', len(data), pts) + data
    with open(prefix + '.packets', 'wb') as f:
        f.write(body)
    cut = prefix + '.cut.ivf'
    with open(cut, 'wb') as f:
        f.write(ivf)
    # FFmpeg's decode as raw planes, one frame a packet (-fps_mode passthrough: none made up or dropped)
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', cut, '-fps_mode', 'passthrough', '-f', 'rawvideo',
                          '-pix_fmt', 'yuv420p' if depth == 8 else 'yuv420p10le', '-'],
                         check=True, stdout=subprocess.PIPE).stdout
    os.unlink(cut)
    width, height = int(width), int(height)
    sample = 1 if depth == 8 else 2
    frame = width * height * 3 // 2 * sample
    hashes = []
    for at in range(0, len(raw), frame):
        planes = numpy.frombuffer(raw[at:at + frame], dtype=numpy.uint8 if depth == 8 else '<u2')
        if depth == 10:
            planes = numpy.minimum((planes.astype(numpy.uint32) + 2) >> 2, 255).astype(numpy.uint8)
        y = planes[:width * height]
        u = planes[width * height:width * height * 5 // 4]
        v = planes[width * height * 5 // 4:]
        uv = numpy.empty(u.size * 2, dtype=numpy.uint8)
        uv[0::2], uv[1::2] = u, v
        hashes.append(hashlib.md5(y.tobytes() + uv.tobytes()).hexdigest())
    if len(hashes) != len(packets):
        raise SystemExit(f'{len(hashes)} decoded frames for {len(packets)} packets')
    with open(prefix + '.md5', 'w') as f:
        for (pts, _), h in zip(packets, hashes):
            f.write(f'{pts} {h}\n')
    print(f'{prefix}: {width}x{height} {depth}-bit, {len(packets)} packets, {len(body)} bytes')


if __name__ == '__main__':
    main()
