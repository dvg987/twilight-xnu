#!/bin/bash
# The package manager's prefix for tvOS on the box, /var/jb: Procursus' iOS rootless bootstrap with OpenSSH, every
# Mach-O in it marked tvOS and signed again ad hoc with its own entitlements.
#
# Procursus' tvOS repository (appletvos-arm64/1800) has no bash, coreutils or libiosexec1, so the iOS one is used,
# as hakios/tools/mk-jb.sh does for the phone: bootstraps/1800/bootstrap-iphoneos-arm64.tar.zst, and
# openssh-{client,server,sftp-server} from the iphoneos-arm64-rootless 1800 pool (uikittools, with uicache, is
# in the bootstrap already). tvOS's dyld refuses an iOS Mach-O, so each one (programs and libraries) gets platform
# tvOS (tools/macho-tvos.py) and a new ad hoc signature (-Cadhoc, as Procursus signs them; entitlements kept).
#
# Leaves (never in the repository: work/ is ignored)
#   work/twl-jb/debs/          what was fetched
#   work/twl-jb/root/var/jb/   the prefix as the box will have it, with .jb-version: tvos/build.sh JB=1 puts
#                              it into the image as /redwood/jb, and init copies it to /private/var/jb on the
#                              data volume once (when absent or of another version)
#
# env: EXTRA="pkg ..."  more packages of the rootless pool to put in (their dependencies must be there)
set -e
cd "$(dirname "$0")/.."
source hakios/xnu-env.sh
LDID=$THEOS/toolchain/linux/iphone/bin/ldid
W=$PWD/work/twl-jb
JB=$W/root/var/jb
MIRROR=https://apt.procurs.us
SUITE=1800
mkdir -p "$W/debs"

get() {                                                 # fetch into debs/, once
	[ -s "$W/debs/$2" ] || curl -sL --fail -o "$W/debs/$2" "$1"
}
get "$MIRROR/bootstraps/$SUITE/bootstrap-iphoneos-arm64.tar.zst" bootstrap.tar.zst
get "$MIRROR/dists/iphoneos-arm64-rootless/$SUITE/main/binary-iphoneos-arm64/Packages.xz" Packages.xz
[ -s "$W/debs/Packages" ] || xz -dc "$W/debs/Packages.xz" > "$W/debs/Packages"

rm -rf "$W/root"
mkdir -p "$W/root"
zstd -dc "$W/debs/bootstrap.tar.zst" | tar -C "$W/root" -xf -
[ -d "$JB" ] || { echo "twl-jb.sh: the bootstrap has no var/jb in it" >&2; exit 1; }
# libkrw0, libdimentio0 and shshd need a jailbreak's kernel plug-in that nothing provides here; with them in dpkg's
# database apt refuses to install anything (hakios/tools/mk-jb.sh)
for package in shshd libkrw0 libdimentio0; do
	list=$JB/Library/dpkg/info/$package.list
	if [ -f "$list" ]; then
		while IFS= read -r path; do
			[ -f "$JB/$path" ] && rm -f "$JB/$path"
		done < "$list"
		rm -f "$JB"/Library/dpkg/info/$package.*
	fi
	awk -v want="Package: $package" 'BEGIN { RS = ""; FS = "\n" }
		{ drop = 0; for (i = 1; i <= NF; i++) if ($i == want) drop = 1; if (!drop) printf "%s\n\n", $0 }' \
		"$JB/Library/dpkg/status" > "$JB/Library/dpkg/status.new"
	mv "$JB/Library/dpkg/status.new" "$JB/Library/dpkg/status"
done

MIRROR=$MIRROR python3 hakios/tools/jb-preinstall.py "$W/root" "$W/debs/Packages" "$W/debs" \
	openssh-client openssh-sftp-server openssh-server ${EXTRA:-}

