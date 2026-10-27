/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "files-core.h"
#include "files-job.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-gettext.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-storage.h>
#include <fplinux/fplinux-utf8.h>

#include <errno.h>
#include <inttypes.h>
#include <libintl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APP_ID "org.fplinux.files"
#define _(text) gettext(text)
#define NC_(context, text) context "\004" text

enum command {
	COMMAND_BACK = 1,
	COMMAND_PLACES,
	COMMAND_SYSTEM,
	COMMAND_REFRESH,
	COMMAND_NEW_FOLDER,
	COMMAND_DETAILS,
	COMMAND_RENAME,
	COMMAND_DELETE,
	COMMAND_CONFIRM_DELETE,
	COMMAND_COPY,
	COMMAND_MOVE,
	COMMAND_PASTE,
	COMMAND_SAVE_NAME,
	COMMAND_CANCEL,
	COMMAND_CANCEL_JOB,
	COMMAND_SEARCH,
	COMMAND_FIND,
	COMMAND_HELP,
	COMMAND_EJECT,
	COMMAND_CONNECT,
	COMMAND_RETRY,
	COMMAND_OTHER_NAME,
	COMMAND_CLOSE_STOP,
	COMMAND_CLOSE_CONTINUE,
};

enum naming_action { NAMING_MKDIR, NAMING_RENAME, NAMING_DESTINATION };

struct files_app;

struct files_folder {
	struct files_app *app;
	struct files_folder *parent;
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_screen *results_screen;
	struct fplinux_ui_screen *search_screen;
	struct fplinux_files_directory directory;
	int descriptor;
	uint32_t volume_id;
	bool read_only;
	bool system;
	char path[FPLINUX_FILES_PATH_MAX];
	char root[FPLINUX_FILES_PATH_MAX];
	char focus[FPLINUX_FILES_NAME_MAX + 1];
	size_t *rows;
	size_t count;
	size_t *matches;
	size_t match_count;
	size_t command_row;
	struct fplinux_ui_screen *command_screen;
	char row_label[FPLINUX_FILES_NAME_MAX * 4 + 1];
};

struct files_app {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *places;
	struct fplinux_ui_screen *picker_places;
	struct fplinux_ui_screen *busy;
	struct fplinux_ui_screen *name_box;
	struct fplinux_ui_screen *decision;
	struct files_folder *folder;
	struct files_folder *origin;
	struct fplinux_files_job job;
	struct fplinux_volume_snapshot volumes;
	uint32_t storage_request;
	enum fplinux_volume_operation storage_operation;
	uint32_t document_request;
	bool picker;
	bool move;
	bool closing;
	bool stop_and_close;
	bool reading;
	enum naming_action naming;
	char home[FPLINUX_FILES_PATH_MAX];
	char selected[FPLINUX_FILES_PATH_MAX];
	char source[FPLINUX_FILES_PATH_MAX];
	char destination[FPLINUX_FILES_PATH_MAX];
	char destination_name[FPLINUX_FILES_NAME_MAX + 1];
};

static void folder_command(struct fplinux_ui *, struct fplinux_ui_screen *, int,
			   void *);
static void show_places(struct files_app *app);
static void refresh_folder(struct files_app *app, const char *focus);
static void show_name(struct files_app *app, enum naming_action action,
		      const char *raw_name);
static void start_mutation(struct files_app *app,
			   enum fplinux_files_action action, const char *source,
			   const char *destination);
static void request_storage(struct files_app *app,
			    enum fplinux_volume_operation operation,
			    uint32_t id);

static const char *base_name(const char *path)
{
	const char *separator = strrchr(path, '/');

	return separator && separator[1] ? separator + 1 : path;
}

/* Raw bytes remain the operation's name; only presentation escapes them. */
static void display_text(const char *raw, char *out, size_t capacity)
{
	const unsigned char *cursor = (const unsigned char *)raw;
	const unsigned char *end = cursor + strlen(raw);
	size_t used = 0;

	while (cursor != end && used + 1 < capacity) {
		const unsigned char *next = cursor;
		uint32_t code;
		bool valid = fplinux_utf8_next(&next, end, &code);
		size_t length = valid ? (size_t)(next - cursor) : 1;

		if (!valid || code < 0x20 || (code >= 0x7f && code <= 0x9f)) {
			if (capacity - used <= 4)
				break;
			snprintf(out + used, capacity - used, "\\x%02X",
				 *cursor++);
			used += 4;
		} else if (code == '\\') {
			if (capacity - used <= 2)
				break;
			out[used++] = '\\';
			out[used++] = '\\';
			cursor++;
		} else {
			if (capacity - used <= length)
				break;
			memcpy(out + used, cursor, length);
			used += length;
			cursor = next;
		}
	}
	out[used] = '\0';
}

static const char *error_message(int error)
{
	switch (error) {
	case EACCES:
	case EPERM:
		return _("Access denied. Choose another folder.");
	case ENOENT:
	case ENOTDIR:
	case ENODEV:
	case ESTALE:
		return _(
			"The file or storage is no longer available. Refresh Places.");
	case EROFS:
		return _("This storage is read-only.");
	case EILSEQ:
		return _(
			"The file name is not valid text. Rename it before opening.");
	case EEXIST:
		return _("That name already exists. Nothing was overwritten.");
	case ENOTEMPTY:
		return _(
			"The folder is not empty. Only empty folders can be deleted.");
	case ENOSPC:
	case EDQUOT:
		return _(
			"Not enough free space. Free some space and try again.");
	case ENAMETOOLONG:
		return _("The name is too long. The limit is 255 bytes.");
	case EINVAL:
		return _(
			"Enter a name without / or line breaks, other than . or .. .");
	case ELOOP:
		return _(
			"Symbolic links can be inspected but are not followed.");
	case EOPNOTSUPP:
		return _(
			"This operation is not supported by the storage or file type.");
	case EISDIR:
		return _("Copy and move are available for files only.");
	case EOVERFLOW:
		return _("This folder has more than 4096 entries.");
	case ECANCELED:
		return _("Operation cancelled.");
	case EBUSY:
		return _(
			"Storage is busy. Close applications using it and try again.");
	case ENOMEM:
		return _("Not enough memory. Close and try again.");
	default:
		return _(
			"The operation failed. Check the storage and try again.");
	}
}

