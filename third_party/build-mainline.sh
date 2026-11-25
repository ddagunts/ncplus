#!/bin/bash
# Build the phase 1 mainline kernel for the MT5398 TV: a raw ARM Image with a built-in
# initramfs, plus the DTB that src/kmod/nckexec passes to it (dtb=...).
#
#   third_party/build-mainline.sh          # download if needed, configure, build
#   third_party/build-mainline.sh config   # reconfigure only
#
# Inputs : src/mainline/{mt5398.config, mt5398-lg-netcast.dts, initramfs/init}
#          src/mainline/kernel/ (new drivers, copied over the tree), src/mainline/patches/
#          build/kinitramfs/bin/busybox (static ARM busybox; copied from build/build-image.sh's build if missing)
#          third_party/mainline/firmware/ (downloaded: built into the Image, EXTRA_FIRMWARE)
# Outputs: third_party/mainline/out/{Image, mt5398-lg-netcast.dtb, config, modules/}
set -euo pipefail

KV=6.12.119
TOP="$(cd "$(dirname "$0")/.." && pwd)"
M="$TOP/third_party/mainline"
SRC="$M/linux-$KV"
OUT="$M/out"
# inside the source tree: the kernel then records relative paths (GCC 7 can't remap __FILE__), so the
# Image doesn't carry the builder's home directory
KB="$SRC/kbuild"
export KBUILD_BUILD_USER=netcast KBUILD_BUILD_HOST=netcast
TC="$TOP/third_party/kernel/armv7-eabihf--glibc--stable-2018.11-1/bin/arm-linux-"
BUSYBOX="$TOP/build/kinitramfs/bin/busybox"
MS="$TOP/src/mainline"

mkdir -p "$M" "$OUT"
if [ ! -d "$SRC" ]; then
	( cd "$M"
	  curl -sSfLO "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KV.tar.xz"
	  curl -sSfL https://cdn.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc \
		| grep " linux-$KV.tar.xz\$" > "linux-$KV.sha256"
	  sha256sum -c "linux-$KV.sha256"
	  tar xf "linux-$KV.tar.xz" )
fi
# Firmware built into the Image (CONFIG_EXTRA_FIRMWARE): the carl9170 USB Wi-Fi firmware
# (GPL-2.0+, source: carl9170fw) from linux-firmware, and wireless-regdb's signed regulatory
# database (ISC). Pinned by hash.
FW="$M/firmware"
LINUX_FW_TAG=20251111
REGDB=wireless-regdb-2026.09.03
fwget() {	# fwget <file> <sha256> <url>
	[ -f "$FW/$1" ] && echo "$2  $FW/$1" | sha256sum -c --quiet - && return
	curl -sSfL --retry 3 -o "$FW/$1.part" "$3"
	echo "$2  $FW/$1.part" | sha256sum -c --quiet -
	mv "$FW/$1.part" "$FW/$1"
}
mkdir -p "$FW"
fwget carl9170-1.fw e1695dbfbc6aa7bb3182615bd47905e2df808317e4050878e50bb24285b37068 \
	"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/carl9170-1.fw?h=$LINUX_FW_TAG"
fwget "$REGDB.tar.xz" b22e0901227b820cd1c280abe681a15b773a5103a5e10dc442e94ebb34cbf58d \
	"https://cdn.kernel.org/pub/software/network/wireless-regdb/$REGDB.tar.xz"
if [ ! -f "$FW/regulatory.db.p7s" ] || [ "$FW/$REGDB.tar.xz" -nt "$FW/regulatory.db.p7s" ]; then
	tar -C "$FW" -xJf "$FW/$REGDB.tar.xz" --strip-components=1 \
		"$REGDB/regulatory.db" "$REGDB/regulatory.db.p7s" "$REGDB/LICENSE"
	mv "$FW/LICENSE" "$FW/wireless-regdb-LICENSE"
	touch "$FW/regulatory.db.p7s"
fi

