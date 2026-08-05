// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host-side console bridge for the Linux USB serial gadget used by FPLinux.
 *
 * This program only exchanges bytes with an already-running USB gadget.  It
 * has no Spreadtrum loader, flash, erase, partition, or NV commands.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_VID 0x0525u
#define DEFAULT_PID 0xa4a6u
#define DEFAULT_TIMEOUT_MS 250u
#define DEFAULT_WAIT_SECONDS 30u
#define DEFAULT_LINGER_MS 500u
#define TRANSFER_BUFFER_SIZE 16384u
#define MAX_UPLOAD_BYTES (8u * 1024u * 1024u)
#define UPLOAD_WINDOW_LINES 16u
#define LOCAL_ESCAPE 0x1du

#define USB_DT_CS_INTERFACE 0x24u
#define USB_CDC_UNION_TYPE 0x06u
#define USB_CDC_DATA_CLASS 0x0au
#define USB_CDC_CONTROL_CLASS 0x02u
#define USB_CDC_REQ_SET_LINE_CODING 0x20u
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE 0x22u
#define USB_CDC_CONTROL_DTR 0x01u
#define USB_CDC_CONTROL_RTS 0x02u

struct options {
	uint16_t vid;
	uint16_t pid;
	int interface_number;
	int bus_number;
	int device_address;
	unsigned int timeout_ms;
	unsigned int wait_seconds;
	unsigned int linger_ms;
	bool detach_kernel_driver;
	bool list_devices;
	bool self_test;
	const char *upload_local;
	const char *upload_remote;
};

struct endpoint_pair {
	int interface_number;
	int alternate_setting;
	int control_interface;
	uint8_t endpoint_in;
	uint8_t endpoint_out;
};

struct reader_state {
	libusb_device_handle *handle;
	uint8_t endpoint;
	unsigned int timeout_ms;
	atomic_bool *stop;
	atomic_int result;
};

struct sha256_state {
	uint32_t words[8];
	uint64_t bytes;
	unsigned char block[64];
	size_t block_size;
};

static volatile sig_atomic_t signal_requested;
static struct termios saved_terminal;
static bool terminal_is_raw;

static void usage(FILE *stream)
{
	fprintf(
	    stream,
	    "Usage: fplinux-usb-console [OPTIONS]\n"
	    "\n"
	    "Forward stdin/stdout over the bulk endpoints of a Linux USB "
	    "serial\n"
	    "gadget. Defaults match non-ACM mainline g_serial (0525:a4a6).\n"
	    "\n"
	    "Options:\n"
	    "  --vid HEX          USB vendor ID (default: 0525)\n"
	    "  --pid HEX          USB product ID (default: a4a6)\n"
	    "  --interface N      data interface number (default: "
	    "auto-detect)\n"
	    "  --bus N            select one USB bus\n"
	    "  --address N        select one USB device address\n"
	    "  --timeout-ms N     bulk-transfer timeout (default: 250)\n"
	    "  --wait N           wait up to N seconds for enumeration "
	    "(default: 30)\n"
	    "  --linger-ms N      keep reading after stdin EOF (default: 500)\n"
	    "  --no-detach        do not detach an active kernel USB driver\n"
	    "  --list             list visible USB devices and exit\n"
	    "  --self-test        run host-only codec and path-safety tests\n"
	    "  --upload LOCAL REMOTE\n"
	    "                     upload LOCAL (up to 8 MiB) to /tmp/FILE\n"
	    "                     and is installed only after device-side "
	    "SHA-256\n"
	    "                     verification\n"
	    "  -h, --help         show this help and exit\n"
	    "\n"
	    "Interactive mode uses Ctrl-] as the local escape. Ctrl-C is sent\n"
	    "to the shell on the phone. This tool cannot load firmware and\n"
	    "contains no flash/erase commands.\n");
}

static void restore_terminal(void)
{
	if (terminal_is_raw) {
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_terminal);
		terminal_is_raw = false;
	}
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

	if (text == NULL || text[0] == '\0' || text[0] == '-') {
		return false;
	}
	errno = 0;
	parsed = strtoul(text, &end, base);
	if (errno != 0 || end == text || *end != '\0' || parsed > maximum) {
		return false;
	}
	*value = parsed;
	return true;
}

static uint32_t rotate_right(uint32_t value, unsigned int count)
{
	return (value >> count) | (value << (32u - count));
}

