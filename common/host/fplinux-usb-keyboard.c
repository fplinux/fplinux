// SPDX-License-Identifier: GPL-2.0-only
/* Forward one Linux evdev keyboard to the FPLinux generic-serial gadget. */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libusb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FPLINUX_USB_KEYBOARD_DEFAULT_VENDOR_ID 0x0525U
#define FPLINUX_USB_KEYBOARD_DEFAULT_PRODUCT_ID 0xa4a6U
#define FPLINUX_USB_KEYBOARD_DEFAULT_TIMEOUT_MS 250U
#define FPLINUX_USB_KEYBOARD_DEFAULT_WAIT_SECONDS 30U
#define FPLINUX_USB_KEYBOARD_BUFFER_BYTES 512U
#define FPLINUX_USB_KEYBOARD_KEY_STATE_BYTES ((KEY_CNT + 7U) / 8U)
#define FPLINUX_USB_KEYBOARD_POLL_MS 200

struct options {
	uint16_t vid;
	uint16_t pid;
	int interface_number;
	int bus_number;
	int device_address;
	unsigned int timeout_ms;
	unsigned int wait_seconds;
	bool detach_kernel_driver;
	bool list_devices;
	bool self_test;
	const char *keyboard_device;
};

struct endpoint_pair {
	int interface_number;
	int alternate_setting;
	uint8_t endpoint_in;
	uint8_t endpoint_out;
};

struct keyboard_forward_state {
	char buffer[FPLINUX_USB_KEYBOARD_BUFFER_BYTES];
	size_t filled;
	bool pressed[KEY_CNT];
	bool pending[KEY_CNT];
	bool dropping;
};

static volatile sig_atomic_t signal_requested;

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage: fplinux-usb-keyboard [OPTIONS] --interface N "
		"--keyboard EVDEV\n"
		"\n"
		"Forward one Linux evdev keyboard to the FPLinux generic-serial "
		"USB\n"
		"function. The selected device is exclusively grabbed while "
		"forwarding.\n"
		"\n"
		"Options:\n"
		"  --vid HEX          exact USB vendor ID (default: 0525)\n"
		"  --pid HEX          exact USB product ID (default: a4a6)\n"
		"  --interface N      exact generic-serial interface (required)\n"
		"  --keyboard EVDEV   evdev device to grab and forward (required)\n"
		"  --bus N            select one USB bus\n"
		"  --address N        select one USB device address\n"
		"  --timeout-ms N     bulk-transfer timeout (default: 250)\n"
		"  --wait N           wait up to N seconds (default: 30)\n"
		"  --no-detach        do not detach an active kernel USB driver\n"
		"  --list             list visible USB devices and exit\n"
		"  --self-test        check the keyboard wire format and exit\n"
		"  -h, --help         show this help and exit\n");
}

static void signal_handler(int number)
{
	(void)number;
	signal_requested = 1;
}

