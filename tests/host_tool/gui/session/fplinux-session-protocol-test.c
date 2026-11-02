/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
/*
 * Host test of the session packet format over a real SOCK_SEQPACKET pair.
 * Raw packets are literal JSON written by the test, not by the serializer.
 */
#include "fplinux-ui-session-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned int failures;

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static int receive_raw(int sender, int receiver, const char *packet,
		       struct fplinux_ui_session_message *message)
{
	if (send(sender, packet, strlen(packet), 0) != (ssize_t)strlen(packet))
		return -2;
	return fplinux_ui_session_receive(receiver, message);
}

static bool send_raw_frame(int sender, const char *packet, size_t bytes,
			   int frame_fd, unsigned int count)
{
	union {
		struct cmsghdr alignment;
		char bytes[CMSG_SPACE(253 * sizeof(int))];
	} control = { 0 };
	struct iovec iov = { .iov_base = (void *)packet, .iov_len = bytes };
	struct msghdr header = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control.bytes,
		.msg_controllen = CMSG_SPACE(count * sizeof(int)),
	};
	struct cmsghdr *rights = CMSG_FIRSTHDR(&header);

	rights->cmsg_level = SOL_SOCKET;
	rights->cmsg_type = SCM_RIGHTS;
	rights->cmsg_len = CMSG_LEN(count * sizeof(int));
	for (unsigned int i = 0; i < count; ++i)
		memcpy((char *)CMSG_DATA(rights) + i * sizeof(int), &frame_fd,
		       sizeof(frame_fd));
	return sendmsg(sender, &header, 0) == (ssize_t)bytes;
}

static unsigned int open_descriptors(void)
{
	unsigned int count = 0;

	/* Every descriptor created by this harness is below this bound. */
	for (int fd = 0; fd < 1024; ++fd)
		if (fcntl(fd, F_GETFD) >= 0)
			++count;
	return count;
}

static void check_launch_frame(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"ACTIVE\",\"width\":2,\"height\":2}",
		"{\"type\":\"LAUNCH\",\"width\":0,\"height\":2}",
		"{\"type\":\"LAUNCH\",\"width\":321,\"height\":2}",
		"{\"type\":\"LAUNCH\",\"width\":320,\"height\":241}",
		"{\"type\":\"LAUNCH\",\"width\":242,\"height\":320}",
		"{\"type\":\"LAUNCH\",\"width\":320,\"height\":320}",
		"{\"type\":\"LAUNCH\",\"width\":2,\"height\":321}",
		"{\"type\":\"LAUNCH\",\"width\":2,\"height\":-1}",
		"{\"type\":\"LAUNCH\",\"width\":2,\"height\":2,\"frame_fd\":4}",
		"{\"type\":\"LAUNCH\",\"width\":2,\"height\":2,\"has_frame\":true}",
		"{\"type\":\"LAUNCH\",\"width\":2,\"height\":2",
	};
	const char *launch = "{\"type\":\"LAUNCH\",\"width\":2,\"height\":2}";
	const uint16_t pixels[] = { 0x1234, 0xffff, 0, 0xabcd };
	uint16_t copied[4] = { 0 };
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_LAUNCH,
		.width = 2,
		.height = 2,
		.has_frame = true,
	};
	struct fplinux_ui_session_message received = { 0 };
	unsigned int baseline = open_descriptors();
	unsigned int with_producer;
	int producer = memfd_create("session-launch-test", MFD_CLOEXEC);

	check(producer >= 0, "the snapshot producer opens a memfd");
	if (producer < 0)
		return;
	check(write(producer, pixels, sizeof(pixels)) ==
		      (ssize_t)sizeof(pixels),
	      "the producer writes tightly packed RGB565");
	sent.frame_fd = producer;
	strcpy(sent.app_id, "org.example.notes");
	check(fplinux_ui_session_send(sender, &sent),
	      "LAUNCH sends its snapshot");
	close(producer);
	check(fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.has_frame && received.frame_fd >= 0 &&
		      received.type == FPLINUX_UI_SESSION_LAUNCH &&
		      received.width == 2 && received.height == 2 &&
		      !strcmp(received.app_id, sent.app_id),
	      "LAUNCH receives one owned descriptor after the producer closes");
	if (received.has_frame) {
		check((fcntl(received.frame_fd, F_GETFD) & FD_CLOEXEC) != 0 &&
			      pread(received.frame_fd, copied, sizeof(copied),
				    0) == (ssize_t)sizeof(copied) &&
			      !memcmp(copied, pixels, sizeof(pixels)),
		      "the received descriptor is CLOEXEC and keeps the pixels alive");
		close(received.frame_fd);
	}
	check(open_descriptors() == baseline,
	      "the caller closes its owned snapshot");
	producer = memfd_create("session-launch-invalid", MFD_CLOEXEC);
	check(producer >= 0, "the invalid-packet producer opens a memfd");
	if (producer < 0)
		return;
	with_producer = open_descriptors();
	sent.frame_fd = producer;
	const int orientations[][2] = { { 240, 320 }, { 320, 240 } };

	for (unsigned int i = 0;
	     i < sizeof(orientations) / sizeof(orientations[0]); ++i) {
		sent.width = orientations[i][0];
		sent.height = orientations[i][1];
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.has_frame &&
			      received.width == sent.width &&
			      received.height == sent.height,
		      "LAUNCH transports portrait and landscape frame geometry");
		if (received.has_frame)
			close(received.frame_fd);
		check(open_descriptors() == with_producer,
		      "both orientations transfer exactly one owned descriptor");
	}
	sent.width = 2;
	sent.height = 2;
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]);
	     ++i) {
		check(send_raw_frame(sender, invalid[i], strlen(invalid[i]),
				     producer, 1),
		      "a raw invalid packet carries one descriptor");
		check(fplinux_ui_session_receive(receiver, &received) == -1 &&
			      errno == EPROTO && !received.has_frame,
		      "invalid attached packets do not transfer descriptor ownership");
		check(open_descriptors() == with_producer,
		      "an invalid attached packet closes the received descriptor");
	}
	for (unsigned int count = 2; count <= 253; count += 251) {
		check(send_raw_frame(sender, launch, strlen(launch), producer,
				     count),
		      "a raw LAUNCH can carry extra descriptors");
		check(fplinux_ui_session_receive(receiver, &received) == -1 &&
			      open_descriptors() == with_producer,
		      "extra descriptors are rejected and all are closed");
	}
	char oversized[FPLINUX_UI_SESSION_PACKET_BYTES + 1];

	memset(oversized, ' ', sizeof(oversized));
	memcpy(oversized, launch, strlen(launch));
	check(send_raw_frame(sender, oversized, sizeof(oversized), producer,
			     1) &&
		      fplinux_ui_session_receive(receiver, &received) == -1 &&
		      open_descriptors() == with_producer,
	      "a truncated body closes its attached descriptor");
	int pass_credentials = 1;

	check(setsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &pass_credentials,
			 sizeof(pass_credentials)) == 0,
	      "the receiver enables kernel-generated credentials");
	check(send_raw_frame(sender, launch, strlen(launch), producer, 1) &&
		      fplinux_ui_session_receive(receiver, &received) == -1 &&
		      errno == EPROTO && open_descriptors() == with_producer,
	      "unexpected credentials reject the packet and close its descriptor");
	pass_credentials = 0;
	check(setsockopt(receiver, SOL_SOCKET, SO_PASSCRED, &pass_credentials,
			 sizeof(pass_credentials)) == 0,
	      "the receiver restores its ordinary ancillary mode");
	sent.frame_fd = producer;
	sent.type = FPLINUX_UI_SESSION_READY;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "READY cannot send a descriptor");
	sent.type = FPLINUX_UI_SESSION_ACTIVE;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "ACTIVE without an end prompt cannot send a descriptor");
	sent.type = FPLINUX_UI_SESSION_LAUNCH;
	sent.width = 321;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "the producer refuses an oversized snapshot");
	sent.width = 320;
	sent.height = 241;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "the producer refuses a snapshot exceeding the pixel budget");
	sent.width = 2;
	sent.height = 2;
	sent.frame_fd = -1;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "the producer refuses a negative descriptor");
	sent.has_frame = false;
	sent.width = 0;
	sent.height = 0;
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      !received.has_frame && received.frame_fd == 0,
	      "LAUNCH without a frame preserves the existing zero defaults");
	close(producer);
	check(open_descriptors() == baseline,
	      "LAUNCH rejection leaves no descriptors");
}