static void sha256_transform(struct sha256_state *state,
			     const unsigned char block[64])
{
	static const uint32_t constants[64] = {
	    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
	    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
	    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
	    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
	    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
	    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
	    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
	    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
	    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
	    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
	uint32_t schedule[64];
	uint32_t a = state->words[0];
	uint32_t b = state->words[1];
	uint32_t c = state->words[2];
	uint32_t d = state->words[3];
	uint32_t e = state->words[4];
	uint32_t f = state->words[5];
	uint32_t g = state->words[6];
	uint32_t h = state->words[7];
	unsigned int index;

	for (index = 0; index < 16; ++index) {
		size_t offset = index * 4u;
		schedule[index] = ((uint32_t)block[offset] << 24) |
				  ((uint32_t)block[offset + 1] << 16) |
				  ((uint32_t)block[offset + 2] << 8) |
				  (uint32_t)block[offset + 3];
	}
	for (index = 16; index < 64; ++index) {
		uint32_t x = schedule[index - 15];
		uint32_t y = schedule[index - 2];
		uint32_t small0 =
		    rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
		uint32_t small1 =
		    rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
		schedule[index] = schedule[index - 16] + small0 +
				  schedule[index - 7] + small1;
	}
	for (index = 0; index < 64; ++index) {
		uint32_t choice = (e & f) ^ (~e & g);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t large0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
				  rotate_right(a, 22);
		uint32_t large1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
				  rotate_right(e, 25);
		uint32_t temporary1 =
		    h + large1 + choice + constants[index] + schedule[index];
		uint32_t temporary2 = large0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	state->words[0] += a;
	state->words[1] += b;
	state->words[2] += c;
	state->words[3] += d;
	state->words[4] += e;
	state->words[5] += f;
	state->words[6] += g;
	state->words[7] += h;
}

static void sha256_init(struct sha256_state *state)
{
	*state = (struct sha256_state){
	    .words =
		{
		    0x6a09e667u,
		    0xbb67ae85u,
		    0x3c6ef372u,
		    0xa54ff53au,
		    0x510e527fu,
		    0x9b05688cu,
		    0x1f83d9abu,
		    0x5be0cd19u,
		},
	};
}

static void sha256_update(struct sha256_state *state, const unsigned char *data,
			  size_t size)
{
	state->bytes += size;
	while (size > 0) {
		size_t available = sizeof(state->block) - state->block_size;
		size_t take = size < available ? size : available;

		memcpy(state->block + state->block_size, data, take);
		state->block_size += take;
		data += take;
		size -= take;
		if (state->block_size == sizeof(state->block)) {
			sha256_transform(state, state->block);
			state->block_size = 0;
		}
	}
}

static void sha256_finish(struct sha256_state *state, unsigned char digest[32])
{
	uint64_t bit_count = state->bytes * 8u;
	unsigned int index;

	state->block[state->block_size++] = 0x80;
	if (state->block_size > 56) {
		memset(state->block + state->block_size, 0,
		       sizeof(state->block) - state->block_size);
		sha256_transform(state, state->block);
		state->block_size = 0;
	}
	memset(state->block + state->block_size, 0, 56 - state->block_size);
	for (index = 0; index < 8; ++index) {
		state->block[63 - index] =
		    (unsigned char)(bit_count >> (index * 8u));
	}
	sha256_transform(state, state->block);
	for (index = 0; index < 8; ++index) {
		digest[index * 4] = (unsigned char)(state->words[index] >> 24);
		digest[index * 4 + 1] =
		    (unsigned char)(state->words[index] >> 16);
		digest[index * 4 + 2] =
		    (unsigned char)(state->words[index] >> 8);
		digest[index * 4 + 3] = (unsigned char)state->words[index];
	}
}

static int parse_options(int argc, char **argv, struct options *options)
{
	enum {
		OPT_VID = 1000,
		OPT_PID,
		OPT_INTERFACE,
		OPT_BUS,
		OPT_ADDRESS,
		OPT_TIMEOUT,
		OPT_WAIT,
		OPT_LINGER,
		OPT_NO_DETACH,
		OPT_LIST,
		OPT_UPLOAD,
		OPT_SELF_TEST,
	};
	static const struct option long_options[] = {
	    {"vid", required_argument, NULL, OPT_VID},
	    {"pid", required_argument, NULL, OPT_PID},
	    {"interface", required_argument, NULL, OPT_INTERFACE},
	    {"bus", required_argument, NULL, OPT_BUS},
	    {"address", required_argument, NULL, OPT_ADDRESS},
	    {"timeout-ms", required_argument, NULL, OPT_TIMEOUT},
	    {"wait", required_argument, NULL, OPT_WAIT},
	    {"linger-ms", required_argument, NULL, OPT_LINGER},
	    {"no-detach", no_argument, NULL, OPT_NO_DETACH},
	    {"list", no_argument, NULL, OPT_LIST},
	    {"upload", required_argument, NULL, OPT_UPLOAD},
	    {"self-test", no_argument, NULL, OPT_SELF_TEST},
	    {"help", no_argument, NULL, 'h'},
	    {NULL, 0, NULL, 0},
	};
	int option;
	unsigned long value;

	*options = (struct options){
	    .vid = DEFAULT_VID,
	    .pid = DEFAULT_PID,
	    .interface_number = -1,
	    .bus_number = -1,
	    .device_address = -1,
	    .timeout_ms = DEFAULT_TIMEOUT_MS,
	    .wait_seconds = DEFAULT_WAIT_SECONDS,
	    .linger_ms = DEFAULT_LINGER_MS,
	    .detach_kernel_driver = true,
	};

	while ((option = getopt_long(argc, argv, "h", long_options, NULL)) !=
	       -1) {
		switch (option) {
		case OPT_VID:
			if (!parse_unsigned(optarg, 16, UINT16_MAX, &value)) {
				fprintf(
				    stderr,
				    "fplinux-usb-console: invalid --vid: %s\n",
				    optarg);
				return -1;
			}
			options->vid = (uint16_t)value;
			break;
		case OPT_PID:
			if (!parse_unsigned(optarg, 16, UINT16_MAX, &value)) {
				fprintf(
				    stderr,
				    "fplinux-usb-console: invalid --pid: %s\n",
				    optarg);
				return -1;
			}
			options->pid = (uint16_t)value;
			break;
		case OPT_INTERFACE:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(stderr,
					"fplinux-usb-console: invalid "
					"--interface: %s\n",
					optarg);
				return -1;
			}
			options->interface_number = (int)value;
			break;
		case OPT_BUS:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(
				    stderr,
				    "fplinux-usb-console: invalid --bus: %s\n",
				    optarg);
				return -1;
			}
			options->bus_number = (int)value;
			break;
		case OPT_ADDRESS:
			if (!parse_unsigned(optarg, 0, 255, &value)) {
				fprintf(stderr,
					"fplinux-usb-console: invalid "
					"--address: %s\n",
					optarg);
				return -1;
			}
			options->device_address = (int)value;
			break;
		case OPT_TIMEOUT:
			if (!parse_unsigned(optarg, 0, 60000, &value) ||
			    value == 0) {
				fprintf(stderr,
					"fplinux-usb-console: invalid "
					"--timeout-ms: %s\n",
					optarg);
				return -1;
			}
			options->timeout_ms = (unsigned int)value;
			break;
		case OPT_WAIT:
			if (!parse_unsigned(optarg, 0, 3600, &value)) {
				fprintf(
				    stderr,
				    "fplinux-usb-console: invalid --wait: %s\n",
				    optarg);
				return -1;
			}
			options->wait_seconds = (unsigned int)value;
			break;
		case OPT_LINGER:
			if (!parse_unsigned(optarg, 0, 60000, &value)) {
				fprintf(stderr,
					"fplinux-usb-console: invalid "
					"--linger-ms: %s\n",
					optarg);
				return -1;
			}
			options->linger_ms = (unsigned int)value;
			break;
		case OPT_NO_DETACH:
			options->detach_kernel_driver = false;
			break;
		case OPT_LIST:
			options->list_devices = true;
			break;
		case OPT_UPLOAD:
			options->upload_local = optarg;
			break;
		case OPT_SELF_TEST:
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
	if (options->upload_local != NULL && optind + 1 == argc) {
		options->upload_remote = argv[optind++];
	}
	if (options->upload_local != NULL && options->upload_remote == NULL) {
		fprintf(stderr, "fplinux-usb-console: --upload requires LOCAL "
				"and REMOTE\n");
		return -1;
	}
	if (optind != argc) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: unexpected positional argument: %s\n",
		    argv[optind]);
		usage(stderr);
		return -1;
	}
	if (options->upload_local != NULL &&
	    (options->list_devices || options->self_test)) {
		fprintf(stderr,
			"fplinux-usb-console: --upload cannot be combined with "
			"--list or --self-test\n");
		return -1;
	}
	if (options->list_devices && options->self_test) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: --list and --self-test are mutually "
		    "exclusive\n");
		return -1;
	}
	if ((options->bus_number < 0) != (options->device_address < 0)) {
		fprintf(stderr, "fplinux-usb-console: --bus and --address must "
				"be used together\n");
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
		fprintf(
		    stderr,
		    "fplinux-usb-console: cannot enumerate USB devices: %s\n",
		    libusb_error_name((int)count));
		return 1;
	}
	for (index = 0; index < count; ++index) {
		struct libusb_device_descriptor descriptor;
		int result =
		    libusb_get_device_descriptor(devices[index], &descriptor);
		if (result != LIBUSB_SUCCESS) {
			continue;
		}
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
	if (count < 0) {
		return (int)count;
	}
	for (index = 0; index < count; ++index) {
		struct libusb_device_descriptor descriptor;
		libusb_device *device = devices[index];
		int result = libusb_get_device_descriptor(device, &descriptor);

		if (result != LIBUSB_SUCCESS ||
		    descriptor.idVendor != options->vid ||
		    descriptor.idProduct != options->pid) {
			continue;
		}
		if (options->bus_number >= 0 &&
		    (libusb_get_bus_number(device) != options->bus_number ||
		     libusb_get_device_address(device) !=
			 options->device_address)) {
			continue;
		}
		++matches;
		if (*match == NULL) {
			*match = libusb_ref_device(device);
		}
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

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
		return 0;
	}
	return (uint64_t)value.tv_sec * 1000u +
	       (uint64_t)value.tv_nsec / 1000000u;
}

