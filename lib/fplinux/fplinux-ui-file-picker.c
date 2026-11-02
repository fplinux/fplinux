/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-files-browse.h"
#include "fplinux-ui-file-picker.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-screen.h"
#include "fplinux-utf8.h"

#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <linux/limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define _(text) dgettext("fplinux-ui", text)

enum picker_command {
	PICKER_BACK = 1,
	PICKER_CHOOSE,
	PICKER_CONNECT,
	PICKER_EJECT,
	PICKER_REFRESH,
};

struct picker_folder {
	struct picker_folder *parent;
	struct fplinux_ui_screen *screen;
	struct fplinux_files_directory directory;
	size_t *rows;
	size_t count;
	int fd;
	uint32_t volume_id;
	char path[PATH_MAX];
};

struct fplinux_ui_file_picker {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *base;
	struct fplinux_ui_screen *places;
	struct picker_folder *folder;
	struct picker_folder *pending;
	struct fplinux_volume_snapshot volumes;
	struct fplinux_volume_snapshot shown_volumes;
	fplinux_ui_file_picker_cb callback;
	void *context;
	enum fplinux_ui_file_picker_mode mode;
	bool writable;
	char *home;
	char *title;
	char *folder_action;
	char **extensions;
	int pipe[2];
	bool watching;
	pthread_t thread;
	atomic_bool cancelled;
	bool running;
	bool waiting;
	int read_error;
	uint32_t read_mount_id;
	uint32_t storage_request;
	bool storage_waiting;
	int storage_error;
	bool card_read_failed;
	lv_timer_t *places_update;
	char row_label[NAME_MAX * 4 + 1];
};

static void show_places(struct fplinux_ui_file_picker *picker);
static void read_folder(struct fplinux_ui_file_picker *picker, const char *path,
			uint32_t volume_id, uint32_t mount_id);
static void request_storage(struct fplinux_ui_file_picker *picker,
			    enum fplinux_volume_operation operation,
			    uint32_t id);

static const struct fplinux_volume *
mounted_volume(const struct fplinux_ui_file_picker *picker, uint32_t id)
{
	for (size_t i = 0; i < picker->volumes.count; i++) {
		const struct fplinux_volume *volume =
			&picker->volumes.volumes[i];

		if (volume->id == id && volume->state == FPLINUX_VOLUME_MOUNTED)
			return volume;
	}
	return NULL;
}

static bool valid_utf8(const char *text)
{
	const unsigned char *cursor = (const unsigned char *)text;
	const unsigned char *end = cursor + strlen(text);
	uint32_t code;

	while (cursor < end) {
		if (!fplinux_utf8_next(&cursor, end, &code))
			return false;
	}
	return true;
}

static void display_name(const char *name, char *out, size_t capacity)
{
	const unsigned char *cursor = (const unsigned char *)name;
	const unsigned char *end = cursor + strlen(name);
	size_t used = 0;

	while (cursor < end && used + 1 < capacity) {
		const unsigned char *next = cursor;
		uint32_t code;

		if (!fplinux_utf8_next(&next, end, &code) || code < 32 ||
		    (code >= 127 && code <= 159) || code == '\\') {
			if (capacity - used <= 4)
				break;
			snprintf(out + used, capacity - used, "\\x%02X",
				 *cursor++);
			used += 4;
		} else {
			size_t length = (size_t)(next - cursor);

			if (capacity - used <= length)
				break;
			memcpy(out + used, cursor, length);
			used += length;
			cursor = next;
		}
	}
	out[used] = '\0';
}

static bool matches_extension(const struct fplinux_ui_file_picker *picker,
			      const char *name)
{
	const char *dot = strrchr(name, '.');

	if (!picker->extensions)
		return true;
	if (!dot || !dot[1])
		return false;
	for (size_t i = 0; picker->extensions[i]; i++) {
		const unsigned char *left = (const unsigned char *)dot + 1;
		const unsigned char *right =
			(const unsigned char *)picker->extensions[i];
		bool equal = true;

		while (*left && *right) {
			unsigned char a = *left++, b = *right++;

			if (a >= 'A' && a <= 'Z')
				a += 'a' - 'A';
			if (b >= 'A' && b <= 'Z')
				b += 'a' - 'A';
			if (a != b) {
				equal = false;
				break;
			}
		}
		if (equal && !*left && !*right)
			return true;
	}
	return false;
}

