/* SPDX-License-Identifier: 0BSD */
#include "mre-control.h"
#include "mre-ipc.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

struct payload_bounds {
	enum mre_control_side sender;
	size_t min_bytes, max_bytes;
};

static bool payload_bounds(enum mre_control_opcode opcode,
			   struct payload_bounds *bounds)
{
	switch (opcode) {
	case MRE_CONTROL_INPUT_DONE:
	case MRE_CONTROL_INPUT_CANCEL:
	case MRE_CONTROL_SELECT_CANCEL:
	case MRE_CONTROL_SNAPSHOT_DONE:
	case MRE_CONTROL_SNAPSHOT_ERROR:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 0, 0 };
		break;
	case MRE_CONTROL_ACK:
	case MRE_CONTROL_VFS_REPLY:
	case MRE_CONTROL_HTTP_ACCEPTED:
	case MRE_CONTROL_HTTP_RESPONSE:
	case MRE_CONTROL_AUDIO_REPLY:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 4, 4 };
		break;
	case MRE_CONTROL_KEY:
	case MRE_CONTROL_PEN:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 5, 5 };
		break;
	case MRE_CONTROL_VISIBILITY:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 1, 1 };
		break;
	case MRE_CONTROL_SELECT:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 1,
						   MRE_VFS_PATH_BYTES - 1 };
		break;
	case MRE_CONTROL_AUDIO_EVENT:
		*bounds = (struct payload_bounds){ MRE_CONTROL_FRONTEND, 8, 8 };
		break;
	case MRE_CONTROL_INPUT:
	case MRE_CONTROL_SELECTOR:
	case MRE_CONTROL_VIEW_PICTURE:
	case MRE_CONTROL_END:
		*bounds = (struct payload_bounds){ MRE_CONTROL_ENGINE, 0, 0 };
		break;
	case MRE_CONTROL_DISPLAY:
	case MRE_CONTROL_FRAME:
	case MRE_CONTROL_VFS_REQUEST:
	case MRE_CONTROL_HTTP_REQUEST:
	case MRE_CONTROL_HTTP_CANCEL:
	case MRE_CONTROL_AUDIO_REQUEST:
		*bounds = (struct payload_bounds){ MRE_CONTROL_ENGINE, 4, 4 };
		break;
	case MRE_CONTROL_POINTER_AVAILABLE:
		*bounds = (struct payload_bounds){ MRE_CONTROL_ENGINE, 1, 1 };
		break;
	case MRE_CONTROL_SNAPSHOT:
		*bounds = (struct payload_bounds){ MRE_CONTROL_ENGINE, 1,
						   MRE_VFS_PATH_BYTES - 1 };
		break;
	case MRE_CONTROL_ERROR:
		*bounds = (struct payload_bounds){ MRE_CONTROL_ENGINE, 0,
						   MRE_CONTROL_ERROR_BYTES };
		break;
	default:
		return false;
	}
	return true;
}

static uint16_t get_u16(const uint8_t *data)
{
	return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8);
}

static uint32_t get_u32(const uint8_t *data)
{
	return (uint32_t)get_u16(data) | (uint32_t)get_u16(data + 2) << 16;
}

static int32_t get_i32(const uint8_t *data)
{
	uint32_t value = get_u32(data);

	if (value > INT32_MAX)
		return -1 - (int32_t)(UINT32_MAX - value);
	return (int32_t)value;
}

static void put_u16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
	put_u16(out, (uint16_t)value);
	put_u16(out + 2, (uint16_t)(value >> 16));
}

static bool key_event_valid(enum mre_key_event event)
{
	return event == MRE_KEY_EVENT_UP || event == MRE_KEY_EVENT_DOWN;
}

static bool pen_valid(enum mre_pen_event event, uint16_t x, uint16_t y)
{
	return event >= MRE_PEN_EVENT_TAP && event <= MRE_PEN_EVENT_MOVE &&
	       x < MRE_FRAME_WIDTH && y < MRE_FRAME_HEIGHT;
}