static int wait_for_device(libusb_context *context,
			   const struct options *options,
			   libusb_device **device)
{
	uint64_t deadline =
	    monotonic_milliseconds() + (uint64_t)options->wait_seconds * 1000u;
	int result;

	do {
		result = find_matching_device(context, options, device);
		if (result == LIBUSB_SUCCESS || result == LIBUSB_ERROR_BUSY) {
			return result;
		}
		if (result != LIBUSB_ERROR_NO_DEVICE) {
			return result;
		}
		if (options->wait_seconds == 0 ||
		    monotonic_milliseconds() >= deadline) {
			break;
		}
		usleep(250000);
	} while (!signal_requested);
	return LIBUSB_ERROR_NO_DEVICE;
}

static int
union_control_interface(const struct libusb_config_descriptor *config,
			int data_interface)
{
	int interface_index;

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
			const unsigned char *extra = alternate->extra;
			int remaining = alternate->extra_length;

			while (remaining >= 3) {
				int length = extra[0];
				if (length < 3 || length > remaining) {
					break;
				}
				if (extra[1] == USB_DT_CS_INTERFACE &&
				    extra[2] == USB_CDC_UNION_TYPE &&
				    length >= 5) {
					int slave;
					for (slave = 4; slave < length;
					     ++slave) {
						if (extra[slave] ==
						    data_interface) {
							return extra[3];
						}
					}
				}
				extra += length;
				remaining -= length;
			}
		}
	}

	return -1;
}

static int find_bulk_endpoints(libusb_device *device, int requested_interface,
			       struct endpoint_pair *pair)
{
	struct libusb_config_descriptor *config = NULL;
	int best_score = -1;
	int best_matches = 0;
	int result;
	int interface_index;

	result = libusb_get_active_config_descriptor(device, &config);
	if (result != LIBUSB_SUCCESS) {
		result = libusb_get_config_descriptor(device, 0, &config);
	}
	if (result != LIBUSB_SUCCESS) {
		return result;
	}

	memset(pair, 0, sizeof(*pair));
	pair->interface_number = -1;
	pair->control_interface = -1;
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
			int endpoint_index;
			int score;

			if (requested_interface >= 0 &&
			    alternate->bInterfaceNumber !=
				requested_interface) {
				continue;
			}
			for (endpoint_index = 0;
			     endpoint_index < alternate->bNumEndpoints;
			     ++endpoint_index) {
				const struct libusb_endpoint_descriptor
				    *endpoint =
					&alternate->endpoint[endpoint_index];
				if ((endpoint->bmAttributes &
				     LIBUSB_TRANSFER_TYPE_MASK) !=
				    LIBUSB_TRANSFER_TYPE_BULK) {
					continue;
				}
				if (endpoint->bEndpointAddress &
				    LIBUSB_ENDPOINT_IN) {
					endpoint_in =
					    endpoint->bEndpointAddress;
				} else {
					endpoint_out =
					    endpoint->bEndpointAddress;
				}
			}
			if (endpoint_in == 0 || endpoint_out == 0) {
				continue;
			}
			score = alternate->bInterfaceClass == USB_CDC_DATA_CLASS
				    ? 100
				    : 0;
			score -= alternate->bAlternateSetting;
			if (score > best_score) {
				best_score = score;
				best_matches = 1;
				pair->interface_number =
				    alternate->bInterfaceNumber;
				pair->alternate_setting =
				    alternate->bAlternateSetting;
				pair->endpoint_in = endpoint_in;
				pair->endpoint_out = endpoint_out;
			} else if (score == best_score) {
				++best_matches;
			}
		}
	}
	if (pair->interface_number >= 0) {
		pair->control_interface =
		    union_control_interface(config, pair->interface_number);
	}
	libusb_free_config_descriptor(config);
	if (requested_interface < 0 && best_matches > 1) {
		return LIBUSB_ERROR_BUSY;
	}
	return pair->interface_number >= 0 ? LIBUSB_SUCCESS
					   : LIBUSB_ERROR_NOT_FOUND;
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
				"fplinux-usb-console: kernel driver owns "
				"interface %d; "
				"retry without --no-detach\n",
				interface_number);
			return LIBUSB_ERROR_BUSY;
		}
		result = libusb_detach_kernel_driver(handle, interface_number);
		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr,
				"fplinux-usb-console: cannot detach kernel "
				"driver from "
				"interface %d: %s\n",
				interface_number, libusb_error_name(result));
			return result;
		}
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
	if (interface_number < 0) {
		return;
	}
	(void)libusb_release_interface(handle, interface_number);
	if (was_detached) {
		(void)libusb_attach_kernel_driver(handle, interface_number);
	}
}

