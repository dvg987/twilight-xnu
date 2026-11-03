#!/usr/bin/env python3
"""Run a command on the rescue Linux (nc -e sh server on :5002) and stream its stdout/stderr; binary-safe."""
import socket, sys

def rx(cmd, host='192.168.77.1', port=5002, out=None):
    s = socket.create_connection((host, port), timeout=30)
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