static bool parse_unsigned(const char *text, int base, unsigned long maximum,
			   unsigned long *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (text == NULL || text[0] == '\0' || text[0] == '-')
		return false;
	errno = 0;
	parsed = strtoul(text, &end, base);
	if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
		return false;
	*value = parsed;
	return true;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	enum {
		OPTION_VENDOR_ID = 1000,
		OPTION_PRODUCT_ID,
		OPTION_INTERFACE,
		OPTION_KEYBOARD,
		OPTION_BUS,
		OPTION_ADDRESS,
		OPTION_TIMEOUT,
		OPTION_WAIT,
		OPTION_NO_DETACH,
		OPTION_LIST,
		OPTION_SELF_TEST,
	};
	static const struct option long_options[] = {
		{ "vid", required_argument, NULL, OPTION_VENDOR_ID },
		{ "pid", required_argument, NULL, OPTION_PRODUCT_ID },
		{ "interface", required_argument, NULL, OPTION_INTERFACE },
		{ "keyboard", required_argument, NULL, OPTION_KEYBOARD },
		{ "bus", required_argument, NULL, OPTION_BUS },
		{ "address", required_argument, NULL, OPTION_ADDRESS },
		{ "timeout-ms", required_argument, NULL, OPTION_TIMEOUT },
		{ "wait", required_argument, NULL, OPTION_WAIT },
		{ "no-detach", no_argument, NULL, OPTION_NO_DETACH },
		{ "list", no_argument, NULL, OPTION_LIST },
		{ "self-test", no_argument, NULL, OPTION_SELF_TEST },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	unsigned long value;
	int option;

	*options = (struct options){
		.vid = FPLINUX_USB_KEYBOARD_DEFAULT_VENDOR_ID,
		.pid = FPLINUX_USB_KEYBOARD_DEFAULT_PRODUCT_ID,
		.interface_number = -1,
		.bus_number = -1,
		.device_address = -1,
		.timeout_ms = FPLINUX_USB_KEYBOARD_DEFAULT_TIMEOUT_MS,
		.wait_seconds = FPLINUX_USB_KEYBOARD_DEFAULT_WAIT_SECONDS,
		.detach_kernel_driver = true,
	};

	while ((option = getopt_long(argc, argv, "h", long_options, NULL)) !=
	       -1) {
		switch (option) {
		case OPTION_VENDOR_ID:
			if (!parse_unsigned(optarg, 16, UINT16_MAX, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --vid: %s\n",
					optarg);
				return -1;
			}
			options->vid = (uint16_t)value;
			break;
		case OPTION_PRODUCT_ID:
			if (!parse_unsigned(optarg, 16, UINT16_MAX, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --pid: %s\n",
					optarg);
				return -1;
			}
			options->pid = (uint16_t)value;
			break;
		case OPTION_INTERFACE:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --interface: %s\n",
					optarg);
				return -1;
			}
			options->interface_number = (int)value;
			break;
		case OPTION_KEYBOARD:
			options->keyboard_device = optarg;
			break;
		case OPTION_BUS:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --bus: %s\n",
					optarg);
				return -1;
			}
			options->bus_number = (int)value;
			break;
		case OPTION_ADDRESS:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --address: %s\n",
					optarg);
				return -1;
			}
			options->device_address = (int)value;
			break;
		case OPTION_TIMEOUT:
			if (!parse_unsigned(optarg, 0, 60000, &value) ||
			    value == 0) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --timeout-ms: %s\n",
					optarg);
				return -1;
			}
			options->timeout_ms = (unsigned int)value;
			break;
		case OPTION_WAIT:
			if (!parse_unsigned(optarg, 0, 3600, &value)) {
				fprintf(stderr,
					"fplinux-usb-keyboard: invalid --wait: %s\n",
					optarg);
				return -1;
			}
			options->wait_seconds = (unsigned int)value;
			break;
		case OPTION_NO_DETACH:
			options->detach_kernel_driver = false;
			break;
		case OPTION_LIST:
			options->list_devices = true;
			break;
		case OPTION_SELF_TEST:
			options->self_test = true;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		default:
			usage(stderr);
			return -1;
		}
	}

	if (optind != argc) {
		fprintf(stderr,
			"fplinux-usb-keyboard: unexpected positional argument: %s\n",
			argv[optind]);
		return -1;
	}
	if ((options->bus_number < 0) != (options->device_address < 0)) {
		fprintf(stderr,
			"fplinux-usb-keyboard: --bus and --address must be used together\n");
		return -1;
	}
	if (options->list_devices || options->self_test) {
		if (options->list_devices && options->self_test) {
			fprintf(stderr,
				"fplinux-usb-keyboard: --list and --self-test are mutually exclusive\n");
			return -1;
		}
		if (options->keyboard_device != NULL ||
		    options->interface_number >= 0) {
			fprintf(stderr,
				"fplinux-usb-keyboard: --list and --self-test cannot be combined with forwarding options\n");
			return -1;
		}
		return 0;
	}
	if (options->interface_number < 0 || options->keyboard_device == NULL) {
		fprintf(stderr,
			"fplinux-usb-keyboard: --interface and --keyboard are required\n");
		return -1;
	}
	return 0;
}

static int list_devices(libusb_context *context)
{
	libusb_device **devices = NULL;
	ssize_t count;
	ssize_t index;

	count = libusb_get_device_list(context, &devices);
	if (count < 0) {
		fprintf(stderr,
			"fplinux-usb-keyboard: cannot enumerate USB devices: %s\n",
			libusb_error_name((int)count));
		return 1;
	}
	for (index = 0; index < count; ++index) {
		struct libusb_device_descriptor descriptor;
		int result = libusb_get_device_descriptor(devices[index],
							  &descriptor);

		if (result != LIBUSB_SUCCESS)
			continue;
		printf("bus=%03u address=%03u vid=%04x pid=%04x\n",
		       libusb_get_bus_number(devices[index]),
		       libusb_get_device_address(devices[index]),
		       descriptor.idVendor, descriptor.idProduct);
	}
	libusb_free_device_list(devices, 1);
	return 0;
}

