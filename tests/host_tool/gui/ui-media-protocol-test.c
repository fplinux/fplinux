/* SPDX-License-Identifier: GPL-2.0-only */
#include "fplinux-ui-session-internal.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static unsigned int failures;

static void check(bool condition, const char *description)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", description);
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

static struct json_object *receive_json(int receiver)
{
	char packet[FPLINUX_UI_SESSION_PACKET_BYTES];
	ssize_t bytes = recv(receiver, packet, sizeof(packet) - 1, 0);

	if (bytes <= 0)
		return NULL;
	packet[bytes] = '\0';
	return json_tokener_parse(packet);
}

static bool boolean_field(struct json_object *object, const char *name,
			  bool expected)
{
	struct json_object *field;

	return json_object_object_get_ex(object, name, &field) &&
	       json_object_is_type(field, json_type_boolean) &&
	       !!json_object_get_boolean(field) == expected;
}

static void check_capabilities(int sender, int receiver)
{
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_CONFIG,
		.audio = true,
		.vibration = true,
	};
	struct fplinux_ui_session_message received;

	for (unsigned int enabled = 0; enabled < 2; ++enabled) {
		struct json_object *object;
		struct json_object *field;

		sent.audio = enabled;
		sent.vibration = enabled;
		check(fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == FPLINUX_UI_SESSION_CONFIG &&
			      received.audio == !!enabled &&
			      received.vibration == !!enabled,
		      "CONFIG round-trips enabled and disabled capabilities");
		check(fplinux_ui_session_send(sender, &sent),
		      "CONFIG sends capabilities to a JSON peer");
		object = receive_json(receiver);
		check(object && boolean_field(object, "audio", enabled) &&
			      boolean_field(object, "vibration", enabled) &&
			      !json_object_object_get_ex(object, "pattern",
							 &field) &&
			      !json_object_object_get_ex(object, "repeat",
							 &field),
		      "CONFIG explicitly encodes booleans without vibration requests");
		json_object_put(object);
	}
	check(receive_raw(sender, receiver, "{\"type\":\"CONFIG\"}",
			  &received) == 1 &&
		      !received.audio && !received.vibration,
	      "CONFIG without capabilities reads both as unavailable");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"CONFIG\",\"audio\":true,\"vibration\":false}",
		      &received) == 1 &&
		      received.audio && !received.vibration,
	      "literal CONFIG capabilities remain independent");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"CONFIG\",\"audio\":false,\"vibration\":true}",
		      &received) == 1 &&
		      !received.audio && received.vibration,
	      "literal CONFIG can offer only vibration");
}