static void check_input_locales(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"CONFIG\",\"input_locale\":1}",
		"{\"type\":\"CONFIG\",\"input_locale\":null}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"r\"}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"russ\"}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"RU\"}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"ru_RU\"}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"рус\"}",
		"{\"type\":\"CONFIG\",\"input_locale\":\"ru\\u0000\"}",
		"{\"type\":\"ACTIVE\",\"input_locale\":\"abcdefgh\"}",
		"{\"type\":\"KEY\",\"input_locale\":\"ru\"}",
		"{\"type\":\"SETTING\",\"input_locale\":\"\"}",
	};
	static const char *const bad_codes[] = {
		"r", "russ", "RU", "r1", "ru_RU", "abcdefgh",
	};
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_CONFIG,
	};
	struct fplinux_ui_session_message received;
	char packet[2048];
	struct iovec iov = { .iov_base = packet,
			     .iov_len = sizeof(packet) - 1 };
	struct msghdr header = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t bytes;

	strcpy(message.locale, "en");
	strcpy(message.input_locale, "ru");
	check(fplinux_ui_session_send(sender, &message),
	      "CONFIG serializes independent language codes");
	bytes = recvmsg(receiver, &header, 0);
	check(bytes > 0 && bytes < (ssize_t)sizeof(packet),
	      "serialized CONFIG is available for wire inspection");
	if (bytes > 0 && bytes < (ssize_t)sizeof(packet)) {
		packet[bytes] = '\0';
		check(strstr(packet, "\"locale\":\"en\"") &&
			      strstr(packet, "\"input_locale\":\"ru\""),
		      "CONFIG wire assigns each language to its own field");
	}
	for (unsigned int i = 0; i < 2; ++i) {
		message.type = i ? FPLINUX_UI_SESSION_ACTIVE :
				   FPLINUX_UI_SESSION_CONFIG;
		check(fplinux_ui_session_send(sender, &message) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      !strcmp(received.locale, "en") &&
			      !strcmp(received.input_locale, "ru"),
		      "CONFIG and ACTIVE preserve independent languages");
	}
	check(receive_raw(sender, receiver,
			  "{\"type\":\"ACTIVE\",\"locale\":\"ru\","
			  "\"input_locale\":\"eng\"}",
			  &received) == 1 &&
		      !strcmp(received.locale, "ru") &&
		      !strcmp(received.input_locale, "eng"),
	      "literal ACTIVE decodes a three-letter input language");
	check(receive_raw(sender, receiver, "{\"type\":\"CONFIG\"}",
			  &received) == 1 &&
		      !received.locale[0] && !received.input_locale[0],
	      "absent languages retain empty defaults");
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_SETTING,
	};
	strcpy(message.name, "input_locale");
	strcpy(message.value, "ru");
	check(fplinux_ui_session_send(sender, &message) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_SETTING &&
		      !strcmp(received.name, "input_locale") &&
		      !strcmp(received.value, "ru"),
	      "SETTING carries the input language through name and value");
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "mistyped, malformed and misplaced input locales are rejected");
	message.type = FPLINUX_UI_SESSION_CONFIG;
	for (size_t i = 0; i < sizeof(bad_codes) / sizeof(bad_codes[0]); ++i) {
		memset(message.input_locale, 0, sizeof(message.input_locale));
		memcpy(message.input_locale, bad_codes[i],
		       strlen(bad_codes[i]));
		check(!fplinux_ui_session_send(sender, &message) &&
			      errno == EINVAL,
		      "serializer rejects malformed or unterminated input locales");
	}
	strcpy(message.input_locale, "ru");
	message.type = FPLINUX_UI_SESSION_TITLE;
	check(!fplinux_ui_session_send(sender, &message) && errno == EINVAL,
	      "serializer refuses input locale outside CONFIG and ACTIVE");
}

static void check_lifecycle(int sender, int receiver)
{
	static const struct {
		const char *packet;
		enum fplinux_ui_session_lifecycle expected;
	} cases[] = {
		{ "{\"type\":\"LIFECYCLE\",\"lifecycle\":\"foreground\"}",
		  FPLINUX_UI_SESSION_FOREGROUND },
		{ "{\"type\":\"LIFECYCLE\",\"lifecycle\":\"background\"}",
		  FPLINUX_UI_SESSION_BACKGROUND },
		{ "{\"type\":\"LIFECYCLE\",\"lifecycle\":\"closing\"}",
		  FPLINUX_UI_SESSION_CLOSING },
	};
	static const char *const invalid[] = {
		"{\"type\":\"LIFECYCLE\"}",
		"{\"type\":\"LIFECYCLE\",\"lifecycle\":\"paused\"}",
		"{\"type\":\"LIFECYCLE\",\"lifecycle\":1}",
		"{\"type\":\"HOME\",\"lifecycle\":\"background\"}",
	};
	struct fplinux_ui_session_message received;
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_LIFECYCLE,
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		check(receive_raw(sender, receiver, cases[i].packet,
				  &received) == 1 &&
			      received.lifecycle == cases[i].expected,
		      "literal lifecycle state has the defined meaning");
		sent.lifecycle = cases[i].expected;
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == FPLINUX_UI_SESSION_LIFECYCLE &&
			      received.lifecycle == cases[i].expected,
		      "lifecycle round-trips independently of visibility");
	}
	sent.lifecycle = 3;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "invalid lifecycle state is not sent");
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "invalid or misplaced lifecycle state is rejected");
}