static int find_matching_device(libusb_context *context,
				const struct options *options,
				libusb_device **match)
{
	libusb_device **devices = NULL;
	ssize_t count;
	ssize_t index;
	int matches = 0;

	*match = NULL;
	count = libusb_get_device_list(context, &devices);
	if (count < 0)
		return (int)count;
	for (index = 0; index < count; ++index) {
		struct libusb_device_descriptor descriptor;
		libusb_device *device = devices[index];
		int result = libusb_get_device_descriptor(device, &descriptor);

		if (result != LIBUSB_SUCCESS ||
		    descriptor.idVendor != options->vid ||
		    descriptor.idProduct != options->pid)
			continue;
		if (options->bus_number >= 0 &&
		    (libusb_get_bus_number(device) != options->bus_number ||
		     libusb_get_device_address(device) !=
			     options->device_address))
			continue;
		++matches;
		if (*match == NULL)
			*match = libusb_ref_device(device);
	}
	libusb_free_device_list(devices, 1);
	if (matches > 1) {
		libusb_unref_device(*match);
		*match = NULL;
		return LIBUSB_ERROR_BUSY;
	}
	return matches == 1 ? LIBUSB_SUCCESS : LIBUSB_ERROR_NO_DEVICE;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000U +
	       (uint64_t)value.tv_nsec / 1000000U;
}

static int wait_for_device(libusb_context *context,
			   const struct options *options,
			   libusb_device **device)
{
	uint64_t deadline = monotonic_milliseconds() +
			    (uint64_t)options->wait_seconds * 1000U;
	int result;

	do {
		result = find_matching_device(context, options, device);
		if (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_BUSY)
			return result;
		if (result != LIBUSB_ERROR_NO_DEVICE)
			return result;
		if (options->wait_seconds == 0 ||
		    monotonic_milliseconds() >= deadline)
			break;
		usleep(250000);
	} while (!signal_requested);
	return LIBUSB_ERROR_NO_DEVICE;
}

static int find_bulk_endpoints(libusb_device *device, int requested_interface,
			       struct endpoint_pair *pair)
{
	struct libusb_config_descriptor *config = NULL;
	int matches = 0;
	int result;
	int interface_index;

	result = libusb_get_active_config_descriptor(device, &config);
	if (result != LIBUSB_SUCCESS)
		result = libusb_get_config_descriptor(device, 0, &config);
	if (result != LIBUSB_SUCCESS)
		return result;

	memset(pair, 0, sizeof(*pair));
	pair->interface_number = -1;
	for (interface_index = 0; interface_index < config->bNumInterfaces;
	     ++interface_index) {
		const struct libusb_interface *interface =
			&config->interface[interface_index];
		int alternate_index;

		for (alternate_index = 0;
		     alternate_index < interface->num_altsetting;
		     ++alternate_index) {
			const struct libusb_interface_descriptor *alternate =
				&interface->altsetting[alternate_index];
			uint8_t endpoint_in = 0;
			uint8_t endpoint_out = 0;
			int endpoints_in = 0;
			int endpoints_out = 0;
			int endpoint_index;

			if (alternate->bInterfaceNumber != requested_interface)
				continue;
			for (endpoint_index = 0;
			     endpoint_index < alternate->bNumEndpoints;
			     ++endpoint_index) {
				const struct libusb_endpoint_descriptor *endpoint =
					&alternate->endpoint[endpoint_index];

				if ((endpoint->bmAttributes &
				     LIBUSB_TRANSFER_TYPE_MASK) !=
				    LIBUSB_TRANSFER_TYPE_BULK)
					continue;
				if (endpoint->bEndpointAddress &
				    LIBUSB_ENDPOINT_IN) {
					endpoint_in =
						endpoint->bEndpointAddress;
					++endpoints_in;
				} else {
					endpoint_out =
						endpoint->bEndpointAddress;
					++endpoints_out;
				}
			}
			if (endpoints_in != 1 || endpoints_out != 1)
				continue;
			++matches;
			pair->interface_number = alternate->bInterfaceNumber;
			pair->alternate_setting = alternate->bAlternateSetting;
			pair->endpoint_in = endpoint_in;
			pair->endpoint_out = endpoint_out;
		}
	}
	libusb_free_config_descriptor(config);
	if (matches > 1)
		return LIBUSB_ERROR_BUSY;
	return matches == 1 ? LIBUSB_SUCCESS : LIBUSB_ERROR_NOT_FOUND;
}

