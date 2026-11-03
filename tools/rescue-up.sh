#!/bin/sh
# Boot the rescue Linux from fastboot and wait until its shell server answers.
cd "$(dirname "$0")/.."
for i in $(seq 1 60); do fastboot devices | grep -q . && break; sleep 1; done
fastboot boot out/rescue.img >/dev/null 2>&1 || { echo "fastboot boot failed"; exit 1; }
for i in $(seq 1 60); do
	if ip link show enp0s20f0u4 >/dev/null 2>&1; then nmcli con up twrescue >/dev/null 2>&1 && break; fi
	sleep 1
done
for i in $(seq 1 30); do
	python3 tools/rsh.py true >/dev/null 2>&1 && break
	sleep 1
done
python3 tools/rsh.py '(while true; do nc -l -p 5002 -e sh; done) >/dev/null 2>&1 & echo rescue up'
sleep 1