static const char *command_icon(int id)
{
	switch (id) {
	case COMMAND_DETAILS:
		return "about";
	case COMMAND_COPY:
		return "copy";
	case COMMAND_MOVE:
		return "folder";
	case COMMAND_RENAME:
		return "edit";
	case COMMAND_DELETE:
		return "delete";
	case COMMAND_SEARCH:
		return "search";
	case COMMAND_NEW_FOLDER:
		return "add";
	case COMMAND_REFRESH:
		return "refresh";
	case COMMAND_PLACES:
		return "folder";
	case COMMAND_SYSTEM:
		return "folder";
	case COMMAND_CONNECT:
	case COMMAND_EJECT:
		return "card";
	default:
		return NULL;
	}
}

static const char *command_short_label(int id)
{
	switch (id) {
	case COMMAND_SAVE_NAME:
		return _("Done");
	case COMMAND_RETRY:
		return fplinux_gettext_context(NC_("short command", "Retry"));
	case COMMAND_OTHER_NAME:
		return _("Name");
	case COMMAND_CLOSE_STOP:
		return _("Stop");
	case COMMAND_CLOSE_CONTINUE:
		return _("Resume");
	case COMMAND_EJECT:
		return _("Eject");
	default:
		return NULL;
	}
}

static void add_command(struct files_app *app, struct fplinux_ui_screen *screen,
			int id, enum fplinux_ui_command_type type,
			const char *label, int priority)
{
	const char *icon = command_icon(id);
	struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.short_label = command_short_label(id),
		.priority = priority,
		.separator_before = id == COMMAND_PLACES || id == COMMAND_HELP,
		.icon = icon ? fplinux_ui_glyph(app->ui, icon,
						FPLINUX_UI_GLYPH_ROW) :
			       NULL,
	};

	fplinux_ui_screen_add_command(screen, &command);
}

static void add_item(struct files_app *app, struct fplinux_ui_screen *screen,
		     size_t row, int id, enum fplinux_ui_command_type type,
		     const char *label, int priority)
{
	const char *icon = command_icon(id);
	struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.short_label = command_short_label(id),
		.priority = priority,
		.separator_before = id == COMMAND_DELETE,
		.icon = icon ? fplinux_ui_glyph(app->ui, icon,
						FPLINUX_UI_GLYPH_ROW) :
			       NULL,
	};

	fplinux_ui_list_add_item_command(screen, row, &command);
}

static void show_error(struct files_app *app, int error)
{
	struct fplinux_ui_screen *alert =
		fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR,
				     _("Files"), error_message(error), 0);

	if (alert)
		fplinux_ui_push(app->ui, alert);
}

static void back_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	(void)screen;
	(void)context;
	if (id == COMMAND_BACK)
		fplinux_ui_go_back(ui);
}

static void show_help(struct files_app *app)
{
	struct fplinux_ui_screen *screen =
		fplinux_ui_form_new(app->ui, _("Help"));

	if (!screen)
		return;
	fplinux_ui_form_append_string(
		screen, NULL,
		_("Open folders and files with the centre key. Back returns to the parent. Left and Right scroll a page."));
	fplinux_ui_form_append_string(
		screen, NULL,
		_("Options offers file actions and Find in folder. Copy and move select a destination first. Existing files are never replaced."));
	fplinux_ui_form_append_string(
		screen, NULL,
		_("My files are in your home directory. Safely eject a memory card from Places before removing it."));
	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(screen, back_command, NULL, app);
	fplinux_ui_navigate(app->ui, screen);
}

static struct fplinux_ui_screen *shown_folder(struct files_folder *folder)
{
	return folder->results_screen ? folder->results_screen : folder->screen;
}

static void pop_to(struct files_app *app, struct fplinux_ui_screen *screen)
{
	fplinux_ui_pop_to(app->ui, screen);
}

static void free_folder(struct files_folder *folder)
{
	if (!folder)
		return;
	fplinux_files_directory_free(&folder->directory);
	if (folder->descriptor >= 0)
		close(folder->descriptor);
	free(folder->rows);
	free(folder->matches);
	free(folder);
}

static void free_folders(struct files_folder *folder)
{
	while (folder) {
		struct files_folder *parent = folder->parent;

		free_folder(folder);
		folder = parent;
	}
}

static void cancel_picker(struct files_app *app)
{
	pop_to(app, shown_folder(app->origin));
	free_folders(app->folder);
	app->folder = app->origin;
	app->origin = NULL;
	app->picker = false;
	app->picker_places = NULL;
	app->name_box = NULL;
	app->source[0] = '\0';
}

static size_t entry_index(struct files_folder *folder,
			  struct fplinux_ui_screen *screen, size_t row)
{
	if (screen == folder->results_screen)
		return row < folder->match_count ? folder->matches[row] :
						   SIZE_MAX;
	return row < folder->count ? folder->rows[row] : SIZE_MAX;
}

static void focused(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		    size_t row, void *context)
{
	struct files_folder *folder = context;
	struct files_app *app = folder->app;
	size_t index = entry_index(folder, screen, row);
	const struct fplinux_files_entry *entry;
	static const int commands[] = { COMMAND_DETAILS, COMMAND_COPY,
					COMMAND_MOVE, COMMAND_RENAME,
					COMMAND_DELETE };

	(void)ui;
	if (folder->command_screen == screen) {
		for (size_t i = 0; i < sizeof(commands) / sizeof(*commands);
		     i++)
			fplinux_ui_screen_remove_item_command(
				folder->command_screen, folder->command_row,
				commands[i]);
	}
	folder->command_screen = screen;
	folder->command_row = row;
	if (app->picker || index == SIZE_MAX)
		return;
	entry = &folder->directory.entries[index];
	add_item(app, screen, row, COMMAND_DETAILS, FPLINUX_UI_COMMAND_ITEM,
		 _("Details"), 10);
	if (entry->kind == FPLINUX_FILES_KIND_FILE && !folder->system)
		add_item(app, screen, row, COMMAND_COPY,
			 FPLINUX_UI_COMMAND_ITEM, _("Copy"), 11);
	if (folder->read_only || folder->system)
		return;
	if (entry->kind == FPLINUX_FILES_KIND_FILE)
		add_item(app, screen, row, COMMAND_MOVE,
			 FPLINUX_UI_COMMAND_ITEM, _("Move"), 12);
	if (entry->kind == FPLINUX_FILES_KIND_FILE ||
	    entry->kind == FPLINUX_FILES_KIND_DIRECTORY) {
		add_item(app, screen, row, COMMAND_RENAME,
			 FPLINUX_UI_COMMAND_ITEM, _("Rename"), 13);
		add_item(app, screen, row, COMMAND_DELETE,
			 FPLINUX_UI_COMMAND_ITEM, _("Delete"), 100);
	}
}

static bool render_row(struct files_folder *folder, size_t index,
		       struct fplinux_ui_virtual_row *out)
{
	const struct fplinux_files_entry *entry;

