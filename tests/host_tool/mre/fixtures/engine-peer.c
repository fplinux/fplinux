/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ipc/mre-ipc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum parent_opcode {
	PARENT_KEY = 1,
	PARENT_PEN = 2,
	PARENT_VISIBILITY = 3,
	PARENT_ACK = 4,
	PARENT_INPUT_DONE = 5,
	PARENT_INPUT_CANCEL = 6,
	PARENT_SELECT_CANCEL = 8,
	PARENT_SNAPSHOT_DONE = 10,
};

struct parent_message {
	uint8_t opcode;
	uint16_t length;
	uint8_t body[259];
};

static const uint8_t display[] = { 16, 4, 0, 240, 0, 64, 1 };
static const uint8_t first_frame[] = { 17, 4, 0, 1, 0, 0, 0 };
static const uint8_t interactive_frame[] = {
	16, 4, 0, 240, 0, 64, 1, /* DISPLAY 240x320 */
	18, 1, 0, 1, /* POINTER_AVAILABLE */
	17, 4, 0, 1,   0, 0,  0, /* FRAME 1 */
};
static const uint8_t display_and_frame[] = {
	16, 4, 0, 240, 0, 64, 1, 17, 4, 0, 1, 0, 0, 0,
};
static const uint8_t end[] = { 27, 0, 0 };

static void send_bytes(const uint8_t *data, size_t bytes)
{
	assert(write(STDOUT_FILENO, data, bytes) == (ssize_t)bytes);
}

static bool read_bytes(uint8_t *data, size_t bytes)
{
	size_t done = 0;

	while (done < bytes) {
		ssize_t count = read(STDIN_FILENO, data + done, bytes - done);

		if (count < 0 && errno == EINTR)
			continue;
		assert(count >= 0);
		if (!count) {
			assert(!done);
			return false;
		}
		done += (size_t)count;
	}
	return true;
}

static uint16_t from_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] + 256U * bytes[1]);
}

static int32_t signed_word(const uint8_t *bytes)
{
	int64_t value =
		from_le16(bytes) + INT64_C(65536) * from_le16(bytes + 2);

	if (value > INT32_MAX)
		value -= INT64_C(4294967296);
	return (int32_t)value;
}

static bool receive_parent(struct parent_message *message)
{
	uint8_t header[3];

	if (!read_bytes(header, sizeof(header)))
		return false;
	message->opcode = header[0];
	message->length = from_le16(header + 1);
	assert(message->length <= sizeof(message->body));
	assert(read_bytes(message->body, message->length));
	return true;
}

static void check_frame_ack(const struct parent_message *message)
{
	static const uint8_t sequence[] = { 1, 0, 0, 0 };

	assert(message->opcode == PARENT_ACK &&
	       message->length == sizeof(sequence));
	assert(!memcmp(message->body, sequence, sizeof(sequence)));
}

static void record_key(const char *path, const struct parent_message *message)
{
	FILE *output = fopen(path, "a");

	assert(output);
	switch (message->opcode) {
	case PARENT_KEY:
		assert(message->length == 5);
		fprintf(output, "KEY\t%u\t%" PRId32 "\n", message->body[0],
			signed_word(message->body + 1));
		break;
	case PARENT_PEN:
		assert(message->length == 5);
		fprintf(output, "PEN\t%u\t%u\t%u\n", message->body[0],
			from_le16(message->body + 1),
			from_le16(message->body + 3));
		break;
	default:
		assert(false);
	}
	assert(fclose(output) == 0);
}

/* A slow step goes on once the test creates the file that path names. */
static void wait_file(const char *path)
{
	assert(path);
	while (access(path, F_OK) < 0) {
		assert(errno == ENOENT);
		usleep(1000);
	}
}

static void record(char kind, char program)
{
	const char *path = getenv("FPLINUX_MRE_ENGINE_RECORD");
	FILE *output = fopen(path, "a");

	assert(output);
	fprintf(output, "%c %c %ld\n", kind, program, (long)getpid());
	assert(fclose(output) == 0);
}

static void check_snapshot(struct mre_shared *shared, char program)
{
	static const char expected_path[] = "E:\\файл.txt";
	static const uint8_t snapshot[] = {
		22,   15,   0,	  'E',	':',  '\\', 0xd1, 0x84, 0xd0,
		0xb0, 0xd0, 0xb9, 0xd0, 0xbb, '.',  't',  'x',	't',
	};
	struct parent_message reply;

	send_bytes(snapshot, 1);
	send_bytes(snapshot + 1, 6);
	send_bytes(snapshot + 7, sizeof(snapshot) - 7);
	assert(receive_parent(&reply));
	assert(reply.opcode == PARENT_SNAPSHOT_DONE && reply.length == 0);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	assert(!strcmp(shared->file_path, expected_path));
	assert(shared->file_bytes == 4);
	assert(!memcmp(shared->file_data, "\x00\x0a\x89\xff", 4));
	record('Q', program);
}