static void check_pattern_roundtrips(int sender, int receiver)
{
	static const struct {
		unsigned int count;
		unsigned int pattern[5];
		bool repeat;
	} cases[] = {
		{ 0, { 0 }, false },
		{ 2, { 0, 50 }, false },
		{ 2, { 60000, 3000 }, false },
		{ 3, { 0, 200, 0 }, false },
		{ 3, { 60000, 50, 60000 }, true },
		{ 3, { 40, 200, 60 }, true },
		{ 4, { 0, 1500, 100, 1500 }, true },
		{ 5, { 0, 100, 60000, 100, 60000 }, true },
	};
	struct fplinux_ui_session_message received;

	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		struct fplinux_ui_session_message sent = {
			.type = FPLINUX_UI_SESSION_VIBRATE,
			.pattern_count = cases[i].count,
			.repeat = cases[i].repeat,
		};
		struct json_object *object;
		struct json_object *pattern;
		struct json_object *field;
		bool wire_valid;

		memcpy(sent.pattern, cases[i].pattern,
		       cases[i].count * sizeof(sent.pattern[0]));
		check(fplinux_ui_session_vibrate_valid(&sent) &&
			      fplinux_ui_session_send(sender, &sent) &&
			      fplinux_ui_session_receive(receiver, &received) ==
				      1 &&
			      received.type == FPLINUX_UI_SESSION_VIBRATE &&
			      received.pattern_count == cases[i].count &&
			      received.repeat == cases[i].repeat &&
			      !memcmp(received.pattern, cases[i].pattern,
				      cases[i].count * sizeof(sent.pattern[0])),
		      "VIBRATE round-trips stops, one-shots and repeats at duration bounds");
		check(fplinux_ui_session_send(sender, &sent),
		      "VIBRATE sends a request to a JSON peer");
		object = receive_json(receiver);
		wire_valid =
			object &&
			json_object_object_get_ex(object, "pattern",
						  &pattern) &&
			json_object_is_type(pattern, json_type_array) &&
			json_object_array_length(pattern) == cases[i].count &&
			boolean_field(object, "repeat", cases[i].repeat) &&
			!json_object_object_get_ex(object, "audio", &field) &&
			!json_object_object_get_ex(object, "vibration", &field);
		for (unsigned int j = 0; wire_valid && j < cases[i].count;
		     ++j) {
			field = json_object_array_get_idx(pattern, j);
			wire_valid =
				json_object_is_type(field, json_type_int) &&
				json_object_get_int64(field) ==
					cases[i].pattern[j];
		}
		check(wire_valid,
		      "VIBRATE explicitly encodes durations and repeat, including an empty stop");
		json_object_put(object);
	}
	check(receive_raw(sender, receiver,
			  "{\"type\":\"VIBRATE\",\"pattern\":[0,50,100,3000]}",
			  &received) == 1 &&
		      received.pattern_count == 4 && received.pattern[0] == 0 &&
		      received.pattern[1] == 50 && received.pattern[2] == 100 &&
		      received.pattern[3] == 3000 && !received.repeat,
	      "literal integer durations retain their values and default to one-shot");
	check(receive_raw(
		      sender, receiver,
		      "{\"type\":\"VIBRATE\",\"pattern\":[],\"repeat\":false}",
		      &received) == 1 &&
		      !received.pattern_count && !received.repeat,
	      "a literal empty pattern requests a stop");
}

static void check_pattern_capacity(int sender, int receiver)
{
	struct fplinux_ui_session_message received;
	struct fplinux_ui_session_message sent = {
		.type = FPLINUX_UI_SESSION_VIBRATE,
		.pattern_count = 64,
	};
	char packet[1024];
	size_t used = 0;

	for (unsigned int i = 0; i < 64; ++i)
		sent.pattern[i] = i % 2 ? 50 : 100;
	check(fplinux_ui_session_send(sender, &sent) &&
		      fplinux_ui_session_receive(receiver, &received) == 1 &&
		      received.pattern_count == 64 &&
		      received.pattern[0] == 100 && received.pattern[63] == 50,
	      "the serializer transports all 64 durations");
	for (unsigned int count = 64; count <= 65; ++count) {
		used = snprintf(packet, sizeof(packet),
				"{\"type\":\"VIBRATE\",\"pattern\":[");
		for (unsigned int i = 0; i < count; ++i)
			used += snprintf(packet + used, sizeof(packet) - used,
					 "%s%u", i ? "," : "",
					 i % 2 ? 50 : 100);
		snprintf(packet + used, sizeof(packet) - used, "]}");
		int result = receive_raw(sender, receiver, packet, &received);

		check(count == 64 ?
			      result == 1 && received.pattern_count == 64 :
			      result == -1 && errno == EPROTO,
		      "a JSON peer can send 64 durations but not 65");
	}
}