	if (index == SIZE_MAX)
		return false;
	entry = &folder->directory.entries[index];
	display_text(entry->name, folder->row_label, sizeof(folder->row_label));
	out->text = folder->row_label;
	out->icon = fplinux_ui_glyph(
		folder->app->ui,
		entry->kind == FPLINUX_FILES_KIND_DIRECTORY ? "folder" : "file",
		FPLINUX_UI_GLYPH_ROW);
	return true;
}

static bool folder_row(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       size_t row, struct fplinux_ui_virtual_row *out,
		       void *context)
{
	struct files_folder *folder = context;

	(void)ui;
	(void)screen;
	return render_row(folder,
			  row < folder->count ? folder->rows[row] : SIZE_MAX,
			  out);
}

static bool filtered_row(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, size_t row,
			 struct fplinux_ui_virtual_row *out, void *context)
{
	struct files_folder *folder = context;

	(void)ui;
	(void)screen;
	return render_row(folder,
			  row < folder->match_count ? folder->matches[row] :
						      SIZE_MAX,
			  out);
}

static void attach_folder_commands(struct files_folder *folder,
				   struct fplinux_ui_screen *screen)
{
	struct files_app *app = folder->app;

	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	if (app->picker) {
		const struct fplinux_ui_command choose = {
			.id = COMMAND_PASTE,
			.type = FPLINUX_UI_COMMAND_SCREEN,
			.label = app->move ? _("Move here") : _("Copy here"),
			.short_label = _("Here"),
			.keep_visible = true,
		};

		fplinux_ui_screen_add_command(screen, &choose);
	} else {
		add_command(app, screen, COMMAND_PLACES,
			    FPLINUX_UI_COMMAND_SCREEN, _("Places"), 20);
		add_command(app, screen, COMMAND_REFRESH,
			    FPLINUX_UI_COMMAND_SCREEN, _("Refresh"), 21);
		add_command(app, screen, COMMAND_SEARCH,
			    FPLINUX_UI_COMMAND_SCREEN, _("Find in folder"), 22);
		if (!folder->read_only && !folder->system)
			add_command(app, screen, COMMAND_NEW_FOLDER,
				    FPLINUX_UI_COMMAND_SCREEN, _("New folder"),
				    23);
		add_command(app, screen, COMMAND_HELP, FPLINUX_UI_COMMAND_HELP,
			    NULL, 99);
	}
	fplinux_ui_screen_set_listener(screen, folder_command, NULL, app);
}

static bool build_folder(struct files_folder *folder)
{
	struct files_app *app = folder->app;
	struct fplinux_ui_screen *screen;
	char title[FPLINUX_FILES_NAME_MAX * 4 + 1];
	const char *label =
		!strcmp(folder->path, app->home) ? _("My files") :
		!strcmp(folder->path, "/")	 ? _("System files") :
						   base_name(folder->path);

	free(folder->rows);
	folder->rows =
		calloc(folder->directory.count ? folder->directory.count : 1,
		       sizeof(*folder->rows));
	if (!folder->rows)
		return false;
	folder->count = 0;
	for (size_t i = 0; i < folder->directory.count; i++) {
		if (!app->picker || folder->directory.entries[i].kind ==
					    FPLINUX_FILES_KIND_DIRECTORY)
			folder->rows[folder->count++] = i;
	}
	display_text(label, title, sizeof(title));
	if (folder->count) {
		screen = fplinux_ui_list_new_virtual(app->ui, title,
						     FPLINUX_UI_LIST_IMPLICIT,
						     folder->count, folder_row,
						     folder);
	} else {
		screen = fplinux_ui_form_new(app->ui, title);
		if (screen)
			fplinux_ui_form_append_string(
				screen, NULL,
				app->picker ?
					_("No subfolders. Select this folder with the left key.") :
					_("This folder is empty."));
	}
	if (!screen)
		return false;
	folder->command_screen = NULL;
	folder->results_screen = NULL;
	attach_folder_commands(folder, screen);
	if (folder->count) {
		fplinux_ui_list_set_select_label(screen, _("Open"));
		for (size_t row = 0; row < folder->count; row++) {
			if (!strcmp(folder->directory.entries[folder->rows[row]]
					    .name,
				    folder->focus))
				fplinux_ui_list_set_focused(screen, row);
		}
		fplinux_ui_list_set_focus_listener(screen, focused, folder);
		focused(app->ui, screen, fplinux_ui_list_get_focused(screen),
			folder);
	}
	folder->screen = screen;
	fplinux_ui_replace(app->ui, screen);
	return true;
}

static void start_folder(struct files_app *app, const char *path,
			 const char *root, uint32_t volume_id, bool read_only,
			 bool system, const char *focus)
{
	struct files_folder *folder = calloc(1, sizeof(*folder));
	struct fplinux_ui_screen *screen;
	uint32_t mount_id = 0;

	if (volume_id) {
		for (size_t i = 0; i < app->volumes.count; i++) {
			const struct fplinux_volume *volume =
				&app->volumes.volumes[i];

			if (volume->id == volume_id &&
			    volume->state == FPLINUX_VOLUME_MOUNTED)
				mount_id = volume->mount_id;
		}
		if (!mount_id) {
			free(folder);
			show_error(app, ENODEV);
			return;
		}
	}

	if (!folder) {
		show_error(app, ENOMEM);
		return;
	}
	folder->app = app;
	folder->parent = app->folder;
	folder->descriptor = -1;
	folder->volume_id = volume_id;
	folder->read_only = read_only;
	folder->system = system;
	snprintf(folder->path, sizeof(folder->path), "%s", path);
	snprintf(folder->root, sizeof(folder->root), "%s", root);
	snprintf(folder->focus, sizeof(folder->focus), "%s",
		 focus ? focus : "");
	screen = fplinux_ui_form_new(app->ui, _("Reading folder"));
	if (!screen) {
		free_folder(folder);
		show_error(app, ENOMEM);
		return;
	}
	fplinux_ui_form_append_string(screen, NULL, _("Please wait…"));
	add_command(app, screen, COMMAND_CANCEL_JOB, FPLINUX_UI_COMMAND_CANCEL,
		    NULL, 0);
	fplinux_ui_screen_set_listener(screen, folder_command, NULL, app);
	folder->screen = screen;
	if (fplinux_files_job_read(&app->job, path, !strcmp(path, app->home),
				   mount_id) < 0) {
		fplinux_ui_push(app->ui, screen);
		fplinux_ui_pop(app->ui);
		free_folder(folder);
		show_error(app, errno);
		return;
	}
	app->folder = folder;
	app->reading = true;
	fplinux_ui_push(app->ui, screen);
}

