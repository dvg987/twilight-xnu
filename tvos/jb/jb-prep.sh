#!/var/jb/bin/sh
# Run once a boot by com.twilight.jbprep, with the system up: what the prefix on the data volume (/var/jb, which
# init copies there once) needs that init cannot do before launchd.
PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
export PATH
[ -d /var/jb/usr ] || exit 0

# dpkg's package "firmware", the system's version, which most packages depend on: Procursus' tool makes it, once
if ! grep -qx 'Package: firmware' /var/jb/Library/dpkg/status; then
	/var/jb/usr/libexec/firmware >/dev/null 2>&1
	grep -qx 'Package: firmware' /var/jb/Library/dpkg/status && echo "jb-prep: dpkg's firmware package made"
fi

# The prefix's applications on the home screen: registered with LaunchServices by uicache, by their own path
# (/var/jb/Applications/NAME.app), as a rootless jailbreak does at every start -- a registration made from outside
# /Applications may not outlive LaunchServices' rebuild of its database. Right after PineBoard starts
# LaunchServices is busy with the system's own applications: asked again every 5 s, two minutes at most, aside.
for app in /var/jb/Applications/*.app; do
	[ -d "$app" ] || continue
	(
		try=1
		while [ $try -le 24 ]; do
			said=$(uicache -p "$app" 2>&1)
			case "$said" in
			*rror*) sleep 5 ;;
			*) echo "jb-prep: ${app##*/} registered (try $try)"; exit 0 ;;
			esac
			try=$((try + 1))
		done
		echo "jb-prep: ${app##*/} not registered: $said"
	) &
done
wait
