/* SPDX-License-Identifier: GPL-2.0-only */
/* UI frame scenarios use an engine peer which never requests audio. */
#include "brokers/audio-broker.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>

struct mre_audio_broker *mre_audio_broker_open(struct fplinux_ui *ui,
					       mre_audio_reply_fn reply,
					       mre_audio_event_fn event,
					       void *context)
{
	(void)ui;
	(void)reply;
	(void)event;
	(void)context;
	assert(!"UI frame engine peer requested an unexpected audio broker");
	errno = ENOTSUP;
	return NULL;
}

void mre_audio_broker_close(struct mre_audio_broker *broker)
{
	assert(broker == NULL);
}

void mre_audio_broker_set_visible(struct mre_audio_broker *broker, bool visible)
{
	(void)visible;
	assert(broker == NULL);
}

bool mre_audio_broker_dispatch(struct mre_audio_broker *broker,
			       const struct mre_audio_request *request)
{
	(void)broker;
	(void)request;
	assert(!"UI frame engine peer requested unexpected audio playback");
	return false;
}