static void refresh_folder(struct files_app *app, const char *focus)
{
	struct files_folder *folder = app->folder;
	char path[FPLINUX_FILES_PATH_MAX];
	char root[FPLINUX_FILES_PATH_MAX];
	uint32_t id = folder->volume_id;
	bool read_only = folder->read_only;
	bool system = folder->system;

	snprintf(path, sizeof(path), "%s", folder->path);
	snprintf(root, sizeof(root), "%s", folder->root);
	pop_to(app, folder->screen);
	fplinux_ui_pop(app->ui);
	app->folder = folder->parent;
	free_folder(folder);
	start_folder(app, path, root, id, read_only, system, focus);
}

static void leave_folder(struct files_app *app)
{
	struct files_folder *folder = app->folder;
	char parent[FPLINUX_FILES_PATH_MAX];
	char root[FPLINUX_FILES_PATH_MAX];
	char focus[FPLINUX_FILES_NAME_MAX + 1];
	uint32_t id = folder->volume_id;
	bool read_only = folder->read_only;
	bool system = folder->system;
	bool load_parent = !folder->parent &&
			   strcmp(folder->path, folder->root);

	if (load_parent) {
		fplinux_files_parent_path(folder->path, parent, sizeof(parent));
		snprintf(root, sizeof(root), "%s", folder->root);
		snprintf(focus, sizeof(focus), "%s", base_name(folder->path));
	}

	pop_to(app, folder->screen);
	fplinux_ui_pop(app->ui);
	app->folder = folder->parent;
	free_folder(folder);
	if (load_parent)
		start_folder(app, parent, root, id, read_only, system, focus);
}

static void format_size(off_t value, char *text, size_t capacity)
{
	if (value < 1024)
		snprintf(text, capacity,
			 ngettext("%jd byte", "%jd bytes",
				  (unsigned long)value),
			 (intmax_t)value);
	else if (value < 1024 * 1024)
		snprintf(text, capacity, _("%.1f KiB"), (double)value / 1024);
	else
		snprintf(text, capacity, _("%.1f MiB"),
			 (double)value / (1024 * 1024));
}

static void show_details(struct files_app *app, size_t index)
{
	struct files_folder *folder = app->folder;
	const struct fplinux_files_entry *entry =
		&folder->directory.entries[index];
	struct fplinux_ui_screen *screen =
		fplinux_ui_form_new(app->ui, _("Details"));
	char name[FPLINUX_FILES_NAME_MAX * 4 + 1];
	char path[FPLINUX_FILES_PATH_MAX];
	char displayed[FPLINUX_FILES_PATH_MAX * 4 + 1];
	char size[64];
	char modified[64];
	struct tm local;
	const char *kind;

	if (!screen)
		return;
	fplinux_files_child_path(folder->path, entry->name, path, sizeof(path));
	display_text(path, displayed, sizeof(displayed));
	display_text(entry->name, name, sizeof(name));
	kind = entry->kind == FPLINUX_FILES_KIND_DIRECTORY ? _("Folder") :
	       entry->kind == FPLINUX_FILES_KIND_FILE	   ? _("File") :
	       entry->kind == FPLINUX_FILES_KIND_SYMLINK ? _("Symbolic link") :
							   _("Special file");
	format_size(entry->size, size, sizeof(size));
	fplinux_ui_form_append_string(screen, _("Name"), name);
	fplinux_ui_form_append_string(screen, _("Type"), kind);
	fplinux_ui_form_append_string(screen, _("Path"), displayed);
	if (entry->kind == FPLINUX_FILES_KIND_FILE)
		fplinux_ui_form_append_string(screen, _("Size"), size);
	if (localtime_r(&entry->modified, &local)) {
		strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &local);
		fplinux_ui_form_append_string(screen, _("Modified"), modified);
	}
	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(screen, back_command, NULL, app);
	fplinux_ui_navigate(app->ui, screen);
}

static void search_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct files_app *app = context;
	struct files_folder *folder = app->folder;
	struct fplinux_ui_screen *results;
	const char *query;

	if (id == COMMAND_BACK) {
		folder->search_screen = NULL;
		fplinux_ui_go_back(ui);
		if (fplinux_ui_current(ui) == folder->screen && folder->count)
			focused(ui, folder->screen,
				fplinux_ui_list_get_focused(folder->screen),
				folder);
		return;
	}
	if (id != COMMAND_FIND)
		return;
	query = fplinux_ui_text_box_get(screen);
	free(folder->matches);
	folder->matches = calloc(folder->count ? folder->count : 1,
				 sizeof(*folder->matches));
	if (!folder->matches) {
		show_error(app, ENOMEM);
		return;
	}
	folder->match_count = 0;
	for (size_t row = 0; row < folder->count; row++) {
		size_t index = folder->rows[row];

		if (fplinux_utf8_contains_casefold(
			    folder->directory.entries[index].name, query))
			folder->matches[folder->match_count++] = index;
	}
	results = folder->match_count ?
			  fplinux_ui_list_new_virtual(ui, _("Search results"),
						      FPLINUX_UI_LIST_IMPLICIT,
						      folder->match_count,
						      filtered_row, folder) :
			  fplinux_ui_form_new(ui, _("Search results"));
	if (!results)
		return;
	folder->results_screen = results;
	if (folder->match_count) {
		attach_folder_commands(folder, results);
		fplinux_ui_screen_remove_command(results, COMMAND_SEARCH);
		fplinux_ui_list_set_focus_listener(results, focused, folder);
		focused(ui, results, 0, folder);
	} else {
		fplinux_ui_form_append_string(
			results, NULL,
			_("No matching names. Go back to change the search."));
		add_command(app, results, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK,
			    NULL, 0);
		fplinux_ui_screen_set_listener(results, folder_command, NULL,
					       app);
	}
	fplinux_ui_push(ui, results);
}

static void show_search(struct files_app *app)
{
	struct fplinux_ui_screen *screen = fplinux_ui_text_box_new(
		app->ui, _("Find in folder"), "", FPLINUX_FILES_NAME_MAX);

	if (!screen)
		return;
	app->folder->search_screen = screen;
	add_command(app, screen, COMMAND_FIND, FPLINUX_UI_COMMAND_SCREEN,
		    _("Find"), 0);
	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(screen, search_command, NULL, app);
	fplinux_ui_navigate(app->ui, screen);
}

