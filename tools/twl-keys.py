#!/home/davi/clones/twilight-xnu/work/venv/bin/python3
"""Send D-pad keys to tvOS on the Mi Box over USB (the kernel's usbkeys=1 driver, 18d1:4ee0 "tvOS keys").

  twl-keys.py                 interactive: arrows, Enter = select, Esc/Backspace = menu, space = play/pause, h = home, q quits
  twl-keys.py up down select  send these and exit (names: up down left right select menu playpause home, rup ... rmenu)
"""
import os, struct, sys, termios, time, tty
import usb.core, usb.util

KEYS = {
    'up': 0x07 << 16 | 0x52, 'down': 0x07 << 16 | 0x51, 'left': 0x07 << 16 | 0x50, 'right': 0x07 << 16 | 0x4f,
    'select': 0x07 << 16 | 0x28, 'menu': 0x07 << 16 | 0x29, 'playpause': 0x0c << 16 | 0xcd, 'home': 0x0c << 16 | 0x223,
    'rup': 0x0c << 16 | 0x42, 'rdown': 0x0c << 16 | 0x43, 'rleft': 0x0c << 16 | 0x44, 'rright': 0x0c << 16 | 0x45,
    'rselect': 0x0c << 16 | 0x41, 'rmenu': 0x0c << 16 | 0x40,
}

def find():
    for d in usb.core.find(find_all=True, idVendor=0x18d1, idProduct=0x4ee0):
        try:
            if usb.util.get_string(d, d.iProduct) == 'tvOS keys':
                return d
        except Exception:
            continue
    return None

def press(dev, name, hold=0.08):
    code = KEYS[name]
    dev.write(0x01, struct.pack('<II', 4, code))
    time.sleep(hold)
    dev.write(0x01, struct.pack('<II', 5, code))

def main():
    dev = find()
    if dev is None:
        sys.exit('twl-keys: no "tvOS keys" device on USB (is tvOS running with usbkeys=1?)')
    try:
        dev.set_configuration()
    except usb.core.USBError:
        pass
    if len(sys.argv) > 1:
        for name in sys.argv[1:]:
            press(dev, name)
            time.sleep(0.4)
        return
    seq = {'\x1b[A': 'up', '\x1b[B': 'down', '\x1b[C': 'right', '\x1b[D': 'left', '\r': 'select', '\x7f': 'menu',
           ' ': 'playpause', 'h': 'home'}
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    print('arrows move, Enter selects, Backspace/Esc = menu, space = play/pause, h = home, q quits')
    try:
        tty.setraw(fd)
        while True:
            c = os.read(fd, 8).decode(errors='ignore')
            if c in ('q', '\x03'):
                break
            name = seq.get(c, 'menu' if c == '\x1b' else None)
            if name:
                press(dev, name)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

if __name__ == '__main__':
    main()
