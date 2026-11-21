/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ipc/mre-control.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_bytes(int fd, const uint8_t *data, size_t bytes)
{
	assert(write(fd, data, bytes) == (ssize_t)bytes);
}

static void check_encoding(void)
{
	static const uint8_t key[] = { 1, 5, 0, 2, 0xfd, 0xff, 0xff, 0xff };
	static const uint8_t display[] = { 16, 4, 0, 0xf0, 0, 0x40, 1 };
	static const uint8_t audio[] = { 15,   8,    0,	   0x78, 0x56, 0x34,
					 0x12, 0xff, 0xff, 0xff, 0xff };
	uint8_t encoded[514];
	struct mre_control_message message = {
		.opcode = MRE_CONTROL_KEY,
		.input.key = { .event = MRE_KEY_EVENT_DOWN, .code = -3 },
	};

	assert(mre_control_encode(&message, MRE_CONTROL_FRONTEND, encoded,
				  sizeof(encoded)) == sizeof(key));
	assert(!memcmp(encoded, key, sizeof(key)));
	assert(!mre_control_encode(&message, MRE_CONTROL_ENGINE, encoded,
				   sizeof(encoded)));
	assert(!mre_control_encode(&message, MRE_CONTROL_FRONTEND, encoded, 7));

	message = (struct mre_control_message){
		.opcode = MRE_CONTROL_DISPLAY,
		.display = { .width = 240, .height = 320 },
	};
	assert(mre_control_encode(&message, MRE_CONTROL_ENGINE, encoded,
				  sizeof(encoded)) == sizeof(display));
	assert(!memcmp(encoded, display, sizeof(display)));

	message = (struct mre_control_message){
		.opcode = MRE_CONTROL_AUDIO_EVENT,
		.audio = { .handle = UINT32_C(0x12345678), .event = -1 },
	};
	assert(mre_control_encode(&message, MRE_CONTROL_FRONTEND, encoded,
				  sizeof(encoded)) == sizeof(audio));
	assert(!memcmp(encoded, audio, sizeof(audio)));

	char path[259];
	static const uint8_t select_header[] = { 7, 3, 1 };

	memset(path, 'a', sizeof(path));
	memcpy(path, "C:\\", 3);
	message = (struct mre_control_message){
		.opcode = MRE_CONTROL_SELECT,
		.text = { .data = path, .length = sizeof(path) },
	};
	assert(mre_control_encode(&message, MRE_CONTROL_FRONTEND, encoded,
				  sizeof(encoded)) == 262);
	assert(!memcmp(encoded, select_header, sizeof(select_header)));
	assert(!memcmp(encoded + sizeof(select_header), path, sizeof(path)));
}

static void check_fragmented_read(void)
{
	static const uint8_t fragment[] = { 4, 0, 0x12, 0x34 };
	static const uint8_t joined[] = {
		0x56, 0x78, /* end of FRAME */
		28,   4,    0, 0xd1, 0x8f, '\n', 'x', /* ERROR with UTF-8 */
		27,   0,    0, /* END */
	};
	static const uint8_t expected_text[] = { 0xd1, 0x8f, '\n', 'x' };
	struct mre_control_message message;
	struct mre_control_reader reader = { 0 };
	int pipe_fds[2];

	assert(pipe2(pipe_fds, O_NONBLOCK) == 0);
	write_bytes(pipe_fds[1], (const uint8_t[]){ 17 }, 1);
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_WAIT);
	/* A split header and partial framebuffer sequence stay in the reader. */
	write_bytes(pipe_fds[1], fragment, sizeof(fragment));
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_WAIT);
	write_bytes(pipe_fds[1], joined, sizeof(joined));
	assert(close(pipe_fds[1]) == 0);

	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_MESSAGE);
	assert(message.opcode == MRE_CONTROL_FRAME);
	assert(message.sequence == UINT32_C(0x78563412));
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_MESSAGE);
	assert(message.opcode == MRE_CONTROL_ERROR);
	assert(message.text.length == sizeof(expected_text));
	assert(!memcmp(message.text.data, expected_text,
		       sizeof(expected_text)));
	assert(!message.text.data[message.text.length]);
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_MESSAGE);
	assert(message.opcode == MRE_CONTROL_END);
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_ENGINE,
				&message) == MRE_CONTROL_EOF);
	assert(close(pipe_fds[0]) == 0);
}

