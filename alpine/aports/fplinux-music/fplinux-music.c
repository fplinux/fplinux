/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "music-player.h"
#include "music-model.h"
#include "music-view.h"
#include "fplinux-cli.h"
#include "fplinux-ui-audio.h"
#include "fplinux-ui-file-picker.h"
#include "fplinux-ui-storage.h"
#include "fplinux-ui-session.h"

#include <errno.h>
#include <libintl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define APP_ID "org.fplinux.music"
#define _(text) gettext(text)
#define MUSIC_SEEK_MS 5000U
#define MUSIC_REPLACE_QUEUE SIZE_MAX

enum command_id {
	CMD_BACK = 1,
	CMD_PLAY,
	CMD_STOP,
	CMD_FILE,
	CMD_FOLDER,
	CMD_QUEUE,
	CMD_REPEAT,
	CMD_DETAILS,
};

struct music_app {
	struct fplinux_ui *ui;
	struct music_view view;
	struct music_player player;
	struct fplinux_ui_prefs *prefs;
	struct fplinux_audio_stream *stream;
	struct fplinux_ui_file_picker *picker;
	struct fplinux_files_directory queue;
	struct fplinux_media_info info;
	struct fplinux_volume_snapshot volumes;
	char home[FPLINUX_FILES_PATH_MAX];
	char source[FPLINUX_FILES_PATH_MAX];
	char path[FPLINUX_FILES_PATH_MAX];
	char folder[FPLINUX_FILES_PATH_MAX];
	char target[FPLINUX_FILES_PATH_MAX];
	char target_selected[FPLINUX_FILES_NAME_MAX + 1];
	bool target_folder;
	size_t target_queue_index;
	bool target_play;
	uint64_t target_position_ms;
	bool open_pending;
	bool close_pending;
	bool seek_pending;
	uint64_t seek_ms;
	bool loaded;
	bool playing;
	bool device_pause_seen;
	bool waiting;
	bool wait_finishing;
	bool loading;
	bool closing;
	bool folder_picker;
	bool source_folder;
	bool held_seek;
	unsigned int held_key;
	uint64_t generation;
	uint64_t base_frames;
	uint64_t submitted_frames;
	uint64_t position_ms;
	uint64_t last_save_ms;
	uint64_t last_storage_ms;
	uint64_t last_redraw_ms;
	uint32_t document;
	uint32_t volume_id;
	uint32_t mount_id;
	int error;
	int output_error;
	bool save_warning;
	bool volume_warning;
	int timer;
	unsigned int timer_interval_ms;
	bool commands_ready;
	bool command_playing;
	bool command_has_file;
	enum music_repeat command_repeat;
	enum music_repeat repeat;
	size_t selected;
	int16_t samples[FPLINUX_MEDIA_BLOCK_FRAMES * 2U];
	size_t frames;
	size_t consumed;
};

static void pump(struct music_app *app);
static void redraw(struct music_app *app);
static void save(struct music_app *app);
static void cancel_load(struct fplinux_ui *ui, void *context);

static const char *file_name(const char *path)
{
	const char *name = strrchr(path, '/');
	return name ? name + 1 : path;
}

static const char *error_text(int error)
{
	switch (error) {
	case ENODEV:
	case ENXIO:
		return _(
			"Memory card removed. Connect it and open the track again.");
	case ENOENT:
	case ESTALE:
		return _("File is unavailable. Open the file or folder again.");
	case ENODATA:
		return _("This folder contains no MP3 or WAV files.");
	case ENOTSUP:
		return _("This audio format is not supported.");
	case EACCES:
	case EPERM:
		return _("Permission to read this file was denied.");
	case EBUSY:
		return _(
			"The file is busy. Try again after the other operation finishes.");
	default:
		return _(
			"Could not read this audio file. Open another file or try again.");
	}
}

static void add_command(struct music_app *app, struct fplinux_ui_screen *screen,
			int id, enum fplinux_ui_command_type type,
			const char *label, int priority, const char *glyph)
{
	struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.priority = priority,
		.icon = glyph ? fplinux_ui_glyph(app->ui, glyph,
						 FPLINUX_UI_GLYPH_ROW) :
				NULL,
	};
	fplinux_ui_screen_add_command(screen, &command);
}

static void close_stream(struct music_app *app)
{
	fplinux_ui_audio_close(app->stream);
	app->stream = NULL;
	app->frames = 0;
	app->consumed = 0;
	app->submitted_frames = 0;
	app->device_pause_seen = false;
}

static uint64_t played_ms(struct music_app *app)
{
	if (!app->stream || !app->info.rate)
		return app->position_ms;
	long queued = fplinux_ui_audio_queued(app->stream);
	if (queued < 0)
		return app->position_ms;
	uint64_t played = app->submitted_frames > (uint64_t)queued ?
				  app->submitted_frames - (uint64_t)queued :
				  0;
	return (app->base_frames + played) * 1000U / app->info.rate;
}