# Not in git: copied from the static busybox that build/build-image.sh builds.
if [ ! -x "$BUSYBOX" ] && [ -x "$TOP/build/busybox-1.36.1/busybox" ]; then
	mkdir -p "$(dirname "$BUSYBOX")" && cp "$TOP/build/busybox-1.36.1/busybox" "$BUSYBOX"
fi
[ -x "$BUSYBOX" ] || { echo "missing static busybox: $BUSYBOX (run build/build-image.sh first)" >&2; exit 1; }

# Our drivers (src/mainline/kernel/ mirrors the tree) and the Kconfig/Makefile hooks for them.
cp -r "$MS/kernel/." "$SRC/"
for p in "$MS"/patches/*.patch; do
	if patch -d "$SRC" -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1; then
		continue	# already applied
	fi
	patch -d "$SRC" -p1 -N -s < "$p"
	echo "applied $(basename "$p")"
done

# initramfs as a gen_init_cpio list: device nodes need no root, and /dev/console
# must exist before devtmpfs is mounted or init starts without stdio.
cat > "$OUT/initramfs.list" <<EOF
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/tty0 0600 0 0 c 4 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
dir /bin 0755 0 0
file /bin/busybox $BUSYBOX 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $MS/initramfs/init 0755 0 0
dir /etc 0755 0 0
file /etc/udhcpc.script $MS/initramfs/udhcpc.script 0755 0 0
EOF

mkdir -p "$KB"
MK=(make -C "$SRC" O="$KB" ARCH=arm CROSS_COMPILE="$TC" -j"$(nproc)")

if [ "${1:-}" = config ] || [ ! -f "$KB/.config" ]; then
	cp "$MS/mt5398.config" "$OUT/allno.config"
	echo "CONFIG_INITRAMFS_SOURCE=\"$OUT/initramfs.list\"" >> "$OUT/allno.config"
	echo "CONFIG_EXTRA_FIRMWARE_DIR=\"$FW\"" >> "$OUT/allno.config"
	KCONFIG_ALLCONFIG="$OUT/allno.config" "${MK[@]}" allnoconfig >/dev/null
	# every =y/=m in the fragment must survive (hidden or unmet dependencies drop silently)
	miss=0
	while IFS= read -r l; do
		case "$l" in CONFIG_*=[ym]) grep -qx "$l" "$KB/.config" || { echo "NOT SET: $l" >&2; miss=1; } ;; esac
	done < "$OUT/allno.config"
	for s in ARM_GIC ARM_GLOBAL_TIMER OF; do
		grep -qx "CONFIG_$s=y" "$KB/.config" || { echo "NOT SET: CONFIG_$s" >&2; miss=1; }
	done
	# CPU1 needs the mach-mt5398 enable-method (nckexec's hold page), not a generic one
	grep -qx 'CONFIG_ARCH_MT5398=y' "$KB/.config" || { echo "NOT SET: CONFIG_ARCH_MT5398" >&2; miss=1; }
	[ $miss = 0 ] || exit 1
	echo "configured: $KB/.config"
	[ "${1:-}" = config ] && exit 0
fi

# "modules" too: it writes Module.symvers, which out-of-tree modules (build-sgx.sh) link against
"${MK[@]}" Image modules
cp "$KB/arch/arm/boot/Image" "$OUT/Image"
cp "$KB/.config" "$OUT/config"
# loadable modules (Bluetooth...): out/modules/lib/modules/<version>/, with modules.dep;
# build-debian.sh puts it into the image's /lib/modules
rm -rf "$OUT/modules"
"${MK[@]}" INSTALL_MOD_PATH="$OUT/modules" INSTALL_MOD_STRIP=1 DEPMOD=/sbin/depmod modules_install >/dev/null
rm -f "$OUT"/modules/lib/modules/*/build

"$KB/scripts/dtc/dtc" -I dts -O dtb -W no-unit_address_vs_reg \
	-o "$OUT/mt5398-lg-netcast.dtb" "$MS/mt5398-lg-netcast.dts"

ls -la "$OUT/Image" "$OUT/mt5398-lg-netcast.dtb"
