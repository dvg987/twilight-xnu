#!/usr/bin/env python3
"""Extract an OTA's payloadv2 (pbzx-compressed Apple Archive "YAA1") into a directory, without Apple's aa(1).

usage: ota-yaa.py OUTDIR payload.000 [payload.001 ...]
Writes files, directories and symbolic links under OUTDIR (as the current user) and appends one line per
entry to OUTDIR/.manifest: type<TAB>mode<TAB>uid<TAB>gid<TAB>flags<TAB>path<TAB>link-or-xattrs.
Extended attributes go to OUTDIR/.xattrs/<path>.<name> (decmpfs data and the like) so nothing is lost.
"""
import lzma, os, struct, sys

def pbzx_chunks(path):
    with open(path, 'rb') as f:
        assert f.read(4) == b'pbzx', path
        f.read(8)
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            usize, csize = struct.unpack('>QQ', h)
            data = f.read(csize)
            if csize == usize:
                yield data
            else:
                yield lzma.decompress(data)

class Stream:
    def __init__(self, gen):
        self.gen, self.buf, self.pos = gen, bytearray(), 0
    def read(self, n):
        while len(self.buf) - self.pos < n:
            try:
                chunk = next(self.gen)
            except StopIteration:
                break
            if self.pos:
                del self.buf[:self.pos]; self.pos = 0
            self.buf += chunk
        out = bytes(self.buf[self.pos:self.pos + n]); self.pos += len(out)
        return out
    def read_into(self, n, f):
        while n:
            take = min(n, 1 << 22)
            b = self.read(take)
            if not b:
                raise EOFError
            if f: f.write(b)
            n -= len(b)

def parse_header(h):
    fields, blobs, p = {}, [], 0
    while p < len(h):
        key, st = h[p:p + 3].decode(), chr(h[p + 3]); p += 4
        if st == '*':
            v = True
        elif st in '1248':
            n = int(st); v = int.from_bytes(h[p:p + n], 'little'); p += n
        elif st == 'P':
            n = struct.unpack_from('<H', h, p)[0]; v = h[p + 2:p + 2 + n].decode('utf-8', 'surrogateescape'); p += 2 + n
        elif st == 'T':
            v = struct.unpack_from('<qI', h, p); p += 12
        elif st == 'S':
            v = struct.unpack_from('<q', h, p)[0]; p += 8
        elif st in 'ABC':
            n = {'A': 2, 'B': 4, 'C': 8}[st]; v = int.from_bytes(h[p:p + n], 'little'); p += n; blobs.append((key, v))
        elif st == 'F':   # digest: SH1F 20, SH2F 32, SH3F 48, SH5F 64, CKSF 4, MD5F 16
            n = {'SH1': 20, 'SH2': 32, 'SH3': 48, 'SH5': 64, 'CKS': 4, 'MD5': 16}.get(key)
            if n is None: raise ValueError(f'unknown digest {key}')
            v = h[p:p + n].hex(); p += n
        else:
            raise ValueError(f'unknown field {key}{st} at {p} in {h[:80]!r}')
        fields[key] = v
    return fields, blobs

def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    man = open(os.path.join(out, '.manifest'), 'a')
    counts = {}
    for payload in sys.argv[2:]:
        s = Stream(pbzx_chunks(payload))
        while True:
            m = s.read(6)
            if len(m) < 6:
                break
            magic, hsz = m[:4], struct.unpack('<H', m[4:])[0]
            if magic not in (b'YAA1', b'AA01'):
                raise ValueError(f'{payload}: bad magic {magic!r}')
            fields, blobs = parse_header(s.read(hsz - 6))
            typ = chr(fields.get('TYP', ord('?')))
            path = fields.get('PAT', '')
            dst = os.path.join(out, path) if path else out
            counts[typ] = counts.get(typ, 0) + 1
            xattr_names = []
            for key, size in blobs:
                if key == 'DAT' and typ == 'F':
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    if os.path.islink(dst): os.unlink(dst)
                    with open(dst, 'wb') as f:
                        s.read_into(size, f)
                elif key == 'XAT':
                    x = s.read(size); q = 0
                    while q < len(x):
                        ln = struct.unpack_from('<I', x, q)[0]
                        ent = x[q + 4:q + ln]; nul = ent.index(b'\0')
                        name, val = ent[:nul].decode(), ent[nul + 1:]
                        xattr_names.append(name)
                        xp = os.path.join(out, '.xattrs', path + '.' + name)
                        os.makedirs(os.path.dirname(xp), exist_ok=True)
                        open(xp, 'wb').write(val)
                        q += ln
                else:
                    s.read_into(size, None)
            if typ == 'D':
                os.makedirs(dst, exist_ok=True)
            elif typ == 'L':
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst): os.unlink(dst)
                os.symlink(fields.get('LNK', ''), dst)
            elif typ == 'F' and not any(k == 'DAT' for k, _ in blobs):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                open(dst, 'wb').close()
            extra = fields.get('LNK', '') if typ == 'L' else ','.join(xattr_names)
            man.write(f"{typ}\t{fields.get('MOD', 0):o}\t{fields.get('UID', 0)}\t{fields.get('GID', 0)}\t{fields.get('FLG', 0):x}\t{path}\t{extra}\n")
        print(f'{os.path.basename(payload)}: {counts}', flush=True)
    man.close()

if __name__ == '__main__':
    main()
