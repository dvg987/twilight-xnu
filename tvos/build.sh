#!/bin/bash
# The RAM part of a tvOS boot on the box, and the payload around it:
#
#   out/tvos/boot.apfs    md0, the root the kernel starts from: stage0 as /sbin/launchd, empty /redwood /flash /dev
#   out/tvos/root.apfs    md1, HakiOS's init and our launchd jobs, mounted on /redwood by stage0; init mounts the tvOS
#                         volume (/dev/emmc_darwin, root_apfs=) on /flash, binds our jobs over its own and makes it the root
#   out/payload-tvos.bin  kernel + both, for tools/twl-run.sh
#
# env: JOBS="labels"  the tvOS jobs started at boot (default below); REST=1 the others on demand
#      ARGS="..."     more boot-args;  OSLOG="labels" jobs whose os_log goes to the console
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
H=hakios/userland
ROOTFS=work/tvos/rootfs
O=out/tvos
source hakios/xnu-env.sh
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
JOBS=${JOBS-"com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.logd com.apple.syslogd
	com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd com.apple.containermanagerd
	com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc
	com.apple.lsd com.apple.powerd com.apple.pluginkit.pkd com.apple.Pineboard"}

rm -rf $O/root $O/boot
mkdir -p $O/obj $O/root/{sbin,bin,etc,lib,share,LaunchDaemons,empty,xpc,hal,dev,private/var} \
	$O/boot/{sbin,redwood,flash,dev,Cryptexes/OS,Cryptexes/App}

# our static programs (no dyld, no libSystem: they run whatever the platform of the volume)
$CC -I$H/libmini -c $H/libmini/mini.c -o $O/obj/mini.o
$CC -c $H/libmini/start.S -o $O/obj/start.o
for prog in init child stage0; do
	$CC -I$H/libmini $LD -o $O/obj/$prog $H/src/$prog.c $O/obj/mini.o $O/obj/start.o
done
$CC -I$H/libmini $LD -o $O/obj/keys tvos/src/keys.c $O/obj/mini.o $O/obj/start.o

cp $O/obj/init $O/root/sbin/init
cp $O/obj/child $O/root/bin/child
cp $O/obj/keys $O/root/bin/keys
cp ${KEYS:-tvos/keys.list} $O/root/etc/keys.list
cp $O/obj/stage0 $O/boot/sbin/launchd
cp $H/rc.launchd $O/root/etc/rc
cp ${RC_JOBS:-tvos/rc.jobs} $O/root/etc/rc.jobs
cp work/tvos/var.skel $O/root/etc/var.skel
# files of ours for the fresh /private/var (tvos/var/): owned by mobile under mobile/, else by root
(cd tvos/var && find . -type f | sort) | while read -r f; do
	f=${f#./}; mkdir -p $O/root/etc/var/$(dirname $f); cp tvos/var/$f $O/root/etc/var/$f
	case $f in mobile/*) id=501;; *) id=0;; esac
	printf 'f\t600\t%d\t%d\t/private/var/%s\t/etc/var/%s\n' $id $id "$f" "$f" >> $O/root/etc/var.skel
done
# our libraries inside tvOS programs (the kernel inserts them: init hands etc/inject.conf to kern.exec_inject).
# Built against the iOS SDK, then marked tvOS (dyld refuses a library of another platform) and signed again.
DYN="xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1"
LDID=$THEOS/toolchain/linux/iphone/bin/ldid
: > $O/root/etc/inject.conf
for lib in ${TVLIBS-twlhook:backboardd}; do
	src=${lib%%:*}; progs=${lib#*:}
	$DYN -dynamiclib -install_name /redwood/lib/lib$src.dylib -framework IOSurface -framework CoreFoundation -framework IOKit -o $O/obj/lib$src.dylib tvos/src/$src.c
	python3 tools/macho-tvos.py $O/obj/lib$src.dylib
	$LDID -S $O/obj/lib$src.dylib
	cp $O/obj/lib$src.dylib $O/root/lib/
	echo "/redwood/lib/lib$src.dylib ${progs//,/ }" >> $O/root/etc/inject.conf
done
[ -n "$QUIET" ] && : > $O/root/etc/quiet
# init's own file test reads this pattern back through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > $O/root/share/pattern.bin
echo "tvOS on twilight: image built $(date '+%Y-%m-%d %H:%M')" > $O/root/etc/motd
# the job that runs etc/rc.jobs once launchd is up
cp $H/com.hakios.demo.plist $O/root/LaunchDaemons/
OSLOG=${OSLOG-com.apple.backboardd} python3 $H/mkjobs.py ${REST:+--rest} $ROOTFS $O/root/LaunchDaemons $JOBS
# xpc without launchd.plist: launchd would read that list of all of tvOS's jobs instead of LaunchDaemons
for f in $ROOTFS/System/Library/xpc/*; do
	case $(basename $f) in launchd.plist*) ;; *) cp $f $O/root/xpc/ ;; esac
done

python3 hakios/tools/apfs/mkapfs.py -z -o $O/root.apfs redwood=$O/root | tail -1
python3 hakios/tools/apfs/mkapfs.py -o $O/boot.apfs Boot=$O/boot | tail -1
python3 hakios/xnuloader/mkpayload.py -o out/payload-tvos.bin kernel=hakios/hakios-kernel \
	ramdisk=$O/boot.apfs ramdisk1=$O/root.apfs --no-version \
	--cmdline "cpus=3 emmc=1 root_apfs=emmc_darwin loader_return_after=${SECS:-180} epoch=$(date +%s) twl_hang=${HANG:-90} $ARGS"
