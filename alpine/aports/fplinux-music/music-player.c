/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "music-player.h"
#include "fplinux-midi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const music_player_extensions[] = { "mp3", "wav", "mid", "midi",
						NULL };

static const char *extension(const char *name)
{
	const char *suffix = strrchr(name, '.');

	return suffix ? suffix + 1 : "";
}

static bool is_midi_name(const char *name)
{
	const char *suffix = extension(name);

	return !strcasecmp(suffix, "mid") || !strcasecmp(suffix, "midi");
}

bool music_player_supported_name(const char *name)
{
	const char *suffix = extension(name);

	for (size_t i = 0; music_player_extensions[i]; i++)
		if (!strcasecmp(suffix, music_player_extensions[i]))
			return true;
	return false;
}

static void close_media(struct music_player *player)
{
	fplinux_media_close(player->media);
	player->media = NULL;
	fplinux_midi_close(player->midi);
	player->midi = NULL;
	if (player->directory_fd >= 0)
		close(player->directory_fd);
	player->directory_fd = -1;
}

static int read_file(int fd, unsigned char *bytes, size_t count)
{
	size_t done = 0;

	while (done < count) {
		ssize_t received = read(fd, bytes + done, count - done);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0) {
			if (!received)
				errno = EIO;
			return -1;
		}
		done += (size_t)received;
	}
	return 0;
}

static struct fplinux_midi *open_midi(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return NULL;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
		errno = EINVAL;
		return NULL;
	}
	if ((uint64_t)st.st_size > FPLINUX_MIDI_RESOURCE_BYTES) {
		errno = EFBIG;
		return NULL;
	}
	size_t count = (size_t)st.st_size;
	unsigned char *bytes = malloc(count);
	if (!bytes)
		return NULL;
	struct fplinux_midi *midi = NULL;
	if (read_file(fd, bytes, count) == 0)
		midi = fplinux_midi_open_bytes(bytes, count);
	int error = errno;
	free(bytes);
	if (midi && fplinux_midi_start(midi) < 0) {
		error = errno;
		fplinux_midi_close(midi);
		midi = NULL;
	}
	if (!midi)
		errno = error;
	return midi;
}

static int open_file(struct music_player *player, const char *path)
{
	char name[FPLINUX_FILES_NAME_MAX + 1];
	int parent = fplinux_files_open_parent(path, name, EINVAL);
	if (parent < 0)
		return -1;
	int fd = openat(parent, name,
			O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		int error = errno;
		close(parent);
		errno = error;
		return -1;
	}
	if (is_midi_name(name))
		player->midi = open_midi(fd);
	else
		player->media = fplinux_media_open_fd(fd);
	int error = errno;
	close(fd);
	if (!player->media && !player->midi) {
		close(parent);
		errno = error;
		return -1;
	}
	player->directory_fd = parent;
	return 0;
}

static bool cancelled(void *context)
{
	struct music_player *player = context;
	pthread_mutex_lock(&player->lock);
	bool stopping = player->stopping;
	pthread_mutex_unlock(&player->lock);
	return stopping;
}

static int open_folder(struct music_player *player,
		       struct music_player_result *result)
{
	if (fplinux_files_directory_read(player->path, &result->directory,
					 cancelled, player) < 0)
		return -1;
	size_t kept = 0;
	bool selected_found = !player->selected[0];
	for (size_t i = 0; i < result->directory.count; i++) {
		struct fplinux_files_entry entry = result->directory.entries[i];
		if (entry.kind != FPLINUX_FILES_KIND_FILE ||
		    !music_player_supported_name(entry.name)) {
			free(entry.name);
			continue;
		}
		if (!strcmp(entry.name, player->selected)) {
			result->selected = kept;
			selected_found = true;
		}
		result->directory.entries[kept++] = entry;
	}
	result->directory.count = kept;
	if (!kept) {
		errno = ENODATA;
		return -1;
	}
	if (!selected_found) {
		errno = ENOENT;
		return -1;
	}
	char path[FPLINUX_FILES_PATH_MAX];
	if (fplinux_files_child_path(
		    player->path,
		    result->directory.entries[result->selected].name, path,
		    sizeof(path)) < 0)
		return -1;
	return open_file(player, path);
}

static int seek_media(struct music_player *player, uint64_t position_ms)
{
	if (player->midi) {
		if (fplinux_midi_seek(player->midi, position_ms) < 0)
			return -1;
		return fplinux_midi_start(player->midi);
	}
	return fplinux_media_seek(player->media, position_ms);
}

static void media_info(struct music_player *player,
		       struct music_player_result *result)
{
	if (player->midi) {
		struct fplinux_midi_info info;

		if (fplinux_midi_get_info(player->midi, &info) < 0)
			return;
		result->midi = true;
		result->info = (struct fplinux_media_info){
			.rate = info.rate,
			.channels = info.channels,
			.position_frames = info.position_frames,
			.duration_ms = (info.duration_us + 999U) / 1000U,
			.duration_known = true,
			.seeking = info.seeking,
			.eof = info.eof,
		};
	} else if (player->media) {
		result->info = *fplinux_media_info(player->media);
	}
}