static void check_state_packets(int sender, int receiver)
{
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.serial = UINT32_MAX,
		.display = FPLINUX_UI_SESSION_DISPLAY_DIM,
		.locked = true,
		.visible = true,
		.enabled = true,
		.battery = 14,
		.charging = true,
		.theme = FPLINUX_UI_SESSION_THEME_DARK,
	};
	struct fplinux_ui_session_message received;
	struct fplinux_ui_session_message ack = {
		.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
		.serial = UINT32_MAX,
	};

	strcpy(sent.active_id, "org.example.notes");
	strcpy(sent.left, "Menu");
	strcpy(sent.centre, "Открыть");
	strcpy(sent.right, "Back");
	strcpy(sent.reason, "unlock-press-star");
	strcpy(sent.locale, "ru");
	strcpy(sent.input_locale, "en");
	strcpy(sent.name, "list");
	strcpy(sent.value, "catalogue");
	strcpy(sent.notice, "battery-low");
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_ACTIVE &&
		      received.serial == UINT32_MAX &&
		      received.display == FPLINUX_UI_SESSION_DISPLAY_DIM &&
		      received.locked && received.visible && received.enabled &&
		      !strcmp(received.active_id, "org.example.notes") &&
		      !strcmp(received.left, "Menu") &&
		      !strcmp(received.centre, "Открыть") &&
		      !strcmp(received.right, "Back") &&
		      !strcmp(received.reason, "unlock-press-star") &&
		      !strcmp(received.locale, "ru") &&
		      !strcmp(received.input_locale, "en") &&
		      !strcmp(received.name, "list") &&
		      !strcmp(received.value, "catalogue") &&
		      received.battery == 14 && received.charging &&
		      !strcmp(received.notice, "battery-low") &&
		      received.theme == FPLINUX_UI_SESSION_THEME_DARK,
	      "ACTIVE round-trips titles, languages, lock, battery and serial");
	check(fplinux_ui_session_send(sender, &ack) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_ACTIVE_ACK &&
		      received.serial == UINT32_MAX,
	      "ACTIVE_ACK preserves the unsigned correlation serial");
	ack.serial = 0;
	check(!fplinux_ui_session_send(sender, &ack) && errno == EINVAL,
	      "ACTIVE_ACK cannot be sent without a serial");
	ack.type = FPLINUX_UI_SESSION_REDRAW_ACK;
	check(!fplinux_ui_session_send(sender, &ack) && errno == EINVAL,
	      "REDRAW_ACK cannot be sent without a serial");
	ack.serial = UINT32_MAX;
	check(fplinux_ui_session_send(sender, &ack) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_REDRAW_ACK &&
		      received.serial == UINT32_MAX,
	      "REDRAW_ACK preserves the applied view serial");
	for (unsigned int i = 0; i < 2; ++i) {
		sent = (struct fplinux_ui_session_message){
			.type = i ? FPLINUX_UI_SESSION_ERROR :
				    FPLINUX_UI_SESSION_SETTING,
			.serial = 23,
			.name = "theme",
			.value = "dark",
		};
		if (i)
			strcpy(sent.reason, "save-failed");
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == sent.type &&
			      received.serial == 23 &&
			      !strcmp(received.name, "theme") &&
			      !strcmp(received.value, "dark") &&
			      !strcmp(received.reason, i ? "save-failed" : ""),
		      "applied settings and save failures retain their redraw serial");
	}
	sent = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_CONFIG,
		.mode = FPLINUX_UI_SESSION_INTEGRATED,
		.screen_width = 128,
		.screen_height = 128,
	};
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CONFIG &&
		      received.mode == FPLINUX_UI_SESSION_INTEGRATED &&
		      received.screen_width == 128 &&
		      received.screen_height == 128 &&
		      received.theme == FPLINUX_UI_SESSION_THEME_LIGHT,
	      "CONFIG preserves the display size for the UI profile");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"DISMISS\",\"notice\":\"battery-low\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_DISMISS &&
		      !strcmp(received.notice, "battery-low") &&
		      received.battery == 0 && !received.charging,
	      "DISMISS identifies the acknowledged battery notice");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"ACTIVE\",\"battery\":-1}",
			  &received) == 1 &&
		      received.battery == -1,
	      "battery -1 means no battery");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"SELECT\",\"selected_id\":\"org.a\","
			  "\"value\":\"catalogue\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_SELECT &&
		      !strcmp(received.selected_id, "org.a") &&
		      !strcmp(received.value, "catalogue") &&
		      received.display == FPLINUX_UI_SESSION_DISPLAY_ON &&
		      !received.locked,
	      "SELECT reports catalogue selection with absent state defaults");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"MODE\",\"mode\":\"fullscreen\"}",
			  &received) == 1 &&
		      received.mode == FPLINUX_UI_SESSION_FULLSCREEN,
	      "MODE names the fullscreen request");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"BUSY\",\"enabled\":true,\"title\":\"Wait\"}",
		      &received) == 1 &&
		      received.enabled && !strcmp(received.title, "Wait"),
	      "BUSY names the operation and explicit busy phase");
}