static void name_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct files_app *app = context;
	const char *name;
	char destination[FPLINUX_FILES_PATH_MAX];

	if (id == COMMAND_CANCEL) {
		app->name_box = NULL;
		fplinux_ui_go_back(ui);
		return;
	}
	if (id != COMMAND_SAVE_NAME)
		return;
	name = fplinux_ui_text_box_get(screen);
	if (strpbrk(name, "\r\n")) {
		show_error(app, EINVAL);
		return;
	}
	if (fplinux_files_child_path(app->folder->path, name, destination,
				     sizeof(destination)) < 0) {
		show_error(app, errno);
		return;
	}
	if (app->naming == NAMING_DESTINATION) {
		snprintf(app->destination_name, sizeof(app->destination_name),
			 "%s", name);
		snprintf(app->destination, sizeof(app->destination), "%s",
			 destination);
		start_mutation(app,
			       app->move ? FPLINUX_FILES_MOVE :
					   FPLINUX_FILES_COPY,
			       app->source, destination);
	} else {
		start_mutation(
			app,
			app->naming == NAMING_MKDIR ? FPLINUX_FILES_MKDIR :
						      FPLINUX_FILES_RENAME,
			app->naming == NAMING_MKDIR ? NULL : app->selected,
			destination);
	}
}

static void show_name(struct files_app *app, enum naming_action action,
		      const char *raw_name)
{
	struct fplinux_ui_screen *screen;
	char name[FPLINUX_FILES_NAME_MAX * 4 + 1];

	app->naming = action;
	display_text(raw_name ? raw_name : "", name, sizeof(name));
	if (raw_name && strcmp(raw_name, name))
		name[0] = '\0';
	screen = fplinux_ui_text_box_new(
		app->ui,
		action == NAMING_MKDIR	? _("New folder") :
		action == NAMING_RENAME ? _("Rename") :
					  _("Another name"),
		name, FPLINUX_FILES_NAME_MAX);
	if (!screen)
		return;
	app->name_box = screen;
	add_command(app, screen, COMMAND_SAVE_NAME, FPLINUX_UI_COMMAND_SCREEN,
		    action == NAMING_MKDIR ? _("Create") : _("Save"), 0);
	add_command(app, screen, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL,
		    NULL, 0);
	fplinux_ui_screen_set_listener(screen, name_command, NULL, app);
	fplinux_ui_navigate(app->ui, screen);
}

static void delete_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct files_app *app = context;

	(void)ui;
	(void)screen;
	app->decision = NULL;
	if (id == COMMAND_CONFIRM_DELETE)
		start_mutation(app, FPLINUX_FILES_REMOVE, app->selected, NULL);
}

static void confirm_delete(struct files_app *app, const char *raw_name)
{
	char name[FPLINUX_FILES_NAME_MAX * 4 + 1];
	char text[sizeof(name) + 64];
	struct fplinux_ui_screen *screen;

	display_text(raw_name, name, sizeof(name));
	snprintf(text, sizeof(text), _("Delete %s?"), name);
	screen = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_CONFIRMATION,
				      _("Delete"), text, 0);
	if (!screen)
		return;
	app->decision = screen;
	add_command(app, screen, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL,
		    NULL, 0);
	add_command(app, screen, COMMAND_CONFIRM_DELETE, FPLINUX_UI_COMMAND_OK,
		    _("Delete"), 0);
	fplinux_ui_screen_set_listener(screen, delete_command, NULL, app);
	fplinux_ui_navigate(app->ui, screen);
}

static void busy_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct files_app *app = context;

	(void)ui;
	(void)screen;
	app->busy = NULL;
	if (id != COMMAND_CANCEL_JOB)
		return;
	fplinux_files_job_cancel(&app->job);
	app->busy = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO,
					 _("Stopping"), _("Please wait…"), 0);
	if (app->busy) {
		fplinux_ui_alert_set_progress(app->busy, 0, 0);
		fplinux_ui_push(app->ui, app->busy);
	}
}

static void show_busy(struct files_app *app)
{
	const char *title =
		app->job.action == FPLINUX_FILES_COPY ? _("Copying") :
		app->job.action == FPLINUX_FILES_MOVE ? _("Moving") :
							_("Working");
	char name[96];

	display_text(base_name(app->job.source[0] ? app->job.source :
						    app->job.destination),
		     name, sizeof(name));
	app->busy = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_INFO, title,
					 name, 0);
	if (!app->busy)
		return;
	fplinux_ui_alert_set_progress(app->busy, 0, 0);
	add_command(app, app->busy, COMMAND_CANCEL_JOB,
		    FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	fplinux_ui_screen_set_listener(app->busy, busy_command, NULL, app);
	fplinux_ui_push(app->ui, app->busy);
}

static void start_mutation(struct files_app *app,
			   enum fplinux_files_action action, const char *source,
			   const char *destination)
{
	if (fplinux_files_job_mutate(&app->job, action, source, destination) <
	    0) {
		show_error(app, errno);
		return;
	}
	app->reading = false;
	show_busy(app);
}

static void operation_error_command(struct fplinux_ui *ui,
				    struct fplinux_ui_screen *screen, int id,
				    void *context)
{
	struct files_app *app = context;

	(void)ui;
	(void)screen;
	app->decision = NULL;
	if (id == COMMAND_RETRY) {
		if (fplinux_files_job_retry(&app->job) < 0)
			show_error(app, errno);
		else
			show_busy(app);
	} else if (id == COMMAND_OTHER_NAME) {
		if (app->name_box)
			pop_to(app, app->name_box);
		else
			show_name(app, NAMING_DESTINATION,
				  app->destination_name);
	} else {
		fplinux_files_job_reset(&app->job);
		if (app->picker && id == COMMAND_CANCEL)
			cancel_picker(app);
		else if (app->folder && !app->name_box)
			refresh_folder(app, NULL);
	}
}

static void show_operation_error(struct files_app *app, int error)
{
	struct fplinux_files_operation *operation = &app->job.operation;
	const char *text = error_message(error);
	const char *title = _("Operation failed");
	bool published = operation->result.state != FPLINUX_STORAGE_UNPUBLISHED;
	struct fplinux_ui_screen *screen;

	if (published) {
		title = _("Finishing not confirmed");
		if (operation->source_state ==
		    FPLINUX_FILES_SOURCE_REMOVED_UNSYNCED)
			text = _(
				"The source is no longer visible. Its removal has not been synchronized. Retry to finish.");
		else if (operation->action == FPLINUX_FILES_MOVE &&
			 operation->result.state == FPLINUX_STORAGE_DURABLE)
			text = _(
				"The file was copied. The source was not removed. Retry to finish the move.");
		else if (operation->action == FPLINUX_FILES_MOVE)
			text = _(
				"The copy is visible but not synchronized. The source was kept. Retry to finish.");
		else
			text = _(
				"The change is visible but not synchronized. Retry to finish.");
	}
	screen = fplinux_ui_alert_new(app->ui, FPLINUX_UI_ALERT_ERROR, title,
				      text, 0);
	if (!screen)
		return;
	app->decision = screen;
	if (published)
		add_command(app, screen, COMMAND_RETRY, FPLINUX_UI_COMMAND_OK,
			    _("Retry"), 0);
	else if (app->picker && error == EEXIST)
		add_command(app, screen, COMMAND_OTHER_NAME,
			    FPLINUX_UI_COMMAND_OK, _("Another name"), 0);
	add_command(app, screen, COMMAND_CANCEL, FPLINUX_UI_COMMAND_CANCEL,
		    NULL, 0);
	fplinux_ui_screen_set_listener(screen, operation_error_command, NULL,
				       app);
	fplinux_ui_push(app->ui, screen);
}

