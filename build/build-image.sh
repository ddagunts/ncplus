#!/bin/bash
# Rebuild the NetCast chroot image (netcast-root.ext4) from scratch.
#
# Inputs:
#   toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot   openlgtv buildroot-nc4 SDK (2026.08), relocated
#   build/busybox-1.36.1.config                      BusyBox config (static; no SYNC_FANCY / SEEDRNG /
#                                                    NSENTER for glibc 2.12). Source is fetched here.
#   tv_dump/glibc/*-2.12.2.so                        LG's glibc, pulled from your TV's /lib
#                                                    with tools/pull-glibc.sh.
#                                                    The SDK's own glibc segfaults on the TV, so
#                                                    the image must use LG's.
# Output:
#   usb/netcast/netcast-root.ext4.gz  (+ busybox, enter.sh, teardown.sh, MD5SUMS)
set -euo pipefail
cd "$(dirname "$0")/.."
TOP=$PWD
SDK=$TOP/toolchain/arm-lgtv-linux-gnueabi_sdk-buildroot
SYSROOT=$SDK/arm-lgtv-linux-gnueabi/sysroot
CROSS=$SDK/bin/arm-lgtv-linux-gnueabi-
B=$TOP/build
OUT=$TOP/usb/netcast
SIZE=${SIZE:-1G}

BB=busybox-1.36.1
if [ ! -f $B/$BB/.config ]; then
  [ -f $B/$BB.tar.bz2 ] || curl -sSfL -o $B/$BB.tar.bz2 https://busybox.net/downloads/$BB.tar.bz2
  echo "b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314  $B/$BB.tar.bz2" | sha256sum -c --quiet -
  tar -C $B -xjf $B/$BB.tar.bz2
  cp $B/$BB.config $B/$BB/.config
fi
ls $TOP/tv_dump/glibc/libc-2.12.2.so >/dev/null || { echo "run tools/pull-glibc.sh first" >&2; exit 1; }
make -C $B/busybox-1.36.1 CROSS_COMPILE=$CROSS -j"$(nproc)" >/dev/null

rm -rf $B/rootfs && mkdir $B/rootfs
tar -C $SYSROOT \
  --exclude='./usr/include' --exclude='*.a' --exclude='*.la' \
  --exclude='./usr/share/man' --exclude='./usr/share/doc' --exclude='./usr/share/info' \
  --exclude='./usr/share/aclocal' --exclude='./usr/share/gtk-doc' \
  --exclude='./usr/lib/pkgconfig' --exclude='./usr/share/pkgconfig' --exclude='./usr/lib/cmake' \
  -cf - . | tar -C $B/rootfs -xf -
make -C $B/busybox-1.36.1 CROSS_COMPILE=$CROSS CONFIG_PREFIX=$B/rootfs install >/dev/null

R=$B/rootfs
# Swap in LG's glibc; keep the SDK copies for reference.
mkdir -p $R/lib/sdk-glibc
for f in $TOP/tv_dump/glibc/*-2.12.2.so; do
  b=$(basename "$f")
  [ -f "$R/lib/$b" ] && mv "$R/lib/$b" "$R/lib/sdk-glibc/$b"
  install -m 755 "$f" "$R/lib/$b"
done

sed -i 's|^export PATH=@PATH@|export PATH=/usr/sbin:/usr/bin:/sbin:/bin|' $R/etc/profile
echo ncroot > $R/etc/hostname
printf 'NAME=ncroot\nPRETTY_NAME="NetCast chroot (buildroot-nc4 2026.08 + LG glibc 2.12.2)"\nID=ncroot\n' > $R/etc/os-release
mkdir -p $R/mnt/lg/res/lglib $R/mnt/vendor $R/mnt/addon $R/mnt/browser $R/mnt/usb $R/usr/local $R/root
cat > $R/etc/profile.d/netcast.sh <<'EOF'
# NetCast chroot environment
export PS1='[ncroot \w]# '
# TV-native graphics/system libs are bind-mounted read-only at their original paths.
# To use them for a GUI program, e.g.:
#   LD_LIBRARY_PATH=/mnt/lg/res/lglib:/mnt/vendor:/usr/local/lib ./app
export TV_LIBS=/mnt/lg/res/lglib:/mnt/vendor:/usr/local/lib
EOF

# ext4 with only features kernel 3.0.13 understands.
rm -f $B/netcast-root.ext4
fakeroot sh -c "chown -R 0:0 $R && chmod 1777 $R/tmp && \
  /sbin/mkfs.ext4 -F -q -L ncroot -m 0 \
    -O ^metadata_csum,^64bit,^orphan_file,^metadata_csum_seed,^large_dir,^inline_data,^ea_inode \
    -E lazy_itable_init=0,lazy_journal_init=0 -d $R $B/netcast-root.ext4 $SIZE"
/sbin/e2fsck -fn $B/netcast-root.ext4 >/dev/null

mkdir -p $OUT
cp $B/busybox-1.36.1/busybox $OUT/busybox
gzip -1 -c $B/netcast-root.ext4 > $OUT/netcast-root.ext4.gz
(cd $OUT && md5sum busybox enter.sh teardown.sh netcast-root.ext4.gz > MD5SUMS)
(cd $B && md5sum netcast-root.ext4) >> $OUT/MD5SUMS
cat $OUT/MD5SUMS