static void check_clock_presentation(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"HOME\",\"clock_presentation\":{}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":[]}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"analog\":1}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"timer_deadline_ms\":1000}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"timer_deadline_ms\":\"-1\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"missed_total\":\"18446744073709551616\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"alarm_utc\":\"9223372036854775808\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"alarm_label\":\"orphan\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"alarm_utc\":\"1735689660\",\"snooze_deadline_ms\":\"1000\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"timer_deadline_ms\":\"1000\",\"timer_remaining_ms\":\"500\"}}",
		"{\"type\":\"ACTIVE\",\"clock_presentation\":{\"stopwatch\":true}}",
	};
	struct fplinux_ui_session_message received;
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_ACTIVE,
		.clock_presentation = {
			.analog = true,
			.snooze_deadline_ms = UINT64_C(9223372036854775817),
			.alarm_label = "Чай \"утром\"",
			.timer_remaining_ms = 1500,
			.missed_total = UINT64_MAX,
		},
	};

	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"ACTIVE\",\"clock_presentation\":{\"analog\":true,\"alarm_utc\":\"1735689660\",\"alarm_label\":\"Утро\",\"timer_deadline_ms\":\"9223372036854775817\",\"missed_total\":\"18446744073709551615\"}}",
		      &received) == 1 &&
		      received.clock_presentation.analog &&
		      received.clock_presentation.alarm_utc == 1735689660 &&
		      !strcmp(received.clock_presentation.alarm_label,
			      "Утро") &&
		      received.clock_presentation.timer_deadline_ms ==
			      UINT64_C(9223372036854775817) &&
		      received.clock_presentation.missed_total == UINT64_MAX,
	      "literal Home state carries an alarm and timer with exact uint64 values");
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.clock_presentation.analog &&
		      !received.clock_presentation.alarm_utc &&
		      received.clock_presentation.snooze_deadline_ms ==
			      UINT64_C(9223372036854775817) &&
		      !strcmp(received.clock_presentation.alarm_label,
			      "Чай \"утром\"") &&
		      received.clock_presentation.timer_remaining_ms == 1500 &&
		      received.clock_presentation.missed_total == UINT64_MAX,
	      "snoozed alarms and paused timers retain their independent states");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"ACTIVE\",\"clock_presentation\":{\"analog\":false}}",
		      &received) == 1 &&
		      !received.clock_presentation.analog &&
		      !received.clock_presentation.alarm_utc &&
		      !received.clock_presentation.snooze_deadline_ms &&
		      !received.clock_presentation.timer_deadline_ms &&
		      !received.clock_presentation.timer_remaining_ms &&
		      !received.clock_presentation.missed_total &&
		      !received.clock_presentation.alarm_label[0],
	      "an empty Home state clears all previous events");
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "Home clock state rejects invalid types, bounds and conflicting deadlines");
	sent.type = FPLINUX_UI_SESSION_HOME;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "only ACTIVE publishes Home clock state");
}

static void check_clock_packets(int sender, int receiver)
{
	static const char data[] =
		"{\"alarms\":[{\"id\":18446744073709551615,\"title\":\"Будильник\"}],"
		"\"overflow\":18446744073709551616,"
		"\"value\":1,\"value\":2,\"\\u0076alue\":3}";
	static const char *const domain_text[] = {
		"{",
		"[]",
		"null",
		"{'id':1}",
		"{\"id\":01}",
		"{\"id\":NaN}",
		"{\"id\\u0000hidden\":1}",
		"{}{}",
	};
	static const char *const invalid_packets[] = {
		"{\"type\":\"clock\",\"clock\":{}}",
		"{\"type\":\"ACTIVE\",\"clock\":[]}",
		"{\"type\":\"clock\",\"clock\":null}",
		"{\"type\":\"clock\",\"clock\":7}",
		"{\"type\":\"clock\",\"clock\":true}",
		"{\"type\":\"clock\",\"clock\":\"a\\u0000b\"}",
		"{\"type\":\"clock\",\"clock\":\"a\\nb\"}",
		"{\"type\":\"clock\",\"clock\":\"a\\tb\"}",
		"{\"type\":\"clock\",\"clock\":\"a\\u007fb\"}",
		"{\"type\":\"clock\",\"clock\":\"\xd0\"}",
	};
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_CLOCK,
		.serial = UINT32_MAX,
		.name = "snapshot",
	};
	struct fplinux_ui_session_message received;
	char packet[FPLINUX_UI_SESSION_CLOCK_BYTES + 64];
	size_t prefix;

	check(receive_raw(sender, receiver, "{\"type\":\"clock\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CLOCK &&
		      !received.clock_data[0],
	      "a literal clock query carries no snapshot text");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"clock\",\"serial\":4294967295,"
			  "\"clock\":\"{\\\"id\\\":18446744073709551615,"
			  "\\\"title\\\":\\\"Будильник\\\"}\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CLOCK &&
		      received.serial == UINT32_MAX &&
		      !strcmp(received.clock_data,
			      "{\"id\":18446744073709551615,\"title\":\"Будильник\"}"),
	      "literal clock text preserves quotes, UTF-8 and uint64 digits");
	strcpy(sent.clock_data, data);
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CLOCK &&
		      received.serial == UINT32_MAX &&
		      !strcmp(received.clock_data, data),
	      "clock text preserves duplicate keys and overflowing numbers for its owner");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"ACTIVE\",\"clock\":\"{\\\"id\\\":7}\","
			  "\"serial\":17,\"title\":\"Clock\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_ACTIVE &&
		      received.serial == 17 &&
		      !strcmp(received.title, "Clock") &&
		      !strcmp(received.clock_data, "{\"id\":7}"),
	      "ACTIVE carries clock text beside ordinary state fields");
	sent.type = FPLINUX_UI_SESSION_HOME;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "an unrelated message cannot send clock text");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"HOME\",\"clock\":\"{}\"}",
			  &received) == -1 &&
		      errno == EPROTO,
	      "an unrelated literal message cannot carry clock data");
	sent.type = FPLINUX_UI_SESSION_CLOCK;
	for (size_t i = 0; i < sizeof(domain_text) / sizeof(domain_text[0]);
	     ++i) {
		strcpy(sent.clock_data, domain_text[i]);
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      !strcmp(received.clock_data, domain_text[i]),
		      "transport preserves malformed domain text for the clock owner's rejection");
	}
	for (size_t i = 0;
	     i < sizeof(invalid_packets) / sizeof(invalid_packets[0]); ++i)
		check(receive_raw(sender, receiver, invalid_packets[i],
				  &received) == -1 &&
			      errno == EPROTO,
		      "clock transport rejects non-strings, controls, NUL and invalid UTF-8");
	for (size_t i = 0; i < sizeof(sent.clock_data) - 1; ++i)
		sent.clock_data[i] = i % 2 ? '\\' : '"';
	sent.clock_data[sizeof(sent.clock_data) - 1] = '\0';
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      !strcmp(received.clock_data, sent.clock_data),
	      "maximum clock text fits when every quote and backslash needs escaping");
	prefix = strlen("{\"type\":\"clock\",\"clock\":\"");
	memcpy(packet, "{\"type\":\"clock\",\"clock\":\"", prefix);
	memset(packet + prefix, 'x', FPLINUX_UI_SESSION_CLOCK_BYTES);
	memcpy(packet + prefix + FPLINUX_UI_SESSION_CLOCK_BYTES, "\"}", 3);
	check(receive_raw(sender, receiver, packet, &received) == -1 &&
		      errno == EPROTO,
	      "clock text exceeding its own bound is rejected inside a valid packet");
	const char *const controls[] = { "a\nb", "a\tb",
					 "a\x7f"
					 "b" };

	for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
		strcpy(sent.clock_data, controls[i]);
		check(!fplinux_ui_session_send(sender, &sent) &&
			      errno == EINVAL,
		      "sender rejects clock control characters before escaping expands them");
	}
	memset(sent.clock_data, 'x', sizeof(sent.clock_data));
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "unterminated clock text is rejected without reading past its bound");
}

