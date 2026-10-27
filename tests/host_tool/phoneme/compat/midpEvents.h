/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_PHONEME_TEST_MIDP_EVENTS_H
#define FPLINUX_PHONEME_TEST_MIDP_EVENTS_H

/* The event fields consumed by the key producer. */
typedef struct {
	int type;
	int action;
	int character;
	int display;
	int intParam1, intParam2, intParam3, intParam5;
	int command;
} MidpEvent;

#define MIDP_EVENT_INITIALIZE(event) ((event) = (MidpEvent){ 0 })
#define MIDP_KEY_EVENT 1
#define MIDP_PEER_CHANGED_EVENT 2
#define MIDP_COMMAND_EVENT 3
#define MIDP_PRESSED 1
#define MIDP_RELEASED 2
#define ACTION action
#define CHR character
#define DISPLAY display
#define COMMAND command

int GetEventQueueFreeCount(int isolate);
void StoreMIDPEventInVmThread(MidpEvent event, int isolate);

#endif
