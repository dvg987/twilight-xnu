#!/usr/bin/env python3
"""Check that every symbol a Mach-O binds from a system library is exported by that library in the tvOS 16.5 shared cache.

usage: check-imports.py BINARY TBD_DIR
TBD_DIR holds `ipsw dyld tbd` output (one .tbd per library; a library's re-exported libraries are documents of its file).
Prints the missing (library, symbol) pairs; exit 1 when a non-weak one is missing.
"""
import os, re, subprocess, sys

binary, tbd_dir = sys.argv[1], sys.argv[2]
# llvm-nm -m names the library by its short name ("libSystem", "Foundation", "libc++").
SHORT = {'libSystem': 'libSystem.B.dylib', 'libc++': 'libc++.1.dylib', 'libobjc': 'libobjc.A.dylib', 'libc++abi': 'libc++abi.dylib'}

def exports(path):
    text = open(path).read()
    out = set()
    for key, body in re.findall(r'(symbols|weak-symbols|thread-local-symbols|objc-classes|objc-eh-types|objc-ivars):\s*\[([^\]]*)\]', text):
        names = [n.strip().strip("'") for n in body.split(',') if n.strip()]
        for n in names:
            if key == 'objc-classes':
                out.update({'_OBJC_CLASS_$_' + n, '_OBJC_METACLASS_$_' + n})
            elif key == 'objc-eh-types':
                out.add('_OBJC_EHTYPE_$_' + n)
            elif key == 'objc-ivars':
                out.add('_OBJC_IVAR_$_' + n)
            else:
                out.add(n)
    return out

cache = {}
def lib_exports(short):
    name = SHORT.get(short, short)
    if name not in cache:
        path = os.path.join(tbd_dir, name + '.tbd')
        if not os.path.exists(path):
            path = os.path.join(tbd_dir, name.replace('.dylib', '') + '.dylib.tbd')
        cache[name] = exports(path) if os.path.exists(path) else None
        if name == 'libc++.1.dylib' and cache[name] is not None:
            extra = os.path.join(tbd_dir, 'libc++abi.dylib.tbd')
            if os.path.exists(extra):
                cache[name] |= exports(extra)
    return cache[name]

nm = subprocess.run(['llvm-nm', '-m', '-u', binary], capture_output=True, text=True, check=True).stdout
missing, checked, bad = [], 0, 0
for line in nm.splitlines():
    m = re.match(r'\s*\(undefined\) (weak )?external (\S+) \(from (\S+)\)', line)
    if not m:
        continue
    weak, sym, lib = bool(m.group(1)), m.group(2), m.group(3)
    ex = lib_exports(lib)
    if ex is None:
        missing.append(('?', lib, sym, weak))
        continue
    checked += 1
    if sym not in ex:
        missing.append(('missing', lib, sym, weak))
        bad += 0 if weak else 1
for kind, lib, sym, weak in missing:
    print(f'{kind:8} {lib:16} {sym}{" (weak)" if weak else ""}')
print(f'{checked} imports checked against the tvOS 16.5 cache, {bad} missing (non-weak)')
sys.exit(1 if bad or any(k == '?' for k, *_ in missing) else 0)
