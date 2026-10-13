// SPDX-License-Identifier: GPL-2.0-only
/*
 * One scripted sound card for fplinux-jack host tests. JACK_TEST_INITIAL=1
 * starts with headphones plugged in. JACK_TEST_EVENTS lists later changes,
 * separated by commas: "insert" and "remove" change the jack, "speaker-on"
 * is another client enabling the speaker. JACK_TEST_NO_SPEAKER removes the
 * speaker switch, and JACK_TEST_FAIL_WRITE names one failing write, such as
 * "headphone on". Each switch write is traced to standard error as
 * "TRACE <output> <on|off>". Once the script ends, the card disappears.
 */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ECHO_EVENT_COUNT 8U

struct test_control {
	const char *name;
	snd_ctl_elem_iface_t iface;
	unsigned int numid;
	const char *output;
	long value;
};

/* The kernel starts with the headphones on and the speaker off. */
static struct test_control jack = { "Headphone Jack", SND_CTL_ELEM_IFACE_CARD,
				    7, NULL, 0 };
static struct test_control headphone = { "Headphone Playback Switch",
					 SND_CTL_ELEM_IFACE_MIXER, 2,
					 "headphone", 1 };
static struct test_control speaker = { "Speaker Playback Switch",
				       SND_CTL_ELEM_IFACE_MIXER, 4, "speaker",
				       0 };
static snd_ctl_t card;
static bool subscribed;
static const char *script = "";
/* The kernel sends a changed value to every subscriber, the writer too. */
static unsigned int echo_numids[ECHO_EVENT_COUNT];
static size_t echo_count;

static struct test_control *find(const snd_ctl_elem_id_t *id)
{
	struct test_control *controls[] = { &jack, &headphone, &speaker };
	size_t index;

	for (index = 0; index < sizeof(controls) / sizeof(controls[0]);
	     ++index) {
		if (controls[index] == &speaker &&
		    getenv("JACK_TEST_NO_SPEAKER"))
			continue;
		if (controls[index]->iface == id->iface &&
		    !strcmp(controls[index]->name, id->name))
			return controls[index];
	}
	return NULL;
}

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
	const char *initial = getenv("JACK_TEST_INITIAL");
	const char *events = getenv("JACK_TEST_EVENTS");

	(void)name;
	(void)mode;
	jack.value = initial && !strcmp(initial, "1");
	if (events)
		script = events;
	*ctl = &card;
	return 0;
}

int snd_ctl_close(snd_ctl_t *ctl)
{
	(void)ctl;
	return 0;
}

int snd_ctl_subscribe_events(snd_ctl_t *ctl, int subscribe)
{
	(void)ctl;
	subscribed = subscribe;
	return 0;
}

void snd_ctl_elem_id_set_interface(snd_ctl_elem_id_t *id,
				   snd_ctl_elem_iface_t iface)
{
	id->iface = iface;
}

void snd_ctl_elem_id_set_name(snd_ctl_elem_id_t *id, const char *name)
{
	id->name = name;
}

void snd_ctl_elem_info_set_id(snd_ctl_elem_info_t *info,
			      const snd_ctl_elem_id_t *id)
{
	info->id = *id;
}

int snd_ctl_elem_info(snd_ctl_t *ctl, snd_ctl_elem_info_t *info)
{
	struct test_control *control = find(&info->id);

	(void)ctl;
	if (!control)
		return -ENOENT;
	info->numid = control->numid;
	return 0;
}

unsigned int snd_ctl_elem_info_get_numid(const snd_ctl_elem_info_t *info)
{
	return info->numid;
}

void snd_ctl_elem_value_set_id(snd_ctl_elem_value_t *value,
			       const snd_ctl_elem_id_t *id)
{
	value->id = *id;
}

int snd_ctl_elem_value_get_boolean(const snd_ctl_elem_value_t *value,
				   unsigned int index)
{
	(void)index;
	return value->boolean != 0;
}

void snd_ctl_elem_value_set_boolean(snd_ctl_elem_value_t *value,
				    unsigned int index, long boolean)
{
	(void)index;
	value->boolean = boolean;
}

int snd_ctl_elem_read(snd_ctl_t *ctl, snd_ctl_elem_value_t *value)
{
	struct test_control *control = find(&value->id);

	(void)ctl;
	if (!control)
		return -ENOENT;
	value->boolean = control->value;
	return 0;
}

int snd_ctl_elem_write(snd_ctl_t *ctl, snd_ctl_elem_value_t *value)
{
	struct test_control *control = find(&value->id);
	const char *failure = getenv("JACK_TEST_FAIL_WRITE");
	char request[64];

	(void)ctl;
	if (!control || !control->output)
		return -EPERM;
	snprintf(request, sizeof(request), "%s %s", control->output,
		 value->boolean ? "on" : "off");
	fprintf(stderr, "TRACE %s\n", request);
	if (failure && !strcmp(failure, request))
		return -EIO;
	if (control->value != value->boolean && subscribed &&
	    echo_count < ECHO_EVENT_COUNT)
		echo_numids[echo_count++] = control->numid;
	control->value = value->boolean;
	return 0;
}

static int next_script_event(snd_ctl_event_t *event)
{
	size_t length = strcspn(script, ",");

	if (!length)
		return -ENODEV;
	event->mask = SND_CTL_EVENT_MASK_VALUE;
	if (length == strlen("insert") && !strncmp(script, "insert", length)) {
		jack.value = 1;
		event->numid = jack.numid;
	} else if (length == strlen("remove") &&
		   !strncmp(script, "remove", length)) {
		jack.value = 0;
		event->numid = jack.numid;
	} else if (length == strlen("speaker-on") &&
		   !strncmp(script, "speaker-on", length)) {
		speaker.value = 1;
		event->numid = speaker.numid;
	} else {
		return -EINVAL;
	}
	script += length;
	if (*script == ',')
		++script;
	return 1;
}

int snd_ctl_read(snd_ctl_t *ctl, snd_ctl_event_t *event)
{
	size_t index;

	(void)ctl;
	if (!subscribed)
		return -EINVAL;
	if (!echo_count)
		return next_script_event(event);
	event->numid = echo_numids[0];
	event->mask = SND_CTL_EVENT_MASK_VALUE;
	for (index = 1; index < echo_count; ++index)
		echo_numids[index - 1] = echo_numids[index];
	--echo_count;
	return 1;
}

snd_ctl_event_type_t snd_ctl_event_get_type(const snd_ctl_event_t *event)
{
	(void)event;
	return SND_CTL_EVENT_ELEM;
}

unsigned int snd_ctl_event_elem_get_numid(const snd_ctl_event_t *event)
{
	return event->numid;
}

unsigned int snd_ctl_event_elem_get_mask(const snd_ctl_event_t *event)
{
	return event->mask;
}

const char *snd_strerror(int error)
{
	(void)error;
	return "test ALSA error";
}
