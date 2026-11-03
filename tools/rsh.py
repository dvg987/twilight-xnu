#!/usr/bin/env python3
"""Run a shell command on the rescue Linux over its telnetd (USB RNDIS) and print the output."""
import socket, sys, time, uuid

HOST = '192.168.77.1'

def strip_iac(buf, sock):
    out = bytearray(); i = 0
    while i < len(buf):
        c = buf[i]
        if c == 255 and i + 1 < len(buf):
            cmd = buf[i + 1]
            if cmd in (251, 252, 253, 254) and i + 2 < len(buf):
                opt = buf[i + 2]
                reply = 254 if cmd in (251, 252) else 252  # DONT / WONT
                if cmd in (251, 253):
                    sock.sendall(bytes([255, reply if cmd == 253 else 254, opt]))
                i += 3; continue
            i += 2; continue
        out.append(c); i += 1
    return bytes(out)

def run(cmd, timeout=60):
    s = socket.create_connection((HOST, 23), timeout=10)
    mark = uuid.uuid4().hex[:12]
    time.sleep(0.3)
    s.sendall(b'stty -echo 2>/dev/null; PS1=; export PS1\n')
    s.sendall(f'echo BEGIN{mark}; ( {cmd} ) 2>&1; echo; echo END{mark} $?\n'.encode())
    data = b''; t0 = time.time()
    s.settimeout(timeout)
    while f'END{mark}'.encode() not in data.split(f'BEGIN{mark}'.encode(), 1)[-1] or data.count(f'BEGIN{mark}'.encode()) < 1:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        data += strip_iac(chunk, s)
        if f'END{mark} '.encode() in data and data.rfind(f'END{mark} '.encode()) > data.rfind(f'BEGIN{mark}'.encode()) and b'\n' in data[data.rfind(f'END{mark} '.encode()):]:
            break
    s.close()
    txt = data.decode('utf-8', 'replace').replace('\r', '')
    b = txt.rfind(f'BEGIN{mark}\n')
    e = txt.rfind(f'END{mark} ')
    body = txt[b + len(mark) + 6:e] if b >= 0 and e > b else txt
    rc = txt[e:].split()[1] if e >= 0 else '?'
    return body.rstrip('\n'), rc

if __name__ == '__main__':
    body, rc = run(' '.join(sys.argv[1:]))
    print(body)
    sys.exit(0 if rc == '0' else 1)