static int configure_acm(libusb_device_handle *handle, int control_interface,
			 unsigned int timeout_ms)
{
	unsigned char line_coding[7] = {
	    0x00, 0xc2, 0x01, 0x00, /* 115200, little endian */
	    0x00,		    /* one stop bit */
	    0x00,		    /* no parity */
	    0x08,		    /* eight data bits */
	};
	int result;

	if (control_interface < 0) {
		return LIBUSB_SUCCESS;
	}
	result = libusb_control_transfer(
	    handle,
	    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE,
	    USB_CDC_REQ_SET_LINE_CODING, 0, (uint16_t)control_interface,
	    line_coding, sizeof(line_coding), timeout_ms);
	if (result < 0) {
		return result;
	}
	result = libusb_control_transfer(
	    handle,
	    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE,
	    USB_CDC_REQ_SET_CONTROL_LINE_STATE,
	    USB_CDC_CONTROL_DTR | USB_CDC_CONTROL_RTS,
	    (uint16_t)control_interface, NULL, 0, timeout_ms);
	return result < 0 ? result : LIBUSB_SUCCESS;
}

static int write_all(int fd, const unsigned char *data, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, data, size);
		if (written > 0) {
			data += written;
			size -= (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR) {
			continue;
		}
		return -1;
	}
	return 0;
}

static void *reader_thread(void *opaque)
{
	struct reader_state *state = opaque;
	unsigned char buffer[TRANSFER_BUFFER_SIZE];

	while (!atomic_load(state->stop)) {
		int transferred = 0;
		int result = libusb_bulk_transfer(
		    state->handle, state->endpoint, buffer, sizeof(buffer),
		    &transferred, state->timeout_ms);

		if (transferred > 0 && write_all(STDOUT_FILENO, buffer,
						 (size_t)transferred) != 0) {
			atomic_store(&state->result, LIBUSB_ERROR_IO);
			atomic_store(state->stop, true);
			break;
		}
		if (result == LIBUSB_SUCCESS ||
		    result == LIBUSB_ERROR_TIMEOUT ||
		    result == LIBUSB_ERROR_INTERRUPTED) {
			continue;
		}
		atomic_store(&state->result, result);
		atomic_store(state->stop, true);
		break;
	}
	return NULL;
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
		if (result == LIBUSB_SUCCESS) {
			continue;
		}
		if (result == LIBUSB_ERROR_TIMEOUT && transferred > 0) {
			continue;
		}
		return result;
	}
	return LIBUSB_SUCCESS;
}

static bool safe_tmp_path(const char *path)
{
	const char *name;

	if (path == NULL || strncmp(path, "/tmp/", 5) != 0 || path[5] == '\0' ||
	    strlen(path) > 128) {
		return false;
	}
	name = path + 5;
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		return false;
	}
	while (*name != '\0') {
		unsigned char character = (unsigned char)*name++;
		if (!(character >= 'a' && character <= 'z') &&
		    !(character >= 'A' && character <= 'Z') &&
		    !(character >= '0' && character <= '9') &&
		    character != '.' && character != '_' && character != '-') {
			return false;
		}
	}
	return true;
}

