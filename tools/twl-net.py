#!/usr/bin/env python3
"""Wi-Fi throughput to and from the box, with tvos/src/twlnet.c on it (in /var/tmp/twlnet).

Usage: twl-net.py down|up [SECONDS]
  down: this host sends, the box receives (what streaming video needs)
  up:   the box sends, this host receives
"""
import socket
import subprocess
import sys
import threading
import time

BOX = '192.168.3.105'
PORT = 5201


def ssh(cmd, **kw):
    return subprocess.Popen(['tools/twl-ssh.sh', cmd], **kw)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('down', 'up'):
        raise SystemExit(__doc__)
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10
    if sys.argv[1] == 'down':
        p = ssh(f'/var/tmp/twlnet sink {PORT}', stdout=subprocess.PIPE, text=True)
        time.sleep(1.5)
        s = socket.create_connection((BOX, PORT))
        buf = b'\0' * 65536
        end = time.time() + seconds
        while time.time() < end:
            s.sendall(buf)
        s.close()
        print(p.communicate(timeout=30)[0], end='')
    else:
        local = socket.gethostbyname(socket.gethostname())
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('0.0.0.0', PORT))
        srv.listen(1)
        route = subprocess.run(['ip', 'route', 'get', BOX], capture_output=True, text=True).stdout.split()
        local = route[route.index('src') + 1] if 'src' in route else local
        p = ssh(f'/var/tmp/twlnet source {local} {PORT} {seconds}', stdout=subprocess.PIPE, text=True)
        c, _ = srv.accept()
        total, start = 0, time.time()
        while True:
            d = c.recv(262144)
            if not d:
                break
            total += len(d)
        t = time.time() - start
        print(p.communicate(timeout=30)[0], end='')
        print(f'host received {total} bytes: {total * 8 / t / 1e6:.1f} Mbit/s')


if __name__ == '__main__':
    main()
