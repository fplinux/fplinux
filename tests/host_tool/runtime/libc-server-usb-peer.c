// SPDX-License-Identifier: GPL-2.0-only
/* Deterministic USB peer for the bridge process; it does not access a device. */
#define _DEFAULT_SOURCE
#include <libusb-1.0/libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int device_token;
static int handle_token;
static int opens, closes, allocated, freed, not_ready, transfers, acknowledged;
static int handle_open, config_live, lifetime_errors;
static unsigned long long waited_us;
static const char *scenario;

static void report(void)
{
	fprintf(stderr,
		"peer opens=%d closes=%d allocated=%d freed=%d not_ready=%d "
		"transfers=%d acknowledged=%d lifetime_errors=%d waited_us=%llu\n",
		opens, closes, allocated, freed, not_ready, transfers,
		acknowledged, lifetime_errors, waited_us);
}

int libusb_init(libusb_context **context)
{
	(void)context;
	scenario = getenv("BRIDGE_USB_SCENARIO");
	if (!scenario)
		abort();
	atexit(report);
	return 0;
}

void libusb_exit(libusb_context *context)
{
	(void)context;
}

libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *context,
						      uint16_t vendor,
						      uint16_t product)
{
	(void)context;
	if (vendor != 0x1782 || product != 0x4d00)
		abort();
	if ((!strcmp(scenario, "open-missing") && not_ready < 2) ||
	    !strcmp(scenario, "open-gone")) {
		not_ready++;
		return NULL;
	}
	if (handle_open || config_live)
		lifetime_errors++;
	handle_open = 1;
	opens++;
	return (libusb_device_handle *)&handle_token;
}

void libusb_close(libusb_device_handle *handle)
{
	if (handle != (libusb_device_handle *)&handle_token)
		abort();
	if (!handle_open || config_live)
		lifetime_errors++;
	handle_open = 0;
	closes++;
}

libusb_device *libusb_get_device(libusb_device_handle *handle)
{
	if (handle != (libusb_device_handle *)&handle_token)
		abort();
	return (libusb_device *)&device_token;
}

int libusb_get_config_descriptor(libusb_device *device, uint8_t index,
				 struct libusb_config_descriptor **output)
{
	static const struct libusb_endpoint_descriptor endpoints[] = {
		{ .bEndpointAddress = 0x81,
		  .bmAttributes = 2,
		  .wMaxPacketSize = 64 },
		{ .bEndpointAddress = 0x02,
		  .bmAttributes = 2,
		  .wMaxPacketSize = 64 },
	};
	static struct libusb_interface_descriptor alternate = {
		.bInterfaceNumber = 0,
		.bNumEndpoints = 2,
		.endpoint = endpoints,
	};
	static struct libusb_interface interface = {
		.altsetting = &alternate,
		.num_altsetting = 1,
	};
	static struct libusb_config_descriptor config = {
		.bNumInterfaces = 1,
		.interface = &interface,
	};

	if (device != (libusb_device *)&device_token || index != 0)
		abort();
	if (!strcmp(scenario, "descriptor-missing") && not_ready < 2) {
		not_ready++;
		return LIBUSB_ERROR_NOT_FOUND;
	}
	if (!strcmp(scenario, "descriptor-gone")) {
		not_ready++;
		return LIBUSB_ERROR_NO_DEVICE;
	}
	if (!strcmp(scenario, "descriptor-access"))
		return LIBUSB_ERROR_ACCESS;
	if (!strcmp(scenario, "descriptor-invalid"))
		return LIBUSB_ERROR_INVALID_PARAM;
	if (!strcmp(scenario, "missing-endpoint"))
		alternate.bNumEndpoints = 1;
	if (config_live)
		lifetime_errors++;
	config_live = 1;
	allocated++;
	*output = &config;
	return 0;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *config)
{
	if (!config)
		abort();
	if (!config_live)
		lifetime_errors++;
	config_live = 0;
	freed++;
}

int libusb_kernel_driver_active(libusb_device_handle *handle, int interface)
{
	(void)handle;
	(void)interface;
	return 0;
}

int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface)
{
	(void)handle;
	(void)interface;
	abort();
}

int libusb_claim_interface(libusb_device_handle *handle, int interface)
{
	(void)handle;
	(void)interface;
	if (!strcmp(scenario, "claim-gone") && !not_ready) {
		not_ready++;
		return LIBUSB_ERROR_NO_DEVICE;
	}
	if (!strcmp(scenario, "claim-access"))
		return LIBUSB_ERROR_ACCESS;
	return 0;
}

const char *libusb_error_name(int error)
{
	switch (error) {
	case LIBUSB_ERROR_NOT_FOUND:
		return "LIBUSB_ERROR_NOT_FOUND";
	case LIBUSB_ERROR_NO_DEVICE:
		return "LIBUSB_ERROR_NO_DEVICE";
	case LIBUSB_ERROR_ACCESS:
		return "LIBUSB_ERROR_ACCESS";
	case LIBUSB_ERROR_INVALID_PARAM:
		return "LIBUSB_ERROR_INVALID_PARAM";
	default:
		return "unexpected error";
	}
}

int usleep(useconds_t interval)
{
	waited_us += interval;
	return 0;
}

int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
			 unsigned char *data, int length, int *actual,
			 unsigned int timeout)
{
	static const unsigned char incoming[] = {
		0x80, 0x01, 0x52, 0x5c, 'x', 0x8a, 0, 0, 0, 0, 0,    0,	   0, 0,
		0,    0,    0,	  0,	0,   0,	   0, 0, 0, 0, 0,    0,	   0, 0,
		0,    0,    0,	  0,	0,   0,	   0, 0, 0, 0, 0xe4, 0x5a,
	};
	static const unsigned char ack[35] = { [33] = 0xe4, [34] = 0x5a };
	static size_t incoming_position, ack_position;
	static int hello_received;

	(void)timeout;
	if (handle != (libusb_device_handle *)&handle_token)
		abort();
	transfers++;
	if (endpoint == 0x81 && hello_received && length > 0) {
		size_t remaining = sizeof(incoming) - incoming_position;
		size_t count = (size_t)length < remaining ? (size_t)length :
							    remaining;

		if (!count)
			abort();
		memcpy(data, incoming + incoming_position, count);
		incoming_position += count;
		*actual = (int)count;
		return 0;
	}
	if (endpoint == 2 && length > 0) {
		if (!hello_received) {
			if (length != 1 || data[0] != 0)
				abort();
			hello_received = 1;
		} else {
			if (incoming_position != sizeof(incoming) ||
			    (size_t)length > sizeof(ack) - ack_position ||
			    memcmp(data, ack + ack_position, (size_t)length))
				abort();
			ack_position += (size_t)length;
			acknowledged = ack_position == sizeof(ack);
		}
		*actual = length;
		return 0;
	}
	abort();
}
