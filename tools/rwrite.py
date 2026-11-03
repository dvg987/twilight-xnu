#!/usr/bin/env python3
"""Stream a local file into a block device of the rescue Linux: rwrite.py FILE DEVICE (bs=1M, fsync at the end)."""
import os, socket, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rx import rx
src, dev = sys.argv[1], sys.argv[2]
size = os.path.getsize(src)
t = threading.Thread(target=rx, args=(f'nc -l -p 5004 | dd of={dev} bs=1M conv=fsync 2>&1 | tail -2',))
t.start(); time.sleep(1)
s = socket.create_connection(('192.168.77.1', 5004))
t0 = time.time(); sent = 0
with open(src, 'rb') as f:
    while True:
        b = f.read(1 << 20)
        if not b: break
        s.sendall(b); sent += len(b)
        if sent % (256 << 20) == 0:
            print(f'  {sent >> 20} of {size >> 20} MB, {sent / (time.time() - t0) / 1e6:.1f} MB/s', flush=True)
s.shutdown(socket.SHUT_WR)
t.join()
s.close()
print(f'{sent} bytes in {time.time() - t0:.0f} s')
