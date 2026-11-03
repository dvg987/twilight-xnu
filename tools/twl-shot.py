#!/usr/bin/env python3
"""The last frame of a run (twloader saves it at 0x70000000 on the way back), through the rescue Linux, as a PNG.

usage: twl-shot.py OUT.png [--half]
"""
import io, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rx import rx
from PIL import Image
W, H = 1920, 1080

def grab(pa, out):
    buf = io.BytesIO()
    rx(f'dd if=/dev/mem bs=1M skip={pa >> 20} count=8 2>/dev/null', out=buf)
    if '--bpp24' not in sys.argv:
        raw = buf.getvalue()[:W * H * 4]
        img = Image.frombuffer('RGBA', (W, H), raw, 'raw', 'BGRA', 0, 1).convert('RGB')
    else:   # a 24-bit frame: B G R, 5760 bytes a row
        raw = buf.getvalue()[:W * H * 3]
        img = Image.frombuffer('RGB', (W, H), raw, 'raw', 'BGR', 0, 1)
    if '--half' in sys.argv:
        img = img.resize((W // 2, H // 2))
    img.save(out)
    print(out)

# usage: twl-shot.py OUT.png [--half] [--slots N]: the last frame, and with --slots the time-lapse's first N slots too
grab(0x70000000, sys.argv[1])
if '--slots' in sys.argv:
    n = int(sys.argv[sys.argv.index('--slots') + 1])
    for i in range(n):
        grab(0x70800000 + i * 0x800000, sys.argv[1].replace('.png', f'-{i:02d}.png'))
