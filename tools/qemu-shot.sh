#!/bin/sh
# A picture of QEMU's screen (ramfb) through its monitor: qemu-shot.sh OUT.png
cd "$(dirname "$0")/.."
P=$PWD/out/qemu/shot.ppm
rm -f "$P"
printf 'screendump %s\n' "$P" | nc -q 2 127.0.0.1 4444 >/dev/null 2>&1
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$P" ] && break; sleep 1; done
python3 -c "from PIL import Image; Image.open('$P').resize((960,540)).save('$1')" && echo "$1"