static void execute(struct music_player *player,
		    struct music_player_result *result)
{
	int status = 0;
	if (result->action == MUSIC_PLAYER_OPEN) {
		close_media(player);
		status = player->folder ? open_folder(player, result) :
					  open_file(player, player->path);
		if (!status && player->position_ms)
			status = seek_media(player, player->position_ms);
	} else if (result->action == MUSIC_PLAYER_CLOSE) {
		close_media(player);
	} else if (!player->media && !player->midi) {
		errno = ENOENT;
		status = -1;
	} else if (result->action == MUSIC_PLAYER_SEEK) {
		status = seek_media(player, player->position_ms);
	} else {
		ssize_t frames =
			player->midi ?
				fplinux_midi_read(player->midi, result->samples,
						  FPLINUX_MIDI_BLOCK_FRAMES) :
				fplinux_media_read(player->media,
						   result->samples,
						   FPLINUX_MEDIA_BLOCK_FRAMES);
		if (frames < 0)
			status = -1;
		else
			result->frames = (size_t)frames;
	}
	result->error = status < 0 ? errno : 0;
	media_info(player, result);
}

static void *run(void *context)
{
	struct music_player *player = context;
	pthread_mutex_lock(&player->lock);
	for (;;) {
		while (!player->requested && !player->stopping)
			pthread_cond_wait(&player->changed, &player->lock);
		if (player->stopping)
			break;
		player->requested = false;
		struct music_player_result result = {
			.action = player->action,
			.generation = player->generation,
		};
		pthread_mutex_unlock(&player->lock);
		execute(player, &result);
		pthread_mutex_lock(&player->lock);
		player->result = result;
		player->done = true;
		uint64_t notice = 1;
		while (write(player->notice, &notice, sizeof(notice)) < 0 &&
		       errno == EINTR)
			;
	}
	pthread_mutex_unlock(&player->lock);
	close_media(player);
	return NULL;
}

int music_player_init(struct music_player *player)
{
	*player = (struct music_player){ .notice = -1, .directory_fd = -1 };
	int error = pthread_mutex_init(&player->lock, NULL);
	if (error)
		goto fail;
	error = pthread_cond_init(&player->changed, NULL);
	if (error) {
		pthread_mutex_destroy(&player->lock);
		goto fail;
	}
	player->notice = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (player->notice < 0) {
		error = errno;
		goto fail_condition;
	}
	error = pthread_create(&player->thread, NULL, run, player);
	if (error) {
		close(player->notice);
		player->notice = -1;
		goto fail_condition;
	}
	player->started = true;
	return 0;
fail_condition:
	pthread_cond_destroy(&player->changed);
	pthread_mutex_destroy(&player->lock);
fail:
	errno = error;
	return -1;
}

void music_player_cleanup(struct music_player *player)
{
	if (!player->started)
		return;
	pthread_mutex_lock(&player->lock);
	player->stopping = true;
	pthread_cond_signal(&player->changed);
	pthread_mutex_unlock(&player->lock);
	pthread_join(player->thread, NULL);
	fplinux_files_directory_free(&player->result.directory);
	close(player->notice);
	pthread_cond_destroy(&player->changed);
	pthread_mutex_destroy(&player->lock);
	player->started = false;
}

bool music_player_request(struct music_player *player,
			  enum music_player_action action, uint64_t generation,
			  const char *path, bool folder, const char *selected,
			  uint64_t position_ms)
{
	if ((path && strlen(path) >= sizeof(player->path)) ||
	    (selected && strlen(selected) >= sizeof(player->selected))) {
		errno = ENAMETOOLONG;
		return false;
	}
	pthread_mutex_lock(&player->lock);
	if (player->busy) {
		pthread_mutex_unlock(&player->lock);
		errno = EBUSY;
		return false;
	}
	player->busy = true;
	player->action = action;
	player->generation = generation;
	player->folder = folder;
	player->position_ms = position_ms;
	strcpy(player->path, path ? path : "");
	strcpy(player->selected, selected ? selected : "");
	player->requested = true;
	pthread_cond_signal(&player->changed);
	pthread_mutex_unlock(&player->lock);
	return true;
}

bool music_player_collect(struct music_player *player,
			  struct music_player_result *result)
{
	uint64_t notice;
	while (read(player->notice, &notice, sizeof(notice)) > 0)
		;
	pthread_mutex_lock(&player->lock);
	if (!player->done) {
		pthread_mutex_unlock(&player->lock);
		return false;
	}
	*result = player->result;
	player->result.directory = (struct fplinux_files_directory){ 0 };
	player->done = false;
	player->busy = false;
	pthread_mutex_unlock(&player->lock);
	return true;
}
