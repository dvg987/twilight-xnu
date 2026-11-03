#!/usr/bin/env python3
"""Read twloader's mailbox (0x6f000000) through the rescue Linux and print the run's state and logs.

usage: twl-log.py [--save DIR] [--kernel-only]
"""
import io, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rx import rx

MBOX = 0x6f000000
STATES = ['none', 'loader', 'kernel running', 'kernel returned', 'HUNG (monitor)', 'loader failed', 'EL2 trap']
FIELDS = ['magic', 'boots', 'state', 'reason'] + [f'r{i}' for i in range(8)] + ['llog_len', 'klog_len', 'phys_base',
          'mem_size', 'slide', 'kernel_live_pa', 'heartbeat', 'monitor_seconds', 'hang_seconds', 't_loader', 't_kernel', 'shots', 'shot_every', 'shot_from'] + [f'shot{i}' for i in range(32)]

def read(addr, size):
    buf = io.BytesIO()
    rx(f'dd if=/dev/mem bs=4096 skip={addr // 4096} count={(size + 4095) // 4096} 2>/dev/null', out=buf)
    return buf.getvalue()[:size]

def main():
    save = sys.argv[sys.argv.index('--save') + 1] if '--save' in sys.argv else None
    hdr = read(MBOX, 0x1000)
    v = dict(zip(FIELDS, struct.unpack_from(f'<{len(FIELDS)}Q', hdr)))
    if v['magic'] != 0x31584f424d4c5754:
        print(f"no mailbox at {MBOX:#x} (magic {v['magic']:#x})"); return 1
    st = STATES[v['state']] if v['state'] < len(STATES) else v['state']
    print(f"run {v['boots']}: {st}; reason {v['reason']:#x} regs {' '.join(hex(v[f'r{i}']) for i in range(6))}")
    print(f"time-lapse: {v['shots']} frames, every {v['shot_every']} s; the monitor ran {v['monitor_seconds']} s")
    print(f"window {v['phys_base']:#x}+{v['mem_size']:#x}, heartbeat {v['heartbeat']}, "
          f"kernel ran {(v['t_kernel'] and 1) and ''}loader log {v['llog_len']} B, kernel log {v['klog_len']} B")
    llog = read(MBOX + 0x1000, min(v['llog_len'], 0xff000)) if v['llog_len'] else b''
    klog = read(MBOX + 0x100000, min(v['klog_len'], 0xf00000)) if v['klog_len'] else b''
    if save:
        os.makedirs(save, exist_ok=True)
        open(os.path.join(save, 'loader.log'), 'wb').write(llog)
        open(os.path.join(save, 'kernel.log'), 'wb').write(klog)
    if '--kernel-only' not in sys.argv:
        print('== LOADER =='); sys.stdout.write(llog.decode('utf-8', 'replace'))
    print('== KERNEL =='); sys.stdout.write(klog.decode('utf-8', 'replace'))
    return 0

if __name__ == '__main__':
    sys.exit(main())
