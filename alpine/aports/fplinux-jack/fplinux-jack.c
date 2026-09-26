// SPDX-License-Identifier: GPL-2.0-only
/*
 * Move playback between the headphones and the speaker when wired headphones
 * are plugged in or removed. The outputs are switched once at start and then
 * only when the jack changes, so a manual switch setting lasts until the next
 * jack change.
 */
#define _POSIX_C_SOURCE 200809L

#include "fplinux-cli.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stdio.h>

struct control {
	const char *name;
	snd_ctl_elem_iface_t iface;
	snd_ctl_elem_value_t *value;
	unsigned int numid;
};

static bool find_control(snd_ctl_t *ctl, struct control *control)
{
	snd_ctl_elem_id_t *id;
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_id_set_interface(id, control->iface);
	snd_ctl_elem_id_set_name(id, control->name);
	snd_ctl_elem_info_set_id(info, id);
	if (snd_ctl_elem_info(ctl, info) < 0)
		return false;
	control->numid = snd_ctl_elem_info_get_numid(info);
	snd_ctl_elem_value_set_id(control->value, id);
	return true;
}

static int read_jack(snd_ctl_t *ctl, const struct control *jack, bool *present)
{
	int result = snd_ctl_elem_read(ctl, jack->value);

	if (result < 0) {
		fprintf(stderr, "fplinux-jack: read %s: %s\n", jack->name,
			snd_strerror(result));
		return result;
	}
	*present = snd_ctl_elem_value_get_boolean(jack->value, 0);
	return 0;
}

static int set_switch(snd_ctl_t *ctl, const struct control *output,
		      bool enabled)
{
	int result;

	snd_ctl_elem_value_set_boolean(output->value, 0, enabled);
	result = snd_ctl_elem_write(ctl, output->value);
	if (result < 0)
		fprintf(stderr, "fplinux-jack: set %s %s: %s\n", output->name,
			enabled ? "on" : "off", snd_strerror(result));
	return result;
}

/*
 * Enable the new output before disabling the old one. Passing through both
 * outputs is silent, while turning the speaker on from no enabled output
 * clicks. A failed enable leaves the old output playing.
 */
static void follow_jack(snd_ctl_t *ctl, const struct control *headphone,
			const struct control *speaker, bool present)
{
	const struct control *enable = present ? headphone : speaker;
	const struct control *disable = present ? speaker : headphone;

	if (set_switch(ctl, enable, true) < 0 ||
	    set_switch(ctl, disable, false) < 0)
		return;
	printf("fplinux-jack: headphones %s; playing through the %s\n",
	       present ? "connected" : "disconnected",
	       present ? "headphones" : "speaker");
}

static int run(snd_ctl_t *ctl)
{
	struct control jack = {
		.name = "Headphone Jack",
		.iface = SND_CTL_ELEM_IFACE_CARD,
	};
	struct control headphone = {
		.name = "Headphone Playback Switch",
		.iface = SND_CTL_ELEM_IFACE_MIXER,
	};
	struct control speaker = {
		.name = "Speaker Playback Switch",
		.iface = SND_CTL_ELEM_IFACE_MIXER,
	};
	struct control *controls[] = { &jack, &headphone, &speaker };
	snd_ctl_event_t *event;
	bool active = true;
	bool present = false;
	size_t index;
	int result;

	snd_ctl_elem_value_alloca(&jack.value);
	snd_ctl_elem_value_alloca(&headphone.value);
	snd_ctl_elem_value_alloca(&speaker.value);
	snd_ctl_event_alloca(&event);
	/* Subscribe first so that a change after the initial read is seen. */
	result = snd_ctl_subscribe_events(ctl, 1);
	if (result < 0) {
		fprintf(stderr, "fplinux-jack: subscribe to ALSA events: %s\n",
			snd_strerror(result));
		return 1;
	}
	for (index = 0; index < sizeof(controls) / sizeof(controls[0]);
	     ++index) {
		if (!find_control(ctl, controls[index])) {
			printf("fplinux-jack: no %s control; outputs stay under manual control\n",
			       controls[index]->name);
			active = false;
			break;
		}
	}
	if (active) {
		if (read_jack(ctl, &jack, &present) < 0)
			return 1;
		follow_jack(ctl, &headphone, &speaker, present);
	}

	/* Keep reading events while inactive so that they do not queue up. */
	for (;;) {
		bool now_present;
		unsigned int mask;

		result = snd_ctl_read(ctl, event);
		if (result < 0) {
			fprintf(stderr, "fplinux-jack: read ALSA events: %s\n",
				snd_strerror(result));
			return 1;
		}
		if (!active || !result ||
		    snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM ||
		    snd_ctl_event_elem_get_numid(event) != jack.numid)
			continue;
		mask = snd_ctl_event_elem_get_mask(event);
		if (mask == SND_CTL_EVENT_MASK_REMOVE) {
			fprintf(stderr, "fplinux-jack: %s was removed\n",
				jack.name);
			return 1;
		}
		/* Queued events can repeat the state that was already applied. */
		if (!(mask & SND_CTL_EVENT_MASK_VALUE) ||
		    read_jack(ctl, &jack, &now_present) < 0 ||
		    now_present == present)
			continue;
		present = now_present;
		follow_jack(ctl, &headphone, &speaker, present);
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = argv[0],
		.description =
			"Play through the headphones while they are plugged in and through the speaker otherwise.",
	};
	snd_ctl_t *ctl;
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	/* The service logger reads standard output through a pipe. */
	setvbuf(stdout, NULL, _IOLBF, 0);
	result = snd_ctl_open(&ctl, "default", 0);
	if (result < 0) {
		fprintf(stderr, "fplinux-jack: open ALSA control: %s\n",
			snd_strerror(result));
		return 1;
	}
	result = run(ctl);
	snd_ctl_close(ctl);
	return result;
}