static void check_simple_packets(int sender, int receiver)
{
	static const struct {
		enum fplinux_ui_session_message_type type;
		const char *packet;
	} cases[] = {
		{ FPLINUX_UI_SESSION_HELLO,
		  "{\"type\":\"HELLO\",\"app_id\":\"org.example.notes\"}" },
		{ FPLINUX_UI_SESSION_READY, "{\"type\":\"READY\"}" },
		{ FPLINUX_UI_SESSION_TITLE,
		  "{\"type\":\"TITLE\",\"title\":\"Notes\"}" },
		{ FPLINUX_UI_SESSION_SOFTKEYS,
		  "{\"type\":\"SOFTKEYS\",\"left\":\"Menu\","
		  "\"centre\":\"Open\",\"right\":\"Back\"}" },
		{ FPLINUX_UI_SESSION_HOME, "{\"type\":\"HOME\"}" },
		{ FPLINUX_UI_SESSION_EXIT, "{\"type\":\"EXIT\"}" },
		{ FPLINUX_UI_SESSION_CLOSE, "{\"type\":\"CLOSE\"}" },
		{ FPLINUX_UI_SESSION_ERROR,
		  "{\"type\":\"ERROR\",\"reason\":\"app-unavailable\"}" },
		{ FPLINUX_UI_SESSION_VISIBILITY,
		  "{\"type\":\"VISIBILITY\",\"visible\":true}" },
		{ FPLINUX_UI_SESSION_KEEP_SCREEN_ON,
		  "{\"type\":\"KEEP_SCREEN_ON\",\"enabled\":true}" },
		{ FPLINUX_UI_SESSION_CANCEL, "{\"type\":\"CANCEL\"}" },
	};
	struct fplinux_ui_session_message received;

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		struct fplinux_ui_session_message sent = {
			.type = cases[i].type,
		};

		check(receive_raw(sender, receiver, cases[i].packet,
				  &received) == 1 &&
			      received.type == cases[i].type,
		      "literal message name resolves to the declared packet type");
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == cases[i].type,
		      "simple message type survives the round trip");
	}
}

static void check_tasks(int sender, int receiver)
{
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_TASKS,
	};
	struct fplinux_ui_session_message received;
	static const char *const invalid[] = {
		"{\"type\":\"TASKS\",\"tasks\":null}",
		"{\"type\":\"TASKS\",\"tasks\":{}}",
		"{\"type\":\"TASKS\",\"tasks\":[{},{},{},{},{}]}",
		"{\"type\":\"TASKS\",\"tasks\":[{\"app_id\":\"org.example.notes\","
		"\"title\":\"Notes\",\"lifecycle\":\"background\"}]}",
		"{\"type\":\"TASKS\",\"tasks\":[{\"app_id\":\"org.example.notes\","
		"\"title\":\"Notes\",\"lifecycle\":\"hidden\",\"token\":1}]}",
		"{\"type\":\"TASKS\",\"tasks\":[{\"app_id\":\"org.example.notes\","
		"\"title\":\"Notes\",\"lifecycle\":\"background\",\"token\":0}]}",
		"{\"type\":\"ACTIVE\",\"tasks\":[]}",
		"{\"type\":\"ACTIVATE\",\"app_id\":\"org.example.notes\"}",
		"{\"type\":\"ACTIVATE\",\"app_id\":\"org.example.notes\",\"token\":-1}",
		"{\"type\":\"ACTIVATE\",\"app_id\":\"org.example.notes\",\"token\":4294967296}",
		"{\"type\":\"ACTIVATE\",\"app_id\":\"org.example.notes\",\"token\":\"1\"}",
		"{\"type\":\"CLOSE_TASK\",\"app_id\":\"../notes\",\"token\":1}",
		"{\"type\":\"CLOSE_TASK\",\"token\":1}",
		"{\"type\":\"HOME\",\"token\":1}",
	};

	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      !received.has_tasks,
	      "TASKS query has no snapshot");
	sent.has_tasks = true;
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.has_tasks && !received.task_count,
	      "empty snapshot remains distinct from a query");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"TASKS\",\"tasks\":["
		      "{\"app_id\":\"org.example.notes\",\"title\":\"Заметки\","
		      "\"lifecycle\":\"background\",\"token\":7},"
		      "{\"app_id\":\"org.example.editor\",\"title\":\"Editor\","
		      "\"lifecycle\":\"closing\",\"token\":4294967295}]}",
		      &received) == 1 &&
		      received.task_count == 2 &&
		      !strcmp(received.tasks[0].app_id, "org.example.notes") &&
		      !strcmp(received.tasks[0].title, "Заметки") &&
		      received.tasks[0].token == 7 &&
		      received.tasks[0].lifecycle ==
			      FPLINUX_UI_SESSION_BACKGROUND &&
		      received.tasks[1].token == UINT32_MAX &&
		      received.tasks[1].lifecycle == FPLINUX_UI_SESSION_CLOSING,
	      "literal task snapshot preserves titles, identities and lifecycle");
	sent = received;
	sent.task_count = 4;
	sent.tasks[2] = (struct fplinux_ui_session_task){
		.app_id = "org.example.third",
		.title = "Third",
		.token = 8,
		.lifecycle = FPLINUX_UI_SESSION_FOREGROUND,
	};
	sent.tasks[3] = (struct fplinux_ui_session_task){
		.app_id = "org.example.fourth",
		.title = "Fourth",
		.token = 9,
		.lifecycle = FPLINUX_UI_SESSION_BACKGROUND,
	};
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.task_count == 4 &&
		      !strcmp(received.tasks[3].app_id, "org.example.fourth") &&
		      received.tasks[3].token == 9,
	      "full four-task snapshot fits the packet");
	sent.task_count = 5;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "oversized snapshot is rejected before indexing tasks");
	sent.task_count = 4;
	sent.tasks[3].token = 8;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "duplicate instance identities are rejected");
	sent.tasks[3].token = 9;
	strcpy(sent.tasks[3].app_id, "org.example.third");
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "duplicate application identities are rejected");
	const enum fplinux_ui_session_message_type commands[] = {
		FPLINUX_UI_SESSION_ACTIVATE,
		FPLINUX_UI_SESSION_CLOSE_TASK,
	};

	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		sent = (struct fplinux_ui_session_message){
			.type = commands[i],
			.app_id = "org.example.notes",
			.token = UINT32_MAX,
		};
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == commands[i] &&
			      received.token == UINT32_MAX &&
			      !strcmp(received.app_id, "org.example.notes"),
		      "task action preserves app and instance together");
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "invalid task packets are rejected");
}