static int hash_file(FILE *file, char hex[65], uint64_t *byte_count,
		     uint64_t maximum_bytes)
{
	static const char digits[] = "0123456789abcdef";
	struct sha256_state state;
	unsigned char buffer[TRANSFER_BUFFER_SIZE];
	unsigned char digest[32];
	size_t size;
	unsigned int index;

	sha256_init(&state);
	*byte_count = 0;
	for (;;) {
		size = fread(buffer, 1, sizeof(buffer), file);
		if ((uint64_t)size > maximum_bytes - *byte_count) {
			return 1;
		}
		sha256_update(&state, buffer, size);
		*byte_count += size;
		if (size < sizeof(buffer)) {
			break;
		}
	}
	if (ferror(file)) {
		return -1;
	}
	sha256_finish(&state, digest);
	for (index = 0; index < sizeof(digest); ++index) {
		hex[index * 2] = digits[digest[index] >> 4];
		hex[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	hex[64] = '\0';
	return fseek(file, 0, SEEK_SET);
}

static size_t encode_base64_line(const unsigned char *input, size_t size,
				 unsigned char output[77])
{
	static const unsigned char table[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t input_index = 0;
	size_t output_index = 0;

	while (input_index < size) {
		uint32_t value = (uint32_t)input[input_index++] << 16;
		bool second = input_index < size;
		bool third;

		if (second) {
			value |= (uint32_t)input[input_index++] << 8;
		}
		third = input_index < size;
		if (third) {
			value |= input[input_index++];
		}
		output[output_index++] = table[(value >> 18) & 0x3f];
		output[output_index++] = table[(value >> 12) & 0x3f];
		output[output_index++] =
		    second ? table[(value >> 6) & 0x3f] : '=';
		output[output_index++] = third ? table[value & 0x3f] : '=';
	}
	output[output_index++] = '\n';
	return output_index;
}

static bool response_contains(const char *response, size_t response_size,
			      const char *marker)
{
	size_t marker_size = strlen(marker);
	size_t index;

	if (marker_size > response_size) {
		return false;
	}
	for (index = 0; index + marker_size <= response_size; ++index) {
		if (memcmp(response + index, marker, marker_size) == 0) {
			return true;
		}
	}
	return false;
}

static int wait_for_upload_result(libusb_device_handle *handle,
				  uint8_t endpoint,
				  unsigned int transfer_timeout_ms,
				  const char *success_marker,
				  const char *failure_marker,
				  bool *failure_seen)
{
	unsigned char input[TRANSFER_BUFFER_SIZE];
	char response[8192];
	size_t response_size = 0;
	uint64_t deadline = monotonic_milliseconds() + 60000u;

	*failure_seen = false;
	while (!signal_requested && monotonic_milliseconds() < deadline) {
		int transferred = 0;
		int result =
		    libusb_bulk_transfer(handle, endpoint, input, sizeof(input),
					 &transferred, transfer_timeout_ms);

		if (transferred > 0) {
			size_t keep = (size_t)transferred;
			if (write_all(STDOUT_FILENO, input, keep) != 0) {
				return LIBUSB_ERROR_IO;
			}
			if (keep >= sizeof(response)) {
				memcpy(response,
				       input + keep - sizeof(response),
				       sizeof(response));
				response_size = sizeof(response);
			} else {
				if (response_size + keep > sizeof(response)) {
					size_t discard = response_size + keep -
							 sizeof(response);
					memmove(response, response + discard,
						response_size - discard);
					response_size -= discard;
				}
				memcpy(response + response_size, input, keep);
				response_size += keep;
			}
			if (response_contains(response, response_size,
					      failure_marker)) {
				*failure_seen = true;
				return LIBUSB_ERROR_IO;
			}
			if (response_contains(response, response_size,
					      success_marker)) {
				return LIBUSB_SUCCESS;
			}
		}
		if (result == LIBUSB_SUCCESS ||
		    result == LIBUSB_ERROR_TIMEOUT ||
		    result == LIBUSB_ERROR_INTERRUPTED) {
			continue;
		}
		return result;
	}
	return signal_requested ? LIBUSB_ERROR_INTERRUPTED
				: LIBUSB_ERROR_TIMEOUT;
}

static int wait_for_upload_prompts(libusb_device_handle *handle,
				   uint8_t endpoint,
				   unsigned int transfer_timeout_ms,
				   const char *marker, unsigned int expected)
{
	unsigned char input[TRANSFER_BUFFER_SIZE];
	size_t marker_size = strlen(marker);
	size_t matched = 0;
	unsigned int seen = 0;
	uint64_t deadline = monotonic_milliseconds() + 60000u;

	if (marker_size == 0 || expected == 0) {
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	while (!signal_requested && monotonic_milliseconds() < deadline) {
		int transferred = 0;
		int result =
		    libusb_bulk_transfer(handle, endpoint, input, sizeof(input),
					 &transferred, transfer_timeout_ms);
		int index;

		if (transferred > 0) {
			if (write_all(STDOUT_FILENO, input,
				      (size_t)transferred) != 0) {
				return LIBUSB_ERROR_IO;
			}
			for (index = 0; index < transferred; ++index) {
				unsigned char byte = input[index];

				if (byte == (unsigned char)marker[matched]) {
					++matched;
				} else {
					matched =
					    byte == (unsigned char)marker[0]
						? 1u
						: 0u;
				}
				if (matched == marker_size) {
					matched = 0;
					if (++seen == expected) {
						return LIBUSB_SUCCESS;
					}
				}
			}
		}
		if (result == LIBUSB_SUCCESS ||
		    result == LIBUSB_ERROR_TIMEOUT ||
		    result == LIBUSB_ERROR_INTERRUPTED) {
			continue;
		}
		return result;
	}
	return signal_requested ? LIBUSB_ERROR_INTERRUPTED
				: LIBUSB_ERROR_TIMEOUT;
}

static int upload_file(libusb_device_handle *handle,
		       const struct endpoint_pair *pair,
		       const struct options *options)
{
	FILE *file = NULL;
	struct stat status;
	char hash[65];
	char nonce[40];
	char delimiter[96];
	char command[1800];
	char ready_marker[96];
	char success_marker[160];
	char failure_marker[128];
	unsigned char input[57];
	unsigned char encoded[77];
	uint64_t byte_count;
	uint64_t sent_bytes = 0;
	unsigned int window_lines = 0;
	int result = LIBUSB_ERROR_OTHER;
	int hash_result;
	int command_size;
	size_t size;
	bool device_echo_disabled = false;
	bool remote_failure = false;

	if (!safe_tmp_path(options->upload_remote)) {
		fprintf(stderr,
			"fplinux-usb-console: upload destination must be one "
			"direct "
			"/tmp/FILE path using only letters, digits, '.', '_', "
			"'-': %s\n",
			options->upload_remote);
		return LIBUSB_ERROR_INVALID_PARAM;
	}
	file = fopen(options->upload_local, "rb");
	if (file == NULL) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: cannot open upload source %s: %s\n",
		    options->upload_local, strerror(errno));
		return LIBUSB_ERROR_NOT_FOUND;
	}
	if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode)) {
		fprintf(stderr,
			"fplinux-usb-console: upload source is not a regular "
			"file: %s\n",
			options->upload_local);
		goto cleanup;
	}
	if ((uint64_t)status.st_size > MAX_UPLOAD_BYTES) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: upload exceeds the 8 MiB RAM-safety "
		    "limit: %s\n",
		    options->upload_local);
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	hash_result = hash_file(file, hash, &byte_count, MAX_UPLOAD_BYTES);
	if (hash_result > 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: upload exceeds the 8 MiB RAM-safety "
		    "limit while reading: %s\n",
		    options->upload_local);
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	if (hash_result < 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: cannot hash upload source %s: %s\n",
		    options->upload_local, strerror(errno));
		goto cleanup;
	}
	if (snprintf(nonce, sizeof(nonce), "%016llx%08lx%.8s",
		     (unsigned long long)monotonic_milliseconds(),
		     (unsigned long)getpid(), hash) >= (int)sizeof(nonce) ||
	    snprintf(delimiter, sizeof(delimiter), "FPLINUX_DATA_%.32s",
		     hash) >= (int)sizeof(delimiter)) {
		fprintf(stderr, "fplinux-usb-console: upload destination path "
				"is too long\n");
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	snprintf(ready_marker, sizeof(ready_marker), "FPLINUX_UPLOAD_READY:%s",
		 nonce);
	snprintf(failure_marker, sizeof(failure_marker),
		 "FPLINUX_UPLOAD_SETUP_FAILED:%s", nonce);
	command_size =
	    snprintf(command, sizeof(command),
		     "if stty -echo && "
		     "fplinux_tmp=$(mktemp /tmp/.fplinux-upload.XXXXXX); then "
		     "fplinux_old_ps2=$PS2; PS2='.'; "
		     "read fplinux_ready; "
		     "printf '\\nFPLINUX_UPLOAD_%%s:%%s\\n' "
		     "READY \"$fplinux_ready\"; "
		     "else printf '\\nFPLINUX_UPLOAD_%%s:%%s\\n' "
		     "SETUP_FAILED '%s'; PS2=$fplinux_old_ps2; stty echo; fi\n"
		     "%s\n",
		     nonce, nonce);
	if (command_size < 0 || command_size >= (int)sizeof(command)) {
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	fprintf(stderr,
		"fplinux-usb-console: uploading %s (%llu bytes, sha256=%s) "
		"to %s\n",
		options->upload_local, (unsigned long long)byte_count, hash,
		options->upload_remote);
	result = send_bytes(handle, pair->endpoint_out,
			    (const unsigned char *)command,
			    (size_t)command_size, options->timeout_ms);
	if (result != LIBUSB_SUCCESS) {
		goto cleanup;
	}
	device_echo_disabled = true;
	result = wait_for_upload_result(handle, pair->endpoint_in,
					options->timeout_ms, ready_marker,
					failure_marker, &remote_failure);
	if (result != LIBUSB_SUCCESS) {
		if (remote_failure) {
			device_echo_disabled = false;
		}
		fprintf(
		    stderr,
		    "\nfplinux-usb-console: device shell did not acknowledge "
		    "upload setup\n");
		goto cleanup;
	}
	command_size = snprintf(
	    command, sizeof(command),
	    "umask 077; base64 -d > \"$fplinux_tmp\" <<'%s'\n", delimiter);
	if (command_size < 0 || command_size >= (int)sizeof(command)) {
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	result = send_bytes(handle, pair->endpoint_out,
			    (const unsigned char *)command,
			    (size_t)command_size, options->timeout_ms);
	if (result != LIBUSB_SUCCESS) {
		goto cleanup;
	}
	result = wait_for_upload_prompts(handle, pair->endpoint_in,
					 options->timeout_ms, ".", 1);
	if (result != LIBUSB_SUCCESS) {
		fprintf(stderr,
			"\nfplinux-usb-console: device shell did not enter "
			"upload data mode\n");
		goto cleanup;
	}
	while (!signal_requested &&
	       (size = fread(input, 1, sizeof(input), file)) > 0) {
		size_t encoded_size = encode_base64_line(input, size, encoded);

		if ((uint64_t)size > MAX_UPLOAD_BYTES - sent_bytes) {
			fprintf(stderr,
				"fplinux-usb-console: upload source grew "
				"beyond the "
				"8 MiB RAM-safety limit while sending: %s\n",
				options->upload_local);
			result = LIBUSB_ERROR_INVALID_PARAM;
			goto cleanup;
		}
		sent_bytes += size;
		result = send_bytes(handle, pair->endpoint_out, encoded,
				    encoded_size, options->timeout_ms);
		if (result != LIBUSB_SUCCESS) {
			goto cleanup;
		}
		++window_lines;
		if (window_lines == UPLOAD_WINDOW_LINES) {
			result = wait_for_upload_prompts(
			    handle, pair->endpoint_in, options->timeout_ms, ".",
			    window_lines);
			if (result != LIBUSB_SUCCESS) {
				fprintf(stderr, "\nfplinux-usb-console: device "
						"shell stopped "
						"consuming upload data\n");
				goto cleanup;
			}
			window_lines = 0;
		}
	}
	if (signal_requested) {
		result = LIBUSB_ERROR_INTERRUPTED;
		goto cleanup;
	}
	if (ferror(file)) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: cannot read upload source %s: %s\n",
		    options->upload_local, strerror(errno));
		result = LIBUSB_ERROR_IO;
		goto cleanup;
	}
	if (window_lines > 0) {
		result = wait_for_upload_prompts(handle, pair->endpoint_in,
						 options->timeout_ms, ".",
						 window_lines);
		if (result != LIBUSB_SUCCESS) {
			fprintf(stderr,
				"\nfplinux-usb-console: device shell stopped "
				"consuming upload data\n");
			goto cleanup;
		}
	}
	command_size = snprintf(
	    command, sizeof(command),
	    "%s\n"
	    "fplinux_got=$(sha256sum \"$fplinux_tmp\"); "
	    "fplinux_got=${fplinux_got%%%% *}; "
	    "fplinux_size=$(wc -c < \"$fplinux_tmp\"); "
	    "if [ \"$fplinux_got\" = '%s' ] && "
	    "[ ! -d '%s' ] && [ ! -L '%s' ] && "
	    "mv -f \"$fplinux_tmp\" '%s'; then "
	    "PS2=$fplinux_old_ps2; "
	    "if stty echo; then "
	    "printf '\\nFPLINUX_UPLOAD_%%s:%%s:%%s\\n' OK '%s' '%s'; "
	    "else printf '\\nFPLINUX_UPLOAD_%%s:%%s\\n' FAIL '%s'; fi; "
	    "else rm -f \"$fplinux_tmp\"; PS2=$fplinux_old_ps2; stty echo; "
	    "printf '\\nFPLINUX_UPLOAD_%%s:%%s:%%s:%%s\\n' "
	    "FAIL '%s' \"$fplinux_got\" \"$fplinux_size\"; fi\n",
	    delimiter, hash, options->upload_remote, options->upload_remote,
	    options->upload_remote, nonce, hash, nonce, nonce);
	if (command_size < 0 || command_size >= (int)sizeof(command)) {
		result = LIBUSB_ERROR_INVALID_PARAM;
		goto cleanup;
	}
	result = send_bytes(handle, pair->endpoint_out,
			    (const unsigned char *)command,
			    (size_t)command_size, options->timeout_ms);
	if (result != LIBUSB_SUCCESS) {
		goto cleanup;
	}
	snprintf(success_marker, sizeof(success_marker),
		 "FPLINUX_UPLOAD_OK:%s:%s", nonce, hash);
	snprintf(failure_marker, sizeof(failure_marker),
		 "FPLINUX_UPLOAD_FAIL:%s", nonce);
	result = wait_for_upload_result(handle, pair->endpoint_in,
					options->timeout_ms, success_marker,
					failure_marker, &remote_failure);
	if (result == LIBUSB_SUCCESS) {
		device_echo_disabled = false;
		fprintf(
		    stderr,
		    "\nfplinux-usb-console: upload verified on device: %s\n",
		    options->upload_remote);
	} else if (result == LIBUSB_ERROR_TIMEOUT) {
		fprintf(
		    stderr,
		    "\nfplinux-usb-console: timed out waiting for device-side "
		    "SHA-256 verification\n");
	} else if (result == LIBUSB_ERROR_IO) {
		if (remote_failure) {
			device_echo_disabled = false;
		}
		fprintf(
		    stderr,
		    "\nfplinux-usb-console: device-side SHA-256 verification "
		    "failed\n");
	}

cleanup:
	if (device_echo_disabled) {
		int cleanup_size = snprintf(command, sizeof(command),
					    "\003\n%s\nrm -f \"$fplinux_tmp\"; "
					    "PS2=$fplinux_old_ps2; stty echo\n",
					    delimiter);
		if (cleanup_size > 0 && cleanup_size < (int)sizeof(command)) {
			(void)send_bytes(handle, pair->endpoint_out,
					 (const unsigned char *)command,
					 (size_t)cleanup_size,
					 options->timeout_ms);
		}
	}
	if (file != NULL) {
		fclose(file);
	}
	return result;
}

