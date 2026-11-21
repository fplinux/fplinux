// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "fplinux-cli.h"
#include "fplinux-audio-protocol.h"
#include "fplinux-audio-radio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define RADIO_DEVICE "/dev/radio0"
#define FM_V4L2_UNITS_PER_100KHZ 1600U
#define FM_SEEK_SPACING_HZ 100000U
#define FM_CHANNEL_COUNT \
	(FPLINUX_AUDIO_FM_HIGH_100KHZ - FPLINUX_AUDIO_FM_LOW_100KHZ + 1U)

enum command { COMMAND_SCAN, COMMAND_PLAY, COMMAND_NONE };

struct arguments {
	enum command command;
	unsigned int frequency_100khz;
	unsigned int seconds;
};

static volatile sig_atomic_t stop_signal;

static void request_stop(int signal_number)
{
	stop_signal = signal_number;
}

static bool parse_frequency(const char *text, unsigned int *frequency_100khz)
{
	unsigned int mhz = 0;
	unsigned int tenth = 0;
	const char *cursor = text;

	if (*cursor < '0' || *cursor > '9')
		return false;
	while (*cursor >= '0' && *cursor <= '9') {
		mhz = mhz * 10U + (unsigned int)(*cursor - '0');
		if (mhz > FPLINUX_AUDIO_FM_HIGH_100KHZ / 10U)
			return false;
		++cursor;
	}
	if (*cursor == '.') {
		++cursor;
		if (*cursor < '0' || *cursor > '9')
			return false;
		tenth = (unsigned int)(*cursor++ - '0');
		while (*cursor == '0')
			++cursor;
	}
	if (*cursor != '\0' ||
	    mhz * 10U + tenth < FPLINUX_AUDIO_FM_LOW_100KHZ ||
	    mhz * 10U + tenth > FPLINUX_AUDIO_FM_HIGH_100KHZ)
		return false;
	*frequency_100khz = mhz * 10U + tenth;
	return true;
}

static const char *parse_option(size_t option, const char *value, void *data)
{
	struct arguments *arguments = data;

	if (option == 0) {
		if (!parse_frequency(value, &arguments->frequency_100khz))
			return "frequency must be 87.5 to 108.0 MHz in 0.1 MHz steps";
	} else if (!fplinux_cli_unsigned(value, 1, 86400,
					 &arguments->seconds)) {
		return "seconds must be between 1 and 86400";
	}
	return NULL;
}

static int parse_arguments(int argc, char **argv, struct arguments *arguments)
{
	struct fplinux_cli_option root_options[] = {
		{
			.metavar = "COMMAND",
			.help = "scan or play",
			.flags = FPLINUX_CLI_REQUIRED,
		},
	};
	struct fplinux_cli_option play_options[] = {
		{
			.metavar = "FREQUENCY",
			.help = "FM frequency in MHz, in 0.1 MHz steps",
			.flags = FPLINUX_CLI_REQUIRED,
		},
		{
			.name = "seconds",
			.metavar = "N",
			.help = "stop after N seconds (default: until interrupted)",
		},
	};
	struct fplinux_cli commands[] = {
		[COMMAND_SCAN] = {
			.command = "scan",
			.description =
				"Find FM station candidates without enabling audio.",
		},
		[COMMAND_PLAY] = {
			.command = "play",
			.description =
				"Tune FM and play through the enabled audio outputs.",
			.options = play_options,
			.option_count = sizeof(play_options) /
					sizeof(play_options[0]),
		},
	};
	struct fplinux_cli root_cli = {
		.program = argv[0],
		.description = "Control the phone's FM radio.",
		.options = root_options,
		.option_count = sizeof(root_options) / sizeof(root_options[0]),
	};
	struct fplinux_cli *selected = NULL;
	enum fplinux_cli_result result;
	enum command command;

	arguments->command = COMMAND_NONE;
	arguments->seconds = 0;
	for (command = COMMAND_SCAN; command < COMMAND_NONE; ++command) {
		if (argc > 1 && !strcmp(argv[1], commands[command].command)) {
			arguments->command = command;
			selected = &commands[command];
			break;
		}
	}
	if (!selected) {
		result = fplinux_cli_parse(&root_cli, argc, argv);
		if (result == FPLINUX_CLI_HELP) {
			fputs("\nCommands:\n", stdout);
			for (command = COMMAND_SCAN; command < COMMAND_NONE;
			     ++command) {
				commands[command].program = argv[0];
				fputs("  ", stdout);
				fplinux_cli_usage(stdout, &commands[command]);
				fputc('\n', stdout);
			}
		}
		if (result != FPLINUX_CLI_READY)
			return result;
		return fplinux_cli_error(&root_cli, "unknown command");
	}
	selected->program = argv[0];
	selected->parse_option =
		arguments->command == COMMAND_PLAY ? parse_option : NULL;
	selected->data = arguments;
	return fplinux_cli_parse(selected, argc - 1, argv + 1);
}