static bool is_finished(struct music_app *app)
{
	return app->info.eof && app->frames == app->consumed &&
	       (!app->stream || fplinux_ui_audio_queued(app->stream) == 0);
}

static void identify_volume(struct music_app *app)
{
	app->volume_id = 0;
	app->mount_id = 0;
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];
		size_t length = strlen(volume->path);
		if (length && volume->state == FPLINUX_VOLUME_MOUNTED &&
		    !strncmp(app->path, volume->path, length) &&
		    app->path[length] == '/') {
			app->volume_id = volume->id;
			app->mount_id = volume->mount_id;
			return;
		}
	}
}

static void stopped(struct music_app *app)
{
	app->position_ms = played_ms(app);
	close_stream(app);
	app->playing = false;
	app->loaded = false;
	app->generation++;
	app->close_pending = true;
	app->open_pending = false;
	app->seek_pending = false;
	app->base_frames = 0;
	app->info.eof = false;
	app->info.seeking = false;
	app->output_error = 0;
}

static void show_error(struct music_app *app, int error)
{
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR,
				     _("Music"), error_text(error), 0);
	if (alert)
		fplinux_ui_push(app->ui, alert);
}

static void wait_done(struct fplinux_ui *ui, void *context)
{
	struct music_app *app = context;
	app->waiting = false;
	app->wait_finishing = false;
	/* A DOCUMENT can replace restoration while its minimum wait is ending. */
	if (app->loading) {
		if (fplinux_ui_wait_begin(ui, _("Opening music"),
					  file_name(app->target), cancel_load,
					  app)) {
			app->waiting = true;
			return;
		}
		app->error = errno;
		app->loading = false;
	}
	redraw(app);
	if (app->document) {
		fplinux_ui_document_reply(ui, app->document,
					  app->error ?
						  FPLINUX_UI_DOCUMENT_ERROR :
						  FPLINUX_UI_DOCUMENT_ACCEPTED,
					  app->error);
		app->document = 0;
	}
	if (app->error)
		show_error(app, app->error);
	else
		save(app);
}

static void finish_wait(struct music_app *app)
{
	if (app->waiting && !app->wait_finishing) {
		app->wait_finishing = true;
		fplinux_ui_wait_end(app->ui, wait_done, app);
	}
}

