/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_CONTROL_H
#define FPLINUX_MRE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MRE_CONTROL_HEADER_BYTES 3
#define MRE_CONTROL_ERROR_BYTES 511
#define MRE_CONTROL_FRAME_BYTES \
	(MRE_CONTROL_HEADER_BYTES + MRE_CONTROL_ERROR_BYTES)

enum mre_control_side {
	MRE_CONTROL_FRONTEND,
	MRE_CONTROL_ENGINE,
};

/* One opcode byte, a little-endian uint16_t payload size, then the payload.
 * Large data and broker arguments/results remain in mre_shared.
 */
enum mre_control_opcode {
	MRE_CONTROL_NO_REPLY = 0,
	MRE_CONTROL_KEY = 1,
	MRE_CONTROL_PEN = 2,
	MRE_CONTROL_VISIBILITY = 3,
	MRE_CONTROL_ACK = 4,
	MRE_CONTROL_INPUT_DONE = 5,
	MRE_CONTROL_INPUT_CANCEL = 6,
	MRE_CONTROL_SELECT = 7,
	MRE_CONTROL_SELECT_CANCEL = 8,
	MRE_CONTROL_VFS_REPLY = 9,
	MRE_CONTROL_SNAPSHOT_DONE = 10,
	MRE_CONTROL_SNAPSHOT_ERROR = 11,
	MRE_CONTROL_HTTP_ACCEPTED = 12,
	MRE_CONTROL_HTTP_RESPONSE = 13,
	MRE_CONTROL_AUDIO_REPLY = 14,
	MRE_CONTROL_AUDIO_EVENT = 15,
	MRE_CONTROL_DISPLAY = 16,
	MRE_CONTROL_FRAME = 17,
	MRE_CONTROL_POINTER_AVAILABLE = 18,
	MRE_CONTROL_INPUT = 19,
	MRE_CONTROL_SELECTOR = 20,
	MRE_CONTROL_VFS_REQUEST = 21,
	MRE_CONTROL_SNAPSHOT = 22,
	MRE_CONTROL_HTTP_REQUEST = 23,
	MRE_CONTROL_HTTP_CANCEL = 24,
	MRE_CONTROL_AUDIO_REQUEST = 25,
	MRE_CONTROL_VIEW_PICTURE = 26,
	MRE_CONTROL_END = 27,
	MRE_CONTROL_ERROR = 28,
};

enum mre_key_event {
	MRE_KEY_EVENT_UP = 1,
	MRE_KEY_EVENT_DOWN = 2,
};

enum mre_pen_event {
	MRE_PEN_EVENT_TAP = 1,
	MRE_PEN_EVENT_RELEASE = 2,
	MRE_PEN_EVENT_MOVE = 3,
};

union mre_control_input {
	struct {
		enum mre_key_event event;
		int32_t code;
	} key;
	struct {
		enum mre_pen_event event;
		uint16_t x, y;
	} pen;
	bool visible;
};

struct mre_control_message {
	enum mre_control_opcode opcode;
	union {
		union mre_control_input input;
		struct {
			uint16_t width, height;
		} display;
		struct {
			uint32_t handle;
			int32_t event;
		} audio;
		uint32_t sequence;
		uint32_t http_id;
		bool pointer_available;
		struct {
			const char *data;
			size_t length;
		} text;
	};
};

struct mre_control_reader {
	uint8_t bytes[MRE_CONTROL_FRAME_BYTES + 1];
	size_t used;
};

enum mre_control_read_result {
	MRE_CONTROL_MESSAGE,
	MRE_CONTROL_WAIT,
	MRE_CONTROL_EOF,
	MRE_CONTROL_TRUNCATED,
	MRE_CONTROL_INVALID,
	MRE_CONTROL_IO_ERROR,
};

/* A decoded text span borrows the reader until its next read. It also has a
 * convenience terminator outside the wire length. Reset used to reuse a reader
 * on a new pipe. EOF, truncation, invalid records and I/O errors end the stream.
 */
enum mre_control_read_result
mre_control_read(struct mre_control_reader *reader, int fd,
		 enum mre_control_side sender,
		 struct mre_control_message *message);

size_t mre_control_encode(const struct mre_control_message *message,
			  enum mre_control_side sender, uint8_t *out,
			  size_t capacity);

/* A would-block write retains the unwritten bytes. On failure errno reports
 * the underlying write error.
 */
bool mre_control_flush(int fd, uint8_t *bytes, size_t *used);

struct mre_control_deferred_input {
	enum mre_control_opcode opcode;
	union mre_control_input payload;
};

struct mre_control_deferred {
	struct mre_control_deferred_input events[32];
	size_t count;
};

enum mre_control_reply_result {
	MRE_CONTROL_REPLY_MATCH,
	MRE_CONTROL_REPLY_SERVICE,
	MRE_CONTROL_REPLY_DEFERRED,
	MRE_CONTROL_REPLY_WRONG_IDENTITY,
	MRE_CONTROL_REPLY_UNEXPECTED,
	MRE_CONTROL_REPLY_QUEUE_FULL,
};

/* While waiting for a reply, the engine copies async broker results now and
 * holds ordinary input until guest callbacks can run. Both reply opcodes
 * are explicit; use MRE_CONTROL_NO_REPLY when there is no alternate.
 */
enum mre_control_reply_result
mre_control_route_reply(struct mre_control_deferred *deferred,
			const struct mre_control_message *message,
			const struct mre_control_message *expected,
			enum mre_control_opcode alternate);
bool mre_control_take_input(struct mre_control_deferred *deferred,
			    struct mre_control_message *message);

#endif
