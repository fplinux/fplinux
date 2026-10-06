#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

PATH=/bin
export PATH

ramroot_fail() {
	printf '<3>fplinux-ramroot: %s\n' "$1" >/dev/kmsg
	printf 'fplinux-ramroot: %s\n' "$1" >&2
	while :; do
		/bin/busybox sleep 3600
	done
}

/bin/busybox mount -t devtmpfs -o nosuid,noexec,mode=0755 dev /dev ||
	ramroot_fail "cannot mount /dev"
/bin/busybox mount -t proc -o nosuid,nodev,noexec proc /proc ||
	ramroot_fail "cannot mount /proc"
/bin/busybox mount -t sysfs -o nosuid,nodev,noexec sysfs /sys ||
	ramroot_fail "cannot mount /sys"
/bin/busybox mount -t tmpfs \
	-o mode=0755,nosuid,nodev,nr_inodes=800k,size=20%,strictatime tmpfs /run ||
	ramroot_fail "cannot mount /run"
/bin/busybox mkdir -m 0700 /run/fplinux-root ||
	ramroot_fail "cannot create root mount directory"
/bin/busybox mkdir /run/fplinux-root/lower /run/fplinux-root/state ||
	ramroot_fail "cannot create root mount points"
/bin/busybox mount -t squashfs -o ro,loop /root.squashfs /run/fplinux-root/lower ||
	ramroot_fail "cannot mount compressed root"
/bin/busybox mount -t tmpfs -o mode=0700,size=40M tmpfs /run/fplinux-root/state ||
	ramroot_fail "cannot mount writable root state"
/bin/busybox mkdir -m 0755 /run/fplinux-root/state/upper ||
	ramroot_fail "cannot create writable root"
/bin/busybox mkdir -m 0700 /run/fplinux-root/state/work ||
	ramroot_fail "cannot create root work directory"
/bin/busybox mount -t overlay overlay \
	-o lowerdir=/run/fplinux-root/lower,upperdir=/run/fplinux-root/state/upper,workdir=/run/fplinux-root/state/work,index=on,redirect_dir=on,xino=auto,metacopy=off \
	/newroot || ramroot_fail "cannot mount writable root"

while read -r _ramroot_device ramroot_path _ramroot_type ramroot_options _ramroot_rest; do
	[ "$ramroot_path" = /newroot ] || continue
	case ",$ramroot_options," in
	*,index=on,*) ;;
	*) ramroot_fail "root does not preserve copied hard links" ;;
	esac
	case ",$ramroot_options," in
	*,redirect_dir=on,*) ;;
	*) ramroot_fail "root does not support directory rename" ;;
	esac
done </proc/mounts

[ -x /newroot/sbin/init ] || ramroot_fail "system init is missing"
for ramroot_mount in run dev proc sys; do
	/bin/busybox mount --move "/$ramroot_mount" "/newroot/$ramroot_mount" ||
		ramroot_fail "cannot transfer /$ramroot_mount"
done

# The loop device retains the compressed file while switch_root removes its
# old name, the bootstrap runtime and the rest of the initial RAM filesystem.
exec /bin/busybox switch_root -c /dev/console /newroot /sbin/init "$@"
