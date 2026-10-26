/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TEST_LOADER_LIBUSB_H
#define FPLINUX_TEST_LOADER_LIBUSB_H

#include <stdint.h>
#include <sys/types.h>

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

#define LIBUSB_ERROR_IO (-1)
#define LIBUSB_ERROR_ACCESS (-3)
#define LIBUSB_ERROR_NO_DEVICE (-4)
#define LIBUSB_ERROR_BUSY (-6)
#define LIBUSB_ERROR_TIMEOUT (-7)
#define LIBUSB_ERROR_NOT_SUPPORTED (-12)

struct libusb_device_descriptor {
	uint16_t idVendor;
	uint16_t idProduct;
};

struct libusb_endpoint_descriptor {
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
};

struct libusb_interface_descriptor {
	uint8_t bInterfaceNumber;
	uint8_t bNumEndpoints;
	const struct libusb_endpoint_descriptor *endpoint;
};

struct libusb_interface {
	const struct libusb_interface_descriptor *altsetting;
	int num_altsetting;
};

struct libusb_config_descriptor {
	uint8_t bNumInterfaces;
	const struct libusb_interface *interface;
};

int libusb_init(libusb_context **context);
void libusb_exit(libusb_context *context);
const char *libusb_error_name(int error);
ssize_t libusb_get_device_list(libusb_context *context,
			       libusb_device ***devices);
void libusb_free_device_list(libusb_device **devices, int unref_devices);
uint8_t libusb_get_bus_number(libusb_device *device);
uint8_t libusb_get_device_address(libusb_device *device);
int libusb_get_device_descriptor(libusb_device *device,
				 struct libusb_device_descriptor *descriptor);
int libusb_open(libusb_device *device, libusb_device_handle **handle);
libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *context,
						      uint16_t vendor,
						      uint16_t product);
void libusb_close(libusb_device_handle *handle);
libusb_device *libusb_get_device(libusb_device_handle *handle);
int libusb_get_config_descriptor(libusb_device *device, uint8_t index,
				 struct libusb_config_descriptor **descriptor);
void libusb_free_config_descriptor(struct libusb_config_descriptor *descriptor);
int libusb_kernel_driver_active(libusb_device_handle *handle, int interface);
int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface);
int libusb_claim_interface(libusb_device_handle *handle, int interface);
int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type,
			    uint8_t request, uint16_t value, uint16_t index,
			    unsigned char *data, uint16_t length,
			    unsigned int timeout);
int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
			 unsigned char *data, int length, int *transferred,
			 unsigned int timeout);

#endif
