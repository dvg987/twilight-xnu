#!/usr/bin/env python3
"""Run a command on the rescue Linux (nc -e sh server on :5002) and stream its stdout/stderr; binary-safe."""
import socket, sys

def rx(cmd, host='192.168.77.1', port=5002, out=None):
    import time
    for attempt in range(50):        # the nc -e loop on the box needs a moment between connections
        try:
            s = socket.create_connection((host, port), timeout=30)
            break
        except ConnectionRefusedError:
            if attempt == 49:
                raise
            time.sleep(0.2)
    s.sendall(('exec 2>&1; ' + cmd + '\n').encode())
    s.shutdown(socket.SHUT_WR)
    s.settimeout(600)
    w = out or sys.stdout.buffer
    while True:
        b = s.recv(1 << 20)
        if not b: break
        w.write(b)
    w.flush()

if __name__ == '__main__':
    rx(' '.join(sys.argv[1:]))