static void check_close_preparation(int sender, int receiver)
{
	struct fplinux_ui_session_message received;
	static const char *const replies[] = { "ready", "cancel", "wait" };
	static const char *const invalid[] = {
		"{\"type\":\"PREPARE_CLOSE\"}",
		"{\"type\":\"PREPARE_CLOSE\",\"serial\":0}",
		"{\"type\":\"CLOSE_REPLY\",\"serial\":7}",
		"{\"type\":\"CLOSE_REPLY\",\"serial\":7,\"value\":\"discard\"}",
		"{\"type\":\"CLOSE_REPLY\",\"serial\":0,\"value\":\"ready\"}",
		"{\"type\":\"CLOSE_ACK\"}",
		"{\"type\":\"CLOSE_ACK\",\"serial\":0}",
	};

	check(receive_raw(sender, receiver,
			  "{\"type\":\"PREPARE_CLOSE\",\"serial\":17,"
			  "\"reason\":\"user-close\"}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_PREPARE_CLOSE &&
		      received.serial == 17 &&
		      !strcmp(received.reason, "user-close"),
	      "ordinary close preparation carries a nonzero attempt serial");
	check(receive_raw(sender, receiver,
			  "{\"type\":\"CLOSE_ACK\",\"serial\":17}",
			  &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CLOSE_ACK &&
		      received.serial == 17 &&
		      fplinux_ui_session_send(sender, &received) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_CLOSE_ACK &&
		      received.serial == 17,
	      "decision frame acknowledgment preserves its close attempt");
	for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); ++i) {
		char packet[128];

		snprintf(
			packet, sizeof(packet),
			"{\"type\":\"CLOSE_REPLY\",\"serial\":17,\"value\":\"%s\"}",
			replies[i]);
		check(receive_raw(sender, receiver, packet, &received) == 1 &&
			      received.type == FPLINUX_UI_SESSION_CLOSE_REPLY &&
			      received.serial == 17 &&
			      !strcmp(received.value, replies[i]),
		      "close reply preserves its attempt and decision");
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "missing attempts and unknown close decisions are rejected");
}

static void check_storage_results(int sender, int receiver)
{
	struct fplinux_ui_session_message received;
	static const char *const valid[] = {
		"{\"type\":\"SETTING\",\"name\":\"theme\",\"value\":\"dark\","
		"\"applied\":true,\"storage_state\":\"durable\",\"storage_error\":0}",
		"{\"type\":\"ERROR\",\"name\":\"theme\",\"value\":\"dark\","
		"\"applied\":true,\"storage_state\":\"unpublished\",\"storage_error\":28}",
		"{\"type\":\"ERROR\",\"name\":\"locale\",\"value\":\"ru\","
		"\"applied\":true,\"storage_state\":\"published-unsynced\",\"storage_error\":5}",
	};
	static const char *const invalid[] = {
		"{\"type\":\"HOME\",\"applied\":true}",
		"{\"type\":\"SETTING\",\"applied\":1}",
		"{\"type\":\"ERROR\",\"storage_state\":\"saved\"}",
		"{\"type\":\"ERROR\",\"applied\":true,\"storage_state\":\"durable\\u0000unsynced\",\"storage_error\":0}",
		"{\"type\":\"ERROR\",\"storage_error\":-1}",
		"{\"type\":\"ERROR\",\"storage_error\":\"28\"}",
		"{\"type\":\"ERROR\",\"storage_state\":\"durable\",\"storage_error\":5}",
		"{\"type\":\"ERROR\",\"storage_state\":\"published-unsynced\",\"storage_error\":0}",
		"{\"type\":\"ERROR\",\"applied\":false,\"storage_state\":\"durable\",\"storage_error\":0}",
		"{\"type\":\"ERROR\",\"applied\":false,\"storage_state\":\"published-unsynced\",\"storage_error\":5}",
	};
	static const enum fplinux_storage_state states[] = {
		FPLINUX_STORAGE_DURABLE,
		FPLINUX_STORAGE_UNPUBLISHED,
		FPLINUX_STORAGE_PUBLISHED_UNSYNCED,
	};
	static const int errors[] = { 0, 28, 5 };

	for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
		check(receive_raw(sender, receiver, valid[i], &received) == 1 &&
			      received.applied &&
			      received.storage_state == states[i] &&
			      received.storage_error == errors[i],
		      "setting replies distinguish applied state from file durability");
		struct fplinux_ui_session_message sent = received;

		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.applied &&
			      received.storage_state == states[i] &&
			      received.storage_error == errors[i],
		      "serialization preserves applied state and the original errno");
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "invalid storage facts and unrelated packet fields are rejected");
}

static void check_activation(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"ACTIVATION\",\"serial\":1}",
		"{\"type\":\"ACTIVATION\",\"activation\":\"open\"}",
		"{\"type\":\"ACTIVATION\",\"serial\":0,\"activation\":\"open\"}",
		"{\"type\":\"ACTIVATION\",\"serial\":1,\"activation\":\"restart\"}",
		"{\"type\":\"ACTIVATION\",\"serial\":1,\"activation\":1}",
		"{\"type\":\"ACTIVATION_ACK\"}",
		"{\"type\":\"ACTIVATION_ACK\",\"serial\":0}",
		"{\"type\":\"ACTIVATION_ACK\",\"serial\":1,\"activation\":\"open\"}",
		"{\"type\":\"LIFECYCLE\",\"lifecycle\":\"foreground\",\"activation\":\"resume\"}",
	};
	struct fplinux_ui_session_message message;

	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"ACTIVATION\",\"serial\":4294967295,\"activation\":\"open\"}",
		      &message) == 1 &&
		      message.type == FPLINUX_UI_SESSION_ACTIVATION &&
		      message.activation ==
			      FPLINUX_UI_SESSION_ACTIVATION_OPEN &&
		      message.serial == UINT32_MAX,
	      "OPEN carries a nonzero unsigned operation serial");
	message.activation = FPLINUX_UI_SESSION_ACTIVATION_RESUME;
	check(fplinux_ui_session_send(sender, &message) &&
		      fplinux_ui_session_receive(receiver, &message) == 1 &&
		      message.activation ==
			      FPLINUX_UI_SESSION_ACTIVATION_RESUME &&
		      message.serial == UINT32_MAX,
	      "RESUME round-trips independently of lifecycle");
	message = (struct fplinux_ui_session_message){
		.type = FPLINUX_UI_SESSION_ACTIVATION_ACK,
		.serial = UINT32_MAX,
	};
	check(fplinux_ui_session_send(sender, &message) &&
		      fplinux_ui_session_receive(receiver, &message) == 1 &&
		      message.type == FPLINUX_UI_SESSION_ACTIVATION_ACK &&
		      message.serial == UINT32_MAX,
	      "ACTIVATION_ACK retains its own message identity and serial");
	message.serial = 0;
	check(!fplinux_ui_session_send(sender, &message) && errno == EINVAL,
	      "ACTIVATION_ACK requires an operation serial");
	message.type = FPLINUX_UI_SESSION_ACTIVATION;
	message.serial = 1;
	message.activation = (enum fplinux_ui_session_activation)2;
	check(!fplinux_ui_session_send(sender, &message) && errno == EINVAL,
	      "only OPEN and RESUME can be sent");
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &message) ==
				      -1 &&
			      errno == EPROTO,
		      "activation requires its own intent and nonzero serial");
}

