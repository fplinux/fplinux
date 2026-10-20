/* SPDX-License-Identifier: GPL-2.0-only */
/* This peer compares literal frames. It does not implement the loader codec. */
#include <libusb-1.0/libusb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct libusb_device {
	uint8_t bus;
	uint8_t address;
};

struct libusb_device_handle {
	libusb_device *device;
};

static libusb_device other = { 7, 8 };
static libusb_device selected = { 7, 9 };
static libusb_device duplicate = { 7, 9 };
static libusb_device *devices[] = { &other, &selected, &duplicate, NULL };
static libusb_device_handle opened;
static char scenario[64];
static FILE *plan;
static FILE *log_file;

static void fail(const char *message)
{
	fprintf(stderr, "USB peer: %s\n", message);
	exit(90);
}

static void record(const char *format, ...)
{
	va_list arguments;

	if (!log_file) {
		log_file = fopen("usb-peer.log", "a");
		if (!log_file)
			fail("cannot open log");
	}
	va_start(arguments, format);
	vfprintf(log_file, format, arguments);
	va_end(arguments);
	fflush(log_file);
}

int libusb_init(libusb_context **context)
{
	FILE *input = fopen("usb-peer-scenario", "r");

	(void)context;
	if (!input || !fgets(scenario, sizeof(scenario), input))
		fail("cannot read scenario");
	fclose(input);
	scenario[strcspn(scenario, "\n")] = '\0';
	record("init\n");
	return 0;
}

void libusb_exit(libusb_context *context)
{
	char remaining[1024];

	(void)context;
	if (plan && fgets(remaining, sizeof(remaining), plan))
		fail("uploader exited before the literal transcript ended");
	if (plan)
		fclose(plan);
	record("exit\n");
	fclose(log_file);
	log_file = NULL;
}

const char *libusb_error_name(int error)
{
	switch (error) {
	case LIBUSB_ERROR_TIMEOUT:
		return "LIBUSB_ERROR_TIMEOUT";
	case LIBUSB_ERROR_NO_DEVICE:
		return "LIBUSB_ERROR_NO_DEVICE";
	default:
		return "LIBUSB_ERROR_IO";
	}
}

ssize_t libusb_get_device_list(libusb_context *context, libusb_device ***list)
{
	(void)context;
	*list = devices;
	record("list\n");
	if (!strcmp(scenario, "list-error"))
		return LIBUSB_ERROR_IO;
	if (!strcmp(scenario, "absent"))
		return 1;
	return !strcmp(scenario, "ambiguous") ? 3 : 2;
}

void libusb_free_device_list(libusb_device **list, int unref_devices)
{
	(void)list;
	(void)unref_devices;
}

uint8_t libusb_get_bus_number(libusb_device *device)
{
	return device->bus;
}

uint8_t libusb_get_device_address(libusb_device *device)
{
	return device->address;
}

int libusb_get_device_descriptor(libusb_device *device,
				 struct libusb_device_descriptor *descriptor)
{
	if (!strcmp(scenario, "descriptor-error"))
		return LIBUSB_ERROR_IO;
	descriptor->idVendor = 0x1782;
	descriptor->idProduct =
		device == &selected && !strcmp(scenario, "wrong-id") ? 0x4002 :
								       0x4d00;
	return 0;
}

int libusb_open(libusb_device *device, libusb_device_handle **handle)
{
	record("open %u:%u\n", device->bus, device->address);
	if (!strcmp(scenario, "open-error"))
		return LIBUSB_ERROR_IO;
	opened.device = device;
	*handle = &opened;
	return 0;
}

libusb_device_handle *libusb_open_device_with_vid_pid(libusb_context *context,
						      uint16_t vendor,
						      uint16_t product)
{
	libusb_device_handle *handle;

	(void)context;
	if (vendor != 0x1782 || product != 0x4d00)
		fail("unexpected VID/PID search");
	record("default-open\n");
	if (libusb_open(&other, &handle))
		return NULL;
	return handle;
}

