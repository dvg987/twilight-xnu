#!/usr/bin/env python3
"""Make Annex B packets and per-frame MD5 references from the start of an HEVC movie.

Usage: hevc-fixture.py MOVIE OUTPUT_PREFIX [FRAMES]

PREFIX.packets: "TWH1", packet count, timescale, bit depth (little-endian u32), then per packet (decode order)
its size (u32), its presentation time (s64) and its Annex B bytes; the first packet starts with the VPS, SPS and
PPS from hvcC.
PREFIX.md5: one line per frame in presentation order, "PTS MD5", the MD5 of the frame as FFmpeg decodes it,
cropped 8-bit NV12 (luma rows, then interleaved chroma rows; 10-bit streams rounded to 8 bits).
"""
import hashlib
import json
import os
import struct
import subprocess
import sys

import numpy


def unhex(dump):
    return b''.join(bytes.fromhex(line.split(':', 1)[1].split('  ', 1)[0])
                    for line in dump.splitlines() if ':' in line)


def hvcc_arrays(hvcc):
    if len(hvcc) < 23 or hvcc[0] != 1:
        raise SystemExit('Expected hvcC')
    length_size = (hvcc[21] & 3) + 1
    out, at = [], 23
    for _ in range(hvcc[22]):
        nal_type = hvcc[at] & 0x3f
        count = int.from_bytes(hvcc[at + 1:at + 3], 'big')
        at += 3
        for _ in range(count):
            size = int.from_bytes(hvcc[at:at + 2], 'big')
            out.append((nal_type, hvcc[at + 2:at + 2 + size]))
            at += 2 + size
    return length_size, out


def annexb(data, length_size):
    out, at = bytearray(), 0
    while at + length_size <= len(data):
        size = int.from_bytes(data[at:at + length_size], 'big')
        at += length_size
        out += b'\0\0\0\1' + data[at:at + size]
        at += size
    return bytes(out)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    movie, prefix = sys.argv[1:3]
    frames = int(sys.argv[3]) if len(sys.argv) == 4 else 300
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams', '-show_packets', '-show_data',
        '-read_intervals', f'%+#{frames}', '-of', 'json', movie]))
    stream = probe['streams'][0]
    depth = {'yuv420p': 8, 'yuv420p10le': 10}.get(stream['pix_fmt'])
    if stream['codec_name'] != 'hevc' or not depth:
        raise SystemExit('Expected 8- or 10-bit 4:2:0 HEVC')
    length_size, params = hvcc_arrays(unhex(stream['extradata']))
    headers = b''.join(b'\0\0\0\1' + nal for nal_type, nal in params if nal_type in (32, 33, 34))
    num, den = (int(x) for x in stream['time_base'].split('/'))
    if num != 1:
        raise SystemExit('Expected a 1/N time base')
    packets = probe['packets'][:frames]
    body = bytearray(b'TWH1' + struct.pack('<III', len(packets), den, depth))
    for i, p in enumerate(packets):
        data = annexb(unhex(p['data']), length_size)
        if i == 0:
            data = headers + data
        body += struct.pack('<Iq', len(data), int(p['pts'])) + data
    with open(prefix + '.packets', 'wb') as f:
        f.write(body)
    # PREFIX.vt for vtprobe (TWV3): "TWV3", VPS/SPS/PPS sizes, count, timescale, the three sets, then per packet its
    # size, its presentation time and its hvcC bytes (four-byte NAL lengths)
    sets = {t: nal for t, nal in reversed(params)}
    if not all(t in sets for t in (32, 33, 34)) or length_size != 4:
        raise SystemExit('Expected one VPS, SPS and PPS and four-byte NAL lengths')
    vt = bytearray(b'TWV3' + struct.pack('<IIIII', len(sets[32]), len(sets[33]), len(sets[34]), len(packets), den))
    vt += sets[32] + sets[33] + sets[34]
    for p in packets:
        data = unhex(p['data'])
        vt += struct.pack('<Iq', len(data), int(p['pts'])) + data
    with open(prefix + '.vt', 'wb') as f:
        f.write(vt)
    cut = prefix + '.cut.mp4'
    subprocess.check_call(['ffmpeg', '-v', 'error', '-y', '-i', movie, '-map', '0:v:0', '-c', 'copy',
                           '-frames:v', str(len(packets)), cut])
    # FFmpeg's decode as raw planes; 10-bit rounded to 8 bits ((v + 2) >> 2, at most 255), as the decoder's 8-bit
    # double write does
    width, height = int(stream['width']), int(stream['height'])
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', cut, '-f', 'rawvideo',
                          '-pix_fmt', 'yuv420p' if depth == 8 else 'yuv420p10le', '-'],
                         check=True, stdout=subprocess.PIPE).stdout
    os.unlink(cut)
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
    times = sorted(int(p['pts']) for p in packets)
    if len(hashes) != len(times):
        raise SystemExit(f'{len(hashes)} decoded frames for {len(times)} packets')
    with open(prefix + '.md5', 'w') as f:
        for t, h in zip(times, hashes):
            f.write(f'{t} {h}\n')
    print(f'{prefix}: {stream["width"]}x{stream["height"]} {stream.get("profile")} {depth}-bit, '
          f'{len(packets)} packets, {len(body)} bytes')


if __name__ == '__main__':
    main()