static void cancel_load(struct fplinux_ui *ui, void *context)
{
	struct music_app *app = context;
	(void)ui;
	stopped(app);
	app->loading = false;
	app->error = 0;
	app->output_error = 0;
	if (app->document) {
		fplinux_ui_document_reply(app->ui, app->document,
					  FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		app->document = 0;
	}
	finish_wait(app);
	pump(app);
}

static bool begin_open(struct music_app *app, const char *path, bool folder,
		       const char *selected, uint64_t position_ms, bool play,
		       size_t queue_index)
{
	if (strlen(path) >= sizeof(app->target) ||
	    (selected && strlen(selected) >= sizeof(app->target_selected))) {
		errno = ENAMETOOLONG;
		return false;
	}
	if (!app->waiting) {
		fplinux_ui_pop_to(app->ui, app->view.screen);
		if (!fplinux_ui_wait_begin(app->ui, _("Opening music"),
					   file_name(path), cancel_load, app))
			return false;
		app->waiting = true;
	}
	app->position_ms = played_ms(app);
	close_stream(app);
	app->generation++;
	strcpy(app->target, path);
	strcpy(app->target_selected, selected ? selected : "");
	app->target_folder = folder;
	app->target_queue_index = queue_index;
	app->target_position_ms = position_ms;
	app->target_play = play;
	app->open_pending = true;
	app->close_pending = false;
	app->seek_pending = false;
	app->loading = true;
	app->loaded = false;
	app->playing = false;
	app->error = 0;
	app->output_error = 0;
	pump(app);
	return true;
}

static void open_queue_track(struct music_app *app, size_t selected, bool play)
{
	if (selected >= app->queue.count)
		return;
	char path[FPLINUX_FILES_PATH_MAX];
	if (fplinux_files_child_path(app->folder,
				     app->queue.entries[selected].name, path,
				     sizeof(path)) < 0 ||
	    !begin_open(app, path, false, NULL, 0, play, selected))
		show_error(app, errno);
}

static void seek_to(struct music_app *app, uint64_t position_ms)
{
	if (!app->loaded || app->loading)
		return;
	close_stream(app);
	app->generation++;
	app->seek_ms = position_ms;
	app->seek_pending = true;
	app->position_ms = position_ms;
	app->info.seeking = true;
	pump(app);
}

static void skip(struct music_app *app, int direction, bool automatic)
{
	struct music_transition transition = music_model_transition(
		app->queue.count, app->selected, played_ms(app), app->repeat,
		automatic     ? MUSIC_END :
		direction < 0 ? MUSIC_PREVIOUS :
				MUSIC_NEXT);
	if (transition.action == MUSIC_TRANSITION_RESTART) {
		seek_to(app, 0);
	} else if (transition.action == MUSIC_TRANSITION_SELECT) {
		open_queue_track(app, transition.selected, app->playing);
	} else {
		app->playing = false;
		fplinux_ui_audio_pause(app->stream, true);
	}
}

static void toggle_play(struct music_app *app)
{
	if (app->loading)
		return;
	if (!app->loaded) {
		size_t queue_index = app->queue.count ? app->selected :
							MUSIC_REPLACE_QUEUE;

		if (app->path[0] &&
		    !begin_open(app, app->path, false, NULL, app->position_ms,
				true, queue_index))
			show_error(app, errno);
		return;
	}
	if (is_finished(app) && !app->playing)
		seek_to(app, 0);
	app->position_ms = played_ms(app);
	app->playing = !app->playing;
	app->output_error = 0;
	if (app->stream &&
	    !fplinux_ui_audio_pause(app->stream, !app->playing)) {
		int error = errno;
		uint64_t position_ms = played_ms(app);
		app->playing = false;
		seek_to(app, position_ms);
		app->output_error = error;
	}
	save(app);
	pump(app);
	redraw(app);
}

static void picker_result(struct fplinux_ui_file_picker *picker,
			  enum fplinux_ui_file_picker_status status,
			  const char *path, int error, void *context)
{
	struct music_app *app = context;
	char selected[FPLINUX_FILES_PATH_MAX];
	if (path)
		snprintf(selected, sizeof(selected), "%s", path);
	fplinux_ui_file_picker_close(picker);
	app->picker = NULL;
	if (status == FPLINUX_UI_FILE_PICKER_SELECTED) {
		if (!begin_open(app, selected, app->folder_picker, NULL, 0,
				true, MUSIC_REPLACE_QUEUE))
			show_error(app, errno);
	} else if (status == FPLINUX_UI_FILE_PICKER_ERROR) {
		show_error(app, error);
	}
}

static void open_picker(struct music_app *app, bool folder)
{
	static const char *const extensions[] = { "mp3", "wav", NULL };
	struct fplinux_ui_file_picker_options options = {
		.mode = folder ? FPLINUX_UI_FILE_PICKER_FOLDER :
				 FPLINUX_UI_FILE_PICKER_FILE,
		.home = app->home,
		.title = folder ? _("Open folder") : _("Open file"),
		.folder_action = _("Play folder"),
		.extensions = extensions,
	};
	app->folder_picker = folder;
	app->picker = fplinux_ui_file_picker_open(app->ui, &options,
						  picker_result, app);
	if (!app->picker)
		show_error(app, errno);
}

static bool queue_row(struct fplinux_ui *ui, struct fplinux_ui_screen *list,
		      size_t index, struct fplinux_ui_virtual_row *row,
		      void *context)
{
	struct music_app *app = context;
	(void)ui;
	(void)list;
	if (!app->queue.count && index == 0 && app->path[0]) {
		row->text = file_name(app->path);
		row->detail = _("Current track");
		return true;
	}
	if (index >= app->queue.count)
		return false;
	row->text = app->queue.entries[index].name;
	row->detail = index == app->selected ? _("Current track") : NULL;
	return true;
}

static void child_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int command,
			  void *context)
{
	struct music_app *app = context;
	if (command == CMD_BACK) {
		fplinux_ui_go_back(ui);
	} else if (command == FPLINUX_UI_SELECT_COMMAND) {
		size_t selected = fplinux_ui_list_get_focused(screen);
		fplinux_ui_pop_to(ui, app->view.screen);
		if (app->queue.count)
			open_queue_track(app, selected, true);
		else if (app->path[0] &&
			 !begin_open(app, app->path, false, NULL, 0, true,
				     MUSIC_REPLACE_QUEUE))
			show_error(app, errno);
	}
}

static void open_queue(struct music_app *app)
{
	if (!app->queue.count && !app->path[0]) {
		struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_INFO, _("Queue"),
			_("No file selected"), 0);
		if (alert)
			fplinux_ui_navigate(app->ui, alert);
		return;
	}
	struct fplinux_ui_screen *list = fplinux_ui_list_new_virtual(
		app->ui, _("Queue"), FPLINUX_UI_LIST_IMPLICIT,
		app->queue.count ? app->queue.count : 1U, queue_row, app);
	if (!list)
		return;
	add_command(app, list, CMD_BACK, FPLINUX_UI_COMMAND_BACK, NULL, 0,
		    NULL);
	fplinux_ui_list_set_select_label(list, _("Play"));
	fplinux_ui_list_set_focused(list, app->selected);
	fplinux_ui_screen_set_listener(list, child_command, NULL, app);
	fplinux_ui_navigate(app->ui, list);
}

