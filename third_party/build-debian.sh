#!/bin/bash
# Build the Debian root for the mainline kernel: an ext4 image that the initramfs
# (src/mainline/initramfs/init) loop-mounts from <stick>/netcast/mainline/debian.img and
# switch_roots into.
#
#   third_party/build-debian.sh            # -> third_party/mainline/out/debian.img (+ .gz)
#
# Debian trixie armhf (hard-float; the kernel doesn't care that LG's userspace is
# soft-float), systemd + networkd DHCP on the USB Ethernet adapter + OpenSSH (key only).
# USB Wi-Fi: wpa_supplicant per wlan interface (netcast-wifi@.service, started by udev) with
# the network from wifi.conf at the root of the stick, then networkd DHCP.
# Needs mmdebstrap and qemu-user-static (binfmt) on the host; no root: mmdebstrap runs in
# a user namespace and mke2fs builds the image straight from the tarball.
#
# SSH: root by key only; tv by password, once one is set. The image has no keys unless $NETCAST_SSH_KEY names a .pub file to bake in
# (for a private image; never for one you hand out). Otherwise root's keys come from
# authorized_keys at the root of the stick at every boot (netcast-sshkeys). Host keys are
# made on the TV at first start. Root has no password: the panel's tty1 autologins (USB keyboard).
#
# Desktop: Xfce on Xorg (modesetting on the mt5398_osd KMS driver, CPU rendering), lightdm
# logs user tv (no password, passwordless sudo) in on vt7. Firefox ESR, swap on zram.
# Video: Parole/GStreamer decode H.264 in hardware (mt5398-vdec) and show it full screen on
# the video plane (mt5398-vplane) through the netcastsink plugin (src/mainline/gst).
#
# Env: DEBIAN_SUITE (trixie), DEBIAN_MIRROR, IMG_MB (3072; the stick is FAT, so < 4096).
# Builds in $TMPDIR (default /tmp), which needs about the root's size (~2 GB) free, and so
# does third_party/mainline/out. mmdebstrap's mapped root must be able to reach $TMPDIR,
# so it can't be under a mode-700 home; TMPDIR=/var/tmp works.
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$TOP/third_party/mainline/out"
OVL="$TOP/src/mainline/debian/overlay"
SUITE=${DEBIAN_SUITE:-trixie}
MIRROR=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
IMG_MB=${IMG_MB:-3072}
TAR="$OUT/debian-rootfs.tar"
IMG="$OUT/debian.img"
MKE2FS=$(command -v mke2fs || echo /usr/sbin/mke2fs)

command -v mmdebstrap >/dev/null || { echo "need mmdebstrap (apt install mmdebstrap)" >&2; exit 1; }
[ -e /proc/sys/fs/binfmt_misc/qemu-arm ] || { echo "need qemu-user-static (armhf binfmt)" >&2; exit 1; }

KEYS=()
[ -n "${NETCAST_SSH_KEY:-}" ] && KEYS=("$NETCAST_SSH_KEY")
mkdir -p "$OUT"
cat "${KEYS[@]}" /dev/null > "$OUT/authorized_keys"

PKGS=(
	systemd-sysv udev dbus libpam-systemd
	systemd-resolved systemd-timesyncd
	openssh-server
	iproute2 iputils-ping netbase ca-certificates
	procps less nano e2fsprogs busybox
	# the display tests on the mt5398_osd KMS driver (netcast-gfxtest.service)
	libdrm-tests weston seatd fonts-dejavu-core
	# Xfce desktop (no Recommends: mmdebstrap's default, so the useful ones are listed)
	xserver-xorg-core xserver-xorg-input-libinput xauth x11-xserver-utils
	lightdm lightdm-gtk-greeter dbus-user-session
	xfce4 xfce4-terminal xfce4-notifyd xfce4-taskmanager mousepad ristretto
	elementary-xfce-icon-theme adwaita-icon-theme fonts-dejavu xdg-utils
	firefox-esr htop wget net-tools dnsutils wireshark tcpdump nmap vlc freerdp3-wayland netsurf-gtk smbclient
	systemd-zram-generator sudo strace
	# TV speakers (kernel: mt5398-mixsnd, 48 kHz stereo only): PipeWire with its Pulse and
	# ALSA front ends, so Firefox, VLC and plain ALSA programs all reach the card
	pipewire pipewire-pulse pipewire-alsa wireplumber alsa-utils
	xfce4-pulseaudio-plugin pavucontrol
	# Video: GStreamer with the hardware H.264 decoder (v4l2codecs in plugins-bad, on
	# mt5398-vdec) and the video plane (v4l2sink in plugins-good, on mt5398-vplane, through
	# our netcastsink plugin, see below); libav for what the hardware doesn't decode
	# (HEVC, VP9, ...: software, SD at best). Parole plays through it, audio via PipeWire.
	gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good
	gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-pulseaudio gstreamer1.0-x
	parole
	# Bluetooth keyboards: the kernel's modules (bluetooth, btusb, hidp, uhid) go to
	# /usr/lib/modules from build-mainline.sh's out/modules; blueman = pairing from the panel
	bluez blueman
	# USB Wi-Fi (carl9170; firmware is in the kernel): netcast-wifi@.service, iw for debugging
	wpasupplicant iw
	# SGX544: LG's DDK 1.9 userspace is soft-float (armel), so the armel C runtime sits
	# beside armhf (multiarch; /lib/ld-linux.so.3). netcast-pvr.service.
	libc6:armel libgcc-s1:armel
	# network shares (kernel modules cifs, nfs)
	cifs-utils nfs-common
)