static void check_documents(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"OPEN_FILE\",\"path\":\"/tmp/game.jar\"}",
		"{\"type\":\"OPEN_FILE\",\"serial\":1,\"path\":\"game.jar\"}",
		"{\"type\":\"DOCUMENT_RESULT\",\"serial\":1}",
		"{\"type\":\"DOCUMENT_RESULT\",\"serial\":1,\"document_status\":\"error\"}",
		"{\"type\":\"DOCUMENT_RESULT\",\"serial\":1,\"document_status\":\"accepted\",\"document_error\":5}",
		"{\"type\":\"HOME\",\"path\":\"/tmp/game.jar\"}",
		"{\"type\":\"DOCUMENT_CHOICES\",\"serial\":1,\"document_choices\":[]}",
	};
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_OPEN_FILE,
		.serial = 17,
	};
	struct fplinux_ui_session_message received;
	char directory[] = "/tmp/fplinux-document-XXXXXX";
	char path[256], link[256], child[256];
	int fd;

	strcpy(message.path, "/tmp/Игра one 'quoted'.JaR");
	check(fplinux_ui_session_send(sender, &message) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.type == FPLINUX_UI_SESSION_OPEN_FILE &&
		      received.serial == 17 &&
		      !strcmp(received.path, message.path),
	      "document path retains spaces, Unicode and quotes as data");
	strcpy(message.path, "/tmp/name\nwith\ttabs.jar");
	check(fplinux_ui_session_send(sender, &message) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      !strcmp(received.path, "/tmp/name\nwith\ttabs.jar"),
	      "raw file-name controls are preserved without becoming UI labels");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"DOCUMENT_CHOICES\",\"serial\":17,\"document_choices\":[{\"app_id\":\"org.example.game\",\"title\":\"Игра\",\"icon\":\"game\"}]}",
		      &received) == 1 &&
		      received.document_choice_count == 1 &&
		      !strcmp(received.document_choices[0].title, "Игра") &&
		      !strcmp(received.document_choices[0].icon, "game"),
	      "chooser uses the installed application's localized title and icon");
	for (unsigned int status = FPLINUX_UI_DOCUMENT_ACCEPTED;
	     status <= FPLINUX_UI_DOCUMENT_RESTART_REQUIRED; ++status) {
		message = (struct fplinux_ui_session_message){
			.type = FPLINUX_UI_SESSION_DOCUMENT_RESULT,
			.serial = 19,
			.document_status = status,
			.document_error =
				status == FPLINUX_UI_DOCUMENT_ERROR ? EIO : 0,
		};
		check(fplinux_ui_session_send(sender, &message) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.serial == 19 &&
			      received.document_status == status &&
			      received.document_error == message.document_error,
		      "typed document decisions survive transport with their request serial");
	}
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "document packets require a typed decision and nonzero request");
	check(mkdtemp(directory) != NULL, "create owned document directory");
	snprintf(path, sizeof(path), "%s/game.jar", directory);
	snprintf(link, sizeof(link), "%s/linked", directory);
	snprintf(child, sizeof(child), "%s/linked/game.jar", directory);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(fd >= 0, "create regular document");
	if (fd >= 0)
		close(fd);
	check(fplinux_ui_session_document_path_error(path) == 0,
	      "regular absolute document is admitted");
	snprintf(child, sizeof(child), "%s/raw-\xff.jar", directory);
	fd = open(child, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	check(fd >= 0, "create a regular document with a non-UTF-8 name");
	if (fd >= 0)
		close(fd);
	check(fplinux_ui_session_document_path_error(child) == EILSEQ,
	      "an unrepresentable file name fails locally without breaking the session");
	check(unlink(child) == 0, "remove the raw-name document");
	snprintf(child, sizeof(child), "%s/linked/game.jar", directory);
	check(fplinux_ui_session_document_path_error(directory) == EISDIR,
	      "directory is not a document");
	check(symlink(directory, link) == 0, "create owned directory symlink");
	check(fplinux_ui_session_document_path_error(child) == ELOOP,
	      "document resolution does not traverse a directory symlink");
	check(unlink(link) == 0 && symlink(path, link) == 0,
	      "replace owned directory link with file link");
	check(fplinux_ui_session_document_path_error(link) == ELOOP,
	      "document resolution does not follow the final symlink");
	check(unlink(link) == 0 && unlink(path) == 0 && rmdir(directory) == 0,
	      "release owned document fixtures");
}

