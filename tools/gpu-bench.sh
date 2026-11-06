#!/bin/bash
# One GPU benchmark: gpu-bench.sh NAME [EXTRA_BOOT_ARGS]. Boots the current build from fastboot, walks the same home screen,
# app open/close and app switching sequence, and writes logs/bench-NAME.txt (frames and the frontend's 10 s status rows).
# The box must be in fastboot, or in Android with adb.
cd "$(dirname "$0")/.." || exit 1
NAME=${1:?name}; EXTRA=$2
PY=work/venv/bin/python
k() { $PY tools/twl-keys.py "$@"; }

if ! fastboot devices | grep -q .; then
	adb reboot bootloader 2>/dev/null || timeout 5 python3 tools/rx.py 'reboot2 bootloader' >/dev/null 2>&1
	for i in $(seq 1 90); do fastboot devices | grep -q . && break; sleep 1; done
fi
fastboot devices | grep -q . || { echo "no fastboot device"; exit 1; }

KEYS=work/keys-none.list OSLOG="" REST=1 SECS=170 \
ARGS="${FB:-fbwidth=1920 fbheight=1080} usbkeys=1 cec=1 w2=6 w2bt=3 twl_monitor=0 hdmitx=1 mali=8 metal=1 mali-pool=256 msgbuf=4194304 $EXTRA" \
	tvos/build.sh 2>&1 | tail -1

tools/twl-run.sh out/payload-tvos.bin 300 > logs/last-run.out 2>&1 &
RUN=$!

for i in $(seq 1 60); do journalctl -k --since '-90s' --no-pager | ag -q 'Product: tvOS keys' && break; sleep 2; done
sleep 70
echo "keys start $(date +%T)"
k rright; sleep 1; k rselect; sleep 5; k rmenu; sleep 4
k rright; sleep 1; k rselect; sleep 5; k rmenu; sleep 4
k rleft; sleep 0.5; k rleft; sleep 1; k rselect; sleep 5
k home; sleep 0.25; k home; sleep 4
k rright; sleep 1.5; k rright; sleep 1.5; k rselect; sleep 4; k rmenu
echo "keys end $(date +%T)"

wait $RUN
tail -1 logs/last-run.out
L=$(ls -td logs/run-*/ | head -1)kernel.log
O=logs/bench-$NAME.txt
{
	echo "# $NAME $(date +%F\ %T) $L args: $EXTRA"
	echo "# crashes:"
	ag -a 'twl: crash|backboardd \[\d+\]\) <Notice>: exited' "$L" | head -5
	echo "# pacing:"
	ag -a 'twl: pace ' "$L" | sed 's/.*twl: pace/pace/'
	echo "# displayed:"
	ag -a 'MiniIOMobileFramebuffer: \d+ frames in' "$L" | sed 's/.*MiniIOMobileFramebuffer: /shown /' | cut -c1-90
	echo "# shaders:"
	ag -a 'MESA_SHADER_FRAGMENT shader' "$L" | sed 's/.*MESA_SHADER_FRAGMENT shader: /frag /' | cut -c1-130
	echo "# status rows:"
	ag -a 'mali: user: status: ' "$L" | cut -c1-220
	echo "# frames:"
	ag -a 'K\d+ t=' "$L" | ag -o 't=[\d.]+s rp\d+ dr\d+ kpx\d+ cs\d+ bl\d+ ctc=\d+us gpu=\d+us st=-?\d+us' | sort -u -t= -k2 -n
} > "$O"
python3 tools/gpu-bench-report.py "$NAME"
echo "saved $O"
