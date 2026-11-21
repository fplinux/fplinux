// SPDX-License-Identifier: GPL-2.0-only
/* Relay keyboard event lines from gadget serial through uinput. */
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define FPLINUX_INPUT_DEFAULT_CHANNEL "/dev/ttyGS0"
#define FPLINUX_INPUT_KEYBOARD_LEASE_MS 1000
#define FPLINUX_INPUT_LINE_BYTES 64
#define FPLINUX_INPUT_VENDOR_ID 0x1d6b
#define FPLINUX_INPUT_PRODUCT_ID 0x0104

struct channel_error {
	const char *operation;
	int code;
};

struct keyboard_record {
	char text[FPLINUX_INPUT_LINE_BYTES];
	size_t filled;
	bool discard;
};

static void report_channel_error(struct channel_error *last,
				 const char *operation)
{
	int error = errno;

	if (last->operation && last->code == error &&
	    !strcmp(last->operation, operation))
		return;
	fprintf(stderr, "fplinux-input: cannot %s %s: %s\n", operation,
		FPLINUX_INPUT_DEFAULT_CHANNEL, strerror(error));
	last->operation = operation;
	last->code = error;
}

static int open_channel(const char *path)
{
	struct termios raw;
	int fd = open(path, O_RDONLY | O_NOCTTY);

	if (fd < 0)
		return -1;
	if (tcgetattr(fd, &raw) == 0) {
		raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
				 IGNCR | ICRNL | IXON);
		raw.c_oflag &= ~OPOST;
		raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
		raw.c_cflag = (raw.c_cflag & ~(CSIZE | PARENB)) | CS8;
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(fd, TCSANOW, &raw);
	}
	return fd;
}

static int open_device(void)
{
	struct uinput_setup setup;
	unsigned int code;
	int fd = open("/dev/uinput", O_WRONLY);

	if (fd < 0) {
		perror("fplinux-input: cannot open /dev/uinput");
		return -1;
	}
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_REP) != 0) {
		perror("fplinux-input: cannot set uinput event types");
		close(fd);
		return -1;
	}
	for (code = KEY_ESC; code < KEY_CNT; code++) {
		if (ioctl(fd, UI_SET_KEYBIT, code) != 0) {
			perror("fplinux-input: cannot set uinput keys");
			close(fd);
			return -1;
		}
	}

	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = FPLINUX_INPUT_VENDOR_ID;
	setup.id.product = FPLINUX_INPUT_PRODUCT_ID;
	strncpy(setup.name, "FPLinux host keyboard", UINPUT_MAX_NAME_SIZE - 1);
	if (ioctl(fd, UI_DEV_SETUP, &setup) != 0 ||
	    ioctl(fd, UI_DEV_CREATE) != 0) {
		perror("fplinux-input: cannot create uinput device");
		close(fd);
		return -1;
	}
	return fd;
}

static bool collect_record(struct keyboard_record *record, char byte)
{
	if (byte == '\n' || byte == '\r') {
		bool complete = record->filled != 0 && !record->discard;

		record->text[record->filled] = '\0';
		record->filled = 0;
		record->discard = false;
		return complete;
	}
	if (byte == '\0' || record->filled == sizeof(record->text) - 1)
		record->discard = true;
	if (!record->discard)
		record->text[record->filled++] = byte;
	return false;
}

static bool read_decimal(const char **text, unsigned long *number)
{
	char *end;

	if (**text < '0' || **text > '9')
		return false;
	errno = 0;
	*number = strtoul(*text, &end, 10);
	*text = end;
	return errno == 0;
}

static bool decode_event(const char *text, struct input_event *event)
{
	unsigned long type, code, value;

	if (!read_decimal(&text, &type) || *text++ != ' ' ||
	    !read_decimal(&text, &code) || *text++ != ' ' ||
	    !read_decimal(&text, &value) || *text != '\0')
		return false;
	if (type == EV_KEY) {
		if (code < KEY_ESC || code >= KEY_CNT || value > 2)
			return false;
	} else if (type != EV_SYN || code != SYN_REPORT || value != 0) {
		return false;
	}
	*event = (struct input_event){ .type = type,
				       .code = code,
				       .value = value };
	return true;
}

static bool inject(int device, const struct input_event *event)
{
	if (write(device, event, sizeof(*event)) != (ssize_t)sizeof(*event)) {
		perror("fplinux-input: cannot write uinput event");
		return false;
	}
	return true;
}

static void release_keys(int device, bool pressed[KEY_CNT])
{
	unsigned int code;
	bool any = false;

	for (code = 0; code < KEY_CNT; ++code) {
		const struct input_event release = { .type = EV_KEY,
						     .code = code };

		if (!pressed[code])
			continue;
		any = true;
		if (inject(device, &release))
			pressed[code] = false;
	}
	if (any) {
		const struct input_event sync = { .type = EV_SYN,
						  .code = SYN_REPORT };

		inject(device, &sync);
	}
}

static void apply_record(int device, bool pressed[KEY_CNT], const char *text)
{
	struct input_event event;

	if (strcmp(text, "reset") == 0) {
		release_keys(device, pressed);
		return;
	}
	if (decode_event(text, &event) && inject(device, &event) &&
	    event.type == EV_KEY && event.value != 2)
		pressed[event.code] = event.value == 1;
}

int main(void)
{
	const char *path = FPLINUX_INPUT_DEFAULT_CHANNEL;
	struct channel_error channel_error = {};
	bool pressed[KEY_CNT] = { false };
	int channel;
	int device;

	device = open_device();
	if (device < 0)
		return 1;
	printf("fplinux-input: %s -> uinput\n", path);
	fflush(stdout);

	/* Keep the uinput device stable while the host reconnects. */
	for (;;) {
		struct keyboard_record record = {};

		channel = open_channel(path);
		if (channel < 0) {
			report_channel_error(&channel_error, "open");
			sleep(1);
			continue;
		}
		for (;;) {
			struct pollfd waiting = {
				.fd = channel,
				.events = POLLIN,
			};
			char byte;
			ssize_t got;
			int ready;

			do {
				ready = poll(&waiting, 1,
					     FPLINUX_INPUT_KEYBOARD_LEASE_MS);
			} while (ready < 0 && errno == EINTR);
			if (ready < 0) {
				report_channel_error(&channel_error, "poll");
				break;
			}
			if (ready == 0) {
				/* A suffix after the lease is not a new record. */
				if (record.filled != 0)
					record.discard = true;
				release_keys(device, pressed);
				continue;
			}
			if (!(waiting.revents & POLLIN))
				break;
			do {
				got = read(channel, &byte, 1);
			} while (got < 0 && errno == EINTR);
			if (got == 0)
				break;
			if (got < 0) {
				report_channel_error(&channel_error, "read");
				break;
			}
			if (channel_error.operation) {
				printf("fplinux-input: input channel readable\n");
				fflush(stdout);
				channel_error.operation = NULL;
			}
			if (collect_record(&record, byte))
				apply_record(device, pressed, record.text);
		}
		release_keys(device, pressed);
		close(channel);
		sleep(1);
	}
}