static void open_details(struct music_app *app)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _("Details"));
	if (!form)
		return;
	fplinux_ui_form_append_property(form, _("File"),
					app->path[0] ? app->path :
						       _("No file selected"));
	if (app->info.title[0])
		fplinux_ui_form_append_property(form, _("Title"),
						app->info.title);
	if (app->info.artist[0])
		fplinux_ui_form_append_property(form, _("Artist"),
						app->info.artist);
	if (app->info.album[0])
		fplinux_ui_form_append_property(form, _("Album"),
						app->info.album);
	if (app->loaded) {
		char format[80];
		snprintf(format, sizeof(format), "%s · %u Hz · %s",
			 app->info.codec == FPLINUX_MEDIA_WAV ? "WAV" : "MP3",
			 app->info.rate,
			 app->info.channels == 1 ? _("Mono") : _("Stereo"));
		fplinux_ui_form_append_property(form, _("Format"), format);
	}
	if (app->error)
		fplinux_ui_form_append_property(form, _("Status"),
						error_text(app->error));
	add_command(app, form, CMD_BACK, FPLINUX_UI_COMMAND_BACK, NULL, 0,
		    NULL);
	fplinux_ui_screen_set_listener(form, child_command, NULL, app);
	fplinux_ui_navigate(app->ui, form);
}

static void command(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    int command_id, void *context)
{
	struct music_app *app = context;
	(void)screen;
	switch (command_id) {
	case CMD_BACK:
		fplinux_ui_go_back(ui);
		break;
	case CMD_PLAY:
		if (app->path[0])
			toggle_play(app);
		else
			open_picker(app, false);
		break;
	case CMD_STOP:
		stopped(app);
		app->position_ms = 0;
		save(app);
		pump(app);
		break;
	case CMD_FILE:
	case CMD_FOLDER:
		open_picker(app, command_id == CMD_FOLDER);
		break;
	case CMD_QUEUE:
		open_queue(app);
		break;
	case CMD_REPEAT:
		app->repeat = (app->repeat + 1) % 3;
		save(app);
		break;
	case CMD_DETAILS:
		open_details(app);
		break;
	default:
		break;
	}
	redraw(app);
}

static void key(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		const struct fplinux_ui_key *key, void *context)
{
	struct music_app *app = context;
	(void)screen;
	if (key->code == KEY_UP || key->code == KEY_DOWN) {
		if (key->pressed && !fplinux_ui_audio_volume_step(
					    ui, key->code == KEY_UP ? 1 : -1)) {
			app->volume_warning = true;
			music_view_update_volume(&app->view, ui, true);
		}
		return;
	}
	if (key->code != KEY_LEFT && key->code != KEY_RIGHT)
		return;
	if (key->pressed) {
		if (app->held_key == key->code) {
			app->held_seek = true;
			uint64_t position = app->seek_pending ||
							    app->info.seeking ?
						    app->seek_ms :
						    played_ms(app);
			if (key->code == KEY_RIGHT)
				position += MUSIC_SEEK_MS;
			else
				position = position > MUSIC_SEEK_MS ?
						   position - MUSIC_SEEK_MS :
						   0;
			app->seek_ms = position;
			seek_to(app, position);
		} else {
			app->held_key = key->code;
			app->held_seek = false;
		}
	} else if (app->held_key == key->code) {
		bool held = app->held_seek;
		app->held_key = 0;
		app->held_seek = false;
		if (!held)
			skip(app, key->code == KEY_RIGHT ? 1 : -1, false);
		save(app);
	}
	redraw(app);
}

static void active(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		   bool enabled, void *context)
{
	struct music_app *app = context;
	(void)ui;
	(void)screen;
	if (!enabled) {
		app->held_key = 0;
		app->held_seek = false;
	}
}

static const char *state_text(struct music_app *app)
{
	if (!fplinux_ui_audio_supported(app->ui))
		return _("Sound is unavailable");
	if (app->error)
		return _("File unavailable");
	if (app->loading)
		return _("Opening…");
	if (app->info.seeking)
		return _("Seeking…");
	if (!app->loaded)
		return _("Stopped");
	if (!app->playing && app->output_error)
		return _("Audio output unavailable");
	if (!app->playing)
		return is_finished(app) ? _("Finished") : _("Paused");
	if (!app->stream)
		return app->output_error ? _("Waiting for audio output") :
					   _("Starting…");
	struct fplinux_ui_audio_status status;
	if (app->stream && fplinux_ui_audio_status(app->stream, &status) &&
	    status.state != FPLINUX_UI_AUDIO_PLAYING) {
		if (status.state == FPLINUX_UI_AUDIO_PAUSED &&
		    status.reason == FPLINUX_UI_AUDIO_REASON_DEVICE &&
		    app->device_pause_seen)
			return _("Resuming…");
		if (status.reason == FPLINUX_UI_AUDIO_REASON_FM)
			return _("Waiting for FM radio");
		if (status.reason == FPLINUX_UI_AUDIO_REASON_CLOCK)
			return _("Paused for alarm");
		return _("Playback paused by system");
	}
	return _("Playing");
}