static bool decode(enum mre_control_opcode opcode, const uint8_t *payload,
		   size_t length, struct mre_control_message *message)
{
	message->opcode = opcode;
	switch (opcode) {
	case MRE_CONTROL_KEY:
		message->input.key.event = payload[0];
		message->input.key.code = get_i32(payload + 1);
		return key_event_valid(message->input.key.event);
	case MRE_CONTROL_PEN:
		message->input.pen.event = payload[0];
		message->input.pen.x = get_u16(payload + 1);
		message->input.pen.y = get_u16(payload + 3);
		return pen_valid(message->input.pen.event, message->input.pen.x,
				 message->input.pen.y);
	case MRE_CONTROL_VISIBILITY:
		message->input.visible = payload[0];
		return payload[0] <= 1;
	case MRE_CONTROL_POINTER_AVAILABLE:
		message->pointer_available = payload[0];
		return payload[0] <= 1;
	case MRE_CONTROL_DISPLAY:
		message->display.width = get_u16(payload);
		message->display.height = get_u16(payload + 2);
		return message->display.width == MRE_FRAME_WIDTH &&
		       message->display.height == MRE_FRAME_HEIGHT;
	case MRE_CONTROL_ACK:
	case MRE_CONTROL_FRAME:
	case MRE_CONTROL_VFS_REQUEST:
	case MRE_CONTROL_VFS_REPLY:
	case MRE_CONTROL_AUDIO_REQUEST:
	case MRE_CONTROL_AUDIO_REPLY:
		message->sequence = get_u32(payload);
		return true;
	case MRE_CONTROL_HTTP_REQUEST:
	case MRE_CONTROL_HTTP_CANCEL:
	case MRE_CONTROL_HTTP_ACCEPTED:
	case MRE_CONTROL_HTTP_RESPONSE:
		message->http_id = get_u32(payload);
		return true;
	case MRE_CONTROL_AUDIO_EVENT:
		message->audio.handle = get_u32(payload);
		message->audio.event = get_i32(payload + 4);
		return true;
	case MRE_CONTROL_SELECT:
	case MRE_CONTROL_SNAPSHOT:
	case MRE_CONTROL_ERROR:
		message->text.data = (const char *)payload;
		message->text.length = length;
		return !memchr(payload, 0, length);
	default:
		return true; /* The validated opcode has no payload. */
	}
}

size_t mre_control_encode(const struct mre_control_message *message,
			  enum mre_control_side sender, uint8_t *out,
			  size_t capacity)
{
	struct payload_bounds bounds;
	size_t length;

	if (!payload_bounds(message->opcode, &bounds) ||
	    bounds.sender != sender)
		return 0;
	length = bounds.min_bytes;
	if (bounds.min_bytes != bounds.max_bytes)
		length = message->text.length;
	if (length < bounds.min_bytes || length > bounds.max_bytes ||
	    capacity < MRE_CONTROL_HEADER_BYTES + length)
		return 0;

	uint8_t *payload = out + MRE_CONTROL_HEADER_BYTES;

	switch (message->opcode) {
	case MRE_CONTROL_KEY:
		if (!key_event_valid(message->input.key.event))
			return 0;
		payload[0] = message->input.key.event;
		put_u32(payload + 1, (uint32_t)message->input.key.code);
		break;
	case MRE_CONTROL_PEN:
		if (!pen_valid(message->input.pen.event, message->input.pen.x,
			       message->input.pen.y))
			return 0;
		payload[0] = message->input.pen.event;
		put_u16(payload + 1, message->input.pen.x);
		put_u16(payload + 3, message->input.pen.y);
		break;
	case MRE_CONTROL_VISIBILITY:
		payload[0] = message->input.visible;
		break;
	case MRE_CONTROL_POINTER_AVAILABLE:
		payload[0] = message->pointer_available;
		break;
	case MRE_CONTROL_DISPLAY:
		if (message->display.width != MRE_FRAME_WIDTH ||
		    message->display.height != MRE_FRAME_HEIGHT)
			return 0;
		put_u16(payload, message->display.width);
		put_u16(payload + 2, message->display.height);
		break;
	case MRE_CONTROL_ACK:
	case MRE_CONTROL_FRAME:
	case MRE_CONTROL_VFS_REQUEST:
	case MRE_CONTROL_VFS_REPLY:
	case MRE_CONTROL_AUDIO_REQUEST:
	case MRE_CONTROL_AUDIO_REPLY:
		put_u32(payload, message->sequence);
		break;
	case MRE_CONTROL_HTTP_REQUEST:
	case MRE_CONTROL_HTTP_CANCEL:
	case MRE_CONTROL_HTTP_ACCEPTED:
	case MRE_CONTROL_HTTP_RESPONSE:
		put_u32(payload, message->http_id);
		break;
	case MRE_CONTROL_AUDIO_EVENT:
		put_u32(payload, message->audio.handle);
		put_u32(payload + 4, (uint32_t)message->audio.event);
		break;
	case MRE_CONTROL_SELECT:
	case MRE_CONTROL_SNAPSHOT:
	case MRE_CONTROL_ERROR:
		if (length) {
			if (memchr(message->text.data, 0, length))
				return 0;
			memcpy(payload, message->text.data, length);
		}
		break;
	default:
		break; /* The validated opcode has no payload. */
	}
	out[0] = message->opcode;
	put_u16(out + 1, (uint16_t)length);
	return MRE_CONTROL_HEADER_BYTES + length;
}

