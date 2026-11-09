#!/usr/bin/env python3
"""Make vtprobe's AVCC packets and packed NV12 reference from a bounded H.264 test movie.

Usage: vdec-probe-fixture.py MOVIE OUTPUT_PREFIX
The movie must contain progressive 64x64 8-bit H.264 I pictures only.
"""
import json
from pathlib import Path
import struct
import subprocess
import sys


def unhex(dump):
    return b''.join(bytes.fromhex(line.split(':', 1)[1].split('  ', 1)[0])
                    for line in dump.splitlines() if ':' in line)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    movie, prefix = sys.argv[1:]
    probe = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams',
        '-show_packets', '-show_data', '-of', 'json', movie]))
    stream = probe['streams'][0]
    frames = json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_frames',
        '-show_entries', 'frame=pict_type,interlaced_frame', '-of', 'json', movie]))['frames']
    if (stream['codec_name'] != 'h264' or stream['width'] != 64 or stream['height'] != 64 or
            stream['pix_fmt'] != 'yuv420p' or not frames or len(frames) > 10000 or
            any(f['pict_type'] != 'I' or f['interlaced_frame'] for f in frames)):
        raise SystemExit('The probe accepts only progressive 64x64 yuv420p H.264 I pictures')
    avcc = unhex(stream['extradata'])
    if len(avcc) < 8 or avcc[0] != 1 or (avcc[4] & 3) != 3 or (avcc[5] & 31) != 1:
        raise SystemExit('Expected avcC with four-byte NAL lengths and exactly one SPS')
    at = 6
    sps_size = int.from_bytes(avcc[at:at+2], 'big'); at += 2
    sps = avcc[at:at+sps_size]; at += sps_size
    if at >= len(avcc) or avcc[at] != 1:
        raise SystemExit('Expected exactly one PPS')
    at += 1
    pps_size = int.from_bytes(avcc[at:at+2], 'big'); at += 2
    pps = avcc[at:at+pps_size]
    if len(sps) != sps_size or not sps or len(pps) != pps_size or not pps:
        raise SystemExit('Truncated avcC')
    packets = [unhex(p['data']) for p in probe['packets']]
    if len(packets) != len(frames) or any(not p or len(p) >= 1024*1024-1024 for p in packets):
        raise SystemExit('Expected one bounded packet per I picture')
    for packet in packets:
        at = 0
        while at < len(packet):
            n = int.from_bytes(packet[at:at+4], 'big'); at += 4
            if not n or n > len(packet)-at:
                raise SystemExit('Invalid AVCC packet')
            at += n
    reference = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-i', movie, '-map', '0:v:0', '-fps_mode', 'passthrough',
        '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'])
    if len(reference) != len(packets)*6144:
        raise SystemExit('Reference frame count does not match packet count')
    encoded = struct.pack('<4sIII', b'TWV1', len(sps), len(pps), len(packets)) + sps + pps
    encoded += b''.join(struct.pack('<I', len(p)) + p for p in packets)
    Path(prefix + '.packets').write_bytes(encoded)
    Path(prefix + '.nv12').write_bytes(reference)
    print(f'{len(packets)} frames: {prefix}.packets and {prefix}.nv12')


if __name__ == '__main__':
    main()