static void free_folder(struct picker_folder *folder)
{
	if (!folder)
		return;
	fplinux_files_directory_free(&folder->directory);
	free(folder->rows);
	if (folder->fd >= 0)
		close(folder->fd);
	free(folder);
}

static void free_folders(struct picker_folder *folder)
{
	while (folder) {
		struct picker_folder *parent = folder->parent;

		free_folder(folder);
		folder = parent;
	}
}

static bool read_cancelled(void *context)
{
	struct fplinux_ui_file_picker *picker = context;

	return atomic_load(&picker->cancelled);
}

static int read_entries(struct fplinux_ui_file_picker *picker)
{
	struct picker_folder *folder = picker->pending;

	folder->fd = fplinux_files_directory_open(folder->path,
						  picker->read_mount_id);
	if (folder->fd < 0 && errno == ENOENT &&
	    !strcmp(folder->path, picker->home)) {
		char name[FPLINUX_FILES_NAME_MAX + 1];
		int parent =
			fplinux_files_open_parent(folder->path, name, EEXIST);

		if (parent >= 0) {
			int result = mkdirat(parent, name, 0777);
			int error = errno;

			if (!result && fsync(parent) < 0) {
				result = -1;
				error = errno;
			}
			close(parent);
			if (!result || error == EEXIST)
				folder->fd = fplinux_files_directory_open(
					folder->path, 0);
			else
				errno = error;
		}
	}
	if (folder->fd < 0)
		return -1;
	if (fplinux_files_directory_read(folder->path, &folder->directory,
					 read_cancelled, picker) < 0)
		return -1;
	folder->rows =
		calloc(folder->directory.count ? folder->directory.count : 1,
		       sizeof(*folder->rows));
	if (!folder->rows)
		return -1;
	for (size_t i = 0; i < folder->directory.count; i++) {
		const struct fplinux_files_entry *entry =
			&folder->directory.entries[i];

		if (entry->kind == FPLINUX_FILES_KIND_DIRECTORY ||
		    (picker->mode == FPLINUX_UI_FILE_PICKER_FILE &&
		     entry->kind == FPLINUX_FILES_KIND_FILE &&
		     matches_extension(picker, entry->name)))
			folder->rows[folder->count++] = i;
	}
	return 0;
}

static void *read_worker(void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	char notice = 1;

	picker->read_error = read_entries(picker) < 0 ? errno : 0;
	while (write(picker->pipe[1], &notice, 1) < 0 && errno == EINTR)
		;
	return NULL;
}

static void add_command(struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label)
{
	struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
	};

	if (id == PICKER_CHOOSE) {
		command.short_label = _("Here");
		command.keep_visible = true;
	}
	fplinux_ui_screen_add_command(screen, &command);
}

static void show_error(struct fplinux_ui_file_picker *picker, int error)
{
	const char *message =
		error == EILSEQ ?
			_("The file name is not valid text.") :
		error == ENOMEM ?
			_("Not enough memory. Close and try again.") :
		error == EACCES || error == EPERM ?
			_("Access denied. Choose another folder.") :
			_("The file or storage is no longer available.");
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		picker->ui, FPLINUX_UI_ALERT_ERROR, picker->title, message, 0);

	if (alert)
		fplinux_ui_push(picker->ui, alert);
}

