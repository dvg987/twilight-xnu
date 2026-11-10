# tvOS on the Mi Box S (3rd gen) from a USB stick

The stick holds everything: the kernel, the tvOS system volume and `/private/var`. The box's own U-Boot starts it,
the way CoreELEC is started. Nothing on the eMMC is written except U-Boot's environment (one variable, see below).
With the stick out, the box boots Android as before.

## What you need

- A Xiaomi Mi Box S 3rd gen (S905X5M, board `s7d_twilight`) with an **unlocked bootloader**.
- A USB stick of 8 GB or more. The box has one USB port; a hub is fine.
- Your own copy of the tvOS 16.5 OTA for AppleTV6,2 (20L563). No Apple software is in this repository.
- On the build machine: Linux, python3, `hfsprogs` (`mkfs.hfsplus`, `fsck.hfsplus`), Theos (`$THEOS`, for its
  toolchain and `ldid`), and what
  `hakios/` needs to build the kernel (`make doctor` there).

## Build

1. The kernel and the loader:

       cd hakios && make kernel
       cd .. && twloader/build.sh

2. The tvOS system volume, from the unpacked OTA (`AssetData/payloadv2/payload.000 ...`):

       python3 tools/ota-yaa.py work/tvos/rootfs AssetData/payloadv2/payload.0*
       mv work/tvos/rootfs/.manifest work/tvos/ota.manifest
       python3 tools/tvos-meta.py work/tvos/ota.manifest work/tvos
       python3 hakios/tools/apfs/mkapfs.py -z -o out/tvos-system.apfs \
           System=work/tvos/rootfs,manifest=work/tvos/system.manifest

3. The Wi-Fi/Bluetooth chip's firmware, from CoreELEC's `wifi_w2_fw_usb.bin`:

       python3 tools/w2-fw.py wifi_w2_fw_usb.bin work/w2fw/bin

4. The stick. Either an image file to write with `dd` or any imaging tool, or the stick itself:

       tools/twl-release.sh                    # out/twilight-usb.img, 8 GB
       SIZE=16G tools/twl-release.sh           # a bigger /private/var
       sudo tools/twl-release.sh /dev/sdX      # straight onto the stick (it must be removable and not mounted)

   Options, as environment variables:

   | | |
   |---|---|
   | `NETWORK=file` | join a Wi-Fi network at the start: its name on the first line, its passphrase on the second. Without it, choose the network in Settings. |
   | `SSH=1` | sshd for root with your public key (`SSH_KEY`, default `~/.ssh/id_ed25519.pub`) and the package manager's prefix `/var/jb`. Run `tools/twl-jb.sh` first. |
   | `ARGS="..."` | more boot arguments. |

## The stick

| partition | | |
|---|---|---|
| 1 | FAT, 256 MB | `aml_autoscript`, `cfgload`, `twl.img` (loader + kernel + the small root image) |
| 2 | APFS | the tvOS system volume, read-only |
| 3 | HFS+ (journaled, case-sensitive) | `/private/var`: settings, apps, their data |

To update, write a new `twl.img` to partition 1 (or build the stick again; that erases `/private/var`).

## First start

U-Boot has to be told once to look at the stick.

- A box that has already started CoreELEC from a stick is set up: put the stick in and start the box. This is the
  only case confirmed on hardware.
- Otherwise the stick's `aml_autoscript` has to run once: it sets U-Boot's `bootcmd` to "try a USB stick's
  `cfgload`, else boot as usual" and saves the environment. U-Boot runs it in update mode: with the stick in, hold
  the reset button (in the AV socket) while plugging in power, or `adb reboot update` from Android. This has not
  been confirmed; on the test box, update mode ended in Android's recovery.

From then on the box starts tvOS whenever the stick is in, and Android when it is not. CoreELEC's sticks use the
same names, so both keep working. The first start from a new stick shows a black screen for a few minutes.

To undo it, set `bootcmd` back to `run storeboot` (U-Boot console: `setenv bootcmd 'run storeboot'; saveenv`).
Leaving it is harmless: without a stick the extra step finds nothing and Android starts.

## Using it

- The TV's remote works over HDMI-CEC from the start, if the TV sends its keys. With no remote paired, a screen
  asks for the Mi remote: hold its Home and Xiaomi TV buttons together for 5 seconds, close to the box, and it is
  paired by itself. A key from the TV's remote takes the screen away for good; other Bluetooth remotes are paired
  in Settings. (The Mi remote keeps one pairing: back in Android it has to be paired again.)
- The first start then asks for the language. It can be changed later in Settings.
- Restart in Settings restarts into tvOS (through U-Boot). Pull the stick for Android.
- OpenGL ES, Metal and hardware video decoding need no per-app setup.