enum mre_control_read_result
mre_control_read(struct mre_control_reader *reader, int fd,
		 enum mre_control_side sender,
		 struct mre_control_message *message)
{
	for (;;) {
		size_t needed = MRE_CONTROL_HEADER_BYTES;

		if (reader->used >= MRE_CONTROL_HEADER_BYTES) {
			enum mre_control_opcode opcode = reader->bytes[0];
			size_t length = get_u16(reader->bytes + 1);
			struct payload_bounds bounds;

			needed += length;
			if (needed >= sizeof(reader->bytes) ||
			    !payload_bounds(opcode, &bounds) ||
			    bounds.sender != sender ||
			    length < bounds.min_bytes ||
			    length > bounds.max_bytes)
				return MRE_CONTROL_INVALID;
			if (reader->used == needed) {
				reader->bytes[needed] = 0;
				reader->used = 0;
				return decode(opcode,
					      reader->bytes +
						      MRE_CONTROL_HEADER_BYTES,
					      length, message) ?
					       MRE_CONTROL_MESSAGE :
					       MRE_CONTROL_INVALID;
			}
		}
		ssize_t count = read(fd, reader->bytes + reader->used,
				     needed - reader->used);

		if (count > 0) {
			reader->used += (size_t)count;
			continue;
		}
		if (!count)
			return reader->used ? MRE_CONTROL_TRUNCATED :
					      MRE_CONTROL_EOF;
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return MRE_CONTROL_WAIT;
		return MRE_CONTROL_IO_ERROR;
	}
}

bool mre_control_flush(int fd, uint8_t *bytes, size_t *used)
{
	while (*used) {
		ssize_t count = write(fd, bytes, *used);

		if (count > 0) {
			*used -= (size_t)count;
			if (*used)
				memmove(bytes, bytes + count, *used);
			continue;
		}
		if (!count) {
			errno = EIO;
			return false;
		}
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK;
	}
	return true;
}

enum mre_control_reply_result
mre_control_route_reply(struct mre_control_deferred *deferred,
			const struct mre_control_message *message,
			const struct mre_control_message *expected,
			enum mre_control_opcode alternate)
{
	if (message->opcode == expected->opcode) {
		switch (message->opcode) {
		case MRE_CONTROL_ACK:
		case MRE_CONTROL_VFS_REPLY:
		case MRE_CONTROL_AUDIO_REPLY:
			if (message->sequence != expected->sequence)
				return MRE_CONTROL_REPLY_WRONG_IDENTITY;
			break;
		case MRE_CONTROL_HTTP_ACCEPTED:
			if (message->http_id != expected->http_id)
				return MRE_CONTROL_REPLY_WRONG_IDENTITY;
			break;
		default:
			break;
		}
		return MRE_CONTROL_REPLY_MATCH;
	}
	if (message->opcode == alternate)
		return MRE_CONTROL_REPLY_MATCH;
	switch (message->opcode) {
	case MRE_CONTROL_AUDIO_EVENT:
	case MRE_CONTROL_HTTP_RESPONSE:
		return MRE_CONTROL_REPLY_SERVICE;
	case MRE_CONTROL_KEY:
	case MRE_CONTROL_PEN:
	case MRE_CONTROL_VISIBILITY:
		if (deferred->count ==
		    sizeof(deferred->events) / sizeof(deferred->events[0]))
			return MRE_CONTROL_REPLY_QUEUE_FULL;
		deferred->events[deferred->count++] =
			(struct mre_control_deferred_input){
				.opcode = message->opcode,
				.payload = message->input,
			};
		return MRE_CONTROL_REPLY_DEFERRED;
	default:
		return MRE_CONTROL_REPLY_UNEXPECTED;
	}
}

bool mre_control_take_input(struct mre_control_deferred *deferred,
			    struct mre_control_message *message)
{
	if (!deferred->count)
		return false;
	message->opcode = deferred->events[0].opcode;
	message->input = deferred->events[0].payload;
	deferred->count--;
	memmove(deferred->events, deferred->events + 1,
		deferred->count * sizeof(deferred->events[0]));
	return true;
}