static int self_test(void)
{
	static const char empty_hash[] = "e3b0c44298fc1c149afbf4c8996fb924"
					 "27ae41e4649b934ca495991b7852b855";
	static const char abc_hash[] = "ba7816bf8f01cfea414140de5dae2223"
				       "b00361a396177a9cb410ff61f20015ad";
	static const unsigned char expected_base64[] = "Zm9vYmFy\n";
	static const unsigned char expected_base64_one[] = "AA==\n";
	static const unsigned char expected_base64_two[] = "AAA=\n";
	static const unsigned char zeroes[2];
	static const char digits[] = "0123456789abcdef";
	struct sha256_state state;
	unsigned char digest[32];
	unsigned char base64[77];
	char hash[65];
	unsigned int index;
	size_t size;

	sha256_init(&state);
	sha256_finish(&state, digest);
	for (index = 0; index < sizeof(digest); ++index) {
		hash[index * 2] = digits[digest[index] >> 4];
		hash[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	hash[64] = '\0';
	if (strcmp(hash, empty_hash) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: self-test failed: SHA-256(empty)\n");
		return 1;
	}
	sha256_init(&state);
	sha256_update(&state, (const unsigned char *)"abc", 3);
	sha256_finish(&state, digest);
	for (index = 0; index < sizeof(digest); ++index) {
		hash[index * 2] = digits[digest[index] >> 4];
		hash[index * 2 + 1] = digits[digest[index] & 0x0f];
	}
	if (memcmp(hash, abc_hash, 64) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: self-test failed: SHA-256(abc)\n");
		return 1;
	}
	size = encode_base64_line((const unsigned char *)"foobar", 6, base64);
	if (size != sizeof(expected_base64) - 1 ||
	    memcmp(base64, expected_base64, size) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: self-test failed: base64 codec\n");
		return 1;
	}
	size = encode_base64_line(zeroes, 1, base64);
	if (size != sizeof(expected_base64_one) - 1 ||
	    memcmp(base64, expected_base64_one, size) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: self-test failed: base64 one-byte "
		    "padding\n");
		return 1;
	}
	size = encode_base64_line(zeroes, 2, base64);
	if (size != sizeof(expected_base64_two) - 1 ||
	    memcmp(base64, expected_base64_two, size) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: self-test failed: base64 two-byte "
		    "padding\n");
		return 1;
	}
	if (!safe_tmp_path("/tmp/test.bin") ||
	    safe_tmp_path("/tmp/fplinux/test.bin") ||
	    safe_tmp_path("/tmp/../nv.bin") || safe_tmp_path("/data/nv.bin") ||
	    safe_tmp_path("/tmp/double//slash")) {
		fprintf(stderr, "fplinux-usb-console: self-test failed: upload "
				"path policy\n");
		return 1;
	}
	puts("SELFTEST OK");
	return 0;
}