static void job_event(struct fplinux_ui *ui, int fd, short events,
		      void *context)
{
	struct files_app *app = context;
	struct fplinux_files_job_status status =
		fplinux_files_job_collect(&app->job);

	(void)fd;
	(void)events;
	if (!status.done) {
		if (app->busy) {
			char text[160], bytes[64], total[64];
			char name[FPLINUX_FILES_NAME_MAX * 4 + 1];

			display_text(base_name(app->job.source[0] ?
						       app->job.source :
						       app->job.destination),
				     name, sizeof(name));
			if (status.syncing) {
				fplinux_ui_alert_set_text(
					app->busy, _("Finishing writes…"));
				fplinux_ui_alert_set_progress(app->busy, 0, 0);
				fplinux_ui_alert_set_footer(app->busy, NULL);
			} else if (status.total > 0) {
				int percentage =
					status.bytes >= status.total ?
						100 :
						(int)((long double)status.bytes *
						      100 / status.total);

				int progress =
					status.bytes >= status.total ?
						INT_MAX :
						(int)((long double)status.bytes *
						      INT_MAX / status.total);

				if (status.bytes > 0 && !progress)
					progress = 1;
				format_size(status.bytes, bytes, sizeof(bytes));
				format_size(status.total, total, sizeof(total));
				snprintf(text, sizeof(text),
					 _("%d%% · %s / %s"), percentage, bytes,
					 total);
				fplinux_ui_alert_set_text(app->busy, name);
				fplinux_ui_alert_set_footer(app->busy, text);
				fplinux_ui_alert_set_progress(
					app->busy, progress, INT_MAX);
			}
		}
		return;
	}
	if (app->closing && app->decision) {
		pop_to(app, app->decision);
		fplinux_ui_pop(ui);
		app->decision = NULL;
	}
	if (app->busy) {
		pop_to(app, app->busy);
		fplinux_ui_pop(ui);
		app->busy = NULL;
	}
	if (app->stop_and_close) {
		app->closing = false;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
		return;
	}
	if (app->closing) {
		app->closing = false;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_CANCEL);
	}
	if (app->reading) {
		struct files_folder *folder = app->folder;

		app->reading = false;
		if (status.error) {
			leave_folder(app);
			fplinux_files_job_reset(&app->job);
			if (status.error != ECANCELED)
				show_error(app, status.error);
			request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
			return;
		}
		folder->directory = app->job.directory;
		app->job.directory = (struct fplinux_files_directory){ 0 };
		folder->descriptor = app->job.directory_fd;
		app->job.directory_fd = -1;
		if (!build_folder(folder)) {
			leave_folder(app);
			show_error(app, ENOMEM);
		}
		return;
	}
	if (status.error) {
		show_operation_error(app, status.error);
		request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
		return;
	}
	app->name_box = NULL;
	if (app->picker) {
		struct files_folder *folder = app->folder;
		char path[FPLINUX_FILES_PATH_MAX];
		char root[FPLINUX_FILES_PATH_MAX];
		char focus[FPLINUX_FILES_NAME_MAX + 1];
		uint32_t id = folder->volume_id;

		snprintf(path, sizeof(path), "%s", folder->path);
		snprintf(root, sizeof(root), "%s", folder->root);
		snprintf(focus, sizeof(focus), "%s", app->destination_name);
		app->picker = false;
		show_places(app);
		start_folder(app, path, root, id, false, false, focus);
	} else {
		char focus[FPLINUX_FILES_NAME_MAX + 1];

		snprintf(focus, sizeof(focus), "%s",
			 app->job.operation.destination_name);
		refresh_folder(app, focus);
	}
}

static const struct fplinux_volume *volume_at(struct files_app *app, size_t row)
{
	if (!row)
		return NULL;
	row--;
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];

		if (app->picker && (volume->state != FPLINUX_VOLUME_MOUNTED ||
				    volume->read_only))
			continue;
		if (!row--)
			return volume;
	}
	return NULL;
}

static void request_storage(struct files_app *app,
			    enum fplinux_volume_operation operation,
			    uint32_t id)
{
	if (app->storage_request)
		return;
	app->storage_request =
		fplinux_ui_storage_request(app->ui, operation, id);
	app->storage_operation = operation;
	if (!app->storage_request && errno != EOPNOTSUPP)
		show_error(app, errno);
}

static void places_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct files_app *app = context;
	size_t row = fplinux_ui_list_get_focused(screen);
	const struct fplinux_volume *volume = volume_at(app, row);

	if (id == COMMAND_BACK) {
		if (app->picker)
			cancel_picker(app);
		else
			fplinux_ui_back(ui);
	} else if (id == COMMAND_HELP) {
		show_help(app);
	} else if (id == COMMAND_REFRESH) {
		request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
	} else if (id == COMMAND_SYSTEM && !app->picker) {
		start_folder(app, "/", "/", 0, true, true, NULL);
	} else if ((id == COMMAND_EJECT || id == COMMAND_CONNECT) && volume) {
		request_storage(app,
				id == COMMAND_EJECT ? FPLINUX_VOLUME_EJECT :
						      FPLINUX_VOLUME_CONNECT,
				volume->id);
	} else if (id == FPLINUX_UI_SELECT_COMMAND) {
		if (!row) {
			start_folder(app, app->home, app->home, 0, false, false,
				     NULL);
		} else if (volume) {
			if (volume->state == FPLINUX_VOLUME_MOUNTED)
				start_folder(app, volume->path, volume->path,
					     volume->id, volume->read_only,
					     false, NULL);
			else if (volume->state == FPLINUX_VOLUME_AVAILABLE)
				request_storage(app, FPLINUX_VOLUME_CONNECT,
						volume->id);
			else
				show_error(app, EOPNOTSUPP);
		}
	}
}