static void redraw(struct music_app *app)
{
	music_view_update_volume(&app->view, app->ui, app->volume_warning);
	app->position_ms = played_ms(app);
	music_view_update(&app->view,
			  app->info.title[0] ? app->info.title :
					       file_name(app->path),
			  app->info.artist, state_text(app), app->position_ms,
			  app->info.duration_ms, app->info.duration_known,
			  !app->path[0] && !app->loading);
	bool has_file = app->path[0] != '\0';
	if (!app->commands_ready || app->command_playing != app->playing ||
	    app->command_has_file != has_file) {
		add_command(app, app->view.screen, CMD_PLAY,
			    FPLINUX_UI_COMMAND_OK,
			    !app->path[0] ? _("Open") :
			    app->playing  ? _("Pause") :
					    _("Play"),
			    0, app->playing ? "pause" : "play");
		app->command_playing = app->playing;
		app->command_has_file = has_file;
	}
	const char *repeat =
		app->repeat == MUSIC_REPEAT_ONE ? _("Repeat: track") :
		app->repeat == MUSIC_REPEAT_ALL ? _("Repeat: queue") :
						  _("Repeat: off");
	if (!app->commands_ready || app->command_repeat != app->repeat) {
		add_command(app, app->view.screen, CMD_REPEAT,
			    FPLINUX_UI_COMMAND_SCREEN, repeat, 5, "refresh");
		app->command_repeat = app->repeat;
	}
	app->commands_ready = true;
}

static void save(struct music_app *app)
{
	if (!app->prefs)
		return;
	app->last_save_ms = fplinux_ui_now_ms();
	app->position_ms = played_ms(app);
	bool updated =
		fplinux_ui_prefs_set(app->prefs, "source", app->source) &&
		fplinux_ui_prefs_set(app->prefs, "track",
				     file_name(app->path)) &&
		fplinux_ui_prefs_set_int(app->prefs, "folder",
					 app->source_folder) &&
		fplinux_ui_prefs_set_int(app->prefs, "position_ms",
					 (long long)app->position_ms) &&
		fplinux_ui_prefs_set_int(app->prefs, "repeat", app->repeat);
	struct fplinux_storage_result result =
		updated ? fplinux_ui_prefs_save(app->prefs) :
			  (struct fplinux_storage_result){
				  .state = FPLINUX_STORAGE_UNPUBLISHED,
				  .error = errno
			  };
	if (result.state == FPLINUX_STORAGE_DURABLE)
		app->save_warning = false;
	if (result.state != FPLINUX_STORAGE_DURABLE && !app->closing &&
	    !app->save_warning) {
		app->save_warning = true;
		struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
			app->ui, FPLINUX_UI_ALERT_WARNING,
			_("Settings not saved"),
			fplinux_ui_storage_error(result), 0);
		if (alert)
			fplinux_ui_push(app->ui, alert);
	}
}

static void loaded_result(struct music_app *app,
			  struct music_player_result *result)
{
	app->info = result->info;
	app->loaded = true;
	app->playing = app->target_play;
	app->base_frames = result->info.position_frames;
	if (!app->info.seeking)
		app->position_ms = app->base_frames * 1000U / app->info.rate;
	if (app->target_queue_index == MUSIC_REPLACE_QUEUE) {
		fplinux_files_directory_free(&app->queue);
		app->queue = result->directory;
		result->directory = (struct fplinux_files_directory){ 0 };
		app->selected = result->selected;
		app->source_folder = app->target_folder;
		strcpy(app->source, app->target);
		strcpy(app->folder, app->target_folder ? app->target : "");
	} else {
		app->selected = app->target_queue_index;
	}
	if (app->target_folder) {
		fplinux_files_child_path(app->target,
					 app->queue.entries[app->selected].name,
					 app->path, sizeof(app->path));
	} else {
		strcpy(app->path, app->target);
	}
	identify_volume(app);
	if (!app->info.seeking) {
		app->loading = false;
		finish_wait(app);
	}
}