static int forward_console(libusb_device_handle *handle,
			   const struct endpoint_pair *pair,
			   const struct options *options)
{
	atomic_bool stop = false;
	pthread_t reader;
	struct reader_state state = {
	    .handle = handle,
	    .endpoint = pair->endpoint_in,
	    .timeout_ms = options->timeout_ms,
	    .stop = &stop,
	};
	unsigned char buffer[TRANSFER_BUFFER_SIZE];
	int result = LIBUSB_SUCCESS;
	bool local_escape_requested = false;

	atomic_init(&state.result, LIBUSB_SUCCESS);
	if (pthread_create(&reader, NULL, reader_thread, &state) != 0) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: cannot start USB reader thread\n");
		return LIBUSB_ERROR_OTHER;
	}

	while (!atomic_load(&stop) && !signal_requested) {
		struct pollfd input = {
		    .fd = STDIN_FILENO,
		    .events = POLLIN | POLLHUP,
		};
		int polled = poll(&input, 1, 100);

		if (polled < 0) {
			if (errno == EINTR) {
				continue;
			}
			result = LIBUSB_ERROR_IO;
			break;
		}
		if (polled == 0) {
			continue;
		}
		if (input.revents & POLLIN) {
			ssize_t size =
			    read(STDIN_FILENO, buffer, sizeof(buffer));
			if (size > 0) {
				unsigned char *escape =
				    memchr(buffer, LOCAL_ESCAPE, (size_t)size);
				size_t send_size =
				    escape == NULL ? (size_t)size
						   : (size_t)(escape - buffer);

				result = send_bytes(handle, pair->endpoint_out,
						    buffer, send_size,
						    options->timeout_ms);
				if (result != LIBUSB_SUCCESS) {
					break;
				}
				if (escape != NULL) {
					local_escape_requested = true;
					break;
				}
			} else if (size == 0) {
				break;
			} else if (errno != EINTR && errno != EAGAIN) {
				result = LIBUSB_ERROR_IO;
				break;
			}
		}
		if (input.revents & (POLLHUP | POLLERR | POLLNVAL)) {
			break;
		}
	}

	if (local_escape_requested) {
		fprintf(stderr, "\nfplinux-usb-console: local Ctrl-] escape\n");
	}
	if (!local_escape_requested && !signal_requested &&
	    result == LIBUSB_SUCCESS && options->linger_ms > 0) {
		uint64_t deadline =
		    monotonic_milliseconds() + options->linger_ms;
		while (!atomic_load(&stop) &&
		       monotonic_milliseconds() < deadline) {
			usleep(10000);
		}
	}
	atomic_store(&stop, true);
	(void)pthread_join(reader, NULL);
	if (result == LIBUSB_SUCCESS) {
		result = atomic_load(&state.result);
	}
	return result;
}

static int set_raw_terminal(void)
{
	struct termios raw;

	if (!isatty(STDIN_FILENO)) {
		return 0;
	}
	if (tcgetattr(STDIN_FILENO, &saved_terminal) != 0) {
		return -1;
	}
	raw = saved_terminal;
	cfmakeraw(&raw);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
		return -1;
	}
	terminal_is_raw = true;
	return 0;
}

static void print_libusb_failure(const char *operation, int result)
{
	fprintf(stderr, "fplinux-usb-console: %s: %s", operation,
		libusb_error_name(result));
	if (result == LIBUSB_ERROR_ACCESS) {
		fprintf(
		    stderr,
		    " (USB permission denied; install a suitable udev rule or "
		    "run with explicit device access)");
	} else if (result == LIBUSB_ERROR_BUSY) {
		fprintf(
		    stderr,
		    " (device/interface is busy; specify --bus and --address "
		    "or allow kernel-driver detachment)");
	} else if (result == LIBUSB_ERROR_NO_DEVICE) {
		fprintf(stderr, " (the gadget disconnected)");
	}
	fputc('\n', stderr);
}

