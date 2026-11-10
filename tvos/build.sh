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
#      VDEC=1         the hardware H.264 decoder for VideoToolbox (and its boot-args optee=2 vdec=7)
#      VAR=1          /private/var kept on the eMMC: emmc=2 var_hfs=emmc_darwin_data (the data volume, darwin from
#                     4 GiB on: tools/twl-datavol.py, written once from the rescue Linux by tools/twl-datavol-write.py)
#      USB=1          tvOS and /private/var on a USB stick instead (tools/twl-usb.py makes it): usbdisk=1
#                     root_apfs=usb_darwin var_hfs=usb_darwin_data; the eMMC is not touched. USB=2: QEMU's xHCI
#      JB=1           the package manager's prefix (tools/twl-jb.sh: work/twl-jb) in the image as /redwood/jb, for
#                     init to copy to /var/jb on the data volume when it has none or another version (needs VAR=1 or USB;
#                     ~43 MB more image, so leave it out once the prefix is there)
#      NET=1          the network without SSH: configd and mDNSResponder for DHCP and names, en0 brought up
#      SSH=1          sshd of the prefix as a launchd job (root, the key in work/ssh/authorized_keys, by default
#                     ~/.ssh/id_ed25519.pub; host key work/ssh/ssh_host_ed25519_key, made once), configd and
#                     mDNSResponder for DHCP and names, en0's address on the console (net_test), /bin/sh and friends
#                     from the prefix (a union over the tvOS volume's /bin), uicache for the prefix's applications
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
H=hakios/userland
ROOTFS=work/tvos/rootfs
O=${OUT:-out/tvos}                     # OUT, PAYLOAD: build somewhere else than out/tvos, out/payload-tvos.bin
source hakios/xnu-env.sh
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
JOBS=${JOBS-"com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.logd com.apple.syslogd
	com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd com.apple.containermanagerd
	com.apple.runningboardd com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc
	com.apple.lsd com.apple.powerd com.apple.pluginkit.pkd com.apple.Pineboard com.apple.wifid"}
[ -n "$SSH" ] && NET=1
[ -n "$NET" ] && JOBS="$JOBS com.apple.configd com.apple.mDNSResponder"   # DHCP on en0 (IPConfiguration), DNS
DISK="emmc=1 root_apfs=emmc_darwin"
[ -n "$VAR" ] && DISK="emmc=2 var_hfs=emmc_darwin_data root_apfs=emmc_darwin"
[ -n "$USB" ] && DISK="usbdisk=$USB root_apfs=usb_darwin var_hfs=usb_darwin_data"
[ -n "$JB" ] && [ ! -f work/twl-jb/root/var/jb/.jb-version ] && { echo "build.sh: JB=1: run tools/twl-jb.sh first" >&2; exit 1; }

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
	d=
	for part in $(dirname "$f" | tr / ' '); do              # its directories first, where the skeleton has none yet
		[ "$part" = . ] && continue
		d=${d:+$d/}$part
		awk -F'\t' -v p="/private/var/$d" '$5 == p { found = 1 } END { exit !found }' $O/root/etc/var.skel ||
			printf 'd\t755\t%d\t%d\t/private/var/%s\n' $id $id "$d" >> $O/root/etc/var.skel
	done
	printf 'f\t600\t%d\t%d\t/private/var/%s\t/etc/var/%s\n' $id $id "$f" "$f" >> $O/root/etc/var.skel
done
# SSH=1: root's key and the host key in /private/var/root/.ssh (from work/ssh, never in the repository; on a kept
# data volume only when not there yet), the jobs, and the prefix's shells as /bin/sh, /bin/bash, /bin/zsh -- the
# boot volume's jb-bin, which init puts over the tvOS volume's /bin as a union (tvOS has no /bin/sh)
if [ -n "$SSH" ]; then
	mkdir -p work/ssh $O/root/etc/var/root/.ssh $O/boot/jb-bin
	[ -f work/ssh/authorized_keys ] || cp "${SSH_KEY:-$HOME/.ssh/id_ed25519.pub}" work/ssh/authorized_keys
	[ -f work/ssh/ssh_host_ed25519_key ] || ssh-keygen -q -t ed25519 -N '' -C twilight -f work/ssh/ssh_host_ed25519_key
	printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> $O/root/etc/var.skel
	for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
		cp work/ssh/$k $O/root/etc/var/root/.ssh/$k
		printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/etc/var/root/.ssh/%s\n' $k $k >> $O/root/etc/var.skel
	done
	for c in sh bash zsh; do
		ln -s /var/jb/bin/$c $O/boot/jb-bin/$c
	done
	cp tvos/jb/com.twilight.sshd.plist tvos/jb/com.twilight.jbprep.plist $O/root/LaunchDaemons/
	cp tvos/jb/jb-prep.sh $O/root/etc/jb-prep.sh
fi
[ -n "$NET" ] && cp tvos/jb/com.twilight.net.plist $O/root/LaunchDaemons/
# JB=1: the prefix, for init to copy onto the data volume (hakios/userland/src/init.c, kept_prefix)
if [ -n "$JB" ]; then
	cp -a work/twl-jb/root/var/jb $O/root/jb
	echo "build.sh: the prefix $(cat $O/root/jb/.jb-version) in the image, $(du -sh $O/root/jb | cut -f1) before compression"
fi
# the W2's Wi-Fi firmware for the kernel (fw/ of the small image; tools/w2-fw.py makes it from CoreELEC's file)
if [ -d work/w2fw/bin ]; then
	mkdir -p $O/root/fw && cp work/w2fw/bin/w2_*.bin $O/root/fw/
fi
# the Mali-G310's CSF firmware (mali=2 and up): CoreELEC's arm/mali/arch10.8/mali_csffw.bin, the v10 image Amlogic runs on the S7D
if [ -f work/malifw/mali_csffw.bin ]; then
	mkdir -p $O/root/fw && cp work/malifw/mali_csffw.bin $O/root/fw/
fi
# the video decoders' microcode and the trusted application that loads it (optee=2): CoreELEC's, for the S905X5M
VFW=work/src/ce-media-fw/usr/lib
if [ -f $VFW/kernel-overlays/base/lib/firmware/video/S905X5M/video_ucode.bin ]; then
	mkdir -p $O/root/fw && cp $VFW/kernel-overlays/base/lib/firmware/video/S905X5M/video_ucode.bin \
		$VFW/ta/S905X5M/526fc4fc-7ee6-4a12-96e3-83da9565bce8.ta $O/root/fw/
fi
# the GPU's Metal plug-in (work/metalbuild: MaliMetal's, with PanVK inside), in /System/Library/Extensions through
# the image's ext/ (init: a union mount); Metal loads it when the kernel's accelerator names it (metal=1)
if [ -d work/metalbuild/out/NativeMaliMetal.bundle ]; then
	mkdir -p $O/root/ext && cp -R work/metalbuild/out/NativeMaliMetal.bundle $O/root/ext/
fi
# the network to join at the start: its name, then its passphrase, one a line (never in the repository)
NETWORK=${NETWORK-work/w2fw/network}       # NETWORK=FILE another one, NETWORK= none: joined from Settings
[ -n "$NETWORK" ] && [ -f "$NETWORK" ] && mkdir -p $O/root/fw && cp "$NETWORK" $O/root/fw/network
# our libraries inside tvOS programs (the kernel inserts them: init hands etc/inject.conf to kern.exec_inject).
# Built against the iOS SDK, then marked tvOS (dyld refuses a library of another platform) and signed again.
DYN="xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1"
LDID=$THEOS/toolchain/linux/iphone/bin/ldid
: > $O/root/etc/inject.conf
for lib in ${TVLIBS-twlhook:backboardd twlpb:PineBoard twlremote:PineBoard,HeadBoard twlbt:TVSettings twlcloud:@apps}; do
	src=${lib%%:*}; progs=${lib#*:}
	if [ -f tvos/src/$src.m ]; then
		$DYN -fobjc-arc -dynamiclib -install_name /redwood/lib/lib$src.dylib -framework UIKit -framework CoreBluetooth -framework Foundation -framework QuartzCore -o $O/obj/lib$src.dylib tvos/src/$src.m
	else
		$DYN -dynamiclib -install_name /redwood/lib/lib$src.dylib -framework IOSurface -framework CoreFoundation -framework IOKit -lobjc -o $O/obj/lib$src.dylib tvos/src/$src.c
	fi
	python3 tools/macho-tvos.py $O/obj/lib$src.dylib
	$LDID -S $O/obj/lib$src.dylib
	cp $O/obj/lib$src.dylib $O/root/lib/
	echo "/redwood/lib/lib$src.dylib ${progs//,/ }" >> $O/root/etc/inject.conf
done
# OpenGL ES by Mesa on the GPU (tvos/gpu/build-mesa-gl.sh) in every application the user installed ("@apps": the
# kernel's word for a program in a .app under /var). The layer takes over EAGLContext and leaves a program without
# that class alone. GL=0 leaves it out (15 MB less image).
if [ "${GL-1}" != 0 ] && [ -f work/glbuild/out/libhakios.gl.dylib ]; then
	cp work/glbuild/out/libhakios.gl.dylib work/glbuild/out/libOSMesa.8.dylib $O/root/lib/
	echo "/redwood/lib/libhakios.gl.dylib @apps" >> $O/root/etc/inject.conf
fi
# programs of ours that are tvOS programs (dyld, libSystem): built the same way
for prog in bluetool metaltest; do
	$DYN -o $O/obj/$prog tvos/src/$prog.c
	python3 tools/macho-tvos.py $O/obj/$prog
	$LDID -S -Cadhoc $O/obj/$prog          # (ad hoc: what the kernel's AMFI stand-in takes for a platform binary)
	cp $O/obj/$prog $O/root/bin/
done
# NET=1: the phone's net_test, which brings en0 up and says its address (com.twilight.net)
if [ -n "$NET" ]; then
	$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o $O/obj/net_test $H/src/net_test.m
	python3 tools/macho-tvos.py $O/obj/net_test
	$LDID -S -Cadhoc $O/obj/net_test
	cp $O/obj/net_test $O/root/bin/
fi
# the HDMI audio output (TVAUDIO=1; it offers AC-3 and MAT as well as LPCM, NOPASSTHROUGH=1 leaves those out): an AudioServerPlugIn for the audio server's HAL, in the image's
# /hal, which init binds over Apple's plug-ins. It writes to /dev/twl-audio, which the kernel has with hdmitx=1 hdmiaudio=3.
if [ -n "$TVAUDIO" ]; then
	mkdir -p $O/root/lib/TwlAudio.driver
	$DYN ${NOPASSTHROUGH:+-DTWL_NO_PASSTHROUGH} -Ihakios/xnu/files/bsd/sys -bundle -framework CoreFoundation -o $O/obj/TwlAudio tvos/src/twlaudio.c
	python3 tools/macho-tvos.py $O/obj/TwlAudio
	$LDID -S $O/obj/TwlAudio
	cp $O/obj/TwlAudio $O/root/lib/TwlAudio.driver/
	cp tvos/src/twlaudio.plist $O/root/lib/TwlAudio.driver/Info.plist
	ln -s /redwood/lib/TwlAudio.driver $O/root/hal/TwlAudio.driver
fi
# the hardware H.264 decoder (VDEC=1): tvos/src/twlvdec.c as /System/Library/VideoDecoders/AVD.videodecoder (the image's
# vdecoders/, which init puts over the volume's directory), the name VideoToolbox loads in every process and calls
# AVDRegister in. It drives /dev/twl-vdec-probe, which the kernel has with optee=2 vdec=7; one decoder at a time, the
# others fall back to software. With it comes tvos/src/twlcvgl.c, in the applications that get Mesa's OpenGL ES:
# CoreVideo's own GL texture import needs Apple's GPU.
if [ -n "${VDEC-}" ]; then
	mkdir -p $O/root/vdecoders
	$DYN -Wall -Wno-pointer-sign -Ihakios/xnu/files/bsd -dynamiclib \
		-install_name /System/Library/VideoDecoders/AVD.videodecoder -framework CoreFoundation \
		-framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework IOSurface -o $O/obj/AVD.videodecoder \
		tvos/src/twlvdec.c tvos/video/h264/dpb.c tvos/video/h264/h264_dpb.c tvos/video/hevc/hevc_dpb.c tvos/video/vp9/vp9_dpb.c
	$DYN -Wno-deprecated-declarations -dynamiclib -install_name /redwood/lib/libtwlcvgl.dylib \
		-Ihakios/xnu/files/bsd -framework CoreFoundation -framework CoreVideo -framework IOSurface -framework OpenGLES -lobjc -o $O/obj/libtwlcvgl.dylib tvos/src/twlcvgl.c
	for f in AVD.videodecoder libtwlcvgl.dylib; do
		python3 tools/macho-tvos.py $O/obj/$f
		$LDID -S -Cadhoc $O/obj/$f
	done
	cp $O/obj/AVD.videodecoder $O/root/vdecoders/
	cp $O/obj/libtwlcvgl.dylib $O/root/lib/
	echo "/redwood/lib/libtwlcvgl.dylib @apps" >> $O/root/etc/inject.conf
	ARGS="$ARGS optee=2 optee_delay=10 vdec=7 vdec_delay=25"
fi
# bluetoothd, one instruction changed: every pairing agent registers as one that answers pairing requests itself
# (kCBMsgArgProgrammaticPairing), so a "Just Works" request goes to Settings' agent, which tvos/src/twlbt.c answers at
# once, and not to an alert: the Xiaomi remote gives up on a pairing 3.4 s after asking for it. Signed again with its
# own entitlements.
python3 tools/patch-bluetoothd.py $ROOTFS/usr/sbin/bluetoothd $O/obj/bluetoothd
$LDID -e $ROOTFS/usr/sbin/bluetoothd > $O/obj/bluetoothd.ents
$LDID -S$O/obj/bluetoothd.ents -Cadhoc $O/obj/bluetoothd
install -m 755 $O/obj/bluetoothd $O/root/bin/
[ -n "$QUIET" ] && : > $O/root/etc/quiet
# init's own file test reads this pattern back through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > $O/root/share/pattern.bin
echo "tvOS on twilight: image built $(date '+%Y-%m-%d %H:%M')" > $O/root/etc/motd
# the job that runs etc/rc.jobs once launchd is up
cp $H/com.hakios.demo.plist $O/root/LaunchDaemons/
ALLOW="bluetooth BTServer ${ALLOW:-}" OSLOG=${OSLOG-com.apple.backboardd} python3 $H/mkjobs.py ${REST:+--rest} $ROOTFS $O/root/LaunchDaemons $JOBS
# fairplayd.T2 cannot run on this box: launchd starts it on demand, it exits at once without answering, and every
# FairPlay request (AppleMediaServices signs the store's) then waits for ever -- itunesstored, accountsd and
# amsaccountsd hung, and with them Settings > Users and Accounts and Settings > Apps. Without the job the lookup fails
# and they go on. (FAIRPLAY=1 keeps it.)
[ -n "${FAIRPLAY:-}" ] || rm -f $O/root/LaunchDaemons/com.apple.fairplayd.T2.plist
# Darwin's Bluetooth on the W2 (w2bt=3): BlueTool is our stand-in (the kernel boots the controller), and bluetoothd
# takes the UART's plain kernel control (com.apple.uart.bluetooth), not the Skywalk one it would by default
python3 - $O/root/LaunchDaemons <<'PY'
import os, plistlib, sys
d = sys.argv[1]
def edit(label, change):
    path = os.path.join(d, label + '.plist')
    if os.path.exists(path):
        job = plistlib.load(open(path, 'rb'))
        change(job)
        plistlib.dump(job, open(path, 'wb'), fmt=plistlib.FMT_BINARY)
def bluetool(job):
    job.pop('Program', None)
    job['ProgramArguments'] = ['/redwood/bin/bluetool']
    job.setdefault('MachServices', {})['com.apple.BlueTool'] = True
def bluetoothd(job):
    job.pop('Program', None)
    job['ProgramArguments'] = ['/redwood/bin/bluetoothd']
    job.setdefault('EnvironmentVariables', {})['BT_HCI_TRANSPORT'] = 'H4BC:POSIX:com.apple.uart.bluetooth,3000000'
edit('com.apple.BlueTool', bluetool)
edit('com.apple.bluetoothd', bluetoothd)
PY
# xpc without launchd.plist: launchd would read that list of all of tvOS's jobs instead of LaunchDaemons
for f in $ROOTFS/System/Library/xpc/*; do
	case $(basename $f) in launchd.plist*) ;; *) cp $f $O/root/xpc/ ;; esac
done

python3 hakios/tools/apfs/mkapfs.py -z -o $O/root.apfs redwood=$O/root | tail -1
python3 hakios/tools/apfs/mkapfs.py -o $O/boot.apfs Boot=$O/boot | tail -1
python3 hakios/xnuloader/mkpayload.py -o ${PAYLOAD:-out/payload-tvos.bin} kernel=hakios/hakios-kernel \
	ramdisk=$O/boot.apfs ramdisk1=$O/root.apfs --no-version \
	--cmdline "cpus=${CPUS:-3} $DISK loader_return_after=${SECS:-180} epoch=$(date +%s) twl_hang=${HANG:-90} $ARGS"