static int claim_interface(libusb_device_handle *handle, int interface_number,
			   bool detach, bool *was_detached)
{
	int active;
	int result;

	*was_detached = false;
	active = libusb_kernel_driver_active(handle, interface_number);
	if (active == 1) {
		if (!detach) {
			fprintf(stderr,
				"fplinux-usb-keyboard: kernel driver owns interface %d\n",
				interface_number);
			return LIBUSB_ERROR_BUSY;
		}
		result = libusb_detach_kernel_driver(handle, interface_number);
		if (result != LIBUSB_SUCCESS)
			return result;
		*was_detached = true;
	} else if (active != 0 && active != LIBUSB_ERROR_NOT_SUPPORTED) {
		return active;
	}

	result = libusb_claim_interface(handle, interface_number);
	if (result != LIBUSB_SUCCESS && *was_detached) {
		(void)libusb_attach_kernel_driver(handle, interface_number);
		*was_detached = false;
	}
	return result;
}

static void release_interface(libusb_device_handle *handle,
			      int interface_number, bool was_detached)
{
	(void)libusb_release_interface(handle, interface_number);
	if (was_detached)
		(void)libusb_attach_kernel_driver(handle, interface_number);
}

static int send_bytes(libusb_device_handle *handle, uint8_t endpoint,
		      const unsigned char *data, size_t size,
		      unsigned int timeout_ms)
{
	while (size > 0) {
		int transferred = 0;
		int chunk = size > INT32_MAX ? INT32_MAX : (int)size;
		int result = libusb_bulk_transfer(handle, endpoint,
						  (unsigned char *)data, chunk,
						  &transferred, timeout_ms);

		if (transferred > 0) {
			data += transferred;
			size -= (size_t)transferred;
		}
		if (result == LIBUSB_SUCCESS)
			continue;
		if (result == LIBUSB_ERROR_TIMEOUT && transferred > 0)
			continue;
		return result;
	}
	return LIBUSB_SUCCESS;
}

static bool keyboard_state_bit(const unsigned char *state, unsigned int code)
{
	return (state[code / 8U] & (1U << (code % 8U))) != 0;
}

static bool keyboard_modifier(unsigned int code)
{
	switch (code) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
	case KEY_LEFTALT:
	case KEY_RIGHTALT:
	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		return true;
	default:
		return false;
	}
}

static int format_keyboard_event(char *line, size_t size, unsigned int type,
				 unsigned int code, int value)
{
	int written = snprintf(line, size, "%u %u %d\n", type, code, value);

	return written > 0 && (size_t)written < size ? written : -1;
}

static int flush_keyboard_events(libusb_device_handle *handle,
				 const struct endpoint_pair *pair,
				 const struct options *options,
				 struct keyboard_forward_state *state)
{
	int result;

	if (state->filled == 0)
		return LIBUSB_SUCCESS;
	result = send_bytes(handle, pair->endpoint_out,
			    (const unsigned char *)state->buffer, state->filled,
			    options->timeout_ms);
	if (result == LIBUSB_SUCCESS)
		memcpy(state->pressed, state->pending, sizeof(state->pressed));
	state->filled = 0;
	return result;
}

static int queue_keyboard_event(libusb_device_handle *handle,
				const struct endpoint_pair *pair,
				const struct options *options,
				struct keyboard_forward_state *state,
				unsigned int type, unsigned int code, int value)
{
	char line[64];
	int written =
		format_keyboard_event(line, sizeof(line), type, code, value);
	int result;

	if (written < 0)
		return LIBUSB_ERROR_OTHER;
	if ((size_t)written > sizeof(state->buffer) - state->filled) {
		result = flush_keyboard_events(handle, pair, options, state);
		if (result != LIBUSB_SUCCESS)
			return result;
	}
	memcpy(state->buffer + state->filled, line, (size_t)written);
	state->filled += (size_t)written;
	if (type == EV_KEY && code < KEY_CNT && (value == 0 || value == 1))
		state->pending[code] = value == 1;
	return LIBUSB_SUCCESS;
}