static bool row_provider(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t row,
			 struct fplinux_ui_virtual_row *out, void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	const struct fplinux_files_entry *entry;

	(void)screen;
	if (!picker->folder || row >= picker->folder->count)
		return false;
	entry = &picker->folder->directory.entries[picker->folder->rows[row]];
	display_name(entry->name, picker->row_label, sizeof(picker->row_label));
	out->text = picker->row_label;
	out->icon = fplinux_ui_glyph(
		ui,
		entry->kind == FPLINUX_FILES_KIND_DIRECTORY ? "folder" : "file",
		FPLINUX_UI_GLYPH_ROW);
	return true;
}

static void folder_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	struct picker_folder *folder = picker->folder;
	char path[PATH_MAX];
	size_t row;
	const struct fplinux_files_entry *entry;

	if (id == PICKER_BACK) {
		picker->folder = folder->parent;
		fplinux_ui_pop(ui);
		free_folder(folder);
		return;
	}
	if (id == PICKER_CHOOSE) {
		if (!valid_utf8(folder->path)) {
			show_error(picker, EILSEQ);
			return;
		}
		/* The callback may close the picker before reading its result. */
		snprintf(path, sizeof(path), "%s", folder->path);
		picker->callback(picker, FPLINUX_UI_FILE_PICKER_SELECTED, path,
				 0, picker->context);
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND)
		return;
	row = fplinux_ui_list_get_focused(screen);
	if (row >= folder->count)
		return;
	entry = &folder->directory.entries[folder->rows[row]];
	if (fplinux_files_child_path(folder->path, entry->name, path,
				     sizeof(path)) < 0) {
		show_error(picker, errno);
		return;
	}
	if (entry->kind == FPLINUX_FILES_KIND_DIRECTORY) {
		uint32_t mount_id = 0;

		for (size_t i = 0; i < picker->volumes.count; i++) {
			if (picker->volumes.volumes[i].id == folder->volume_id)
				mount_id = picker->volumes.volumes[i].mount_id;
		}
		if (folder->volume_id && !mount_id)
			show_error(picker, ENODEV);
		else
			read_folder(picker, path, folder->volume_id, mount_id);
	} else if (!valid_utf8(path)) {
		show_error(picker, EILSEQ);
	} else {
		picker->callback(picker, FPLINUX_UI_FILE_PICKER_SELECTED, path,
				 0, picker->context);
	}
}

static void read_present(struct fplinux_ui *ui, void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	struct picker_folder *folder = picker->pending;
	struct fplinux_ui_screen *screen;
	char title[NAME_MAX * 4 + 1];
	const char *name;

	picker->waiting = false;
	picker->pending = NULL;
	if (folder->volume_id && !mounted_volume(picker, folder->volume_id)) {
		free_folder(folder);
		show_places(picker);
		show_error(picker, ENODEV);
		return;
	}
	if (picker->read_error) {
		int error = picker->read_error;
		bool card = folder->volume_id != 0;

		free_folder(folder);
		if (error == ENOMEM) {
			picker->callback(picker, FPLINUX_UI_FILE_PICKER_ERROR,
					 NULL, error, picker->context);
		} else if (card && error != ECANCELED) {
			picker->card_read_failed = true;
			if (!picker->storage_request)
				request_storage(picker, FPLINUX_VOLUME_REFRESH,
						0);
			if (!picker->storage_request) {
				picker->card_read_failed = false;
				show_error(picker, error);
			}
		} else if (error != ECANCELED) {
			show_error(picker, error);
		}
		return;
	}
	name = !strcmp(folder->path, picker->home) ?
		       _("My files") :
	       folder->volume_id && !picker->folder ?
		       _("Memory card") :
		       strrchr(folder->path, '/') + 1;
	display_name(name, title, sizeof(title));
	if (folder->count)
		screen = fplinux_ui_list_new_virtual(ui, title,
						     FPLINUX_UI_LIST_IMPLICIT,
						     folder->count,
						     row_provider, picker);
	else {
		screen = fplinux_ui_form_new(ui, title);
		if (screen)
			fplinux_ui_form_append_string(
				screen, NULL,
				picker->mode == FPLINUX_UI_FILE_PICKER_FOLDER ?
					_("No subfolders. Select this folder with the left key.") :
					_("This folder is empty."));
	}
	if (!screen) {
		free_folder(folder);
		show_error(picker, ENOMEM);
		return;
	}
	add_command(screen, PICKER_BACK, FPLINUX_UI_COMMAND_BACK, NULL);
	if (picker->mode == FPLINUX_UI_FILE_PICKER_FOLDER)
		add_command(screen, PICKER_CHOOSE, FPLINUX_UI_COMMAND_SCREEN,
			    picker->folder_action);
	if (folder->count)
		fplinux_ui_list_set_select_label(screen, _("Open"));
	fplinux_ui_screen_set_listener(screen, folder_command, NULL, picker);
	folder->screen = screen;
	folder->parent = picker->folder;
	picker->folder = folder;
	fplinux_ui_push(ui, screen);
}

