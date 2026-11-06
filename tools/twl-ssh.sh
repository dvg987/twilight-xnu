#!/bin/sh
# ssh to the box as root over Wi-Fi: twl-ssh.sh [COMMAND...]  (TWL_HOST, default the address it last had)
cd "$(dirname "$0")/.."
exec ssh -o UserKnownHostsFile="$PWD/work/ssh/known_hosts" -o ConnectTimeout=10 root@"${TWL_HOST:-192.168.3.105}" "$@"
