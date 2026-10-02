/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake discovery boundary; enumeration and hotplug use controlled devices. */
#ifndef FPLINUX_TEST_LIBUDEV_H
#define FPLINUX_TEST_LIBUDEV_H

struct udev;
struct udev_device;
struct udev_monitor;
struct udev_enumerate;
struct udev_list_entry;
struct udev *udev_new(void);
struct udev *udev_unref(struct udev *context);
struct udev_monitor *udev_monitor_new_from_netlink(struct udev *context,
						   const char *name);
struct udev_monitor *udev_monitor_unref(struct udev_monitor *monitor);
int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *monitor,
						    const char *subsystem,
						    const char *devtype);
int udev_monitor_enable_receiving(struct udev_monitor *monitor);
int udev_monitor_get_fd(struct udev_monitor *monitor);
struct udev_device *udev_monitor_receive_device(struct udev_monitor *monitor);
struct udev_enumerate *udev_enumerate_new(struct udev *context);
struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *scan);
int udev_enumerate_add_match_subsystem(struct udev_enumerate *scan,
				       const char *subsystem);
int udev_enumerate_scan_devices(struct udev_enumerate *scan);
struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *scan);
struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *entry);
const char *udev_list_entry_get_name(struct udev_list_entry *entry);
struct udev_device *udev_device_new_from_syspath(struct udev *context,
						 const char *syspath);
struct udev_device *udev_device_unref(struct udev_device *device);
const char *udev_device_get_devnode(struct udev_device *device);
const char *udev_device_get_sysname(struct udev_device *device);
const char *udev_device_get_action(struct udev_device *device);
const char *udev_device_get_property_value(struct udev_device *device,
					   const char *key);

#endif
