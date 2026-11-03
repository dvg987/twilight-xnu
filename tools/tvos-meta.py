#!/usr/bin/env python3
"""From the OTA's entry list (tools/ota-yaa.py's manifest) make what HakiOS's image tools read:

  system.manifest   mode (octal, with the type bits) uid gid path [link]   -- tools/apfs/mkapfs.py
  var.skel          the directories and links of /private/var             -- userland/src/init.c restore_var

usage: tvos-meta.py OTA_MANIFEST OUTDIR
"""
import os, stat, sys

TYPES = {'D': stat.S_IFDIR, 'F': stat.S_IFREG, 'L': stat.S_IFLNK}
SKIP_VAR = ('private/var/staged_system_apps', 'private/var/MobileAsset/')
EXTRA = [   # what a running Darwin makes and its daemons expect (sysimage/mkvarskel.py)
    ('/private/var/db/uuidtext', 0o755, 272, 272),
    ('/private/var/db/diagnostics/timesync', 0o755, 272, 272),
    ('/private/var/containers', 0o755, 0, 0),
    ('/private/var/containers/Data', 0o755, 0, 0),
    ('/private/var/containers/Shared', 0o755, 0, 0),
    ('/private/var/containers/Bundle', 0o755, 0, 0),
    ('/private/var/mobile/Containers', 0o755, 501, 501),
    ('/private/var/mobile/Containers/Data', 0o755, 501, 501),
    ('/private/var/mobile/Containers/Shared', 0o755, 501, 501),
    ('/private/var/mobile/Library/Caches', 0o755, 501, 501),
    ('/private/var/mobile/Library/Preferences', 0o755, 501, 501),
    ('/private/var/mobile/Library/Logs', 0o755, 501, 501),
    ('/private/var/root/Library/Caches', 0o700, 0, 0),
    ('/private/var/root/Library/Preferences', 0o700, 0, 0),
    ('/private/var/root/Library/Logs', 0o700, 0, 0),
]

def main():
    src, out = sys.argv[1], sys.argv[2]
    entries = {}
    for line in open(src, encoding='utf-8', errors='surrogateescape'):
        f = line.rstrip('\n').split('\t')
        if len(f) < 6 or not f[5]:
            continue
        typ, mode, uid, gid, _, path = f[:6]
        extra = f[6] if len(f) > 6 else ''
        if typ not in TYPES:
            continue
        entries[path] = (TYPES[typ] | (int(mode, 8) & 0o7777), int(uid), int(gid), extra if typ == 'L' else '')
    with open(os.path.join(out, 'system.manifest'), 'w', encoding='utf-8', errors='surrogateescape') as m:
        for path in sorted(entries):
            mode, uid, gid, link = entries[path]
            m.write(f'{mode:o}\t{uid}\t{gid}\t{path}' + (f'\t{link}' if link else '') + '\n')
    dirs, links = {}, []
    for path, (mode, uid, gid, link) in entries.items():
        if not path.startswith('private/var') or path.startswith(SKIP_VAR):
            continue
        if stat.S_ISDIR(mode):
            dirs['/' + path] = (mode & 0o7777, uid, gid)
        elif stat.S_ISLNK(mode):
            links.append((link, '/' + path))
    for path, mode, uid, gid in EXTRA:
        dirs.setdefault(path, (mode, uid, gid))
    dirs.pop('/private/var', None)
    with open(os.path.join(out, 'var.skel'), 'w') as v:
        for path in sorted(dirs):
            mode, uid, gid = dirs[path]
            v.write(f'd\t{mode:o}\t{uid}\t{gid}\t{path}\n')
        for target, path in links:
            v.write(f'l\t{target}\t{path}\n')
        v.write('l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo\n')
    print(f'{len(entries)} entries, {len(dirs)} directories of /private/var')

if __name__ == '__main__':
    main()