static void cancel_read(struct fplinux_ui *ui, void *context)
{
	struct fplinux_ui_file_picker *picker = context;

	(void)ui;
	atomic_store(&picker->cancelled, true);
}

static void read_folder(struct fplinux_ui_file_picker *picker, const char *path,
			uint32_t volume_id, uint32_t mount_id)
{
	struct picker_folder *folder;
	int error;

	if (picker->running || picker->waiting)
		return;
	folder = calloc(1, sizeof(*folder));
	if (!folder) {
		show_error(picker, ENOMEM);
		return;
	}
	folder->fd = -1;
	folder->volume_id = volume_id;
	snprintf(folder->path, sizeof(folder->path), "%s", path);
	if (!fplinux_ui_wait_begin(picker->ui, _("Reading folder"),
				   _("Please wait…"), cancel_read, picker)) {
		free_folder(folder);
		show_error(picker, errno);
		return;
	}
	picker->waiting = true;
	picker->pending = folder;
	picker->read_mount_id = mount_id;
	atomic_store(&picker->cancelled, false);
	error = pthread_create(&picker->thread, NULL, read_worker, picker);
	if (error) {
		picker->read_error = error;
		if (!fplinux_ui_wait_end(picker->ui, read_present, picker))
			read_present(picker->ui, picker);
		return;
	}
	picker->running = true;
}

static void fd_event(struct fplinux_ui *ui, int fd, short events, void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	char notice[32];

	(void)events;
	while (read(fd, notice, sizeof(notice)) > 0)
		;
	if (!picker->running)
		return;
	pthread_join(picker->thread, NULL);
	picker->running = false;
	if (!fplinux_ui_wait_end(ui, read_present, picker))
		read_present(ui, picker);
}

static const struct fplinux_volume *
place_volume(const struct fplinux_ui_file_picker *picker,
	     const struct fplinux_volume_snapshot *snapshot, size_t row)
{
	if (!row)
		return NULL;
	row--;
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct fplinux_volume *volume = &snapshot->volumes[i];

		if (volume->state == FPLINUX_VOLUME_PROTECTED ||
		    (picker->writable && volume->read_only))
			continue;
		if (!row--)
			return volume;
	}
	return NULL;
}

static const char *volume_detail(const struct fplinux_volume *volume)
{
	if (volume->state == FPLINUX_VOLUME_AVAILABLE)
		return _("Select to connect");
	if (volume->state == FPLINUX_VOLUME_UNSUPPORTED)
		return _("Unsupported format");
	return volume->read_only ? _("Read-only") : _("Connected");
}

static const char *home_detail(const struct fplinux_volume_snapshot *snapshot)
{
	if (!snapshot->home_persistent)
		return _("Temporary: lost after reboot");
	for (size_t i = 0; i < snapshot->count; i++) {
		if (snapshot->volumes[i].protection ==
		    FPLINUX_VOLUME_SYSTEM_ROOT)
			return _("On the memory card");
	}
	return _("Home directory");
}