void libusb_close(libusb_device_handle *handle)
{
	record("close %u:%u\n", handle->device->bus, handle->device->address);
}

libusb_device *libusb_get_device(libusb_device_handle *handle)
{
	return handle->device;
}

int libusb_get_config_descriptor(libusb_device *device, uint8_t index,
				 struct libusb_config_descriptor **descriptor)
{
	static const struct libusb_endpoint_descriptor endpoints[] = {
		{ 0x81, 2, 64 }, { 0x01, 2, 64 }
	};
	static const struct libusb_interface_descriptor alternate = {
		0, 2, endpoints
	};
	static const struct libusb_interface interface = { &alternate, 1 };
	static struct libusb_config_descriptor configuration = { 1,
								 &interface };

	(void)device;
	(void)index;
	*descriptor = &configuration;
	return 0;
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *descriptor)
{
	(void)descriptor;
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
	fail("no kernel driver is active");
	return LIBUSB_ERROR_IO;
}

int libusb_claim_interface(libusb_device_handle *handle, int interface)
{
	record("claim %u:%u %d\n", handle->device->bus, handle->device->address,
	       interface);
	return 0;
}

int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type,
			    uint8_t request, uint16_t value, uint16_t index,
			    unsigned char *data, uint16_t length,
			    unsigned int timeout)
{
	(void)data;
	(void)timeout;
	if (request_type != 0x21 || request != 34 || value != 0x601 || index ||
	    length)
		fail("unexpected control transfer");
	record("control %u:%u\n", handle->device->bus, handle->device->address);
	return 0;
}

int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
			 unsigned char *data, int length, int *transferred,
			 unsigned int timeout)
{
	unsigned char literal[1024];
	char line[2048], operation[32], hex[2048];
	size_t count, i;
	int is_input = endpoint == 0x81;

	(void)timeout;
	if (!is_input && endpoint != 0x01)
		fail("unexpected endpoint");
	if (!plan)
		plan = fopen("usb-peer.plan", "r");
	if (!plan || !fgets(line, sizeof(line), plan))
		fail("unexpected transfer after the literal transcript");
	if (sscanf(line, "%31s %2047s", operation, hex) != 2)
		fail("invalid transcript line");
	if (is_input != (strstr(operation, "in") != NULL))
		fail("unexpected transfer direction");
	record("%s %u:%u %s\n", operation, handle->device->bus,
	       handle->device->address, hex);
	*transferred = 0;
	if (!strcmp(operation, "timeout-in"))
		return LIBUSB_ERROR_TIMEOUT;
	if (!strcmp(operation, "disconnect-in"))
		return LIBUSB_ERROR_NO_DEVICE;
	count = strlen(hex) / 2;
	if (strlen(hex) % 2 || count > sizeof(literal))
		fail("invalid literal frame size");
	for (i = 0; i < count; i++) {
		unsigned int value;

		if (sscanf(hex + i * 2, "%2x", &value) != 1)
			fail("invalid literal byte");
		literal[i] = value;
	}
	if (is_input) {
		if ((size_t)length < count)
			fail("input buffer cannot fit literal frame");
		memcpy(data, literal, count);
	} else if ((size_t)length != count || memcmp(data, literal, count)) {
		fprintf(stderr, "USB peer: expected %s; actual %d bytes:", hex,
			length);
		for (i = 0; i < (size_t)length && i < 64; i++)
			fprintf(stderr, "%02x", data[i]);
		fputc('\n', stderr);
		fail("transmitted frame differs from literal expectation");
	}
	*transferred = (int)count;
	if (!strcmp(operation, "error-out"))
		return LIBUSB_ERROR_IO;
	*transferred = !strcmp(operation, "short-out") ? (int)count - 1 :
							 (int)count;
	return 0;
}