static void check_volumes(int sender, int receiver)
{
	static const char *const invalid[] = {
		"{\"type\":\"STORAGE_REQUEST\",\"serial\":1,\"volume_operation\":\"connect\",\"volume_id\":0}",
		"{\"type\":\"STORAGE_REQUEST\",\"serial\":1,\"volume_operation\":\"refresh\",\"volume_id\":7}",
		"{\"type\":\"STORAGE_REQUEST\",\"volume_operation\":\"refresh\",\"volume_id\":0}",
		"{\"type\":\"STORAGE_REQUEST\",\"serial\":1,\"volume_operation\":\"format\",\"volume_id\":7}",
		"{\"type\":\"STORAGE_REQUEST\",\"serial\":1,\"volume_operation\":\"eject\",\"volume_id\":4294967296}",
		"{\"type\":\"STORAGE_REQUEST\",\"serial\":1,\"volume_operation\":\"connect\",\"volume_id\":7,\"path\":\"/dev/mmcblk0\"}",
		"{\"type\":\"STORAGE_RESULT\",\"serial\":1,\"volume_error\":0,\"home_persistent\":false,\"volumes\":[{},{},{},{},{}]}",
		"{\"type\":\"STORAGE_RESULT\",\"serial\":1,\"volume_error\":-1,\"home_persistent\":false,\"volumes\":[]}",
		"{\"type\":\"STORAGE_RESULT\",\"serial\":1,\"volume_error\":0,\"volumes\":[]}",
		"{\"type\":\"READY\",\"volumes\":[]}",
	};
	struct fplinux_ui_session_message message;
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_STORAGE_RESULT,
		.serial = 42,
		.volume_result = {
			.request_id = 42,
			.error = EBUSY,
			.snapshot = {
				.count = 1,
				.home_persistent = true,
				.volumes = {{
					.id = UINT32_MAX,
					.mount_id = 31,
					.state = FPLINUX_VOLUME_MOUNTED,
					.label = "Карта one 'quoted'",
					.path = "/mnt/card",
				}},
			},
		},
	};

	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"STORAGE_REQUEST\",\"serial\":9,\"volume_operation\":\"connect\",\"volume_id\":4294967295}",
		      &message) == 1 &&
		      message.serial == 9 &&
		      message.volume_operation == FPLINUX_VOLUME_CONNECT &&
		      message.volume_id == UINT32_MAX,
	      "storage accepts the entire opaque ID range without raw device paths");
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &message) == 1 &&
		      message.volume_result.request_id == 42 &&
		      message.volume_result.error == EBUSY &&
		      message.volume_result.snapshot.home_persistent &&
		      message.volume_result.snapshot.count == 1 &&
		      message.volume_result.snapshot.volumes[0].id ==
			      UINT32_MAX &&
		      message.volume_result.snapshot.volumes[0].mount_id ==
			      31 &&
		      !strcmp(message.volume_result.snapshot.volumes[0].label,
			      "Карта one 'quoted'") &&
		      !strcmp(message.volume_result.snapshot.volumes[0].path,
			      "/mnt/card"),
	      "a busy result retains the complete mounted volume snapshot");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"STORAGE_RESULT\",\"serial\":10,\"volume_error\":0,\"home_persistent\":false,\"volumes\":[]}",
		      &message) == 1 &&
		      !message.volume_result.snapshot.count,
	      "no card is a successful empty snapshot");
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(sender, receiver, invalid[i], &message) ==
				      -1 &&
			      errno == EPROTO,
		      "storage rejects invalid operations, IDs and data in the wrong direction");
	sent.volume_result.snapshot.volumes[0].state = FPLINUX_VOLUME_AVAILABLE;
	check(!fplinux_ui_session_send(sender, &sent) && errno == EINVAL,
	      "an unmounted volume cannot expose the empty mount directory");
}

int main(void)
{
	static const char *const invalid[] = {
		"{\"type\":\"ACTIVE\",\"colour\":\"red\"}",
		"{\"type\":\"ACTIVE\",\"display\":\"bright\"}",
		"{\"type\":\"ACTIVE\",\"display\":1}",
		"{\"type\":\"ACTIVE\",\"locked\":\"yes\"}",
		"{\"type\":\"ACTIVE_ACK\"}",
		"{\"type\":\"ACTIVE_ACK\",\"serial\":0}",
		"{\"type\":\"ACTIVE_ACK\",\"serial\":-1}",
		"{\"type\":\"ACTIVE_ACK\",\"serial\":4294967296}",
		"{\"type\":\"ACTIVE_ACK\",\"serial\":\"1\"}",
		"{\"type\":\"REDRAW_ACK\"}",
		"{\"type\":\"REDRAW_ACK\",\"serial\":0}",
		"{\"type\":\"REDRAW_ACK\",\"serial\":-1}",
		"{\"type\":\"VISIBILITY\",\"visible\":1}",
		"{\"type\":\"KEEP_SCREEN_ON\",\"enabled\":null}",
		"{\"type\":\"SETTING\",\"name\":\"a_setting_name_longer_than_31_bytes\"}",
		"{\"type\":\"CONFIG\",\"locale\":\"ru_RU.UTF-8\"}",
		"{\"type\":\"CONFIG\",\"theme\":\"blue\"}",
		"{\"type\":\"CONFIG\",\"screen_width\":\"240\"}",
		"{\"type\":\"SOFTKEYS\",\"centre\":7}",
		"{\"type\":\"SOFTKEYS\",\"centre\":\"\xd0\"}",
		"{\"type\":\"TITLE\",\"title\":\"a\\u000bb\"}",
		"{\"type\":\"SOFTKEYS\",\"left\":\"a\\u007fb\"}",
		"{\"type\":\"SLEEP\"}",
		"{\"type\":\"ACTIVE\",\"battery\":101}",
		"{\"type\":\"ACTIVE\",\"battery\":-2}",
		"{\"type\":\"ACTIVE\",\"battery\":\"50\"}",
		"{\"type\":\"ACTIVE\",\"charging\":1}",
		"{\"type\":\"MODE\",\"mode\":\"windowed\"}",
		"{\"display\":\"on\"}",
		"[\"ACTIVE\"]",
		"{\"type\":\"HELLO\"}{}",
		"{\"type\":\"control\",\"name\":\"volume\"}",
		"{\"type\":\"ACTIVE\",\"control_centre\":false}",
		"{\"type\":\"CONFIG\",\"audio\":\"false\"}",
		"{\"type\":\"CONFIG\",\"background_audio\":false}",
		"{\"type\":\"CONFIG\",\"vibration\":null}",
		"{\"type\":\"ACTIVE\",\"pattern\":[]}",
	};
	struct fplinux_ui_session_message received;
	int pair[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) < 0) {
		perror("socketpair");
		return 1;
	}
	check_state_packets(pair[0], pair[1]);
	check_simple_packets(pair[0], pair[1]);
	check_clock_packets(pair[0], pair[1]);
	check_clock_presentation(pair[0], pair[1]);
	check_input_locales(pair[0], pair[1]);
	check_lifecycle(pair[0], pair[1]);
	check_activation(pair[0], pair[1]);
	check_documents(pair[0], pair[1]);
	check_volumes(pair[0], pair[1]);
	check_tasks(pair[0], pair[1]);
	check_close_preparation(pair[0], pair[1]);
	check_storage_results(pair[0], pair[1]);
	check_launch_frame(pair[0], pair[1]);
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
		check(receive_raw(pair[0], pair[1], invalid[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      "unknown, mistyped and oversized fields are rejected");
	close(pair[0]);
	close(pair[1]);
	if (failures) {
		fprintf(stderr, "%u checks failed\n", failures);
		return 1;
	}
	puts("PASS: session packets, input languages and launch descriptor ownership");
	return 0;
}