# the video plane's GStreamer sink (src/mainline/gst/build.sh)
GSTSINK=$OUT/libgstnetcastsink.so
[ -f "$GSTSINK" ] || { echo "missing $GSTSINK: run src/mainline/gst/build.sh first" >&2; exit 1; }

# loadable kernel modules, from third_party/build-mainline.sh (it runs modules_install)
MODS=$OUT/modules/lib/modules
[ -f "$MODS"/*/modules.dep ] || { echo "missing $MODS/<version>/modules.dep: run third_party/build-mainline.sh first" >&2; exit 1; }

# what the overlay creates (sync-in keeps the host uid: chown it to root afterwards)
OVL_PATHS=$(cd "$OVL" && find . -mindepth 1 | sed 's/^/"/; s/$/"/' | tr '\n' ' ')

# Hooks run in mmdebstrap's user namespace: "$1" is the root, owned by (mapped) root, and
# it can't read $HOME; sync-in/upload copy the overlay and the key in from outside.
mmdebstrap --mode=unshare --variant=minbase --architectures=armhf,armel \
	--include="$(IFS=,; echo "${PKGS[*]}")" \
	--aptopt='Acquire::Languages "none"' \
	--dpkgopt='path-exclude=/usr/share/man/*' \
	--dpkgopt='path-exclude=/usr/share/info/*' \
	--dpkgopt='path-exclude=/usr/share/locale/*' \
	--dpkgopt='path-exclude=/usr/share/doc/*' \
	--dpkgopt='path-include=/usr/share/doc/*/copyright' \
	--customize-hook="sync-in '$OVL' /" \
	--customize-hook='mkdir -p "$1/usr/lib/modules"' \
	--customize-hook="sync-in '$MODS' /usr/lib/modules" \
	--customize-hook='chown -R root:root "$1/usr/lib/modules" && chmod -R go-w "$1/usr/lib/modules"' \
	--customize-hook="upload '$GSTSINK' /usr/lib/arm-linux-gnueabihf/gstreamer-1.0/libgstnetcastsink.so" \
	--customize-hook='chmod 644 "$1/usr/lib/arm-linux-gnueabihf/gstreamer-1.0/libgstnetcastsink.so"' \
	--customize-hook='mkdir -p "$1/root/.ssh"' \
	--customize-hook="upload '$OUT/authorized_keys' /root/.ssh/authorized_keys" \
	--customize-hook='chown -R root:root "$1/root/.ssh" && chmod 700 "$1/root/.ssh" && chmod 600 "$1/root/.ssh/authorized_keys"' \
	--customize-hook="cd \"\$1\" && chown root:root . $OVL_PATHS && chmod go-w . $OVL_PATHS" \
	--customize-hook="upload '$TOP/src/mainline/debian/panel-volume.sh' /tmp/panel-volume.sh" \
	--customize-hook='sh "$1/tmp/panel-volume.sh" "$1/etc/xdg/xfce4/panel/default.xml" && rm "$1/tmp/panel-volume.sh"' \
	--customize-hook='chmod 440 "$1/etc/sudoers.d/tv"' \
	--customize-hook='chroot "$1" useradd -m -u 1000 -U -G video,input,render,sudo -s /bin/bash tv && chroot "$1" passwd -dq tv' \
	--customize-hook='rm -f "$1"/etc/ssh/ssh_host_*' \
	--customize-hook='ln -sf /run/systemd/resolve/stub-resolv.conf "$1/etc/resolv.conf"' \
	--customize-hook='chroot "$1" systemctl enable systemd-networkd.service systemd-resolved.service systemd-timesyncd.service ssh.service netcast-autoreboot.service netcast-gfxtest.service netcast-pvr.service lightdm.service netcast-rclocal.service' \
	--customize-hook='chroot "$1" systemctl mask systemd-firstboot.service systemd-pstore.service' \
	--customize-hook='chroot "$1" systemctl disable wpa_supplicant.service' \
	--customize-hook='chroot "$1" systemctl disable rpcbind.service rpcbind.socket nfs-blkmap.service' \
	--customize-hook='rm -rf "$1"/var/cache/apt/*.bin "$1"/var/lib/apt/lists/*_Packages' \
	"$SUITE" "$TAR" "$MIRROR"

rm -f "$IMG"
# No lazy init: ext4lazyinit's zeroing (WRITE_ZEROES) fails on a loop device over vfat
"$MKE2FS" -q -t ext4 -L netcast-debian -m 1 -E lazy_itable_init=0,lazy_journal_init=0 \
	-d "$TAR" "$IMG" "${IMG_MB}M"
/usr/sbin/e2fsck -fn "$IMG" >/dev/null 2>&1 || { echo "e2fsck found problems in $IMG" >&2; exit 1; }
rm -f "$TAR"
gzip -1 -c "$IMG" > "$IMG.gz"
ls -la "$IMG" "$IMG.gz"
