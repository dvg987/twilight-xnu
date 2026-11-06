#!/bin/bash
# Put the Mesa GL layer (tvos/gpu/build-mesa-gl.sh's work/glbuild/out) into the box's /var/mobile/gl over SSH, where the image's inject.conf
# (tvos/build.sh GLAPPS=...) expects it. libhakios.gl.dylib names /redwood/lib/libOSMesa.8.dylib; the copy here names /var/mobile/gl/libOSMesa.dylib
# (the same length) and is signed again. The apps that use it pick up the new libraries when they are next started.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=work/glbuild/out D=work/glbuild/install LDID=${THEOS:-$HOME/theos}/toolchain/linux/iphone/bin/ldid
mkdir -p "$D"
cp "$OUT/libOSMesa.8.dylib" "$D/libOSMesa.dylib"
python3 - "$OUT/libhakios.gl.dylib" "$D/libhakios.gl.dylib" <<'PY'
import sys
b = open(sys.argv[1], 'rb').read()
old, new = b'/redwood/lib/libOSMesa.8.dylib', b'/var/mobile/gl/libOSMesa.dylib'
assert len(old) == len(new) and b.count(old) == 1, 'libhakios.gl.dylib: the load path is not there once'
open(sys.argv[2], 'wb').write(b.replace(old, new))
PY
"$LDID" -S "$D/libhakios.gl.dylib"
tools/twl-ssh.sh 'mkdir -p /var/mobile/gl'
for f in libOSMesa.dylib libhakios.gl.dylib; do
	tools/twl-ssh.sh "cat > /var/mobile/gl/$f.new && mv /var/mobile/gl/$f.new /var/mobile/gl/$f" < "$D/$f"
done
tools/twl-ssh.sh 'ls -l /var/mobile/gl'
