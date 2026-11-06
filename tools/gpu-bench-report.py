#!/usr/bin/env python3
"""Compare logs/bench-*.txt: gpu-bench-report.py NAME [NAME...]. Render frames only (rp>0); the presenter's blits (bl1) are reported apart."""
import re
import statistics
import sys


def pct(v, p):
    return sorted(v)[min(len(v) - 1, int(len(v) * p))] / 1000 if v else 0


for name in sys.argv[1:]:
    text = open(f'logs/bench-{name}.txt', errors='replace').read()
    rows = re.findall(r'^t=([\d.]+)s rp(\d+) dr(\d+) kpx(\d+) cs\d+ bl(\d+) ctc=(\d+)us gpu=(\d+)us st=(-?\d+)us', text, re.M)
    render = [r for r in rows if int(r[1]) > 0 and int(r[6]) > 0]
    blits = [r for r in rows if int(r[4]) > 0]
    gpu = [int(r[6]) for r in render]
    ctc = [int(r[5]) for r in render]
    st = [int(r[7]) for r in render]
    heavy = [r for r in render if int(r[1]) >= 3]
    hg = [int(r[6]) for r in heavy]
    rate = [int(r[3]) / (int(r[6]) / 1000) for r in render]
    total = re.findall(r'G commit-to-complete n=(\d+) total=(\d+)ms max=(\d+)ms mean=(\d+)us', text)
    jetsam = len(re.findall(r'OS_REASON_JETSAM', text))
    crash = len(re.findall(r'twl: crash', text))
    print(f'== {name}: {len(render)} render frames, {len(blits)} blits, jetsam {jetsam}, crash {crash}')
    if not render:
        continue
    print(f'   gpu ms   median {pct(gpu, .5):6.1f}  p90 {pct(gpu, .9):6.1f}  max {max(gpu) / 1000:6.1f}  over 16.7: {sum(g > 16667 for g in gpu)}/{len(gpu)}')
    print(f'   ctc ms   median {pct(ctc, .5):6.1f}  p90 {pct(ctc, .9):6.1f}  max {max(ctc) / 1000:6.1f}')
    print(f'   st  ms   median {pct(st, .5):6.1f}  p90 {pct(st, .9):6.1f}')
    print(f'   heavy (rp>=3) {len(hg)} frames, gpu median {pct(hg, .5):.1f} p90 {pct(hg, .9):.1f}')
    print(f'   kpx per ms median {statistics.median(rate):.0f}')
    pace = re.findall(r'^pace n=60 gap (\d+) ms <20:(\d+) <36:(\d+) <53:(\d+) <70:(\d+) <105:(\d+) <250:(\d+) render (\d+) us wait (\d+) us copy (\d+) us', text, re.M)
    if pace:
        rows_ = [list(map(int, p)) for p in pace]
        n = len(rows_)
        print(f'   pacing {n * 60} moving frames: mean gap {sum(r[0] for r in rows_) / n:.1f} ms = {1000 * n / sum(r[0] for r in rows_):.0f} fps')
        print('   gap ms <20/<36/<53/<70/<105/<250:', [sum(r[i] for r in rows_) for i in range(1, 7)])
        print(f'   per frame us: render {sum(r[7] for r in rows_) // n}, gpu wait {sum(r[8] for r in rows_) // n}, copy {sum(r[9] for r in rows_) // n}')
        print('   slowest windows gap ms:', sorted((r[0] for r in rows_), reverse=True)[:6])
    shown = [tuple(map(int, x)) for x in re.findall(r'^shown (\d+) frames in (\d+) ms \(\d+ a second\), (\d+) us each', text, re.M)]
    busy = [x for x in shown if x[0] * 1000 / x[1] >= 8]
    if busy:
        print('   displayed, busy windows (fps/ms in driver):', [(round(x[0] * 1000 / x[1]), x[2] // 1000) for x in busy])
    frag = [tuple(map(float, x)) for x in re.findall(r'^frag (\d+) instrs, ([\d.]+) cycles', text, re.M)]
    if frag:
        print(f'   fragment shaders {len(frag)}: instrs max {max(f[0] for f in frag):.0f}, cycles max {max(f[1] for f in frag):.1f}, cycles sum {sum(f[1] for f in frag):.0f}')
    if total:
        n, t, m, mean = total[-1]
        print(f'   all commits n={n} mean ctc {int(mean) / 1000:.1f} ms max {m} ms')
