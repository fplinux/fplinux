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

/* Where a folder appears once it has been read. */
enum folder_target {
	/* Above the current screen: a subfolder or a place. */
	FOLDER_CHILD,
	/* Instead of the current folder: its parent, after Back. */
	FOLDER_PARENT,
	/* Instead of the current folder, read again; Find results close. */
	FOLDER_REFRESH,
	/* Instead of the current folder after its change; Find stays. */
	FOLDER_CHANGED,
	/* After copy or move: return to the source and keep Find. */
	FOLDER_RETURN,
};

/* A list's focus by name, else by row, and its first row in view. */
struct list_position {
	char name[FPLINUX_FILES_NAME_MAX + 1];
	size_t row;
	size_t first;
};

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
	/* Where its list opens; SIZE_MAX rows keep the list's default. */
	struct list_position position;
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
	/* The wait's busy alert, until the end of the wait. */
	struct fplinux_ui_screen *busy;
	struct fplinux_ui_screen *name_box;
	struct fplinux_ui_screen *decision;
	struct files_folder *folder;
	struct files_folder *origin;
	/* The folder being read, shown at its target when the wait ends. */
	struct files_folder *pending;
	enum folder_target target;
	/* Presents the work's outcome when its wait ends. */
	void (*outcome)(struct files_app *app);
	/* The error that the outcome reports, or zero. */
	int error;
	struct fplinux_files_job job;
	struct fplinux_volume_snapshot volumes;
	/* The rows in the current Places list, including while it is covered. */
	struct fplinux_volume_snapshot places_volumes;
	lv_timer_t *places_update;
	uint32_t storage_request;
	enum fplinux_volume_operation storage_operation;
	uint32_t document_request;
	bool picker;
	bool move;
	bool closing;
	bool stop_and_close;
	bool reading;
	/* From fplinux_ui_wait_begin() until the outcome. */
	bool waiting;
	/* Cancelled work: the busy alert says Stopping and ignores progress. */
	bool stopping;
	/* The pending storage request ends the wait. */
	bool storage_waiting;
	/* A failed card read waits for its storage snapshot before reporting. */
	bool read_storage_waiting;
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
static void refresh_places(struct files_app *app);
static bool places_current(const struct files_app *app);
static void refresh_places_later(lv_timer_t *timer);
static void refresh_folder(struct files_app *app);
static void show_name(struct files_app *app, enum naming_action action,
		      const char *raw_name);
static void start_mutation(struct files_app *app,
			   enum fplinux_files_action action, const char *source,
			   const char *destination);