static void collect(struct music_app *app)
{
	struct music_player_result result;
	if (!music_player_collect(&app->player, &result))
		return;
	if (result.generation != app->generation) {
		fplinux_files_directory_free(&result.directory);
		return;
	}
	if (result.error &&
	    (result.action == MUSIC_PLAYER_OPEN || result.error != EAGAIN)) {
		app->position_ms = played_ms(app);
		app->error = result.error;
		app->playing = false;
		app->loaded = false;
		app->loading = false;
		close_stream(app);
		app->close_pending = true;
		if (app->volume_id)
			fplinux_ui_storage_request(app->ui,
						   FPLINUX_VOLUME_REFRESH, 0);
		finish_wait(app);
	} else if (result.action == MUSIC_PLAYER_OPEN) {
		loaded_result(app, &result);
	} else if (result.action == MUSIC_PLAYER_READ ||
		   result.action == MUSIC_PLAYER_SEEK) {
		bool was_seeking = app->info.seeking;
		app->info = result.info;
		if (was_seeking && !app->info.seeking) {
			app->base_frames = app->info.position_frames;
			app->position_ms =
				app->base_frames * 1000U / app->info.rate;
			app->loading = false;
			finish_wait(app);
		}
		if (result.frames) {
			memcpy(app->samples, result.samples,
			       result.frames * app->info.channels *
				       sizeof(*app->samples));
			app->frames = result.frames;
			app->consumed = 0;
		}
	}
	fplinux_files_directory_free(&result.directory);
}

static bool output_admitted(struct music_app *app)
{
	return fplinux_ui_is_visible(app->ui) ||
	       fplinux_ui_audio_background_supported(app->ui);
}

static void arm_timer(struct music_app *app)
{
	unsigned int interval_ms = 0;
	if (!app->closing && app->loaded && app->playing &&
	    !app->info.seeking && !app->loading && app->stream) {
		struct fplinux_ui_audio_status status;
		interval_ms = fplinux_ui_audio_status(app->stream, &status) &&
					      status.state ==
						      FPLINUX_UI_AUDIO_PLAYING ?
				      20U :
			      output_admitted(app) ? 100U :
						     0U;
	}
	if (!app->closing && app->loaded && app->volume_id && !interval_ms)
		interval_ms = 1000U;
	if (app->timer < 0 || interval_ms == app->timer_interval_ms)
		return;
	struct itimerspec interval = {
		.it_interval = { .tv_sec = interval_ms / 1000U,
				 .tv_nsec = (long)(interval_ms % 1000U) *
					    1000000L },
	};
	interval.it_value = interval.it_interval;
	if (timerfd_settime(app->timer, 0, &interval, NULL) == 0)
		app->timer_interval_ms = interval_ms;
	else {
		app->output_error = errno;
		app->playing = false;
	}
}

static void advance_player(struct music_app *app)
{
	collect(app);
	if (app->closing)
		return;
	if (app->open_pending) {
		if (music_player_request(
			    &app->player, MUSIC_PLAYER_OPEN, app->generation,
			    app->target, app->target_folder,
			    app->target_selected, app->target_position_ms))
			app->open_pending = false;
		return;
	}
	if (app->close_pending) {
		if (music_player_request(&app->player, MUSIC_PLAYER_CLOSE,
					 app->generation, NULL, false, NULL, 0))
			app->close_pending = false;
		return;
	}
	if (app->seek_pending) {
		if (music_player_request(&app->player, MUSIC_PLAYER_SEEK,
					 app->generation, NULL, false, NULL,
					 app->seek_ms))
			app->seek_pending = false;
		return;
	}
	if (!app->loaded)
		return;
	if (app->info.seeking) {
		music_player_request(&app->player, MUSIC_PLAYER_READ,
				     app->generation, NULL, false, NULL, 0);
		return;
	}
	if (!app->playing)
		return;
	if (!app->stream) {
		if (!output_admitted(app)) {
			app->output_error = EAGAIN;
			return;
		}
		app->stream = fplinux_ui_audio_open(app->ui, app->info.rate,
						    app->info.channels);
		if (!app->stream) {
			app->output_error = errno;
			app->playing = false;
			return;
		}
		app->output_error = 0;
	}
	struct fplinux_ui_audio_status status;
	bool has_status = fplinux_ui_audio_status(app->stream, &status);
	if (has_status && status.state == FPLINUX_UI_AUDIO_PAUSED &&
	    status.reason == FPLINUX_UI_AUDIO_REASON_DEVICE) {
		if (!app->device_pause_seen) {
			app->position_ms = played_ms(app);
			app->playing = false;
			app->output_error = ENODEV;
			app->device_pause_seen = true;
			return;
		}
	} else if (has_status) {
		app->device_pause_seen = false;
	}
	if (has_status && status.state == FPLINUX_UI_AUDIO_ENDED) {
		app->position_ms = played_ms(app);
		uint64_t position_ms = app->position_ms;
		app->playing = false;
		seek_to(app, position_ms);
		app->output_error = EPIPE;
		return;
	}
	if (app->frames > app->consumed) {
		long written = fplinux_ui_audio_write(
			app->stream,
			app->samples + app->consumed * app->info.channels,
			app->frames - app->consumed);
		if (written < 0) {
			int error = errno;
			uint64_t position_ms = played_ms(app);
			app->playing = false;
			seek_to(app, position_ms);
			app->output_error = error;
			return;
		}
		app->consumed += (size_t)written;
		app->submitted_frames += (size_t)written;
		if (app->consumed != app->frames)
			return;
		app->frames = app->consumed = 0;
	}
	long queued = fplinux_ui_audio_queued(app->stream);
	if (app->info.eof) {
		if (!queued) {
			app->position_ms = played_ms(app);
			skip(app, 1, true);
			save(app);
		}
		return;
	}
	if (queued >= 0 && queued < (long)app->info.rate / 4 &&
	    (!fplinux_ui_audio_status(app->stream, &status) ||
	     status.state == FPLINUX_UI_AUDIO_PLAYING))
		music_player_request(&app->player, MUSIC_PLAYER_READ,
				     app->generation, NULL, false, NULL, 0);
}