# every dependency of what is in dpkg's database there too (firmware is made on the box: usr/libexec/firmware)
python3 - "$JB/Library/dpkg/status" <<'PY'
import re, sys
st = [s for s in open(sys.argv[1], encoding='utf-8').read().split('\n\n') if s.strip()]
f = lambda s, n: (re.search(r'(?m)^%s: (.*)$' % n, s) or [None, ''])[1]
have = set()
for s in st:
    have.add(f(s, 'Package'))
    have.update(p.split()[0] for p in f(s, 'Provides').split(',') if p.strip())
missing = set()
for s in st:
    for group in (f(s, 'Depends') + ',' + f(s, 'Pre-Depends')).split(','):
        names = [a.split()[0] for a in group.split('|') if a.strip()]
        if names and not any(n in have for n in names) and names[0] != 'firmware':
            missing.add('%s (for %s)' % (group.strip(), f(s, 'Package')))
print('twl-jb.sh: dependencies not there: ' + ', '.join(sorted(missing)) if missing else
      'twl-jb.sh: every dependency is there')
PY

rm -f "$JB/prep_bootstrap.sh"
# apt: the rootless suite, and the certificates straight from the bootstrap's file (no answer from trustd here)
mkdir -p "$JB/etc/apt/sources.list.d" "$JB/etc/apt/apt.conf.d"
cat > "$JB/etc/apt/sources.list.d/procursus.sources" <<EOF
Types: deb
URIs: $MIRROR
Suites: iphoneos-arm64-rootless/$SUITE
Components: main
EOF
echo 'Acquire::https::CaInfo "/var/jb/etc/ssl/certs/cacert.pem";' > "$JB/etc/apt/apt.conf.d/10twilight.conf"

# tvOS Mach-Os: platform 3 in every slice, then signed again, ad hoc (flags 0x2, as Procursus' are) with the
# entitlements each one had
python3 - "$JB" > "$W/machos.txt" <<'PY'
import os, sys
for top, dirs, files in os.walk(sys.argv[1]):
    for n in files:
        p = os.path.join(top, n)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as f:
            if f.read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf'):
                print(p)
PY
# (0xcafebabe is Java's class file magic too; there are none in the bootstrap, and macho-tvos.py would say so)
xargs -d '\n' -a "$W/machos.txt" python3 tools/macho-tvos.py
n=0
while IFS= read -r f; do
	if "$LDID" -e "$f" > "$W/ents.plist" 2>/dev/null && [ -s "$W/ents.plist" ]; then
		"$LDID" -S"$W/ents.plist" -Cadhoc "$f"
	else
		"$LDID" -S -Cadhoc "$f"
	fi
	n=$((n + 1))
done < "$W/machos.txt"
rm -f "$W/ents.plist"
# set-user-ID and set-group-ID bits back on: unpacking as an ordinary user drops them, and so does ldid's rewrite
# (sudo, su, login, ...)
zstd -dc "$W/debs/bootstrap.tar.zst" | tar -tvf - | awk '$1 ~ /^-/ && ($1 ~ /^-..[sS]/ || $1 ~ /^-.....[sS]/) {
	print substr($1, 4, 1) substr($1, 7, 1), $NF }' | while read -r bits path; do
	[ -f "$W/root/$path" ] || continue
	case $bits in [sS]?) chmod u+s "$W/root/$path" ;; esac
	case $bits in ?[sS]) chmod g+s "$W/root/$path" ;; esac
done

# what init compares with the copy on the box: other inputs, another value, another copy (the same inputs, the same)
stamp=$( (sha256sum "$W/debs/bootstrap.tar.zst" "$0" tools/macho-tvos.py; sed -n '/^Package: /p;/^Version: /p' \
	"$JB/Library/dpkg/status") | sha256sum | cut -c1-16)
echo "procursus-$SUITE-$stamp" > "$JB/.jb-version"
echo "twl-jb.sh: $JB ($(du -sh "$JB" | cut -f1), $(find "$JB" | wc -l) entries, $n Mach-Os made tvOS), version $(cat "$JB/.jb-version")"