static int queue_keyboard_state_changes(libusb_device_handle *handle,
					const struct endpoint_pair *pair,
					const struct options *options,
					struct keyboard_forward_state *state,
					const unsigned char *physical,
					bool down, bool modifiers)
{
	unsigned int code;

	for (code = 0; code < KEY_CNT; ++code) {
		int result;

		if (keyboard_state_bit(physical, code) != down ||
		    state->pending[code] == down ||
		    keyboard_modifier(code) != modifiers)
			continue;
		result = queue_keyboard_event(handle, pair, options, state,
					      EV_KEY, code, down ? 1 : 0);
		if (result != LIBUSB_SUCCESS)
			return result;
	}
	return LIBUSB_SUCCESS;
}

static int resync_keyboard(libusb_device_handle *handle,
			   const struct endpoint_pair *pair,
			   const struct options *options, int keyboard,
			   struct keyboard_forward_state *state,
			   bool *transport_alive)
{
	static const struct {
		bool down;
		bool modifiers;
	} phases[] = {
		{ false, false },
		{ false, true },
		{ true, true },
		{ true, false },
	};
	unsigned char physical[FPLINUX_USB_KEYBOARD_KEY_STATE_BYTES] = { 0 };
	size_t phase;
	int result;

	if (ioctl(keyboard, EVIOCGKEY(sizeof(physical)), physical) < 0)
		return LIBUSB_ERROR_IO;
	for (phase = 0; phase < sizeof(phases) / sizeof(phases[0]); ++phase) {
		result = queue_keyboard_state_changes(handle, pair, options,
						      state, physical,
						      phases[phase].down,
						      phases[phase].modifiers);
		if (result != LIBUSB_SUCCESS) {
			*transport_alive = false;
			return result;
		}
	}
	result = queue_keyboard_event(handle, pair, options, state, EV_SYN,
				      SYN_REPORT, 0);
	if (result == LIBUSB_SUCCESS)
		result = flush_keyboard_events(handle, pair, options, state);
	if (result != LIBUSB_SUCCESS)
		*transport_alive = false;
	return result;
}

static int release_keyboard(libusb_device_handle *handle,
			    const struct endpoint_pair *pair,
			    const struct options *options,
			    struct keyboard_forward_state *state)
{
	unsigned char released[FPLINUX_USB_KEYBOARD_KEY_STATE_BYTES] = { 0 };
	unsigned int code;
	bool any = false;
	int result;

	state->filled = 0;
	memcpy(state->pending, state->pressed, sizeof(state->pending));
	for (code = 0; code < KEY_CNT; ++code) {
		if (state->pressed[code]) {
			any = true;
			break;
		}
	}
	if (!any)
		return LIBUSB_SUCCESS;
	result = queue_keyboard_state_changes(handle, pair, options, state,
					      released, false, false);
	if (result == LIBUSB_SUCCESS)
		result = queue_keyboard_state_changes(
			handle, pair, options, state, released, false, true);
	if (result == LIBUSB_SUCCESS)
		result = queue_keyboard_event(handle, pair, options, state,
					      EV_SYN, SYN_REPORT, 0);
	if (result == LIBUSB_SUCCESS)
		result = flush_keyboard_events(handle, pair, options, state);
	return result;
}

