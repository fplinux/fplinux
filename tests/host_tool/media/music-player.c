/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "music-player.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static pthread_t requester;
static int gate[2];
static atomic_bool delay_read;
static atomic_uint delayed_reads;

ssize_t __real_pread(int fd, void *buffer, size_t count, off_t offset);

static void wait_for_requester(void)
{
	if (!atomic_exchange(&delay_read, false))
		return;
	assert(!pthread_equal(requester, pthread_self()));
	atomic_fetch_add(&delayed_reads, 1);
	char release;
	assert(read(gate[0], &release, 1) == 1);
}

ssize_t __wrap_pread(int fd, void *buffer, size_t count, off_t offset)
{
	wait_for_requester();
	return __real_pread(fd, buffer, count, offset);
}

static struct music_player_result collect(struct music_player *player)
{
	struct pollfd fd = { .fd = player->notice, .events = POLLIN };
	assert(poll(&fd, 1, 5000) == 1);
	struct music_player_result result;
	assert(music_player_collect(player, &result));
	return result;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	requester = pthread_self();
	assert(pipe(gate) == 0);
	atomic_init(&delay_read, true);
	atomic_init(&delayed_reads, 0);
	struct music_player player;
	assert(music_player_init(&player) == 0);
	assert(music_player_request(&player, MUSIC_PLAYER_OPEN, 7, argv[1],
				    true, "z.mp3", 0));
	assert(!music_player_request(&player, MUSIC_PLAYER_CLOSE, 8, NULL,
				     false, NULL, 0));
	assert(errno == EBUSY);
	struct pollfd notice = { .fd = player.notice, .events = POLLIN };
	assert(poll(&notice, 1, 0) == 0);
	assert(write(gate[1], "x", 1) == 1);
	struct music_player_result result = collect(&player);
	assert(atomic_load(&delayed_reads) == 1);
	assert(!result.error && result.generation == 7 && result.selected == 1);
	assert(result.directory.count == 2);
	assert(!strcmp(result.directory.entries[0].name, "a.wav"));
	assert(!strcmp(result.directory.entries[1].name, "z.mp3"));
	assert(result.info.codec == FPLINUX_MEDIA_MP3 &&
	       result.info.duration_ms == 2000);
	fplinux_files_directory_free(&result.directory);
	assert(music_player_request(&player, MUSIC_PLAYER_SEEK, 9, NULL, false,
				    NULL, 1000));
	result = collect(&player);
	assert(!result.error && result.info.seeking && result.generation == 9);
	unsigned int steps = 0;
	while (result.info.seeking) {
		assert(music_player_request(&player, MUSIC_PLAYER_READ, 9, NULL,
					    false, NULL, 0));
		result = collect(&player);
		assert(!result.error || result.error == EAGAIN);
		assert(++steps < 100);
	}
	assert(result.info.position_frames == 44100);
	assert(music_player_request(&player, MUSIC_PLAYER_CLOSE, 10, NULL,
				    false, NULL, 0));
	result = collect(&player);
	assert(!result.error);
	assert(music_player_request(&player, MUSIC_PLAYER_READ, 10, NULL, false,
				    NULL, 0));
	result = collect(&player);
	assert(result.error == ENOENT);
	char path[FPLINUX_FILES_PATH_MAX];
	char renamed[FPLINUX_FILES_PATH_MAX];
	assert(fplinux_files_child_path(argv[1], "z.mp3", path, sizeof(path)) ==
	       0);
	assert(fplinux_files_child_path(argv[1], "renamed.mp3", renamed,
					sizeof(renamed)) == 0);
	assert(rename(path, renamed) == 0);
	/* Restoring a removed track must not seek into a different queued song. */
	assert(music_player_request(&player, MUSIC_PLAYER_OPEN, 11, argv[1],
				    true, "z.mp3", 1000));
	result = collect(&player);
	assert(result.error == ENOENT);
	fplinux_files_directory_free(&result.directory);
	assert(music_player_request(&player, MUSIC_PLAYER_READ, 11, NULL, false,
				    NULL, 0));
	result = collect(&player);
	assert(result.error == ENOENT);
	/* A newly chosen folder still starts with its first supported file. */
	assert(music_player_request(&player, MUSIC_PLAYER_OPEN, 12, argv[1],
				    true, NULL, 0));
	result = collect(&player);
	assert(!result.error && result.selected == 0 &&
	       result.directory.count == 2);
	assert(result.info.codec == FPLINUX_MEDIA_WAV &&
	       result.info.position_frames == 0);
	fplinux_files_directory_free(&result.directory);
	assert(fplinux_files_child_path(argv[1], "a.wav", path, sizeof(path)) ==
	       0);
	assert(music_player_request(&player, MUSIC_PLAYER_OPEN, 11, path, false,
				    NULL, 0));
	result = collect(&player);
	assert(!result.error && result.info.codec == FPLINUX_MEDIA_WAV);
	assert(music_player_request(&player, MUSIC_PLAYER_READ, 11, NULL, false,
				    NULL, 0));
	result = collect(&player);
	assert(!result.error && result.frames == 8 && result.info.eof);
	assert(result.samples[0] == -32768 && result.samples[4] == 32767 &&
	       result.samples[7] == 8192);
	music_player_cleanup(&player);
	close(gate[0]);
	close(gate[1]);
	puts("music worker: ok");
	return 0;
}
