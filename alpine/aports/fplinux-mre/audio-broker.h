/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_AUDIO_BROKER_H
#define FPLINUX_MRE_AUDIO_BROKER_H

#include <stdbool.h>
#include "mre-audio.h"

struct fplinux_ui;
struct mre_audio_broker;

typedef bool (*mre_audio_reply_fn)(uint32_t sequence, int32_t result,
				   void *context);
typedef bool (*mre_audio_event_fn)(uint32_t handle, int32_t event,
				   void *context);

/* Every operation and notification belongs to the UI owner thread. Closing
 * retires all handles before worker cleanup and never calls either listener. */
struct mre_audio_broker *mre_audio_broker_open(struct fplinux_ui *ui,
					       mre_audio_reply_fn reply,
					       mre_audio_event_fn event,
					       void *context);
void mre_audio_broker_close(struct mre_audio_broker *broker);
void mre_audio_broker_set_visible(struct mre_audio_broker *broker,
				  bool visible);
bool mre_audio_broker_dispatch(struct mre_audio_broker *broker,
			       const struct mre_audio_request *request);

#endif