int main(int argc, char **argv)
{
	struct options options;
	libusb_context *context = NULL;
	libusb_device *device = NULL;
	libusb_device_handle *handle = NULL;
	struct endpoint_pair pair = {
	    .interface_number = -1,
	    .control_interface = -1,
	};
	bool data_detached = false;
	bool control_detached = false;
	bool data_claimed = false;
	bool control_claimed = false;
	int result;
	int exit_status = 1;

	if (parse_options(argc, argv, &options) != 0) {
		return 2;
	}
	if (options.self_test) {
		return self_test();
	}
	if (options.upload_local != NULL &&
	    !safe_tmp_path(options.upload_remote)) {
		fprintf(stderr,
			"fplinux-usb-console: upload destination must be one "
			"direct "
			"/tmp/FILE path using only letters, digits, '.', '_', "
			"'-': %s\n",
			options.upload_remote);
		return 2;
	}
	result = libusb_init(&context);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot initialize libusb", result);
		return 1;
	}
	if (options.list_devices) {
		exit_status = list_devices(context);
		goto cleanup;
	}

	fprintf(stderr,
		"fplinux-usb-console: waiting for %04x:%04x (up to %u s)\n",
		options.vid, options.pid, options.wait_seconds);
	result = wait_for_device(context, &options, &device);
	if (result == LIBUSB_ERROR_BUSY) {
		fprintf(
		    stderr,
		    "fplinux-usb-console: multiple %04x:%04x devices found; "
		    "select one with --bus and --address (see --list)\n",
		    options.vid, options.pid);
		goto cleanup;
	}
	if (result != LIBUSB_SUCCESS) {
		if (result == LIBUSB_ERROR_NO_DEVICE) {
			fprintf(
			    stderr,
			    "fplinux-usb-console: USB gadget %04x:%04x did not "
			    "appear within %u seconds\n",
			    options.vid, options.pid, options.wait_seconds);
		} else {
			print_libusb_failure("cannot enumerate the USB gadget",
					     result);
		}
		goto cleanup;
	}

	result = libusb_open(device, &handle);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot open the USB gadget", result);
		goto cleanup;
	}
	result = find_bulk_endpoints(device, options.interface_number, &pair);
	if (result != LIBUSB_SUCCESS) {
		if (result == LIBUSB_ERROR_BUSY &&
		    options.interface_number < 0) {
			fprintf(
			    stderr,
			    "fplinux-usb-console: multiple equally suitable "
			    "bulk "
			    "interfaces found; select one with --interface\n");
			goto cleanup;
		}
		fprintf(
		    stderr,
		    "fplinux-usb-console: no interface with bulk IN and OUT "
		    "endpoints was found");
		if (options.interface_number >= 0) {
			fprintf(stderr, " on interface %d",
				options.interface_number);
		}
		fputc('\n', stderr);
		goto cleanup;
	}

	if (pair.control_interface >= 0 &&
	    pair.control_interface != pair.interface_number) {
		result = claim_interface(handle, pair.control_interface,
					 options.detach_kernel_driver,
					 &control_detached);
		if (result != LIBUSB_SUCCESS) {
			print_libusb_failure(
			    "cannot claim CDC control interface", result);
			goto cleanup;
		}
		control_claimed = true;
	}
	result = claim_interface(handle, pair.interface_number,
				 options.detach_kernel_driver, &data_detached);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot claim USB data interface", result);
		goto cleanup;
	}
	data_claimed = true;
	if (pair.alternate_setting != 0) {
		result = libusb_set_interface_alt_setting(
		    handle, pair.interface_number, pair.alternate_setting);
		if (result != LIBUSB_SUCCESS) {
			print_libusb_failure(
			    "cannot select USB alternate setting", result);
			goto cleanup;
		}
	}
	result =
	    configure_acm(handle, pair.control_interface, options.timeout_ms);
	if (result != LIBUSB_SUCCESS) {
		print_libusb_failure("cannot initialize CDC ACM control state",
				     result);
		goto cleanup;
	}

	fprintf(stderr,
		"fplinux-usb-console: connected bus=%03u address=%03u "
		"interface=%d bulk-in=0x%02x bulk-out=0x%02x\n",
		libusb_get_bus_number(device),
		libusb_get_device_address(device), pair.interface_number,
		pair.endpoint_in, pair.endpoint_out);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	if (options.upload_local != NULL) {
		result = upload_file(handle, &pair, &options);
		if (result != LIBUSB_SUCCESS) {
			if (result != LIBUSB_ERROR_IO &&
			    result != LIBUSB_ERROR_TIMEOUT &&
			    result != LIBUSB_ERROR_INVALID_PARAM &&
			    result != LIBUSB_ERROR_NOT_FOUND) {
				print_libusb_failure("USB upload failed",
						     result);
			}
			goto cleanup;
		}
		exit_status = 0;
		goto cleanup;
	}
	fprintf(stderr, "fplinux-usb-console: forwarding stdin/stdout; press "
			"Ctrl-] to exit\n");
	if (set_raw_terminal() != 0) {
		fprintf(stderr,
			"fplinux-usb-console: cannot put stdin terminal into "
			"raw mode: "
			"%s\n",
			strerror(errno));
		goto cleanup;
	}
	atexit(restore_terminal);

	result = forward_console(handle, &pair, &options);
	restore_terminal();
	if (result != LIBUSB_SUCCESS && !signal_requested) {
		print_libusb_failure("USB console transfer failed", result);
		goto cleanup;
	}
	exit_status = 0;

cleanup:
	restore_terminal();
	if (handle != NULL && pair.control_interface >= 0) {
		(void)libusb_control_transfer(
		    handle,
		    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
			LIBUSB_RECIPIENT_INTERFACE,
		    USB_CDC_REQ_SET_CONTROL_LINE_STATE, 0,
		    (uint16_t)pair.control_interface, NULL, 0,
		    options.timeout_ms);
	}
	if (handle != NULL && data_claimed) {
		release_interface(handle, pair.interface_number, data_detached);
	}
	if (handle != NULL && control_claimed) {
		release_interface(handle, pair.control_interface,
				  control_detached);
	}
	if (handle != NULL) {
		libusb_close(handle);
	}
	if (device != NULL) {
		libusb_unref_device(device);
	}
	libusb_exit(context);
	return exit_status;
}