static void pump(struct music_app *app)
{
	bool was_playing = app->playing;
	advance_player(app);
	arm_timer(app);
	if (was_playing != app->playing && fplinux_ui_is_visible(app->ui))
		redraw(app);
}

static void fd_ready(struct fplinux_ui *ui, int fd, short events, void *context)
{
	struct music_app *app = context;
	(void)ui;
	if (events & POLLIN) {
		if (fd == app->timer) {
			uint64_t ticks;
			while (read(fd, &ticks, sizeof(ticks)) > 0)
				;
		}
		pump(app);
		if (app->closing)
			return;
		uint64_t now = fplinux_ui_now_ms();
		if (app->playing && now - app->last_save_ms >= 30000U)
			save(app);
		if (app->loaded && app->volume_id &&
		    now - app->last_storage_ms >= 1000U) {
			app->last_storage_ms = now;
			fplinux_ui_storage_request(app->ui,
						   FPLINUX_VOLUME_REFRESH, 0);
		}
		if (fplinux_ui_is_visible(app->ui) &&
		    now - app->last_redraw_ms >= 200U) {
			app->last_redraw_ms = now;
			redraw(app);
		}
	}
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *result,
			   void *context)
{
	struct music_app *app = context;
	(void)ui;
	if (app->picker)
		fplinux_ui_file_picker_storage_result(app->picker, result);
	app->volumes = result->snapshot;
	if (app->loaded && !app->volume_id)
		identify_volume(app);
	arm_timer(app);
	if (!app->volume_id || (!app->loaded && !app->error))
		return;
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];
		if (volume->id == app->volume_id &&
		    volume->state == FPLINUX_VOLUME_MOUNTED &&
		    volume->mount_id == app->mount_id)
			return;
	}
	stopped(app);
	app->error = ENODEV;
	pump(app);
	redraw(app);
}