static void check_read_error(const uint8_t *bytes, size_t length,
			     enum mre_control_side sender,
			     enum mre_control_read_result expected)
{
	struct mre_control_message message;
	struct mre_control_reader reader = { 0 };
	int pipe_fds[2];

	assert(pipe2(pipe_fds, O_NONBLOCK) == 0);
	write_bytes(pipe_fds[1], bytes, length);
	if (expected == MRE_CONTROL_TRUNCATED)
		assert(close(pipe_fds[1]) == 0);
	assert(mre_control_read(&reader, pipe_fds[0], sender, &message) ==
	       expected);
	if (expected != MRE_CONTROL_TRUNCATED)
		assert(close(pipe_fds[1]) == 0);
	assert(close(pipe_fds[0]) == 0);
}

static void check_malformed_frames(void)
{
	check_read_error((const uint8_t[]){ 17, 4 }, 2, MRE_CONTROL_ENGINE,
			 MRE_CONTROL_TRUNCATED);
	check_read_error((const uint8_t[]){ 17, 4, 0, 1 }, 4,
			 MRE_CONTROL_ENGINE, MRE_CONTROL_TRUNCATED);
	check_read_error((const uint8_t[]){ 28, 0, 2 }, 3, MRE_CONTROL_ENGINE,
			 MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 4, 4, 0 }, 3, MRE_CONTROL_ENGINE,
			 MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 255, 0, 0 }, 3, MRE_CONTROL_ENGINE,
			 MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 17, 5, 0 }, 3, MRE_CONTROL_ENGINE,
			 MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 3, 1, 0, 2 }, 4,
			 MRE_CONTROL_FRONTEND, MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 1, 5, 0, 0, 0xfe, 0xff, 0xff,
					    0xff },
			 8, MRE_CONTROL_FRONTEND, MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 2, 5, 0, 1, 0, 0, 0x40, 1 }, 8,
			 MRE_CONTROL_FRONTEND, MRE_CONTROL_INVALID);
	check_read_error((const uint8_t[]){ 22, 4, 0, 'E', ':', '\\', 0 }, 7,
			 MRE_CONTROL_ENGINE, MRE_CONTROL_INVALID);
}

static void check_partial_write(void)
{
	int pipe_fds[2];

	assert(pipe2(pipe_fds, O_NONBLOCK) == 0);
	int capacity = fcntl(pipe_fds[1], F_GETPIPE_SZ);
	static const uint8_t suffix[] = { 4, 4, 0, 0x67, 0x45, 0x23, 1 };

	assert(capacity > 0);
	size_t queued = (size_t)capacity + sizeof(suffix);
	uint8_t *bytes = malloc(queued);
	uint8_t *received = malloc((size_t)capacity);

	assert(bytes && received);
	memset(bytes, 0x5a, (size_t)capacity);
	memcpy(bytes + capacity, suffix, sizeof(suffix));
	assert(mre_control_flush(pipe_fds[1], bytes, &queued));
	assert(queued == sizeof(suffix));
	assert(read(pipe_fds[0], received, (size_t)capacity) == capacity);
	for (ssize_t i = 0; i < capacity; ++i)
		assert(received[i] == 0x5a);
	assert(mre_control_flush(pipe_fds[1], bytes, &queued));
	assert(!queued);
	assert(read(pipe_fds[0], received, sizeof(suffix)) == sizeof(suffix));
	assert(!memcmp(received, suffix, sizeof(suffix)));
	free(received);
	free(bytes);
	assert(close(pipe_fds[0]) == 0 && close(pipe_fds[1]) == 0);
}

