/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "mre-ipc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void record(char kind, char program)
{
	const char *path = getenv("FPLINUX_MRE_ENGINE_RECORD");
	FILE *output = fopen(path, "a");

	assert(output);
	fprintf(output, "%c %c %ld\n", kind, program, (long)getpid());
	assert(fclose(output) == 0);
}

int main(int argc, char **argv)
{
	char program, line[128];
	int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

	assert(argc == 3 && !strcmp(argv[1], "/proc/self/fd/4"));
	assert((fcntl(4, F_GET_SEALS) & required) == required);
	assert(pread(4, &program, 1, 0) == 1);
	if (!strcmp(argv[2], "--validate")) {
		usleep(75000);
		return program == '-' ? 65 : 0;
	}
	assert(!strcmp(argv[2], "--shared"));
	struct mre_shared *shared = mmap(NULL, sizeof(*shared),
					 PROT_READ | PROT_WRITE, MAP_SHARED, 3,
					 0);
	assert(shared != MAP_FAILED);
	record('S', program);
	if (program == 'I' || program == 'S' || program == 'X' ||
	    program == 'E') {
		const char *release = getenv("FPLINUX_MRE_ENGINE_RELEASE");

		puts("DISPLAY\t240\t320");
		fflush(stdout);
		assert(release);
		while (access(release, F_OK) < 0) {
			assert(errno == ENOENT);
			usleep(1000);
		}
		if (program == 'S') {
			strcpy(shared->selector_folder, "E:\\");
			shared->selector_type = 0;
		} else {
			strcpy(shared->editor_title, "Input expression");
			memcpy(shared->editor_data, "2+3", 4);
			shared->editor_bytes = 3;
			shared->editor_max_shorts = program == 'X' ? -1 : 32;
			shared->editor_method = 4;
		}
		__atomic_thread_fence(__ATOMIC_RELEASE);
		puts(program == 'S' ? "SELECTOR" : "INPUT");
		fflush(stdout);
		if (program == 'E') {
			const char *end = getenv("FPLINUX_MRE_ENGINE_END");

			assert(end);
			while (access(end, F_OK) < 0) {
				assert(errno == ENOENT);
				usleep(1000);
			}
			record('N', program);
			puts("END");
			fflush(stdout);
			assert(munmap(shared, sizeof(*shared)) == 0);
			return 0;
		}
		while (fgets(line, sizeof(line), stdin)) {
			if (!strcmp(line, "INPUT_DONE\n")) {
				assert(shared->editor_bytes == 3 &&
				       !memcmp(shared->editor_data, "2+3", 4));
				record('E', program);
				break;
			}
			if (!strcmp(line, "SELECT_CANCEL\n")) {
				record('C', program);
				break;
			}
		}
	}
	for (size_t i = 0;
	     i < sizeof(shared->pixels) / sizeof(shared->pixels[0]); ++i)
		shared->pixels[i] = program == 'A' ? 0xf800 : 0x001f;
	shared->frame_sequence = 1;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	puts(program == 'I' || program == 'S' || program == 'X' ?
		     "FRAME\t1" :
		     "DISPLAY\t240\t320\nFRAME\t1");
	fflush(stdout);
	while (fgets(line, sizeof(line), stdin)) {
		if (!strncmp(line, "KEY\t2\t", 6))
			record('K', program);
		else if (!strncmp(line, "VISIBILITY\t", 11)) {
			record(line[11] == '1' ? 'V' : 'H', program);
			if (program == 'P' && line[11] == '0') {
				record('N', program);
				puts("VIEW_PICTURE\nEND");
				fflush(stdout);
				assert(munmap(shared, sizeof(*shared)) == 0);
				return 0;
			}
		}
	}
	return 0;
}
