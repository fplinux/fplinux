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

#define DEFAULT_CHANNEL "/dev/ttyGS1"
#define KEYBOARD_LEASE_MS 1000
#define LINE_BYTES 64
#define VENDOR 0x1d6b
#define PRODUCT 0x0104

static int open_channel(const char *path)
{
	struct termios raw;
	int fd = open(path, O_RDONLY | O_NOCTTY);

	if (fd < 0) {
		perror(path);
		return -1;
	}
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
		perror("/dev/uinput");
		return -1;
	}
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_REP) != 0) {
		perror("UI_SET_EVBIT");
		close(fd);
		return -1;
	}
	for (code = KEY_ESC; code < KEY_CNT; code++) {
		if (ioctl(fd, UI_SET_KEYBIT, code) != 0) {
			perror("UI_SET_KEYBIT");
			close(fd);
			return -1;
		}
	}

	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = VENDOR;
	setup.id.product = PRODUCT;
	strncpy(setup.name, "FPLinux host keyboard", UINPUT_MAX_NAME_SIZE - 1);
	if (ioctl(fd, UI_DEV_SETUP, &setup) != 0 ||
	    ioctl(fd, UI_DEV_CREATE) != 0) {
		perror("UI_DEV_CREATE");
		close(fd);
		return -1;
	}
	return fd;
}

static bool inject(int device, unsigned int type, unsigned int code, int value)
{
	struct input_event event;

	memset(&event, 0, sizeof(event));
	event.type = (unsigned short)type;
	event.code = (unsigned short)code;
	event.value = value;
	if (write(device, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
		perror("uinput write");
		return false;
	}
	return true;
}

static void release_keys(int device, bool pressed[KEY_CNT])
{
	unsigned int code;
	bool any = false;

	for (code = 0; code < KEY_CNT; ++code) {
		if (!pressed[code])
			continue;
		any = true;
		if (inject(device, EV_KEY, code, 0))
			pressed[code] = false;
	}
	if (any)
		inject(device, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : DEFAULT_CHANNEL;
	bool pressed[KEY_CNT] = {false};
	char line[LINE_BYTES];
	size_t filled = 0;
	int channel;
	int device;

	device = open_device();
	if (device < 0)
		return 1;
	printf("fplinux-input: %s -> uinput\n", path);
	fflush(stdout);

	/* Keep the uinput device stable while the host reconnects. */
	for (;;) {
		channel = open_channel(path);
		if (channel < 0) {
			sleep(1);
			continue;
		}
		filled = 0;
		for (;;) {
			struct pollfd waiting = {
			    .fd = channel,
			    .events = POLLIN,
			};
			char byte;
			ssize_t got;
			int ready;

			do {
				ready = poll(&waiting, 1, KEYBOARD_LEASE_MS);
			} while (ready < 0 && errno == EINTR);
			if (ready < 0) {
				perror("poll");
				break;
			}
			if (ready == 0) {
				filled = 0;
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
				perror("read");
				break;
			}
			if (byte != '\n' && byte != '\r') {
				if (filled + 1 < sizeof(line))
					line[filled++] = byte;
				continue;
			}
			line[filled] = '\0';
			filled = 0;
			if (strcmp(line, "reset") == 0) {
				release_keys(device, pressed);
				continue;
			}
			if (line[0] != '\0') {
				unsigned int type;
				unsigned int code;
				int value;

				if (sscanf(line, "%u %u %d", &type, &code,
					   &value) == 3 &&
				    inject(device, type, code, value) &&
				    type == EV_KEY && code < KEY_CNT &&
				    (value == 0 || value == 1))
					pressed[code] = value == 1;
			}
		}
		release_keys(device, pressed);
		close(channel);
		sleep(1);
	}

	ioctl(device, UI_DEV_DESTROY);
	close(device);
	return 0;
}