static struct fplinux_ui_document_result
document(struct fplinux_ui *ui,
	 const struct fplinux_ui_document_request *request, void *context)
{
	struct music_app *app = context;
	(void)ui;
	if (app->picker || app->document)
		return (struct fplinux_ui_document_result){
			.status = FPLINUX_UI_DOCUMENT_ERROR, .error = EBUSY
		};
	if (!begin_open(app, request->path, false, NULL, 0, true,
			MUSIC_REPLACE_QUEUE))
		return (struct fplinux_ui_document_result){
			.status = FPLINUX_UI_DOCUMENT_ERROR, .error = errno
		};
	app->document = request->request_id;
	return (struct fplinux_ui_document_result){
		.status = FPLINUX_UI_DOCUMENT_WAIT
	};
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	struct music_app *app = context;
	(void)ui;
	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		app->closing = true;
		save(app);
		close_stream(app);
	} else if (event->type == FPLINUX_UI_EVENT_RESIZE) {
		music_view_layout(&app->view, app->ui);
	} else if (event->type == FPLINUX_UI_EVENT_SYSTEM_LEVELS) {
		music_view_update_volume(&app->view, ui, app->volume_warning);
	} else if (event->type == FPLINUX_UI_EVENT_SESSION &&
		   !strcmp(event->session->name, "media_volume_step") &&
		   strcmp(event->session->reason, "superseded") &&
		   (event->session->type == FPLINUX_UI_SESSION_SETTING ||
		    event->session->type == FPLINUX_UI_SESSION_ERROR)) {
		app->volume_warning = event->session->type ==
				      FPLINUX_UI_SESSION_ERROR;
		music_view_update_volume(&app->view, ui, app->volume_warning);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY ||
		   event->type == FPLINUX_UI_EVENT_LIFECYCLE) {
		pump(app);
		if (fplinux_ui_is_visible(app->ui))
			redraw(app);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		app->held_key = 0;
		app->held_seek = false;
	} else if (event->type == FPLINUX_UI_EVENT_ACTIVATION) {
		if (!app->waiting && !app->picker)
			fplinux_ui_pop_to(app->ui, app->view.screen);
	}
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-music",
		.description = "Play local MP3 and WAV files",
	};
	int result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;
	struct music_app app = { .timer = -1 };
	const char *home = getenv("HOME");
	if (!home || fplinux_files_child_path(home, "Files", app.home,
					      sizeof(app.home)) < 0) {
		fputs("fplinux-music: an absolute HOME directory is required\n",
		      stderr);
		return 1;
	}
	if (music_player_init(&app.player) < 0) {
		perror("fplinux-music: decoder worker");
		return 1;
	}
	app.ui = fplinux_ui_open(APP_ID, event, &app);
	if (!app.ui)
		goto fail;
	if (!music_view_create(&app.view, app.ui))
		goto fail;
	fplinux_ui_screen_set_listener(app.view.screen, command, NULL, &app);
	fplinux_ui_canvas_set_key_handler(app.view.screen, key, &app);
	fplinux_ui_canvas_set_key_repeat(app.view.screen,
					 FPLINUX_UI_CANVAS_REPEAT_NAVIGATION);
	fplinux_ui_canvas_set_active_listener(app.view.screen, active, &app);
	add_command(&app, app.view.screen, CMD_BACK, FPLINUX_UI_COMMAND_BACK,
		    NULL, 0, NULL);
	add_command(&app, app.view.screen, CMD_FILE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Open file"), 1, "file");
	add_command(&app, app.view.screen, CMD_FOLDER,
		    FPLINUX_UI_COMMAND_SCREEN, _("Open folder"), 2, "folder");
	add_command(&app, app.view.screen, CMD_QUEUE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Queue"), 3, "list");
	add_command(&app, app.view.screen, CMD_STOP, FPLINUX_UI_COMMAND_SCREEN,
		    _("Stop"), 4, "stop");
	add_command(&app, app.view.screen, CMD_DETAILS,
		    FPLINUX_UI_COMMAND_SCREEN, _("Details"), 6, "about");
	if (!fplinux_ui_push(app.ui, app.view.screen))
		goto fail;
	fplinux_ui_set_document_handler(app.ui, document, &app);
	fplinux_ui_set_storage_handler(app.ui, storage_result, &app);
	app.timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (app.timer < 0 ||
	    !fplinux_ui_watch_fd(app.ui, app.timer, POLLIN, fd_ready, &app) ||
	    !fplinux_ui_watch_fd(app.ui, app.player.notice, POLLIN, fd_ready,
				 &app))
		goto fail;
	app.prefs = fplinux_ui_prefs_open(APP_ID);
	if (app.prefs) {
		long long repeat =
			fplinux_ui_prefs_get_int(app.prefs, "repeat", 0);
		if (repeat >= MUSIC_REPEAT_OFF && repeat <= MUSIC_REPEAT_ONE)
			app.repeat = (enum music_repeat)repeat;
		const char *source =
			fplinux_ui_prefs_get(app.prefs, "source", "");
		long long position =
			fplinux_ui_prefs_get_int(app.prefs, "position_ms", 0);
		const char *track =
			fplinux_ui_prefs_get(app.prefs, "track", "");
		bool folder =
			fplinux_ui_prefs_get_int(app.prefs, "folder", 0) != 0;
		if (source[0] && strlen(source) < sizeof(app.source)) {
			strcpy(app.source, source);
			app.source_folder = folder;
			if (folder) {
				strcpy(app.folder, source);
				if (track[0])
					fplinux_files_child_path(
						source, track, app.path,
						sizeof(app.path));
			} else {
				strcpy(app.path, source);
			}
			app.position_ms = position > 0 ? (uint64_t)position : 0;
			begin_open(&app, source, folder, track, app.position_ms,
				   false, MUSIC_REPLACE_QUEUE);
		}
	} else {
		struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
			app.ui, FPLINUX_UI_ALERT_WARNING,
			_("Settings could not be loaded"), strerror(errno), 0);
		if (alert)
			fplinux_ui_push(app.ui, alert);
	}
	fplinux_ui_storage_request(app.ui, FPLINUX_VOLUME_REFRESH, 0);
	redraw(&app);
	result = fplinux_ui_run(app.ui);
	app.closing = true;
	close_stream(&app);
	if (app.picker)
		fplinux_ui_file_picker_close(app.picker);
	fplinux_ui_unwatch_fd(app.ui, app.player.notice);
	fplinux_ui_unwatch_fd(app.ui, app.timer);
	close(app.timer);
	music_player_cleanup(&app.player);
	fplinux_files_directory_free(&app.queue);
	fplinux_ui_prefs_close(app.prefs);
	fplinux_ui_close(app.ui);
	return result < 0;
fail:
	perror("fplinux-music");
	if (app.ui) {
		fplinux_ui_unwatch_fd(app.ui, app.player.notice);
		if (app.timer >= 0)
			fplinux_ui_unwatch_fd(app.ui, app.timer);
	}
	if (app.timer >= 0)
		close(app.timer);
	if (app.ui)
		fplinux_ui_close(app.ui);
	music_player_cleanup(&app.player);
	return 1;
}