static void places_focused(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, size_t row,
			   void *context)
{
	struct files_app *app = context;
	const struct fplinux_volume *volume = volume_at(app, row);

	(void)ui;
	for (size_t i = 0; i < fplinux_ui_list_size(screen); i++) {
		fplinux_ui_screen_remove_item_command(screen, i,
						      COMMAND_CONNECT);
		fplinux_ui_screen_remove_item_command(screen, i, COMMAND_EJECT);
	}
	if (!app->picker && volume && volume->state == FPLINUX_VOLUME_MOUNTED)
		add_item(app, screen, row, COMMAND_EJECT,
			 FPLINUX_UI_COMMAND_ITEM, _("Safely eject"), 0);
}

static struct fplinux_ui_screen *new_places(struct files_app *app)
{
	struct fplinux_ui_screen *screen = fplinux_ui_list_new(
		app->ui,
		app->picker ? (app->move ? _("Move to") : _("Copy to")) :
			      _("Files"),
		FPLINUX_UI_LIST_IMPLICIT);

	if (!screen)
		return NULL;
	fplinux_ui_list_append(screen, _("My files"),
			       app->volumes.home_persistent ?
				       _("Home directory") :
				       _("Temporary: lost after reboot"));
	fplinux_ui_list_set_icon(screen, 0,
				 fplinux_ui_glyph(app->ui, "folder",
						  FPLINUX_UI_GLYPH_ROW),
				 FPLINUX_UI_LIST_ICON_IMAGE);
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];
		const char *detail;
		size_t row;

		if (app->picker && (volume->state != FPLINUX_VOLUME_MOUNTED ||
				    volume->read_only))
			continue;
		detail = volume->state == FPLINUX_VOLUME_AVAILABLE ?
				 _("Select to connect") :
			 volume->state == FPLINUX_VOLUME_UNSUPPORTED ?
				 _("Unsupported format") :
			 volume->state == FPLINUX_VOLUME_PROTECTED ?
				 _("Used by the system") :
			 volume->read_only ? _("Read-only") :
					     _("Connected");
		row = fplinux_ui_list_append(screen, _("Memory card"), detail);
		fplinux_ui_list_set_icon(screen, row,
					 fplinux_ui_glyph(app->ui, "card",
							  FPLINUX_UI_GLYPH_ROW),
					 FPLINUX_UI_LIST_ICON_IMAGE);
	}
	if (!app->picker) {
		add_command(app, screen, COMMAND_SYSTEM,
			    FPLINUX_UI_COMMAND_SCREEN, _("System files"), 11);
		add_command(app, screen, COMMAND_REFRESH,
			    FPLINUX_UI_COMMAND_SCREEN, _("Refresh"), 10);
		add_command(app, screen, COMMAND_HELP, FPLINUX_UI_COMMAND_HELP,
			    NULL, 20);
	}
	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK,
		    app->picker ? _("Cancel") : NULL, 0);
	fplinux_ui_list_set_select_label(screen, _("Open"));
	fplinux_ui_screen_set_listener(screen, places_command, NULL, app);
	fplinux_ui_list_set_focus_listener(screen, places_focused, app);
	return screen;
}

static void show_places(struct files_app *app)
{
	struct fplinux_ui_screen *screen;

	if (app->places)
		pop_to(app, app->places);
	free_folders(app->folder);
	free_folders(app->origin);
	app->folder = NULL;
	app->origin = NULL;
	app->name_box = NULL;
	app->picker_places = NULL;
	app->picker = false;
	fplinux_files_job_reset(&app->job);
	screen = new_places(app);
	if (!screen)
		return;
	app->places = screen;
	if (fplinux_ui_current(app->ui))
		fplinux_ui_replace(app->ui, screen);
	else
		fplinux_ui_push(app->ui, screen);
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *result,
			   void *context)
{
	struct files_app *app = context;
	struct fplinux_ui_screen *screen;

	if (result->request_id != app->storage_request)
		return;
	app->storage_request = 0;
	app->volumes = result->snapshot;
	if (app->folder && app->folder->volume_id && !app->job.active) {
		bool present = false;

		for (size_t i = 0; i < app->volumes.count; i++) {
			if (app->volumes.volumes[i].id ==
				    app->folder->volume_id &&
			    app->volumes.volumes[i].state ==
				    FPLINUX_VOLUME_MOUNTED)
				present = true;
		}
		if (!present) {
			show_places(app);
			show_error(app, ENODEV);
			return;
		}
	}
	if (!app->folder &&
	    fplinux_ui_current(ui) ==
		    (app->picker ? app->picker_places : app->places)) {
		size_t row =
			fplinux_ui_list_get_focused(fplinux_ui_current(ui));

		screen = new_places(app);
		if (screen) {
			if (app->picker)
				app->picker_places = screen;
			else
				app->places = screen;
			fplinux_ui_list_set_focused(screen, row);
			places_focused(ui, screen,
				       fplinux_ui_list_get_focused(screen),
				       app);
			fplinux_ui_replace(ui, screen);
		}
	}
	if (result->error && result->error != EOPNOTSUPP)
		show_error(app, result->error);
}

