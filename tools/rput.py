#!/usr/bin/env python3
"""Copy a file to the rescue Linux: rput.py LOCAL REMOTE [mode]."""
import socket, sys, time, os
sys.path.insert(0, os.path.dirname(__file__))
from rx import rx
import io, threading
local, remote = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else '755'
data = open(local, 'rb').read()
t = threading.Thread(target=rx, args=(f'nc -l -p 5003 > {remote}; chmod {mode} {remote}; ls -la {remote}',))
t.start(); time.sleep(0.7)
s = socket.create_connection(('192.168.77.1', 5003)); s.sendall(data); s.close()
t.join()
