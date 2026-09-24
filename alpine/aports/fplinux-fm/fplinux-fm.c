// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "fplinux-cli.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RADIO_DEVICE "/dev/radio0"
#define FM_CONTROL "FM Playback Switch"
#define FM_LOW_100KHZ 875U
#define FM_HIGH_100KHZ 1080U
#define FM_V4L2_UNITS_PER_100KHZ 1600U
#define FM_SEEK_SPACING_HZ 100000U
#define FM_CHANNEL_COUNT (FM_HIGH_100KHZ - FM_LOW_100KHZ + 1U)

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
		if (mhz > FM_HIGH_100KHZ / 10U)
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
	if (*cursor != '\0' || mhz * 10U + tenth < FM_LOW_100KHZ ||
	    mhz * 10U + tenth > FM_HIGH_100KHZ)
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
				"Tune FM and play through the selected audio output.",
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
		.rangelow = FM_LOW_100KHZ * FM_V4L2_UNITS_PER_100KHZ,
		.rangehigh = FM_HIGH_100KHZ * FM_V4L2_UNITS_PER_100KHZ,
	};

	if (set_frequency(fd, FM_LOW_100KHZ) < 0)
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

static int set_fm_switch(snd_ctl_t *control, snd_ctl_elem_value_t *value,
			 bool enabled)
{
	int result;

	snd_ctl_elem_value_set_boolean(value, 0, enabled);
	result = snd_ctl_elem_write(control, value);
	if (result < 0)
		fprintf(stderr, "fplinux-fm: set %s %s: %s\n", FM_CONTROL,
			enabled ? "on" : "off", snd_strerror(result));
	return result;
}

static int set_audio_mute(int fd, bool muted)
{
	struct v4l2_control control = {
		.id = V4L2_CID_AUDIO_MUTE,
		.value = muted,
	};

	if (ioctl(fd, VIDIOC_S_CTRL, &control) < 0) {
		fprintf(stderr, "fplinux-fm: set V4L2 audio mute %s: %s\n",
			muted ? "on" : "off", strerror(errno));
		return -1;
	}
	return 0;
}

static int wait_for_stop(unsigned int seconds)
{
	struct timespec now;
	time_t deadline = 0;

	if (seconds) {
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
			return -1;
		deadline = now.tv_sec + seconds;
	}
	while (!stop_signal) {
		if (seconds) {
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
				return -1;
			if (now.tv_sec >= deadline)
				break;
		}
		if (poll(NULL, 0, 1000) < 0 && errno != EINTR)
			return -1;
	}
	return 0;
}

static int play(int fd, const struct arguments *arguments)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_value_t *value;
	snd_ctl_t *control = NULL;
	struct sigaction action = { .sa_handler = request_stop };
	bool switch_owned = false;
	int result;
	int status = 1;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0) {
		fprintf(stderr, "fplinux-fm: install signal handler: %s\n",
			strerror(errno));
		return 1;
	}
	result = snd_ctl_open(&control, "default", 0);
	if (result < 0) {
		fprintf(stderr, "fplinux-fm: open ALSA control: %s\n",
			snd_strerror(result));
		goto out;
	}
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, FM_CONTROL);
	snd_ctl_elem_value_set_id(value, id);
	result = snd_ctl_elem_read(control, value);
	if (result < 0) {
		fprintf(stderr, "fplinux-fm: read %s: %s\n", FM_CONTROL,
			snd_strerror(result));
		goto out;
	}
	if (snd_ctl_elem_value_get_boolean(value, 0)) {
		fputs("fplinux-fm: FM playback is already active\n", stderr);
		goto out;
	}
	if (stop_signal)
		goto out;
	if (set_frequency(fd, arguments->frequency_100khz) < 0)
		goto out;
	if (stop_signal)
		goto out;
	/* A failed write may still have reached the control; always try to mute. */
	switch_owned = true;
	if (set_fm_switch(control, value, true) < 0 || stop_signal)
		goto out;
	if (set_audio_mute(fd, false) < 0)
		goto out;
	printf("Playing %u.%u MHz; press Ctrl-C to stop.\n",
	       arguments->frequency_100khz / 10U,
	       arguments->frequency_100khz % 10U);
	fflush(stdout);
	if (wait_for_stop(arguments->seconds) < 0) {
		fprintf(stderr, "fplinux-fm: wait: %s\n", strerror(errno));
		goto out;
	}
	status = stop_signal ? 128 + stop_signal : 0;
out:
	if (switch_owned) {
		if (set_audio_mute(fd, true) < 0)
			status = 1;
		if (set_fm_switch(control, value, false) < 0)
			status = 1;
	}
	if (control)
		snd_ctl_close(control);
	if (stop_signal && status == 1)
		status = 128 + stop_signal;
	return status;
}

int main(int argc, char **argv)
{
	struct arguments arguments = { 0 };
	int fd;
	int status;

	status = parse_arguments(argc, argv, &arguments);
	if (status != FPLINUX_CLI_READY)
		return status;
	fd = open_radio(arguments.command == COMMAND_SCAN);
	if (fd < 0)
		return 1;
	status = arguments.command == COMMAND_SCAN ? scan(fd) :
						     play(fd, &arguments);
	close(fd);
	return status;
}