static void fill_places(struct fplinux_ui_file_picker *picker)
{
	struct fplinux_ui_screen *screen = picker->places;
	size_t row = 0;
	const struct fplinux_volume *volume;

	if (!fplinux_ui_list_size(screen)) {
		fplinux_ui_list_append(screen, _("My files"),
				       home_detail(&picker->volumes));
		fplinux_ui_list_set_icon(screen, 0,
					 fplinux_ui_glyph(picker->ui, "folder",
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
	} else {
		fplinux_ui_list_set(screen, 0, _("My files"),
				    home_detail(&picker->volumes));
	}
	for (row = 1; (volume = place_volume(picker, &picker->volumes, row));
	     row++) {
		if (row < fplinux_ui_list_size(screen))
			fplinux_ui_list_set(screen, row, _("Memory card"),
					    volume_detail(volume));
		else {
			fplinux_ui_list_append(screen, _("Memory card"),
					       volume_detail(volume));
			fplinux_ui_list_set_icon(
				screen, row,
				fplinux_ui_glyph(picker->ui, "card",
						 FPLINUX_UI_GLYPH_ROW),
				FPLINUX_UI_LIST_ICON_IMAGE);
		}
	}
	while (fplinux_ui_list_size(screen) > row)
		fplinux_ui_list_delete(screen, row);
	picker->shown_volumes = picker->volumes;
}

static void places_focused(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, size_t row,
			   void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	const struct fplinux_volume *volume =
		place_volume(picker, &picker->shown_volumes, row);
	struct fplinux_ui_command command = { 0 };

	(void)ui;
	for (size_t i = 0; i < fplinux_ui_list_size(screen); i++) {
		fplinux_ui_screen_remove_item_command(screen, i,
						      PICKER_CONNECT);
		fplinux_ui_screen_remove_item_command(screen, i, PICKER_EJECT);
	}
	if (!volume)
		return;
	if (volume->state == FPLINUX_VOLUME_AVAILABLE) {
		command.id = PICKER_CONNECT;
		command.type = FPLINUX_UI_COMMAND_OK;
		command.label = _("Connect");
	} else if (volume->state == FPLINUX_VOLUME_MOUNTED) {
		command.id = PICKER_EJECT;
		command.type = FPLINUX_UI_COMMAND_ITEM;
		command.label = _("Safely eject");
		command.short_label = _("Eject");
	} else {
		return;
	}
	fplinux_ui_list_add_item_command(screen, row, &command);
}

static void update_places(struct fplinux_ui_file_picker *picker)
{
	size_t row, first, selected = 0;
	const struct fplinux_volume *old;
	uint32_t id;

	if (!picker->places)
		return;
	row = fplinux_ui_list_get_focused(picker->places);
	first = fplinux_ui_list_get_first_visible(picker->places);
	old = place_volume(picker, &picker->shown_volumes, row);
	id = old ? old->id : 0;
	fill_places(picker);
	if (id) {
		const struct fplinux_volume *volume;

		for (size_t i = 1;
		     (volume = place_volume(picker, &picker->shown_volumes, i));
		     i++) {
			if (volume->id == id) {
				selected = i;
				break;
			}
		}
		if (!selected && row < fplinux_ui_list_size(picker->places))
			selected = row;
	}
	fplinux_ui_list_set_focused(picker->places, selected);
	fplinux_ui_list_set_first_visible(picker->places, first);
	places_focused(picker->ui, picker->places, selected, picker);
}

static void update_places_later(lv_timer_t *timer)
{
	struct fplinux_ui_file_picker *picker = lv_timer_get_user_data(timer);

	if (fplinux_ui_menu_is_open(picker->ui))
		return;
	picker->places_update = NULL;
	lv_timer_delete(timer);
	update_places(picker);
}

static void request_storage(struct fplinux_ui_file_picker *picker,
			    enum fplinux_volume_operation operation,
			    uint32_t id)
{
	if (picker->storage_request)
		return;
	picker->storage_request =
		fplinux_ui_storage_request(picker->ui, operation, id);
	if (!picker->storage_request && errno != EOPNOTSUPP &&
	    !picker->card_read_failed) {
		show_error(picker, errno);
		return;
	}
	if (picker->storage_request && operation != FPLINUX_VOLUME_REFRESH)
		picker->storage_waiting =
			fplinux_ui_wait_begin(
				picker->ui, _("Memory card"),
				operation == FPLINUX_VOLUME_CONNECT ?
					_("Connecting…") :
					_("Ejecting…"),
				NULL, picker) != NULL;
}

static void places_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct fplinux_ui_file_picker *picker = context;
	size_t row = fplinux_ui_list_get_focused(screen);
	const struct fplinux_volume *volume =
		place_volume(picker, &picker->shown_volumes, row);

	(void)ui;
	if (id == PICKER_BACK) {
		picker->callback(picker, FPLINUX_UI_FILE_PICKER_CANCELLED, NULL,
				 0, picker->context);
		return;
	}
	if (id == PICKER_REFRESH) {
		request_storage(picker, FPLINUX_VOLUME_REFRESH, 0);
		return;
	}
	if (id == PICKER_CONNECT || id == PICKER_EJECT) {
		if (volume)
			request_storage(picker,
					id == PICKER_CONNECT ?
						FPLINUX_VOLUME_CONNECT :
						FPLINUX_VOLUME_EJECT,
					volume->id);
		return;
	}
	if (id != FPLINUX_UI_SELECT_COMMAND)
		return;
	if (!row) {
		read_folder(picker, picker->home, 0, 0);
	} else if (volume && volume->state == FPLINUX_VOLUME_MOUNTED) {
		read_folder(picker, volume->path, volume->id, volume->mount_id);
	} else if (volume && volume->state == FPLINUX_VOLUME_AVAILABLE) {
		request_storage(picker, FPLINUX_VOLUME_CONNECT, volume->id);
	} else {
		show_error(picker, EOPNOTSUPP);
	}
}

static void show_places(struct fplinux_ui_file_picker *picker)
{
	if (picker->folder) {
		fplinux_ui_pop_to(picker->ui, picker->places);
		free_folders(picker->folder);
		picker->folder = NULL;
	}
	update_places(picker);
}

static void storage_present(struct fplinux_ui *ui, void *context)
{
	struct fplinux_ui_file_picker *picker = context;

	(void)ui;
	picker->storage_waiting = false;
	update_places(picker);
	if (picker->storage_error && picker->storage_error != EOPNOTSUPP)
		show_error(picker, picker->storage_error);
}

void fplinux_ui_file_picker_storage_result(
	struct fplinux_ui_file_picker *picker,
	const struct fplinux_volume_result *result)
{
	bool own;

	if (!picker || !result)
		return;
	own = result->request_id &&
	      result->request_id == picker->storage_request;
	if (own)
		picker->storage_request = 0;
	picker->volumes = result->snapshot;
	if (own && picker->card_read_failed) {
		picker->card_read_failed = false;
		if (picker->folder && picker->folder->volume_id &&
		    !mounted_volume(picker, picker->folder->volume_id))
			show_places(picker);
		else
			update_places(picker);
		show_error(picker, picker->read_error);
		return;
	}
	if (own && picker->storage_waiting) {
		picker->storage_error = result->error;
		if (!fplinux_ui_wait_end(picker->ui, storage_present, picker))
			storage_present(picker->ui, picker);
		return;
	}
	if (picker->folder && picker->folder->volume_id && !picker->waiting &&
	    !mounted_volume(picker, picker->folder->volume_id)) {
		show_places(picker);
		show_error(picker, ENODEV);
		return;
	}
	if (fplinux_ui_menu_is_open(picker->ui)) {
		if (!picker->places_update)
			picker->places_update = lv_timer_create(
				update_places_later, 25, picker);
	} else {
		update_places(picker);
	}
	if (own && result->error && result->error != EOPNOTSUPP)
		show_error(picker, result->error);
}

static bool copy_options(struct fplinux_ui_file_picker *picker,
			 const struct fplinux_ui_file_picker_options *options)
{
	size_t count = 0;

	picker->home = strdup(options->home);
	picker->title = strdup(options->title ? options->title : _("Files"));
	picker->folder_action = strdup(
		options->folder_action ? options->folder_action : _("Select"));
	if (!picker->home || !picker->title || !picker->folder_action)
		return false;
	if (!options->extensions)
		return true;
	while (options->extensions[count])
		count++;
	picker->extensions = calloc(count + 1, sizeof(*picker->extensions));
	if (!picker->extensions)
		return false;
	for (size_t i = 0; i < count; i++) {
		picker->extensions[i] = strdup(options->extensions[i]);
		if (!picker->extensions[i])
			return false;
	}
	return true;
}

struct fplinux_ui_file_picker *fplinux_ui_file_picker_open(
	struct fplinux_ui *ui,
	const struct fplinux_ui_file_picker_options *options,
	fplinux_ui_file_picker_cb callback, void *context)
{
	struct fplinux_ui_file_picker *picker;
	int error;

	if (!ui || !options || !options->home || options->home[0] != '/' ||
	    strlen(options->home) >= PATH_MAX || !callback ||
	    (options->mode != FPLINUX_UI_FILE_PICKER_FILE &&
	     options->mode != FPLINUX_UI_FILE_PICKER_FOLDER) ||
	    !fplinux_ui_current(ui)) {
		errno = EINVAL;
		return NULL;
	}
	picker = calloc(1, sizeof(*picker));
	if (!picker)
		return NULL;
	picker->pipe[0] = picker->pipe[1] = -1;
	picker->ui = ui;
	picker->base = fplinux_ui_current(ui);
	picker->callback = callback;
	picker->context = context;
	picker->mode = options->mode;
	picker->writable = options->writable;
	atomic_init(&picker->cancelled, false);
	if (!copy_options(picker, options) ||
	    pipe2(picker->pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		goto failed;
	if (!fplinux_ui_watch_fd(ui, picker->pipe[0], POLLIN, fd_event, picker))
		goto failed;
	picker->watching = true;
	picker->places = fplinux_ui_list_new(ui, picker->title,
					     FPLINUX_UI_LIST_IMPLICIT);
	if (!picker->places)
		goto failed;
	fill_places(picker);
	add_command(picker->places, PICKER_BACK, FPLINUX_UI_COMMAND_BACK,
		    _("Cancel"));
	add_command(picker->places, PICKER_REFRESH, FPLINUX_UI_COMMAND_SCREEN,
		    _("Refresh"));
	fplinux_ui_list_set_select_label(picker->places, _("Open"));
	fplinux_ui_screen_set_listener(picker->places, places_command, NULL,
				       picker);
	fplinux_ui_list_set_focus_listener(picker->places, places_focused,
					   picker);
	if (!fplinux_ui_push(ui, picker->places))
		goto failed;
	request_storage(picker, FPLINUX_VOLUME_REFRESH, 0);
	return picker;

failed:
	error = errno;
	fplinux_ui_file_picker_close(picker);
	errno = error;
	return NULL;
}

void fplinux_ui_file_picker_close(struct fplinux_ui_file_picker *picker)
{
	if (!picker)
		return;
	if (picker->watching)
		fplinux_ui_unwatch_fd(picker->ui, picker->pipe[0]);
	if (picker->base)
		fplinux_ui_pop_to(picker->ui, picker->base);
	if (picker->running) {
		atomic_store(&picker->cancelled, true);
		pthread_join(picker->thread, NULL);
	}
	if (picker->places_update)
		lv_timer_delete(picker->places_update);
	free_folders(picker->folder);
	free_folder(picker->pending);
	if (picker->pipe[0] >= 0)
		close(picker->pipe[0]);
	if (picker->pipe[1] >= 0)
		close(picker->pipe[1]);
	if (picker->extensions) {
		for (size_t i = 0; picker->extensions[i]; i++)
			free(picker->extensions[i]);
		free(picker->extensions);
	}
	free(picker->home);
	free(picker->title);
	free(picker->folder_action);
	free(picker);
}
