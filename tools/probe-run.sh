#!/bin/sh
# Run a payload image once: (rescue ->) fastboot -> payload -> wait for fastboot (or Android). Prints where it landed.
cd "$(dirname "$0")/.."
IMG=$1; WAIT=${2:-90}
if ping -c1 -W1 192.168.77.1 >/dev/null 2>&1; then
	timeout 5 python3 tools/rx.py 'reboot2 bootloader' >/dev/null 2>&1
fi
for i in $(seq 1 60); do fastboot devices | grep -q . && break; sleep 1; done
fastboot boot "$IMG" 2>&1 | tail -1
sleep 5
for i in $(seq 1 $WAIT); do
	if fastboot devices | grep -q .; then echo "landed: fastboot (${i}s)"; exit 0; fi
	if adb devices | grep -v '^emulator-' | grep -q 'device$'; then echo "landed: android (${i}s) reason=$(adb shell getprop ro.boot.bootreason)"; exit 2; fi
	sleep 1
done
echo "landed: nowhere after ${WAIT}s"; exit 3