static int open_radio(bool seek)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_tuner tuner = { .index = 0 };
	unsigned int device_caps;
	int fd = open(RADIO_DEVICE, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "fplinux-fm: open %s: %s\n", RADIO_DEVICE,
			strerror(errno));
		return -1;
	}
	if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		fprintf(stderr, "fplinux-fm: VIDIOC_QUERYCAP: %s\n",
			strerror(errno));
		goto fail;
	}
	device_caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS ?
			      capability.device_caps :
			      capability.capabilities;
	if (!(device_caps & V4L2_CAP_RADIO) ||
	    (seek && !(device_caps & V4L2_CAP_HW_FREQ_SEEK))) {
		fputs("fplinux-fm: radio does not support requested FM operation\n",
		      stderr);
		goto fail;
	}
	if (ioctl(fd, VIDIOC_G_TUNER, &tuner) < 0) {
		fprintf(stderr, "fplinux-fm: VIDIOC_G_TUNER: %s\n",
			strerror(errno));
		goto fail;
	}
	if (tuner.type != V4L2_TUNER_RADIO ||
	    !(tuner.capability & V4L2_TUNER_CAP_LOW) ||
	    (seek && !(tuner.capability & V4L2_TUNER_CAP_HWSEEK_BOUNDED))) {
		fputs("fplinux-fm: incompatible FM tuner capabilities\n",
		      stderr);
		goto fail;
	}
	return fd;

fail:
	close(fd);
	return -1;
}