static bool request_storage(struct files_app *app,
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
		return _("The file or storage is no longer available.");
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

/* Cancel during work stops it; the alert, once shown, says so. */
static void stop_work(struct files_app *app)
{
	fplinux_files_job_cancel(&app->job);
	if (app->reading || !app->busy || app->stopping)
		return;
	app->stopping = true;
	fplinux_ui_screen_set_title(app->busy, _("Stopping"));
	fplinux_ui_alert_set_text(app->busy, _("Please wait…"));
	fplinux_ui_alert_set_footer(app->busy, NULL);
	fplinux_ui_alert_set_progress(app->busy, 0, 0);
}

static void cancel_work(struct fplinux_ui *ui, void *context)
{
	struct files_app *app = context;

	(void)ui;
	/* The change is complete: only the folder that shows it is read. */
	if (app->reading &&
	    (app->target == FOLDER_CHANGED || app->target == FOLDER_RETURN))
		return;
	stop_work(app);
}

/*
 * The current screen stays while the work runs; its outcome appears in one
 * step when the wait ends, after a busy alert only for longer work.
 */
static bool begin_wait(struct files_app *app, const char *title,
		       const char *text, bool cancellable)
{
	app->busy = fplinux_ui_wait_begin(
		app->ui, title, text, cancellable ? cancel_work : NULL, app);
	if (!app->busy)
		return false;
	app->waiting = true;
	app->stopping = false;
	return true;
}

static void wait_ended(struct fplinux_ui *ui, void *context)
{
	struct files_app *app = context;
	void (*outcome)(struct files_app *app) = app->outcome;

	(void)ui;
	app->waiting = false;
	app->outcome = NULL;
	if (outcome)
		outcome(app);
}

static void end_wait(struct files_app *app,
		     void (*outcome)(struct files_app *app))
{
	app->busy = NULL;
	app->outcome = outcome;
	if (!fplinux_ui_wait_end(app->ui, wait_ended, app))
		wait_ended(app->ui, app);
}

static void show_wait_error(struct files_app *app)
{
	if (app->error)
		show_error(app, app->error);
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
	add_item(app, screen, row, COMMAND_DETAILS,
		 entry->kind == FPLINUX_FILES_KIND_FILE ||
				 entry->kind == FPLINUX_FILES_KIND_DIRECTORY ?
			 FPLINUX_UI_COMMAND_ITEM :
			 FPLINUX_UI_COMMAND_OK,
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

/* Focuses the saved name, else the saved row: a deleted row's neighbour. */
static void restore_position(struct files_folder *folder,
			     struct fplinux_ui_screen *screen,
			     const size_t *entries, size_t count,
			     const struct list_position *position)
{
	size_t focus = SIZE_MAX;

	for (size_t row = 0; row < count && focus == SIZE_MAX; row++) {
		if (!strcmp(folder->directory.entries[entries[row]].name,
			    position->name))
			focus = row;
	}
	if (focus == SIZE_MAX && position->row != SIZE_MAX)
		focus = position->row < count ? position->row : count - 1;
	if (focus != SIZE_MAX)
		fplinux_ui_list_set_focused(screen, focus);
	if (position->first != SIZE_MAX)
		fplinux_ui_list_set_first_visible(screen, position->first);
}

/* Keeps a given name, else takes the focused one, with the list's view. */
static void save_position(struct files_folder *folder,
			  struct fplinux_ui_screen *screen,
			  struct list_position *position)
{
	size_t row = fplinux_ui_list_get_focused(screen);
	size_t index = entry_index(folder, screen, row);

	if (!position->name[0] && index != SIZE_MAX)
		snprintf(position->name, sizeof(position->name), "%s",
			 folder->directory.entries[index].name);
	position->row = row;
	position->first = fplinux_ui_list_get_first_visible(screen);
}

static struct fplinux_ui_screen *build_folder(struct files_folder *folder)
{
	struct files_app *app = folder->app;
	struct fplinux_ui_screen *screen;
	char title[FPLINUX_FILES_NAME_MAX * 4 + 1];
	const char *label =
		!strcmp(folder->path, app->home) ? _("My files") :
		!strcmp(folder->path, "/")	 ? _("System files") :
		folder->volume_id && !strcmp(folder->path, folder->root) ?
					     _("Memory card") :
					     base_name(folder->path);

	free(folder->rows);
	folder->rows =
		calloc(folder->directory.count ? folder->directory.count : 1,
		       sizeof(*folder->rows));
	if (!folder->rows)
		return NULL;
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
		return NULL;
	folder->command_screen = NULL;
	folder->results_screen = NULL;
	attach_folder_commands(folder, screen);
	if (folder->count) {
		fplinux_ui_list_set_select_label(screen, _("Open"));
		restore_position(folder, screen, folder->rows, folder->count,
				 &folder->position);
		fplinux_ui_list_set_focus_listener(screen, focused, folder);
		focused(app->ui, screen, fplinux_ui_list_get_focused(screen),
			folder);
	}
	folder->screen = screen;
	return screen;
}

static const struct fplinux_volume *mounted_volume(const struct files_app *app,
						   uint32_t id)
{
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];

		if (volume->id == id && volume->state == FPLINUX_VOLUME_MOUNTED)
			return volume;
	}
	return NULL;
}

/* Starts reading path for target; the current wait presents the folder. */
static bool read_folder(struct files_app *app, const char *path,
			const char *root, uint32_t volume_id, bool read_only,
			bool system, const char *focus,
			enum folder_target target)
{
	struct files_folder *folder;
	uint32_t mount_id = 0;

	if (volume_id) {
		const struct fplinux_volume *volume =
			mounted_volume(app, volume_id);

		if (volume)
			mount_id = volume->mount_id;
		if (!mount_id) {
			errno = ENODEV;
			return false;
		}
	}
	folder = calloc(1, sizeof(*folder));
	if (!folder)
		return false;
	folder->app = app;
	folder->descriptor = -1;
	folder->volume_id = volume_id;
	folder->read_only = read_only;
	folder->system = system;
	snprintf(folder->path, sizeof(folder->path), "%s", path);
	snprintf(folder->root, sizeof(folder->root), "%s", root);
	snprintf(folder->position.name, sizeof(folder->position.name), "%s",
		 focus ? focus : "");
	folder->position.row = SIZE_MAX;
	folder->position.first = SIZE_MAX;
	if (fplinux_files_job_read(&app->job, path, !strcmp(path, app->home),
				   mount_id) < 0) {
		int error = errno;

		free_folder(folder);
		errno = error;
		return false;
	}
	app->pending = folder;
	app->target = target;
	app->reading = true;
	return true;
}

static void open_folder(struct files_app *app, const char *path,
			const char *root, uint32_t volume_id, bool read_only,
			bool system, const char *focus,
			enum folder_target target)
{
	if (!begin_wait(app, _("Reading folder"), _("Please wait…"), true)) {
		show_error(app, errno);
		return;
	}
	if (!read_folder(app, path, root, volume_id, read_only, system, focus,
			 target)) {
		app->error = errno;
		end_wait(app, show_wait_error);
	}
}

static void refresh_folder(struct files_app *app)
{
	struct files_folder *folder = app->folder;

	open_folder(app, folder->path, folder->root, folder->volume_id,
		    folder->read_only, folder->system, NULL, FOLDER_REFRESH);
}

static void leave_folder(struct files_app *app)
{
	struct files_folder *folder = app->folder;
	char parent[FPLINUX_FILES_PATH_MAX];

	/* A folder opened without its parents reads the parent when left. */
	if (!folder->parent && strcmp(folder->path, folder->root)) {
		fplinux_files_parent_path(folder->path, parent, sizeof(parent));
		open_folder(app, parent, folder->root, folder->volume_id,
			    folder->read_only, folder->system,
			    base_name(folder->path), FOLDER_PARENT);
		return;
	}
	pop_to(app, folder->screen);
	fplinux_ui_pop(app->ui);
	app->folder = folder->parent;
	free_folder(folder);
	if (!app->folder)
		refresh_places(app);
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

/* The folder's rows matching query; position, if given, opens the list. */
static struct fplinux_ui_screen *
build_results(struct files_folder *folder, const char *query,
	      const struct list_position *position)
{
	struct files_app *app = folder->app;
	struct fplinux_ui_screen *results;

	free(folder->matches);
	folder->matches = calloc(folder->count ? folder->count : 1,
				 sizeof(*folder->matches));
	if (!folder->matches)
		return NULL;
	folder->match_count = 0;
	for (size_t row = 0; row < folder->count; row++) {
		size_t index = folder->rows[row];

		if (fplinux_utf8_contains_casefold(
			    folder->directory.entries[index].name, query))
			folder->matches[folder->match_count++] = index;
	}
	results = folder->match_count ?
			  fplinux_ui_list_new_virtual(app->ui,
						      _("Search results"),
						      FPLINUX_UI_LIST_IMPLICIT,
						      folder->match_count,
						      filtered_row, folder) :
			  fplinux_ui_form_new(app->ui, _("Search results"));
	if (!results)
		return NULL;
	folder->results_screen = results;
	if (folder->match_count) {
		attach_folder_commands(folder, results);
		fplinux_ui_screen_remove_command(results, COMMAND_SEARCH);
		if (position)
			restore_position(folder, results, folder->matches,
					 folder->match_count, position);
		fplinux_ui_list_set_focus_listener(results, focused, folder);
		focused(app->ui, results, fplinux_ui_list_get_focused(results),
			folder);
	} else {
		fplinux_ui_form_append_string(
			results, NULL,
			_("No matching names. Go back to change the search."));
		add_command(app, results, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK,
			    NULL, 0);
		fplinux_ui_screen_set_listener(results, folder_command, NULL,
					       app);
	}
	return results;
}

static void search_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct files_app *app = context;
	struct files_folder *folder = app->folder;
	struct fplinux_ui_screen *results;

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
	results = build_results(folder, fplinux_ui_text_box_get(screen), NULL);
	if (results)
		fplinux_ui_push(ui, results);
	else
		show_error(app, ENOMEM);
}

static struct fplinux_ui_screen *new_search_box(struct files_app *app,
						const char *query)
{
	struct fplinux_ui_screen *screen = fplinux_ui_text_box_new(
		app->ui, _("Find in folder"), query, FPLINUX_FILES_NAME_MAX);

	if (!screen)
		return NULL;
	app->folder->search_screen = screen;
	add_command(app, screen, COMMAND_FIND, FPLINUX_UI_COMMAND_SCREEN,
		    _("Find"), 0);
	add_command(app, screen, COMMAND_BACK, FPLINUX_UI_COMMAND_BACK, NULL,
		    0);
	fplinux_ui_screen_set_listener(screen, search_command, NULL, app);
	return screen;
}

static void show_search(struct files_app *app)
{
	struct fplinux_ui_screen *screen = new_search_box(app, "");

	if (screen)
		fplinux_ui_navigate(app->ui, screen);
}

/* After a change from Search results: the same query over the new rows. */
static void show_search_again(struct files_app *app, const char *query,
			      const struct list_position *position)
{
	struct fplinux_ui_screen *search = new_search_box(app, query);
	struct fplinux_ui_screen *results;

	if (!search)
		return;
	fplinux_ui_push(app->ui, search);
	results = build_results(app->folder, query, position);
	if (results)
		fplinux_ui_push(app->ui, results);
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

static bool begin_busy(struct files_app *app, enum fplinux_files_action action,
		       const char *source, const char *destination)
{
	const char *title = action == FPLINUX_FILES_COPY ? _("Copying") :
			    action == FPLINUX_FILES_MOVE ? _("Moving") :
							   _("Working");
	char name[96];

	display_text(base_name(source && source[0] ? source : destination),
		     name, sizeof(name));
	return begin_wait(app, title, name, true);
}

static void start_mutation(struct files_app *app,
			   enum fplinux_files_action action, const char *source,
			   const char *destination)
{
	if (!begin_busy(app, action, source, destination)) {
		show_error(app, errno);
		return;
	}
	if (fplinux_files_job_mutate(&app->job, action, source, destination) <
	    0) {
		app->error = errno;
		end_wait(app, show_wait_error);
		return;
	}
	app->reading = false;
}

static void operation_error_command(struct fplinux_ui *ui,
				    struct fplinux_ui_screen *screen, int id,
				    void *context)
{
	struct files_app *app = context;
	bool published = app->job.operation.result.state !=
			 FPLINUX_STORAGE_UNPUBLISHED;

	(void)ui;
	(void)screen;
	app->decision = NULL;
	if (id == COMMAND_RETRY) {
		if (!begin_busy(app, app->job.action, app->job.source,
				app->job.destination)) {
			show_error(app, errno);
		} else if (fplinux_files_job_retry(&app->job) < 0) {
			app->error = errno;
			end_wait(app, show_wait_error);
		} else {
			app->reading = false;
		}
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
		/* An unpublished failure changed nothing to read again. */
		else if (published && app->folder && !app->name_box)
			refresh_folder(app);
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

static void show_failed_operation(struct files_app *app)
{
	show_operation_error(app, app->error);
}

/* Stopping on request leaves nothing to report. */
static void show_cancelled_operation(struct files_app *app)
{
	fplinux_files_job_reset(&app->job);
	if (app->picker)
		cancel_picker(app);
}

static void show_read_error(struct files_app *app)
{
	if (app->target == FOLDER_RETURN)
		cancel_picker(app);
	if (app->folder && app->folder->volume_id &&
	    !mounted_volume(app, app->folder->volume_id))
		show_places(app);
	if (!app->folder)
		refresh_places(app);
	show_wait_error(app);
}

static void show_read_folder(struct files_app *app)
{
	struct files_folder *folder = app->pending;
	struct files_folder *old = app->folder;
	struct fplinux_ui_screen *screen;
	struct list_position results = { .row = SIZE_MAX, .first = SIZE_MAX };
	char query[FPLINUX_FILES_NAME_MAX * 4 + 1] = "";
	bool keep_search = false;

	app->pending = NULL;
	if (app->target == FOLDER_RETURN) {
		cancel_picker(app);
		old = app->folder;
	}
	if (app->target == FOLDER_REFRESH || app->target == FOLDER_CHANGED ||
	    app->target == FOLDER_RETURN) {
		keep_search = app->target != FOLDER_REFRESH &&
			      old->search_screen && old->results_screen;
		if (keep_search) {
			snprintf(query, sizeof(query), "%s",
				 fplinux_ui_text_box_get(old->search_screen));
			snprintf(results.name, sizeof(results.name), "%s",
				 folder->position.name);
			save_position(old, old->results_screen, &results);
		}
		save_position(old, old->screen, &folder->position);
	}
	folder->parent = app->target == FOLDER_CHILD ? old :
			 old			     ? old->parent :
						       NULL;
	screen = build_folder(folder);
	if (!screen) {
		free_folder(folder);
		show_error(app, ENOMEM);
		return;
	}
	app->folder = folder;
	if (!old || app->target == FOLDER_CHILD) {
		fplinux_ui_push(app->ui, screen);
		return;
	}
	pop_to(app, old->screen);
	fplinux_ui_replace(app->ui, screen);
	free_folder(old);
	app->name_box = NULL;
	if (keep_search)
		show_search_again(app, query, &results);
}

static void read_finished(struct files_app *app, int error)
{
	struct files_folder *folder = app->pending;

	app->reading = false;
	if (error) {
		bool card = folder->volume_id != 0;

		app->pending = NULL;
		free_folder(folder);
		fplinux_files_job_reset(&app->job);
		app->error = error == ECANCELED ? 0 : error;
		if (card && error != ECANCELED) {
			app->read_storage_waiting = true;
			if (app->storage_request ||
			    request_storage(app, FPLINUX_VOLUME_REFRESH, 0))
				return;
			app->read_storage_waiting = false;
		}
		end_wait(app, show_read_error);
		if (!card && error != ECANCELED)
			request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
		return;
	}
	folder->directory = app->job.directory;
	app->job.directory = (struct fplinux_files_directory){ 0 };
	folder->descriptor = app->job.directory_fd;
	app->job.directory_fd = -1;
	end_wait(app, show_read_folder);
}

static void mutation_finished(struct files_app *app, int error)
{
	const struct fplinux_files_operation *operation = &app->job.operation;
	struct files_folder *folder = app->picker ? app->origin : app->folder;
	bool stopped = error == ECANCELED &&
		       operation->result.state == FPLINUX_STORAGE_UNPUBLISHED;

	if (error) {
		app->error = error;
		end_wait(app, stopped ? show_cancelled_operation :
					show_failed_operation);
		if (!stopped)
			request_storage(app, FPLINUX_VOLUME_REFRESH, 0);
		return;
	}
	/* The same wait goes on until the changed folder is shown. */
	if (app->picker ?
		    read_folder(app, folder->path, folder->root,
				folder->volume_id, folder->read_only,
				folder->system, base_name(app->source),
				FOLDER_RETURN) :
		    read_folder(app, folder->path, folder->root,
				folder->volume_id, folder->read_only,
				folder->system, operation->destination_name,
				FOLDER_CHANGED))
		return;
	app->error = errno;
	app->target = app->picker ? FOLDER_RETURN : FOLDER_CHANGED;
	end_wait(app, show_read_error);
}

static void show_progress(struct files_app *app,
			  const struct fplinux_files_job_status *status)
{
	char text[160], bytes[64], total[64];
	char name[FPLINUX_FILES_NAME_MAX * 4 + 1];

	display_text(base_name(app->job.source[0] ? app->job.source :
						    app->job.destination),
		     name, sizeof(name));
	if (status->syncing) {
		fplinux_ui_alert_set_text(app->busy, _("Finishing writes…"));
		fplinux_ui_alert_set_progress(app->busy, 0, 0);
		fplinux_ui_alert_set_footer(app->busy, NULL);
	} else if (status->total > 0) {
		int percentage = status->bytes >= status->total ?
					 100 :
					 (int)((long double)status->bytes *
					       100 / status->total);

		int progress = status->bytes >= status->total ?
				       INT_MAX :
				       (int)((long double)status->bytes *
					     INT_MAX / status->total);

		if (status->bytes > 0 && !progress)
			progress = 1;
		format_size(status->bytes, bytes, sizeof(bytes));
		format_size(status->total, total, sizeof(total));
		snprintf(text, sizeof(text), _("%d%% · %s / %s"), percentage,
			 bytes, total);
		fplinux_ui_alert_set_text(app->busy, name);
		fplinux_ui_alert_set_footer(app->busy, text);
		fplinux_ui_alert_set_progress(app->busy, progress, INT_MAX);
	}
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
		if (app->busy && !app->reading && !app->stopping)
			show_progress(app, &status);
		return;
	}
	if (app->stop_and_close) {
		app->closing = false;
		end_wait(app, NULL);
		fplinux_ui_close_reply(ui, FPLINUX_UI_CLOSE_READY);
		return;
	}
	if (app->reading)
		read_finished(app, status.error);
	else
		mutation_finished(app, status.error);
}

/* The system's own cards can be neither opened nor connected nor ejected. */
static bool shows_volume(const struct files_app *app,
			 const struct fplinux_volume *volume)
{
	if (volume->state == FPLINUX_VOLUME_PROTECTED)
		return false;
	return !app->picker ||
	       (volume->state == FPLINUX_VOLUME_MOUNTED && !volume->read_only);
}

static const struct fplinux_volume *
place_volume(const struct files_app *app,
	     const struct fplinux_volume_snapshot *snapshot, size_t row)
{
	if (!row)
		return NULL;
	row--;
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct fplinux_volume *volume = &snapshot->volumes[i];

		if (!shows_volume(app, volume))
			continue;
		if (!row--)
			return volume;
	}
	return NULL;
}

static const struct fplinux_volume *volume_at(struct files_app *app, size_t row)
{
	return place_volume(app, &app->places_volumes, row);
}

static size_t place_row(struct files_app *app, uint32_t id)
{
	const struct fplinux_volume *volume;

	for (size_t row = 1; (volume = volume_at(app, row)); row++) {
		if (volume->id == id)
			return row;
	}
	return 0;
}

/* My files lie on the memory card that holds the system, if it does. */
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

static const char *volume_detail(const struct fplinux_volume *volume)
{
	return volume->state == FPLINUX_VOLUME_AVAILABLE ?
		       _("Select to connect") :
	       volume->state == FPLINUX_VOLUME_UNSUPPORTED ?
		       _("Unsupported format") :
	       volume->read_only ? _("Read-only") :
				   _("Connected");
}

static bool request_storage(struct files_app *app,
			    enum fplinux_volume_operation operation,
			    uint32_t id)
{
	if (app->storage_request)
		return false;
	app->storage_request =
		fplinux_ui_storage_request(app->ui, operation, id);
	app->storage_operation = operation;
	if (!app->storage_request && errno != EOPNOTSUPP)
		show_error(app, errno);
	return app->storage_request != 0;
}

/* Connecting and ejecting keep Places until the session answers. */
static void change_volume(struct files_app *app,
			  enum fplinux_volume_operation operation, uint32_t id)
{
	if (!request_storage(app, operation, id))
		return;
	app->storage_waiting = begin_wait(app, _("Memory card"),
					  operation == FPLINUX_VOLUME_EJECT ?
						  _("Ejecting…") :
						  _("Connecting…"),
					  false);
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
		open_folder(app, "/", "/", 0, true, true, NULL, FOLDER_CHILD);
	} else if ((id == COMMAND_EJECT || id == COMMAND_CONNECT) && volume) {
		change_volume(app,
			      id == COMMAND_EJECT ? FPLINUX_VOLUME_EJECT :
						    FPLINUX_VOLUME_CONNECT,
			      volume->id);
	} else if (id == FPLINUX_UI_SELECT_COMMAND) {
		if (!row) {
			open_folder(app, app->home, app->home, 0, false, false,
				    NULL, FOLDER_CHILD);
		} else if (volume) {
			if (volume->state == FPLINUX_VOLUME_MOUNTED)
				open_folder(app, volume->path, volume->path,
					    volume->id, volume->read_only,
					    false, NULL, FOLDER_CHILD);
			else if (volume->state == FPLINUX_VOLUME_AVAILABLE)
				change_volume(app, FPLINUX_VOLUME_CONNECT,
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
	if (app->picker || !volume)
		return;
	/* The row's own OK command replaces Open on a card to connect. */
	if (volume->state == FPLINUX_VOLUME_AVAILABLE)
		add_item(app, screen, row, COMMAND_CONNECT,
			 FPLINUX_UI_COMMAND_OK, _("Connect"), 0);
	else if (volume->state == FPLINUX_VOLUME_MOUNTED)
		add_item(app, screen, row, COMMAND_EJECT,
			 FPLINUX_UI_COMMAND_ITEM, _("Safely eject"), 0);
}

static void set_place(struct files_app *app, struct fplinux_ui_screen *screen,
		      size_t row, const char *text, const char *detail,
		      const char *icon)
{
	if (row < fplinux_ui_list_size(screen)) {
		fplinux_ui_list_set(screen, row, text, detail);
		return;
	}
	fplinux_ui_list_append(screen, text, detail);
	fplinux_ui_list_set_icon(screen, row,
				 fplinux_ui_glyph(app->ui, icon,
						  FPLINUX_UI_GLYPH_ROW),
				 FPLINUX_UI_LIST_ICON_IMAGE);
}

static void fill_places(struct files_app *app, struct fplinux_ui_screen *screen)
{
	size_t row = 1;

	set_place(app, screen, 0, _("My files"), home_detail(&app->volumes),
		  "folder");
	for (size_t i = 0; i < app->volumes.count; i++) {
		const struct fplinux_volume *volume = &app->volumes.volumes[i];

		if (!shows_volume(app, volume))
			continue;
		set_place(app, screen, row++, _("Memory card"),
			  volume_detail(volume), "card");
	}
	while (fplinux_ui_list_size(screen) > row)
		fplinux_ui_list_delete(screen, row);
	app->places_volumes = app->volumes;
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
	fill_places(app, screen);
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
	const struct fplinux_volume *volume =
		app->folder ?
			NULL :
			volume_at(app,
				  fplinux_ui_list_get_focused(app->places));
	uint32_t focus = app->folder ? app->folder->volume_id :
			 volume	     ? volume->id :
				       0;

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
	fplinux_ui_list_set_focused(screen, place_row(app, focus));
	places_focused(app->ui, screen, fplinux_ui_list_get_focused(screen),
		       app);
	if (fplinux_ui_current(app->ui))
		fplinux_ui_replace(app->ui, screen);
	else
		fplinux_ui_push(app->ui, screen);
}

static bool places_unchanged(struct files_app *app)
{
	if (strcmp(home_detail(&app->places_volumes),
		   home_detail(&app->volumes)))
		return false;
	for (size_t row = 1;; row++) {
		const struct fplinux_volume *old = volume_at(app, row);
		const struct fplinux_volume *next =
			place_volume(app, &app->volumes, row);

		if (!old || !next)
			return old == next;
		if (old->id != next->id ||
		    strcmp(volume_detail(old), volume_detail(next)))
			return false;
	}
}

/* Keep the screen and menu; focus follows the volume ID or the old row. */
static void refresh_places(struct files_app *app)
{
	struct fplinux_ui_screen *screen = app->picker ? app->picker_places :
							 app->places;
	size_t row = fplinux_ui_list_get_focused(screen);
	const struct fplinux_volume *volume = volume_at(app, row);
	uint32_t focus = volume ? volume->id : 0;
	size_t first = fplinux_ui_list_get_first_visible(screen);
	size_t selected;

	if (!screen)
		return;
	if (places_unchanged(app)) {
		app->places_volumes = app->volumes;
		return;
	}
	/* Item-command changes would dismiss a menu still using those rows. */
	if (fplinux_ui_menu_is_open(app->ui)) {
		if (!app->places_update)
			app->places_update =
				lv_timer_create(refresh_places_later, 25, app);
		return;
	}
	fill_places(app, screen);
	selected = place_row(app, focus);
	if (!selected)
		selected = row < fplinux_ui_list_size(screen) ?
				   row :
				   fplinux_ui_list_size(screen) - 1;
	fplinux_ui_list_set_focused(screen, selected);
	fplinux_ui_list_set_first_visible(screen, first);
	places_focused(app->ui, screen, fplinux_ui_list_get_focused(screen),
		       app);
}

static bool places_current(const struct files_app *app)
{
	return !app->folder &&
	       fplinux_ui_current(app->ui) ==
		       (app->picker ? app->picker_places : app->places);
}

static void refresh_places_later(lv_timer_t *timer)
{
	struct files_app *app = lv_timer_get_user_data(timer);

	if (fplinux_ui_menu_is_open(app->ui))
		return;
	app->places_update = NULL;
	lv_timer_delete(timer);
	refresh_places(app);
}

static void show_volume_result(struct files_app *app)
{
	if (places_current(app))
		refresh_places(app);
	show_wait_error(app);
}

static void storage_result(struct fplinux_ui *ui,
			   const struct fplinux_volume_result *result,
			   void *context)
{
	struct files_app *app = context;

	(void)ui;
	if (result->request_id != app->storage_request)
		return;
	app->storage_request = 0;
	app->volumes = result->snapshot;
	if (app->read_storage_waiting) {
		app->read_storage_waiting = false;
		end_wait(app, show_read_error);
		return;
	}
	if (app->storage_waiting) {
		app->storage_waiting = false;
		app->error = result->error == EOPNOTSUPP ? 0 : result->error;
		end_wait(app, show_volume_result);
		return;
	}
	/* A wait keeps its screens until its own outcome. */
	if (app->waiting)
		return;
	if (app->folder && app->folder->volume_id && !app->job.active) {
		if (!mounted_volume(app, app->folder->volume_id)) {
			show_places(app);
			show_error(app, ENODEV);
			return;
		}
	}
	if (places_current(app))
		refresh_places(app);
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

	if (app->job.active || app->waiting)
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
		refresh_folder(app);
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
			open_folder(app, path, folder->root, folder->volume_id,
				    folder->read_only, folder->system, NULL,
				    FOLDER_CHILD);
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
	if (id == COMMAND_CLOSE_STOP && app->job.active) {
		app->stop_and_close = true;
		stop_work(app);
	} else {
		/* Work that ended while the question was open needs no stop. */
		app->closing = false;
		fplinux_ui_close_reply(ui, id == COMMAND_CLOSE_STOP ?
						   FPLINUX_UI_CLOSE_READY :
						   FPLINUX_UI_CLOSE_CANCEL);
	}
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct files_app *app = context;
	struct fplinux_ui_screen *screen;

	/* Reading a folder changes nothing: it stops as Files closes. */
	if (!app->job.active || app->reading)
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
		    !app->job.active && !app->waiting && !app->name_box &&
		    !app->decision && !app->picker && !app->document_request &&
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
					_("The file or storage is no longer available.") :
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
	if (app.places_update)
		lv_timer_delete(app.places_update);
	fplinux_ui_close(app.ui);
	free_folders(app.folder);
	free_folders(app.origin);
	free_folder(app.pending);
	return result < 0;
}