static void check_invalid_packets(int sender, int receiver)
{
	static const char *const packets[] = {
		"{\"type\":\"VIBRATE\"}",
		"{\"type\":\"VIBRATE\",\"pattern\":null}",
		"{\"type\":\"VIBRATE\",\"pattern\":false}",
		"{\"type\":\"VIBRATE\",\"pattern\":\"0,200\"}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,49]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,3001]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[60001,50]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,50,60001]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,50,99,50]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[-1,50]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,-50]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,4294967296]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,50.0]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,true]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,\"50\"]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,null]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,{}]}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"repeat\":true}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,50],\"repeat\":true}",
		"{\"type\":\"VIBRATE\",\"pattern\":[40,50,59],\"repeat\":true}",
		"{\"type\":\"VIBRATE\",\"pattern\":[0,1500,100,1501],\"repeat\":true}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"repeat\":0}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"repeat\":\"false\"}",
		"{\"type\":\"CONFIG\",\"audio\":1}",
		"{\"type\":\"CONFIG\",\"audio\":null}",
		"{\"type\":\"CONFIG\",\"vibration\":\"true\"}",
		"{\"type\":\"CONFIG\",\"pattern\":[]}",
		"{\"type\":\"CONFIG\",\"repeat\":false}",
		"{\"type\":\"ACTIVE\",\"audio\":false}",
		"{\"type\":\"HOME\",\"vibration\":false}",
		"{\"type\":\"READY\",\"pattern\":[]}",
		"{\"type\":\"HOME\",\"repeat\":false}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"audio\":false}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"vibration\":false}",
		"{\"type\":\"VIBRATE\",\"pattern\":[],\"extra\":true}",
	};
	struct fplinux_ui_session_message received;

	for (size_t i = 0; i < ARRAY_SIZE(packets); ++i) {
		errno = 0;
		check(receive_raw(sender, receiver, packets[i], &received) ==
				      -1 &&
			      errno == EPROTO,
		      packets[i]);
	}
}

static void check_invalid_sends(int sender, int receiver)
{
	static const struct fplinux_ui_session_message cases[] = {
		{ .type = FPLINUX_UI_SESSION_VIBRATE, .pattern_count = 65 },
		{ .type = FPLINUX_UI_SESSION_VIBRATE, .pattern_count = 1 },
		{ .type = FPLINUX_UI_SESSION_VIBRATE,
		  .pattern_count = 2,
		  .pattern = { 0, 49 } },
		{ .type = FPLINUX_UI_SESSION_VIBRATE, .repeat = true },
		{ .type = FPLINUX_UI_SESSION_VIBRATE,
		  .pattern_count = 2,
		  .pattern = { 0, 50 },
		  .repeat = true },
		{ .type = FPLINUX_UI_SESSION_VIBRATE,
		  .pattern_count = 3,
		  .pattern = { 0, 50, 99 },
		  .repeat = true },
		{ .type = FPLINUX_UI_SESSION_VIBRATE,
		  .pattern_count = 4,
		  .pattern = { 0, 1500, 100, 1501 },
		  .repeat = true },
		{ .type = FPLINUX_UI_SESSION_HOME, .audio = true },
		{ .type = FPLINUX_UI_SESSION_HOME, .vibration = true },
		{ .type = FPLINUX_UI_SESSION_CONFIG,
		  .pattern_count = 2,
		  .pattern = { 0, 50 } },
		{ .type = FPLINUX_UI_SESSION_CONFIG, .repeat = true },
	};
	char byte;

	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		errno = 0;
		check(!fplinux_ui_session_send(sender, &cases[i]) &&
			      errno == EINVAL,
		      "invalid media values fail before send with EINVAL");
		check(recv(receiver, &byte, 1, MSG_DONTWAIT) == -1 &&
			      (errno == EAGAIN || errno == EWOULDBLOCK),
		      "invalid media values never queue a packet");
	}
}

int main(void)
{
	int pair[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) < 0)
		return 2;
	check_capabilities(pair[0], pair[1]);
	check_pattern_roundtrips(pair[0], pair[1]);
	check_pattern_capacity(pair[0], pair[1]);
	check_invalid_packets(pair[0], pair[1]);
	check_invalid_sends(pair[0], pair[1]);
	close(pair[0]);
	close(pair[1]);
	return failures ? 1 : 0;
}
