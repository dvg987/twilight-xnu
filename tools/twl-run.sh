#!/bin/sh
# One run on the box: twl-run.sh PAYLOAD [WAIT]: boot twloader + PAYLOAD from fastboot, wait for the box to come back
# to fastboot, boot the rescue Linux and save the mailbox under logs/run-<time>/.
cd "$(dirname "$0")/.."
PAYLOAD=$1; WAIT=${2:-240}
python3 tools/mkbootimg.py --kernel twloader/twloader.Image --ramdisk "$PAYLOAD" --kernel-offset 0x2000000 \
	--cmdline twloader -o out/twl.img >/dev/null || exit 1
tools/probe-run.sh out/twl.img "$WAIT" || { echo "the box did not come back to fastboot"; exit 1; }
tools/rescue-up.sh >/dev/null || exit 1
D=logs/run-$(date +%m%d-%H%M%S)
python3 tools/twl-log.py --save "$D" > "$D.txt" 2>&1
python3 tools/twl-shot.py "$D.png" --half --slots ${SLOTS:-10} >/dev/null 2>&1
head -4 "$D.txt"; echo "saved: $D.txt $D.png"