static void folder_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct files_app *app = context;
	struct files_folder *folder = app->folder;
	size_t row;
	size_t index;
	const struct fplinux_files_entry *entry;
	char path[FPLINUX_FILES_PATH_MAX];

	if (id == COMMAND_CANCEL_JOB) {
		fplinux_files_job_cancel(&app->job);
		return;
	}
	if (app->job.active)
		return;
	if (id == COMMAND_BACK) {
		if (screen == folder->results_screen) {
			folder->results_screen = NULL;
			folder->command_screen = NULL;
			fplinux_ui_go_back(ui);
			return;
		}
		leave_folder(app);
		return;
	}
	if (id == COMMAND_PLACES) {
		show_places(app);
		return;
	}
	if (id == COMMAND_REFRESH) {
		refresh_folder(app, NULL);
		request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
		return;
	}
	if (id == COMMAND_HELP) {
		show_help(app);
		return;
	}
	if (id == COMMAND_SEARCH) {
		show_search(app);
		return;
	}
	if (id == COMMAND_NEW_FOLDER) {
		show_name(app, NAMING_MKDIR, NULL);
		return;
	}
	if (id == COMMAND_PASTE) {
		if (fplinux_files_child_path(
			    folder->path, app->destination_name,
			    app->destination, sizeof(app->destination)) < 0)
			show_error(app, errno);
		else
			start_mutation(app,
				       app->move ? FPLINUX_FILES_MOVE :
						   FPLINUX_FILES_COPY,
				       app->source, app->destination);
		return;
	}
	row = fplinux_ui_screen_get_command_item(screen);
	if (row == SIZE_MAX)
		row = fplinux_ui_list_get_focused(screen);
	index = entry_index(folder, screen, row);
	if (index == SIZE_MAX)
		return;
	entry = &folder->directory.entries[index];
	if (fplinux_files_child_path(folder->path, entry->name, path,
				     sizeof(path)) < 0) {
		show_error(app, errno);
		return;
	}
	switch (id) {
	case FPLINUX_UI_SELECT_COMMAND:
		if (entry->kind == FPLINUX_FILES_KIND_DIRECTORY)
			start_folder(app, path, folder->root, folder->volume_id,
				     folder->read_only, folder->system, NULL);
		else if (entry->kind == FPLINUX_FILES_KIND_FILE) {
			if (!app->document_request) {
				app->document_request =
					fplinux_ui_open_file(ui, path);
				if (!app->document_request)
					show_error(app, errno);
			}
		} else
			show_details(app, index);
		break;
	case COMMAND_DETAILS:
		show_details(app, index);
		break;
	case COMMAND_RENAME:
		snprintf(app->selected, sizeof(app->selected), "%s", path);
		show_name(app, NAMING_RENAME, entry->name);
		break;
	case COMMAND_DELETE:
		snprintf(app->selected, sizeof(app->selected), "%s", path);
		confirm_delete(app, entry->name);
		break;
	case COMMAND_COPY:
	case COMMAND_MOVE:
		app->move = id == COMMAND_MOVE;
		app->picker = true;
		snprintf(app->source, sizeof(app->source), "%s", path);
		snprintf(app->destination_name, sizeof(app->destination_name),
			 "%s", entry->name);
		app->origin = folder;
		app->folder = NULL;
		app->picker_places = new_places(app);
		if (app->picker_places)
			fplinux_ui_push(ui, app->picker_places);
		else {
			app->folder = app->origin;
			app->origin = NULL;
			app->picker = false;
		}
		break;
	default:
		break;
	}
}

static void close_command(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, int id,
			  void *context)
{
	struct files_app *app = context;

	(void)screen;
	app->decision = NULL;
	if (id == COMMAND_CLOSE_STOP) {
		app->stop_and_close = true;
		fplinux_files_job_cancel(&app->job);
	} else {
		app->closing = false;
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_CANCEL);
	}
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct files_app *app = context;
	struct fplinux_ui_screen *screen;

	if (!app->job.active)
		return FPLINUX_UI_CLOSE_READY;
	if (app->closing)
		return FPLINUX_UI_CLOSE_WAIT;
	screen = fplinux_ui_alert_new(
		ui, FPLINUX_UI_ALERT_CONFIRMATION, _("Close Files?"),
		_("An operation is running. Stop it and close Files?"), 0);
	if (!screen)
		return FPLINUX_UI_CLOSE_CANCEL;
	app->closing = true;
	app->decision = screen;
	add_command(app, screen, COMMAND_CLOSE_STOP, FPLINUX_UI_COMMAND_OK,
		    _("Stop and close"), 0);
	add_command(app, screen, COMMAND_CLOSE_CONTINUE,
		    FPLINUX_UI_COMMAND_CANCEL, _("Continue"), 0);
	fplinux_ui_screen_set_listener(screen, close_command, NULL, app);
	fplinux_ui_push(ui, screen);
	return FPLINUX_UI_CLOSE_WAIT;
}

static bool has_pending_search(const struct files_folder *folder)
{
	for (; folder; folder = folder->parent) {
		if (folder->search_screen)
			return true;
	}
	return false;
}

static void app_event(struct fplinux_ui *ui,
		      const struct fplinux_ui_event *event, void *context)
{
	struct files_app *app = context;

	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		fplinux_files_job_cancel(&app->job);
		fplinux_ui_quit(ui);
	} else if (event->type == FPLINUX_UI_EVENT_ACTIVATION) {
		if (event->activation == FPLINUX_UI_ACTIVATION_OPEN &&
		    !app->job.active && !app->name_box && !app->decision &&
		    !app->picker && !app->document_request &&
		    !has_pending_search(app->folder))
			show_places(app);
		request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
	} else if (event->type == FPLINUX_UI_EVENT_DOCUMENT_RESULT &&
		   event->document_result.request_id == app->document_request &&
		   event->document_result.status != FPLINUX_UI_DOCUMENT_WAIT) {
		app->document_request = 0;
		if (event->document_result.status ==
		    FPLINUX_UI_DOCUMENT_ERROR) {
			struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
				ui, FPLINUX_UI_ALERT_ERROR, _("Open file"),
				event->document_result.error == ENOEXEC ?
					_("No application can open this file type.") :
				event->document_result.error == EUSERS ?
					_("Too many applications are open. Close an application and try again.") :
				event->document_result.error == ENOENT ||
						event->document_result.error ==
							ENODEV ?
					_("The file or storage is no longer available. Refresh Places.") :
					_("The application could not open this file."),
				0);

			if (alert)
				fplinux_ui_push(ui, alert);
		}
	}
}

int main(int argc, char **argv)
{
	struct files_app app = { 0 };
	struct fplinux_cli cli = {
		.program = "fplinux-files",
		.description =
			"Browse local folders and open files with installed applications.",
	};
	const char *home;
	int result = fplinux_cli_parse(&cli, argc, argv);

	if (result != FPLINUX_CLI_READY)
		return result;
	home = getenv("HOME");
	if (!home || fplinux_files_child_path(home, "Files", app.home,
					      sizeof(app.home)) < 0) {
		fputs("fplinux-files: an absolute HOME directory is required\n",
		      stderr);
		return 1;
	}
	if (fplinux_files_job_init(&app.job) < 0) {
		perror("fplinux-files: worker");
		return 1;
	}
	app.ui = fplinux_ui_open(APP_ID, app_event, &app);
	if (!app.ui) {
		perror("fplinux-files: open UI");
		fplinux_files_job_close(&app.job);
		return 1;
	}
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	fplinux_ui_set_storage_handler(app.ui, storage_result, &app);
	fplinux_ui_watch_fd(app.ui, app.job.pipe[0], POLLIN, job_event, &app);
	show_places(&app);
	request_storage(&app, FPLINUX_VOLUME_REFRESH, 0);
	result = fplinux_ui_run(app.ui);
	fplinux_ui_watch_fd(app.ui, -1, 0, NULL, NULL);
	fplinux_files_job_close(&app.job);
	fplinux_ui_close(app.ui);
	free_folders(app.folder);
	free_folders(app.origin);
	return result < 0;
}
