/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_MUSIC_PLAYER_H
#define FPLINUX_MUSIC_PLAYER_H

#include "fplinux-files-browse.h"
#include "fplinux-media.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

enum music_player_action {
	MUSIC_PLAYER_OPEN,
	MUSIC_PLAYER_READ,
	MUSIC_PLAYER_SEEK,
	MUSIC_PLAYER_CLOSE,
};

struct music_player_result {
	enum music_player_action action;
	uint64_t generation;
	int error;
	bool midi;
	struct fplinux_media_info info;
	int16_t samples[FPLINUX_MEDIA_BLOCK_FRAMES * 2U];
	size_t frames;
	/* Ownership transfers on collect; free with directory_free(). */
	struct fplinux_files_directory directory;
	size_t selected;
};

struct music_player {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int notice;
	bool started;
	bool stopping;
	bool requested;
	bool busy;
	bool done;
	enum music_player_action action;
	uint64_t generation;
	char path[FPLINUX_FILES_PATH_MAX];
	char selected[FPLINUX_FILES_NAME_MAX + 1];
	bool folder;
	uint64_t position_ms;
	struct music_player_result result;
	/* Owned and accessed only by the worker. */
	struct fplinux_media *media;
	struct fplinux_midi *midi;
	int directory_fd;
};

extern const char *const music_player_extensions[];

int music_player_init(struct music_player *player);
void music_player_cleanup(struct music_player *player);
bool music_player_request(struct music_player *player,
			  enum music_player_action action, uint64_t generation,
			  const char *path, bool folder, const char *selected,
			  uint64_t position_ms);
bool music_player_collect(struct music_player *player,
			  struct music_player_result *result);
bool music_player_supported_name(const char *name);

#endif