int main(int argc, char **argv)
{
	char program;
	struct parent_message parent;
	int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

	assert(argc == 3 && !strcmp(argv[1], "/proc/self/fd/4"));
	assert((fcntl(4, F_GET_SEALS) & required) == required);
	assert(pread(4, &program, 1, 0) == 1);
	if (!strcmp(argv[2], "--validate")) {
		if (program == 'W' || program == 'F')
			wait_file(getenv("FPLINUX_MRE_ENGINE_RELEASE"));
		usleep(75000);
		return program == '-' ? 65 : 0;
	}
	assert(!strcmp(argv[2], "--shared"));
	struct mre_shared *shared = mmap(NULL, sizeof(*shared),
					 PROT_READ | PROT_WRITE, MAP_SHARED, 3,
					 0);
	assert(shared != MAP_FAILED);
	record('S', program);
	if (program == 'A' && getenv("FPLINUX_MRE_CHECK_SNAPSHOT"))
		check_snapshot(shared, program);
	bool first_modal = strchr("ISXEDMR", program) != NULL;
	if (first_modal) {
		send_bytes(display, sizeof(display));
		wait_file(getenv("FPLINUX_MRE_ENGINE_RELEASE"));
		if (program == 'S') {
			strcpy(shared->selector_folder, "E:\\");
			shared->selector_type = 0;
		} else {
			strcpy(shared->editor_title, "Input expression");
			memcpy(shared->editor_data, "2+3", 4);
			shared->editor_bytes = 3;
			shared->editor_max_shorts = program == 'X' ? -1 : 32;
			shared->editor_method = program == 'R' ? 0 :
						program == 'M' ? 1 :
								 4;
		}
		__atomic_thread_fence(__ATOMIC_RELEASE);
		uint8_t modal[] = { program == 'S' ? 20 : 19, 0, 0 };

		send_bytes(modal, sizeof(modal));
		if (program == 'E') {
			wait_file(getenv("FPLINUX_MRE_ENGINE_END"));
			record('N', program);
			send_bytes(end, sizeof(end));
			assert(munmap(shared, sizeof(*shared)) == 0);
			return 0;
		}
		while (receive_parent(&parent)) {
			if (parent.opcode == PARENT_INPUT_DONE) {
				const char *expected =
					program == 'D' ? "" :
					program == 'M' ? "2+3\n" :
							 "2+3";

				__atomic_thread_fence(__ATOMIC_ACQUIRE);
				assert(parent.length == 0);
				assert(shared->editor_bytes ==
					       strlen(expected) &&
				       !strcmp((const char *)shared->editor_data,
					       expected));
				record('E', program);
				break;
			}
			if (parent.opcode == PARENT_SELECT_CANCEL ||
			    parent.opcode == PARENT_INPUT_CANCEL) {
				assert(parent.length == 0);
				record('C', program);
				break;
			}
		}
	}
	if (program == 'W')
		wait_file(getenv("FPLINUX_MRE_ENGINE_END"));
	for (size_t i = 0;
	     i < sizeof(shared->pixels) / sizeof(shared->pixels[0]); ++i)
		shared->pixels[i] = program == 'A' ? 0xf800 : 0x001f;
	shared->frame_sequence = 1;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	if (first_modal)
		send_bytes(first_frame, sizeof(first_frame));
	else if (program == 'T')
		send_bytes(interactive_frame, sizeof(interactive_frame));
	else
		send_bytes(display_and_frame, sizeof(display_and_frame));
	/* Fails once its first frame is acknowledged. */
	if (program == 'F') {
		assert(receive_parent(&parent));
		check_frame_ack(&parent);
		return 3;
	}
	while (receive_parent(&parent)) {
		const char *keys = getenv("FPLINUX_MRE_KEY_RECORD");

		if (keys && (parent.opcode == PARENT_KEY ||
			     parent.opcode == PARENT_PEN))
			record_key(keys, &parent);
		if (parent.opcode == PARENT_ACK)
			check_frame_ack(&parent);
		else if (parent.opcode == PARENT_KEY) {
			assert(parent.length == 5);
			if (parent.body[0] == 2)
				record('K', program);
		} else if (parent.opcode == PARENT_VISIBILITY) {
			assert(parent.length == 1 && parent.body[0] <= 1);
			record(parent.body[0] ? 'V' : 'H', program);
			if (program == 'P' && !parent.body[0]) {
				static const uint8_t picture_and_end[] = {
					26, 0, 0, 27, 0, 0,
				};

				record('N', program);
				send_bytes(picture_and_end,
					   sizeof(picture_and_end));
				assert(munmap(shared, sizeof(*shared)) == 0);
				return 0;
			}
		}
	}
	return 0;
}