static void check_viewer_reply_and_async_input(void)
{
	static const uint8_t
		pending
			[] = {
				1,    5,    0,	  2,	0xfd, 0xff,
				0xff, 0xff, /* KEY down -3 */
				15,   8,    0,	  5,	0,    0,
				0,    0xff, 0xff, 0xff, 0xff, /* AUDIO_EVENT */
				2,    5,    0,	  1,	0xef, 0,
				0x3f, 1, /* PEN at edge */
				13,   4,    0,	  7,	0,    0,
				0, /* HTTP_RESPONSE */
				3,    1,    0,	  0, /* VISIBILITY hidden */
				4,    4,    0,	  0x78, 0x56, 0x34,
				0x12, /* correct ACK */
				4,    4,    0,	  0x79, 0x56, 0x34,
				0x12, /* different frame */
				14,   4,    0,	  0x78, 0x56, 0x34,
				0x12, /* same identity, wrong reply */
				12,   4,    0,	  8,	0,    0,
				0, /* HTTP acceptance for another request */
				8,    0,    0, /* SELECT_CANCEL */
			};
	static const enum mre_control_reply_result results[] = {
		MRE_CONTROL_REPLY_DEFERRED,	  MRE_CONTROL_REPLY_SERVICE,
		MRE_CONTROL_REPLY_DEFERRED,	  MRE_CONTROL_REPLY_SERVICE,
		MRE_CONTROL_REPLY_DEFERRED,	  MRE_CONTROL_REPLY_MATCH,
		MRE_CONTROL_REPLY_WRONG_IDENTITY, MRE_CONTROL_REPLY_UNEXPECTED,
	};
	struct mre_control_deferred deferred = { 0 };
	struct mre_control_reader reader = { 0 };
	struct mre_control_message expected = {
		.opcode = MRE_CONTROL_ACK,
		.sequence = UINT32_C(0x12345678),
	};
	struct mre_control_message message;
	int pipe_fds[2];

	assert(pipe(pipe_fds) == 0);
	write_bytes(pipe_fds[1], pending, sizeof(pending));
	assert(close(pipe_fds[1]) == 0);
	for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
		assert(mre_control_read(&reader, pipe_fds[0],
					MRE_CONTROL_FRONTEND,
					&message) == MRE_CONTROL_MESSAGE);
		assert(mre_control_route_reply(&deferred, &message, &expected,
					       MRE_CONTROL_NO_REPLY) ==
		       results[i]);
		if (message.opcode == MRE_CONTROL_HTTP_RESPONSE)
			assert(message.http_id == 7);
		if (message.opcode == MRE_CONTROL_AUDIO_EVENT)
			assert(message.audio.handle == 5 &&
			       message.audio.event == -1);
	}
	expected = (struct mre_control_message){
		.opcode = MRE_CONTROL_HTTP_ACCEPTED, .http_id = 7
	};
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_FRONTEND,
				&message) == MRE_CONTROL_MESSAGE);
	assert(mre_control_route_reply(&deferred, &message, &expected,
				       MRE_CONTROL_NO_REPLY) ==
	       MRE_CONTROL_REPLY_WRONG_IDENTITY);
	expected = (struct mre_control_message){ .opcode = MRE_CONTROL_SELECT };
	assert(mre_control_read(&reader, pipe_fds[0], MRE_CONTROL_FRONTEND,
				&message) == MRE_CONTROL_MESSAGE);
	assert(mre_control_route_reply(&deferred, &message, &expected,
				       MRE_CONTROL_SELECT_CANCEL) ==
	       MRE_CONTROL_REPLY_MATCH);
	assert(close(pipe_fds[0]) == 0);

	assert(mre_control_take_input(&deferred, &message));
	assert(message.opcode == MRE_CONTROL_KEY &&
	       message.input.key.event == MRE_KEY_EVENT_DOWN &&
	       message.input.key.code == -3);
	assert(mre_control_take_input(&deferred, &message));
	assert(message.opcode == MRE_CONTROL_PEN &&
	       message.input.pen.x == 239 && message.input.pen.y == 319 &&
	       message.input.pen.event == MRE_PEN_EVENT_TAP);
	assert(mre_control_take_input(&deferred, &message));
	assert(message.opcode == MRE_CONTROL_VISIBILITY &&
	       !message.input.visible);
	assert(!mre_control_take_input(&deferred, &message));
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "encoding"))
		check_encoding();
	else if (!strcmp(argv[1], "fragmented-read"))
		check_fragmented_read();
	else if (!strcmp(argv[1], "malformed"))
		check_malformed_frames();
	else if (!strcmp(argv[1], "partial-write"))
		check_partial_write();
	else if (!strcmp(argv[1], "viewer-reply"))
		check_viewer_reply_and_async_input();
	else
		assert(false);
	return 0;
}
