#!/bin/bash
# Build a bootable MT5398 kernel (raw ARM Image) from the Vizio MediaTek BSP
# (linux-3.0.13, arch/arm/mach-mt53xx), for warm-booting via src/kmod/nckexec.
#
#   - machine nr 5398 (nckexec passes r1=5398), params_phys 0x100, TEXT_OFFSET 0x8000
#   - built-in initramfs (build/kinitramfs) with static busybox telnetd on wired eth0
#   - toolchain: Bootlin armv7-eabihf 2018.11 (GCC 7.3), same as the kernel modules
#
# Output: $SRC/arch/arm/boot/Image  (hand to nckexec, NOT a zImage)
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
K="$TOP/third_party/kernel"
BL="$K/armv7-eabihf--glibc--stable-2018.11-1"
SRC="$K/vizio_oss/linux-3.0"
IRFS="$TOP/build/kinitramfs"
JOBS="$(nproc)"

chmod +x "$TOP/build/kbin/lockfile" 2>/dev/null || true
export PATH="$TOP/build/kbin:$BL/bin:$PATH"   # kbin provides a 'lockfile' shim (MTK BSP build dep)
# ARCH/CROSS_COMPILE must be passed as make *arguments* (env export does not
# override the Makefile's SUBARCH-derived default -> it would pick x86).
# CC must be forced on the command line: the BSP changed the kernel's `CC =`
# to `CC ?=`, so make's built-in default (CC=cc) wins and the cross-compiler
# is never used. A command-line assignment overrides both.
MK=(make ARCH=arm CROSS_COMPILE=arm-linux- CC=arm-linux-gcc HOSTCC=gcc)

cd "$SRC"

# GCC 7 shim for a 3.0 tree (compiler-gccN.h is selected by __GNUC__).
[ -f include/linux/compiler-gcc7.h ] || cp include/linux/compiler-gcc4.h include/linux/compiler-gcc7.h

# Host-compat patch: modern Perl (>=5.22) removed defined(@array).
sed -i 's/if (!defined(@val))/if (!@val)/' kernel/timeconst.pl

if [ "${1:-}" = "config" ] || [ ! -f .config ]; then
  "${MK[@]}" mt5398_smp_mod_defconfig >/dev/null
  ./scripts/config \
    --enable BLK_DEV_INITRD \
    --set-str INITRAMFS_SOURCE "$IRFS" \
    --set-val INITRAMFS_ROOT_UID 0 \
    --set-val INITRAMFS_ROOT_GID 0 \
    --enable DEVTMPFS \
    --enable DEVTMPFS_MOUNT \
    --enable DEVPTS_MULTIPLE_INSTANCES \
    --enable UNIX98_PTYS \
    --enable PROC_FS --enable SYSFS --enable TMPFS \
    --enable INET --enable PACKET --enable UNIX \
    --set-str LOCALVERSION "-nc" --disable LOCALVERSION_AUTO
  # Resolve new symbols non-interactively (3.0 has oldnoconfig, not olddefconfig).
  yes "" | "${MK[@]}" oldnoconfig >/dev/null 2>&1 || "${MK[@]}" oldnoconfig </dev/null >/dev/null 2>&1 || true
  echo "configured. INITRAMFS_SOURCE=$IRFS"
  grep -E 'MACH_MT5398|MAC5398_MII|BLK_DEV_INITRD|INITRAMFS_SOURCE|DEVTMPFS=|SMP=' .config
  [ "${1:-}" = "config" ] && exit 0
fi

# GCC 7 vs a 2011 kernel: force gnu89, silence the newer -Werror classes.
KCFLAGS="-fno-pic -std=gnu89 -Wno-error -Wno-attribute-alias -Wno-implicit-fallthrough -Wno-misleading-indentation -Wno-int-in-bool-context -Wno-bool-operation -Wno-format-truncation -Wno-stringop-overflow -Wno-sizeof-pointer-memaccess -Wno-pointer-sign -Wno-array-bounds"

echo ">>> building Image with $JOBS jobs"
"${MK[@]}" KCFLAGS="$KCFLAGS" -j"$JOBS" Image 2>&1 | tail -40

ls -l "$SRC/arch/arm/boot/Image"