static int forward_keyboard(libusb_device_handle *handle,
			    const struct endpoint_pair *pair,
			    const struct options *options)
{
	static const unsigned char reset[] = "reset\n";
	struct keyboard_forward_state state = { 0 };
	bool transport_alive = true;
	int keyboard;
	int result = LIBUSB_SUCCESS;

	keyboard = open(options->keyboard_device, O_RDONLY | O_CLOEXEC);
	if (keyboard < 0) {
		fprintf(stderr, "fplinux-usb-keyboard: cannot open %s: %s\n",
			options->keyboard_device, strerror(errno));
		return LIBUSB_ERROR_OTHER;
	}
	if (ioctl(keyboard, EVIOCGRAB, 1) != 0) {
		fprintf(stderr, "fplinux-usb-keyboard: cannot grab %s: %s\n",
			options->keyboard_device, strerror(errno));
		close(keyboard);
		return LIBUSB_ERROR_OTHER;
	}
	result = send_bytes(handle, pair->endpoint_out, reset,
			    sizeof(reset) - 1, options->timeout_ms);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr,
			"fplinux-usb-keyboard: cannot reset keyboard channel: %s\n",
			libusb_strerror(result));
		goto cleanup;
	}
	result = resync_keyboard(handle, pair, options, keyboard, &state,
				 &transport_alive);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr,
			"fplinux-usb-keyboard: cannot read initial keyboard state: %s\n",
			transport_alive ? strerror(errno) :
					  libusb_strerror(result));
		goto cleanup;
	}
	fprintf(stderr,
		"fplinux-usb-keyboard: forwarding %s; it no longer reaches the desktop\n",
		options->keyboard_device);

	while (!signal_requested) {
		struct pollfd waiting = {
			.fd = keyboard,
			.events = POLLIN,
		};
		struct input_event event;
		ssize_t got;
		int ready = poll(&waiting, 1, FPLINUX_USB_KEYBOARD_POLL_MS);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "fplinux-usb-keyboard: %s: %s\n",
				options->keyboard_device, strerror(errno));
			result = LIBUSB_ERROR_IO;
			break;
		}
		if (ready == 0) {
			result = send_bytes(handle, pair->endpoint_out,
					    (const unsigned char *)"\n", 1,
					    options->timeout_ms);
			if (result == LIBUSB_SUCCESS)
				continue;
			transport_alive = false;
			fprintf(stderr,
				"fplinux-usb-keyboard: keyboard channel disconnected: %s\n",
				libusb_strerror(result));
			break;
		}
		if (waiting.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr,
				"fplinux-usb-keyboard: keyboard input disconnected\n");
			result = LIBUSB_ERROR_IO;
			break;
		}
		if (!(waiting.revents & POLLIN))
			continue;
		got = read(keyboard, &event, sizeof(event));
		if (got < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "fplinux-usb-keyboard: %s: %s\n",
				options->keyboard_device, strerror(errno));
			result = LIBUSB_ERROR_IO;
			break;
		}
		if (got != (ssize_t)sizeof(event)) {
			fprintf(stderr,
				"fplinux-usb-keyboard: keyboard input disconnected\n");
			result = LIBUSB_ERROR_IO;
			break;
		}
		if (state.dropping) {
			if (event.type != EV_SYN || event.code != SYN_REPORT)
				continue;
			state.dropping = false;
			result = resync_keyboard(handle, pair, options,
						 keyboard, &state,
						 &transport_alive);
			if (result == LIBUSB_SUCCESS)
				continue;
			fprintf(stderr,
				"fplinux-usb-keyboard: cannot resynchronize keyboard: %s\n",
				transport_alive ? strerror(errno) :
						  libusb_strerror(result));
			break;
		}
		if (event.type == EV_SYN && event.code == SYN_DROPPED) {
			state.filled = 0;
			memcpy(state.pending, state.pressed,
			       sizeof(state.pending));
			state.dropping = true;
			continue;
		}
		if (event.type != EV_KEY && event.type != EV_SYN)
			continue;
		if (event.type == EV_KEY && event.value == 2)
			continue;
		result = queue_keyboard_event(handle, pair, options, &state,
					      event.type, event.code,
					      event.value);
		if (result == LIBUSB_SUCCESS && event.type == EV_SYN)
			result = flush_keyboard_events(handle, pair, options,
						       &state);
		if (result == LIBUSB_SUCCESS)
			continue;
		transport_alive = false;
		fprintf(stderr,
			"fplinux-usb-keyboard: cannot send key events: %s\n",
			libusb_strerror(result));
		break;
	}

	if (transport_alive) {
		int release_result =
			release_keyboard(handle, pair, options, &state);

		if (result == LIBUSB_SUCCESS &&
		    release_result != LIBUSB_SUCCESS)
			result = release_result;
	}

cleanup:
	(void)ioctl(keyboard, EVIOCGRAB, 0);
	close(keyboard);
	return result;
}

