#!/usr/bin/env python3
"""Make vtprobe's packets and per-frame MD5 reference from the start of a real H.264 movie.

Usage: vdec-real-fixture.py MOVIE OUTPUT_PREFIX [FRAMES]

PREFIX.packets: "TWV2", SPS size, PPS size, packet count, timescale (little-endian u32), the SPS and PPS,
then per packet (decode order) its size (u32), its presentation time (s64) and its AVCC bytes.
PREFIX.md5: one line per frame in presentation order, "PTS MD5", the MD5 of the frame as FFmpeg decodes it,
cropped NV12 (luma rows, then interleaved chroma rows).
"""
import json
import os
import struct
import subprocess
import sys


def unhex(dump):
    return b''.join(bytes.fromhex(line.split(':', 1)[1].split('  ', 1)[0])
                    for line in dump.splitlines() if ':' in line)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    movie, prefix = sys.argv[1:3]
    frames = int(sys.argv[3]) if len(sys.argv) == 4 else 300
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams', '-show_packets', '-show_data',
        '-read_intervals', f'%+#{frames}', '-of', 'json', movie]))
    stream = probe['streams'][0]
    if stream['codec_name'] != 'h264' or stream['pix_fmt'] != 'yuv420p':
        raise SystemExit('Expected 8-bit 4:2:0 H.264')
    avcc = unhex(stream['extradata'])
    if len(avcc) < 8 or avcc[0] != 1 or (avcc[4] & 3) != 3 or (avcc[5] & 31) < 1:
        raise SystemExit('Expected avcC with four-byte NAL lengths')
    at = 6
    sps_size = int.from_bytes(avcc[at:at + 2], 'big'); at += 2
    sps = avcc[at:at + sps_size]; at += sps_size
    for _ in range((avcc[5] & 31) - 1):  # further SPSs are not used
        at += 2 + int.from_bytes(avcc[at:at + 2], 'big')
    if avcc[at] < 1:
        raise SystemExit('Expected a PPS')
    at += 1
    pps_size = int.from_bytes(avcc[at:at + 2], 'big'); at += 2
    pps = avcc[at:at + pps_size]
    num, den = (int(x) for x in stream['time_base'].split('/'))
    if num != 1:
        raise SystemExit('Expected a 1/N time base')
    packets = probe['packets'][:frames]
    body = bytearray(b'TWV2' + struct.pack('<IIII', len(sps), len(pps), len(packets), den) + sps + pps)
    for p in packets:
        data = unhex(p['data'])
        body += struct.pack('<Iq', len(data), int(p['pts'])) + data
    with open(prefix + '.packets', 'wb') as f:
        f.write(body)
    # FFmpeg's own decode of exactly these packets (with B frames the first N in decode order are not the
    # first N shown), in presentation order, one MD5 per frame of cropped NV12
    cut = prefix + '.cut.mp4'
    subprocess.check_call(['ffmpeg', '-v', 'error', '-y', '-i', movie, '-map', '0:v:0', '-c', 'copy',
                           '-frames:v', str(len(packets)), cut])
    md5 = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', cut, '-pix_fmt', 'nv12', '-f', 'framemd5', '-'],
                                  text=True)
    os.unlink(cut)
    hashes = [line.rsplit(',', 1)[1].strip() for line in md5.splitlines() if line and not line.startswith('#')]
    times = sorted(int(p['pts']) for p in packets)
    if len(hashes) != len(times):
        raise SystemExit(f'{len(hashes)} decoded frames for {len(times)} packets')
    with open(prefix + '.md5', 'w') as f:
        for t, h in zip(times, hashes):
            f.write(f'{t} {h}\n')
    print(f'{prefix}: {stream["width"]}x{stream["height"]} {stream.get("profile")}, {len(packets)} packets, '
          f'{len(body)} bytes')


if __name__ == '__main__':
    main()