static int set_frequency(int fd, unsigned int frequency_100khz)
{
	struct v4l2_frequency frequency = {
		.tuner = 0,
		.type = V4L2_TUNER_RADIO,
		.frequency = frequency_100khz * FM_V4L2_UNITS_PER_100KHZ,
	};

	if (ioctl(fd, VIDIOC_S_FREQUENCY, &frequency) < 0) {
		fprintf(stderr, "fplinux-fm: VIDIOC_S_FREQUENCY: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

static int scan(int fd)
{
	uint32_t seen[FM_CHANNEL_COUNT];
	size_t count = 0;
	struct v4l2_hw_freq_seek seek = {
		.tuner = 0,
		.type = V4L2_TUNER_RADIO,
		.seek_upward = 1,
		.wrap_around = 0,
		.spacing = FM_SEEK_SPACING_HZ,
		.rangelow =
			FPLINUX_AUDIO_FM_LOW_100KHZ * FM_V4L2_UNITS_PER_100KHZ,
		.rangehigh =
			FPLINUX_AUDIO_FM_HIGH_100KHZ * FM_V4L2_UNITS_PER_100KHZ,
	};

	if (set_frequency(fd, FPLINUX_AUDIO_FM_LOW_100KHZ) < 0)
		return 1;
	while (count < FM_CHANNEL_COUNT) {
		struct v4l2_frequency frequency = {
			.tuner = 0,
			.type = V4L2_TUNER_RADIO,
		};
		size_t i;

		if (ioctl(fd, VIDIOC_S_HW_FREQ_SEEK, &seek) < 0) {
			if (errno == ENODATA || errno == ENOENT)
				break;
			fprintf(stderr,
				"fplinux-fm: VIDIOC_S_HW_FREQ_SEEK: %s\n",
				strerror(errno));
			return 1;
		}
		if (ioctl(fd, VIDIOC_G_FREQUENCY, &frequency) < 0) {
			fprintf(stderr, "fplinux-fm: VIDIOC_G_FREQUENCY: %s\n",
				strerror(errno));
			return 1;
		}
		for (i = 0; i < count; ++i) {
			if (seen[i] == frequency.frequency)
				break;
		}
		if (i < count)
			break;
		if (frequency.frequency < seek.rangelow ||
		    frequency.frequency > seek.rangehigh ||
		    frequency.frequency % FM_V4L2_UNITS_PER_100KHZ) {
			fputs("fplinux-fm: seek returned an invalid frequency\n",
			      stderr);
			return 1;
		}
		seen[count++] = frequency.frequency;
		printf("candidate %u.%u MHz\n",
		       frequency.frequency / FM_V4L2_UNITS_PER_100KHZ / 10U,
		       frequency.frequency / FM_V4L2_UNITS_PER_100KHZ % 10U);
		if (frequency.frequency == seek.rangehigh)
			break;
		/* The receiver includes the supplied frequency in each seek. */
		if (set_frequency(fd, frequency.frequency /
						      FM_V4L2_UNITS_PER_100KHZ +
					      1U) < 0)
			return 1;
	}
	if (!count)
		puts("No FM candidates found.");
	return 0;
}

static int claim_audio(const struct arguments *arguments)
{
	struct sockaddr_un address = {
		.sun_family = AF_UNIX,
		.sun_path = FPLINUX_AUDIO_RADIO_SOCKET,
	};
	const struct fplinux_audio_radio command = {
		.type = arguments->command == COMMAND_SCAN ?
				FPLINUX_AUDIO_RADIO_SCAN :
				FPLINUX_AUDIO_RADIO_PLAY,
		.frequency = arguments->frequency_100khz,
	};
	struct fplinux_audio_radio reply;
	struct pollfd ready;
	int error;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		goto fail;
	if (fplinux_audio_radio_send(fd, &command) < 0)
		goto fail;
	ready = (struct pollfd){ .fd = fd, .events = POLLIN };
	if (poll(&ready, 1, 2000) <= 0) {
		errno = ETIMEDOUT;
		goto fail;
	}
	if (fplinux_audio_radio_receive(fd, FPLINUX_AUDIO_RADIO_REPLY, &reply) >
	    0) {
		if (reply.type == FPLINUX_AUDIO_RADIO_READY)
			return fd;
		errno = reply.error;
		goto fail;
	}
	errno = EBUSY;
fail:
	error = errno;
	close(fd);
	errno = error;
	return -1;
}

static int wait_for_stop(int fd, unsigned int seconds)
{
	struct timespec now;
	time_t deadline = 0;

	if (seconds) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		deadline = now.tv_sec + seconds;
	}
	while (!stop_signal) {
		struct pollfd service = { .fd = fd, .events = POLLIN };
		int result;

		if (seconds) {
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
				return -1;
			if (now.tv_sec >= deadline)
				break;
		}
		result = poll(&service, 1, 1000);
		if (result < 0 && errno != EINTR)
			return -1;
		if (result > 0) {
			errno = EPIPE;
			return -1;
		}
	}
	return 0;
}

static int play(const struct arguments *arguments)
{
	struct sigaction action = { .sa_handler = request_stop };
	int fd;
	int status = 1;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0) {
		fprintf(stderr, "fplinux-fm: install signal handler: %s\n",
			strerror(errno));
		return 1;
	}
	fd = claim_audio(arguments);
	if (fd < 0) {
		fprintf(stderr, "fplinux-fm: audio service: %s\n",
			strerror(errno));
		return 1;
	}
	printf("Playing %u.%u MHz; press Ctrl-C to stop.\n",
	       arguments->frequency_100khz / 10U,
	       arguments->frequency_100khz % 10U);
	fflush(stdout);
	if (wait_for_stop(fd, arguments->seconds) < 0) {
		fprintf(stderr, "fplinux-fm: wait: %s\n", strerror(errno));
		goto out;
	}
	status = stop_signal ? 128 + stop_signal : 0;
out:
	close(fd);
	if (stop_signal && status == 1)
		status = 128 + stop_signal;
	return status;
}

int main(int argc, char **argv)
{
	struct arguments arguments = { 0 };
	int fd, lease;
	int status;

	status = parse_arguments(argc, argv, &arguments);
	if (status != FPLINUX_CLI_READY)
		return status;
	if (arguments.command == COMMAND_PLAY)
		return play(&arguments);
	lease = claim_audio(&arguments);
	if (lease < 0) {
		fprintf(stderr, "fplinux-fm: audio service: %s\n",
			strerror(errno));
		return 1;
	}
	fd = open_radio(true);
	if (fd < 0)
		status = 1;
	else {
		status = scan(fd);
		close(fd);
	}
	close(lease);
	return status;
}