static int self_test(void)
{
	char line[64];
	int written;

	written = format_keyboard_event(line, sizeof(line), EV_KEY, KEY_A, 1);
	if (written != 7 || memcmp(line, "1 30 1\n", 7) != 0) {
		fprintf(stderr,
			"fplinux-usb-keyboard: self-test failed: key event format\n");
		return 1;
	}
	written = format_keyboard_event(line, sizeof(line), EV_SYN, SYN_REPORT,
					0);
	if (written != 6 || memcmp(line, "0 0 0\n", 6) != 0) {
		fprintf(stderr,
			"fplinux-usb-keyboard: self-test failed: sync event format\n");
		return 1;
	}
	if (!keyboard_modifier(KEY_LEFTSHIFT) ||
	    !keyboard_modifier(KEY_RIGHTMETA) || keyboard_modifier(KEY_A)) {
		fprintf(stderr,
			"fplinux-usb-keyboard: self-test failed: modifier classification\n");
		return 1;
	}
	puts("SELFTEST OK");
	return 0;
}

static void print_libusb_failure(const char *operation, int result)
{
	fprintf(stderr, "fplinux-usb-keyboard: %s: %s", operation,
		libusb_error_name(result));
	if (result == LIBUSB_ERROR_ACCESS)
		fprintf(stderr,
			" (USB permission denied; install a suitable udev rule or grant explicit device access)");
	else if (result == LIBUSB_ERROR_BUSY)
		fprintf(stderr,
			" (device or interface is busy; select one device or allow driver detachment)");
	else if (result == LIBUSB_ERROR_NO_DEVICE)
		fprintf(stderr, " (the gadget disconnected)");
	fputc('\n', stderr);
}

int main(int argc, char **argv)
{
	struct options options;
	libusb_context *context = NULL;
	libusb_device *device = NULL;
	libusb_device_handle *handle = NULL;
	struct endpoint_pair pair = { .interface_number = -1 };
	bool detached = false;
	bool claimed = false;
	int result;
	int exit_status = 1;

	if (parse_options(argc, argv, &options) != 0)
		return 2;
	if (options.self_test)
		return self_test();
	result = libusb_init(&context);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot initialize libusb", result);
		return 1;
	}
	if (options.list_devices) {
		exit_status = list_devices(context);
		goto cleanup;
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr,
		"fplinux-usb-keyboard: waiting for %04x:%04x (up to %u s)\n",
		options.vid, options.pid, options.wait_seconds);
	result = wait_for_device(context, &options, &device);
	if (result == LIBUSB_ERROR_BUSY) {
		fprintf(stderr,
			"fplinux-usb-keyboard: multiple %04x:%04x devices found; select one with --bus and --address (see --list)\n",
			options.vid, options.pid);
		goto cleanup;
	}
	if (result != LIBUSB_SUCCESS) {
		if (result == LIBUSB_ERROR_NO_DEVICE)
			fprintf(stderr,
				"fplinux-usb-keyboard: USB gadget %04x:%04x did not appear within %u seconds\n",
				options.vid, options.pid, options.wait_seconds);
		else
			print_libusb_failure("cannot enumerate the USB gadget",
					     result);
		goto cleanup;
	}
	result = libusb_open(device, &handle);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot open the USB gadget", result);
		goto cleanup;
	}
	result = find_bulk_endpoints(device, options.interface_number, &pair);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr,
			"fplinux-usb-keyboard: interface %d does not contain exactly one bulk IN/OUT endpoint pair\n",
			options.interface_number);
		goto cleanup;
	}
	result = claim_interface(handle, pair.interface_number,
				 options.detach_kernel_driver, &detached);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot claim keyboard interface", result);
		goto cleanup;
	}
	claimed = true;
	if (pair.alternate_setting != 0) {
		result = libusb_set_interface_alt_setting(
			handle, pair.interface_number, pair.alternate_setting);
		if (result != LIBUSB_SUCCESS) {
			print_libusb_failure(
				"cannot select USB alternate setting", result);
			goto cleanup;
		}
	}
	fprintf(stderr,
		"fplinux-usb-keyboard: connected bus=%03u address=%03u interface=%d bulk-in=0x%02x bulk-out=0x%02x\n",
		libusb_get_bus_number(device),
		libusb_get_device_address(device), pair.interface_number,
		pair.endpoint_in, pair.endpoint_out);
	result = forward_keyboard(handle, &pair, &options);
	if (result == LIBUSB_SUCCESS || signal_requested)
		exit_status = 0;
	else
		print_libusb_failure("keyboard transfer failed", result);

cleanup:
	if (handle != NULL && claimed)
		release_interface(handle, pair.interface_number, detached);
	if (handle != NULL)
		libusb_close(handle);
	if (device != NULL)
		libusb_unref_device(device);
	libusb_exit(context);
	return exit_status;
}
