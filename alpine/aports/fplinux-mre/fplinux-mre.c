/* SPDX-License-Identifier: 0BSD */
#define _GNU_SOURCE
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-text.h>
#include <fplinux/fplinux-keypad.h>
#include <fplinux/fplinux-multitap.h>
#include "fplinux-cli.h"

#include "mre-ipc.h"
#include "text-codec.h"
#include "http-broker.h"
#include "runtime-paths.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <libintl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define UI_ID "org.fplinux.mre"
#define _(text) gettext(text)
#define ENGINE "/usr/libexec/fplinux/mre-engine"
#define PROTOCOL_LIMIT 16384U
#define SEND_LIMIT 65536U
#define DIRECTORY_LIMIT 4096U
#define VFS_REQUEST_LIMIT 1000000U
#define HTTP_REQUEST_LIMIT 100000U
#define MRE_KEY_EVENT_DOWN 2
#define MRE_KEY_EVENT_UP 1
#define MRE_PEN_EVENT_TAP 1
#define MRE_PEN_EVENT_RELEASE 2
#define POINTER_STEP_PX 16
#define POINTER_SIZE_PX 9

enum command {
	CMD_BACK = 1,
	CMD_REPLACE,
	CMD_ACCEPT,
	CMD_RIGHT,
	CMD_CANCEL,
	CMD_UP,
	CMD_CARD,
	CMD_PRIVATE
};
enum view {
	VIEW_IDLE,
	VIEW_WAIT,
	VIEW_INPUT,
	VIEW_ERROR,
	VIEW_SELECTOR,
	VIEW_NATIVE
};

struct directory_entry {
	char name[NAME_MAX + 1];
	bool directory;
};

struct app {
	struct fplinux_ui *ui;
	struct fplinux_ui_screen *screen;
	struct fplinux_ui_text *field;
	lv_obj_t *canvas, *pointer;
	uint16_t *frame_pixels;
	uint16_t *display_pixels;
	int display_width, display_height;
	struct mre_shared *shared;
	lv_timer_t *tick;
	enum view view;
	struct directory_entry *entries;
	size_t entry_count;
	char filename[NAME_MAX + 1];
	char selector_folder[MRE_VFS_PATH_BYTES];
	int selector_type;
	bool modal;
	char protocol[PROTOCOL_LIMIT + 1];
	size_t protocol_used;
	char outgoing[SEND_LIMIT];
	size_t outgoing_used;
	pid_t child, validator;
	uint64_t validation_started;
	enum view deferred_modal;
	int input_fd, output_fd;
	uint64_t transport_failure_at;
	bool saw_end, closing, native;
	int state_root_fd;
	struct mre_document pending_document;
	uint32_t document_id;
	bool replacing, close_pending;
	struct fplinux_ui_screen *decision;
	struct mre_vfs *vfs;
	uint32_t vfs_sequence;
	struct mre_http_broker *http;
	uint32_t http_active_id, http_last_id, http_requests;
	bool picture_requested, picture_retained, has_frame;
	bool viewer_visible, pointer_available, pointer_mode;
	bool pointer_toggle_held, pointer_fire_held, pointer_fire_withheld;
	unsigned int pointer_arrows_withheld, native_keys_held;
	int pointer_x, pointer_y;
};

static void show_idle(struct app *app);
static void show_error(struct app *app, const char *message);
static void show_selector(struct app *app);
static bool show_native(struct app *app);
static bool send_line(struct app *app, const char *line);
static void accept_document_ui(struct app *app);
static void start_engine_document(struct app *app);
static void finish_document(struct app *app,
			    enum fplinux_ui_document_status status, int error);
static void remove_decision(struct app *app);
static void restore_guest_ui(struct app *app);
static void screen_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context);

static void update_tick(struct app *app)
{
	bool active = app->validator > 0 || app->child > 0 ||
		      app->input_fd >= 0 || app->output_fd >= 0 ||
		      app->outgoing_used || app->protocol_used || app->http ||
		      app->http_active_id || app->transport_failure_at;

	if (!app->tick)
		return;
	if (!active) {
		lv_timer_pause(app->tick);
	} else if (lv_timer_get_paused(app->tick)) {
		lv_timer_resume(app->tick);
		lv_timer_reset(app->tick);
	}
}

static void clear_entries(struct app *app)
{
	free(app->entries);
	app->entries = NULL;
	app->entry_count = 0;
}

static bool shortcut_key(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen,
			 const struct fplinux_ui_key *key, void *context)
{
	(void)screen;
	(void)context;
	if (key->code != KEY_HOME)
		return false;
	if (key->pressed)
		fplinux_ui_home(ui);
	return true;
}

static void replace_screen(struct app *app, struct fplinux_ui_screen *screen,
			   enum view view)
{
	if (app->view == VIEW_NATIVE && view != VIEW_NATIVE) {
		app->pointer_toggle_held = false;
		app->pointer_fire_withheld = false;
		app->pointer_arrows_withheld = 0;
		app->native_keys_held = 0;
	}
	app->field = NULL;
	app->canvas = app->pointer = NULL;
	app->screen = screen;
	app->view = view;
	fplinux_ui_screen_set_listener(screen, screen_command, NULL, app);
	fplinux_ui_screen_set_key_filter(screen, shortcut_key, NULL);
	if (fplinux_ui_current(app->ui))
		fplinux_ui_replace(app->ui, screen);
	else
		fplinux_ui_push(app->ui, screen);
}

static void add_command(struct fplinux_ui_screen *screen, int id,
			enum fplinux_ui_command_type type, const char *label,
			int priority)
{
	const struct fplinux_ui_command command = {
		.id = id,
		.type = type,
		.label = label,
		.priority = priority,
	};
	fplinux_ui_screen_add_command(screen, &command);
}

static void close_pipe(struct app *app)
{
	fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
	if (app->output_fd >= 0)
		close(app->output_fd);
	if (app->input_fd >= 0)
		close(app->input_fd);
	app->output_fd = app->input_fd = -1;
	app->outgoing_used = app->protocol_used = 0;
	app->transport_failure_at = 0;
}

static void reset_pointer(struct app *app)
{
	app->pointer_available = app->pointer_mode = false;
	app->pointer_toggle_held = app->pointer_fire_held = false;
	app->pointer_fire_withheld = false;
	app->pointer_arrows_withheld = app->native_keys_held = 0;
	app->pointer_x = MRE_FRAME_WIDTH / 2;
	app->pointer_y = MRE_FRAME_HEIGHT / 2;
	if (app->pointer)
		lv_obj_set_hidden(app->pointer, true);
}

static void release_runtime(struct app *app)
{
	if (app->field) {
		fplinux_ui_text_set_change_cb(app->field, NULL, NULL);
		app->field = NULL;
		fplinux_ui_canvas_set_key_handler(app->screen, NULL, NULL);
		fplinux_ui_screen_remove_command(app->screen, CMD_ACCEPT);
		fplinux_ui_screen_remove_command(app->screen, CMD_CANCEL);
		fplinux_ui_screen_remove_command(app->screen, CMD_RIGHT);
	}
	close_pipe(app);
	mre_http_broker_destroy(app->http);
	app->http = NULL;
	mre_vfs_destroy(app->vfs);
	app->vfs = NULL;
	if (app->shared)
		munmap(app->shared, sizeof(*app->shared));
	app->shared = NULL;
	app->vfs_sequence = app->http_active_id = app->http_last_id = 0;
	app->http_requests = 0;
	app->native = app->modal = false;
	app->deferred_modal = VIEW_IDLE;
	app->picture_requested = app->picture_retained = app->has_frame = false;
	reset_pointer(app);
	update_tick(app);
}

static void stop_engine(struct app *app)
{
	if (app->child > 0) {
		pid_t result;
		do {
			result = waitpid(app->child, NULL, WNOHANG);
		} while (result < 0 && errno == EINTR);
		/* A still-owned, unreaped child cannot have its PID reused. */
		if (result == 0) {
			kill(app->child, SIGKILL);
			while (waitpid(app->child, NULL, 0) < 0 &&
			       errno == EINTR) {
			}
		}
		app->child = 0;
	}
	release_runtime(app);
}

static void show_error(struct app *app, const char *message)
{
	struct fplinux_ui_screen *screen;
	/* message may point into the shared mapping being released. */
	char copy[PROTOCOL_LIMIT / 2U + 1U];
	snprintf(copy, sizeof(copy), "%s", message);
	remove_decision(app);
	finish_document(app, FPLINUX_UI_DOCUMENT_ERROR, EIO);
	stop_engine(app);
	fplinux_ui_set_fullscreen(app->ui, false);
	screen = fplinux_ui_form_new(app->ui, _("Application error"));
	if (!screen) {
		fplinux_ui_quit(app->ui);
		return;
	}
	fplinux_ui_form_append_string(screen, app->filename, copy);
	add_command(screen, CMD_BACK, FPLINUX_UI_COMMAND_BACK, NULL, 0);
	replace_screen(app, screen, VIEW_ERROR);
}

static void show_wait(struct app *app)
{
	struct fplinux_ui_screen *screen =
		fplinux_ui_form_new(app->ui, app->filename);
	if (!screen) {
		show_error(app, _("Not enough memory."));
		return;
	}
	fplinux_ui_form_append_string(screen, NULL, _("Opening…"));
	add_command(screen, CMD_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	replace_screen(app, screen, VIEW_WAIT);
}

static bool valid_name(const char *name)
{
	size_t bytes = strlen(name);
	if (!bytes || bytes > NAME_MAX || !strcmp(name, ".") ||
	    !strcmp(name, "..") || strpbrk(name, "\\/:"))
		return false;
	for (size_t i = 0; i < bytes; ++i)
		if ((unsigned char)name[i] < 32U ||
		    (unsigned char)name[i] == 127U)
			return false;
	return mre_utf8_decode((const unsigned char *)name, bytes, NULL,
			       NAME_MAX + 1U, NULL);
}

static bool suffix(const char *name, const char *extension)
{
	size_t bytes = strlen(name), tail = strlen(extension);
	return bytes > tail && !strcasecmp(name + bytes - tail, extension);
}

static bool image_name(const char *name)
{
	return suffix(name, ".jpg") || suffix(name, ".jpeg") ||
	       suffix(name, ".png") || suffix(name, ".bmp") ||
	       suffix(name, ".gif") || suffix(name, ".abm");
}

static int entry_compare(const void *a, const void *b)
{
	const struct directory_entry *first = a, *second = b;
	if (first->directory != second->directory)
		return first->directory ? -1 : 1;
	return strcasecmp(first->name, second->name);
}

/* Only plain directory entries become rows; the broker revalidates every
 * selected path before an operation. No filename grants guest authority. */
static int read_entries(struct app *app, int fd)
{
	DIR *folder;
	struct dirent *entry;
	int error = 0;
	clear_entries(app);
	if (fd < 0)
		return -1;
	folder = fdopendir(fd);
	if (!folder) {
		close(fd);
		return -1;
	}
	int folder_fd = dirfd(folder);
	if (folder_fd < 0) {
		int saved_error = errno;
		closedir(folder);
		errno = saved_error;
		return -1;
	}
	for (;;) {
		errno = 0;
		entry = readdir(folder);
		if (!entry) {
			error = errno;
			break;
		}
		struct stat st;
		const char *name = entry->d_name;
		bool directory;
		if (!valid_name(name) ||
		    !mre_vfs_entry_visible(app->vfs, folder_fd, name) ||
		    !strncmp(name, ".mre-vfs-", 9) ||
		    fstatat(folder_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
			continue;
		directory = S_ISDIR(st.st_mode);
		if (!directory) {
			if (!S_ISREG(st.st_mode) ||
			    (app->selector_type == 1 && !image_name(name)) ||
			    (app->selector_type == 2 && !suffix(name, ".txt")))
				continue;
		}
		if (app->entry_count == DIRECTORY_LIMIT) {
			error = E2BIG;
			break;
		}
		struct directory_entry *grown = realloc(
			app->entries, (app->entry_count + 1U) * sizeof(*grown));
		if (!grown) {
			error = ENOMEM;
			break;
		}
		app->entries = grown;
		snprintf(grown[app->entry_count].name, NAME_MAX + 1, "%s",
			 name);
		grown[app->entry_count++].directory = directory;
	}
	closedir(folder);
	if (error) {
		clear_entries(app);
		errno = error;
		return -1;
	}
	qsort(app->entries, app->entry_count, sizeof(*app->entries),
	      entry_compare);
	return 0;
}

static bool directory_row(struct fplinux_ui *ui,
			  struct fplinux_ui_screen *screen, size_t index,
			  struct fplinux_ui_virtual_row *row, void *context)
{
	struct app *app = context;
	(void)screen;
	if (index >= app->entry_count)
		return false;
	row->text = app->entries[index].name;
	row->icon = fplinux_ui_glyph(
		ui, app->entries[index].directory ? "folder" : "file",
		FPLINUX_UI_GLYPH_ROW);
	return true;
}

static void show_idle(struct app *app)
{
	struct fplinux_ui_screen *screen;

	fplinux_ui_set_fullscreen(app->ui, false);
	screen = fplinux_ui_form_new(app->ui, "MRE");
	if (!screen) {
		fplinux_ui_quit(app->ui);
		return;
	}
	fplinux_ui_form_append_string(screen, NULL,
				      _("Open a VXP file from Files."));
	add_command(screen, CMD_BACK, FPLINUX_UI_COMMAND_BACK, NULL, 0);
	replace_screen(app, screen, VIEW_IDLE);
}

static bool flush_outgoing(struct app *app)
{
	while (app->outgoing_used && app->input_fd >= 0) {
		ssize_t count =
			write(app->input_fd, app->outgoing, app->outgoing_used);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return true;
			if (errno == EPIPE) {
				/* Drain END and reap before classifying a
                                 * final ACK racing with ordinary child exit. */
				close(app->input_fd);
				app->input_fd = -1;
				app->outgoing_used = 0;
				app->transport_failure_at = fplinux_ui_now_ms();
				return true;
			}
			return false;
		}
		if (!count)
			return false;
		memmove(app->outgoing, app->outgoing + count,
			app->outgoing_used - (size_t)count);
		app->outgoing_used -= (size_t)count;
	}
	return !app->outgoing_used;
}

static bool send_line(struct app *app, const char *line)
{
	size_t length = strlen(line);
	if (app->input_fd < 0 || length > PIPE_BUF ||
	    length > SEND_LIMIT - app->outgoing_used)
		return false;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	memcpy(app->outgoing + app->outgoing_used, line, length);
	app->outgoing_used += length;
	return flush_outgoing(app);
}

static bool parse_number(const char *text, uint32_t *number)
{
	char *end;
	unsigned long value;
	if (!text[0])
		return false;
	for (const char *cursor = text; *cursor; ++cursor)
		if (*cursor < '0' || *cursor > '9')
			return false;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno || *end || value > UINT32_MAX)
		return false;
	*number = (uint32_t)value;
	return true;
}

static int hex_digit(unsigned char byte)
{
	if (byte >= '0' && byte <= '9')
		return byte - '0';
	if (byte >= 'a' && byte <= 'f')
		return byte - 'a' + 10;
	if (byte >= 'A' && byte <= 'F')
		return byte - 'A' + 10;
	return -1;
}

static bool decode_text(const char *hex, char *text, size_t capacity)
{
	size_t length = strlen(hex);
	if (length % 2U || length / 2U >= capacity)
		return false;
	for (size_t i = 0; i < length / 2U; ++i) {
		int high = hex_digit((unsigned char)hex[i * 2U]);
		int low = hex_digit((unsigned char)hex[i * 2U + 1U]);
		if (high < 0 || low < 0 || !(high | low))
			return false;
		text[i] = (char)((high << 4) | low);
	}
	text[length / 2U] = '\0';
	return mre_utf8_decode((const unsigned char *)text, length / 2U, NULL,
			       capacity, NULL);
}

static bool send_path(struct app *app, const char *command, const char *path)
{
	static const char digits[] = "0123456789abcdef";
	char line[MRE_VFS_PATH_BYTES * 2U + 32U];
	size_t used = (size_t)snprintf(line, sizeof(line), "%s\t", command);
	size_t bytes = strlen(path);
	if (bytes >= MRE_VFS_PATH_BYTES)
		return false;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char byte = (unsigned char)path[i];
		line[used++] = digits[byte >> 4];
		line[used++] = digits[byte & 15U];
	}
	line[used++] = '\n';
	line[used] = '\0';
	return send_line(app, line);
}

struct frame_viewport {
	int x, y, width, height;
};

static struct frame_viewport frame_viewport(struct app *app)
{
	int width = fplinux_ui_width(app->ui);
	int height = width * MRE_FRAME_HEIGHT / MRE_FRAME_WIDTH;

	if (height > fplinux_ui_height(app->ui)) {
		height = fplinux_ui_height(app->ui);
		width = height * MRE_FRAME_WIDTH / MRE_FRAME_HEIGHT;
	}
	return (struct frame_viewport){
		(fplinux_ui_width(app->ui) - width) / 2,
		(fplinux_ui_height(app->ui) - height) / 2,
		width,
		height,
	};
}

static void layout_pointer(struct app *app)
{
	if (!app->pointer)
		return;
	struct frame_viewport viewport = frame_viewport(app);
	lv_obj_set_pos(
		app->pointer,
		viewport.x + app->pointer_x * viewport.width / MRE_FRAME_WIDTH -
			POINTER_SIZE_PX / 2,
		viewport.y +
			app->pointer_y * viewport.height / MRE_FRAME_HEIGHT -
			POINTER_SIZE_PX / 2);
	lv_obj_set_hidden(app->pointer,
			  !app->pointer_available || !app->pointer_mode ||
				  !fplinux_ui_is_fullscreen(app->ui));
}

static bool send_pen(struct app *app, int event)
{
	char line[64];

	snprintf(line, sizeof(line), "PEN\t%d\t%d\t%d\n", event, app->pointer_x,
		 app->pointer_y);
	if (send_line(app, line))
		return true;
	show_error(app, _("The application did not respond."));
	return false;
}

static bool pause_pointer(struct app *app)
{
	app->pointer_mode = false;
	layout_pointer(app);
	if (!app->pointer_fire_held)
		return true;
	app->pointer_fire_held = false;
	return send_pen(app, MRE_PEN_EVENT_RELEASE);
}

static bool pointer_key(struct app *app, const struct fplinux_ui_key *key,
			int code)
{
	unsigned int bit;

	if (app->view != VIEW_NATIVE)
		return false;
	if (key->source == FPLINUX_UI_KEY_PHONE &&
	    key->code == FPLINUX_KEY_POUND &&
	    (app->pointer_available || app->pointer_toggle_held)) {
		if (!key->pressed) {
			app->pointer_toggle_held = false;
		} else if (!app->pointer_toggle_held) {
			app->pointer_toggle_held = true;
			if (app->pointer_mode)
				pause_pointer(app);
			else {
				app->pointer_mode = true;
				layout_pointer(app);
			}
		}
		return true;
	}
	if (code > -1 || code < -5)
		return false;
	bit = 1U << (-code - 1);
	/* A press already sent as KEY must get its original KEY release. */
	if (app->native_keys_held & bit)
		return false;
	if (code == -5) {
		if (!app->pointer_fire_withheld &&
		    (!app->pointer_available || !app->pointer_mode))
			return false;
		if (key->pressed && !app->pointer_fire_withheld) {
			app->pointer_fire_withheld = true;
			app->pointer_fire_held = true;
			send_pen(app, MRE_PEN_EVENT_TAP);
		} else if (!key->pressed) {
			app->pointer_fire_withheld = false;
			if (app->pointer_fire_held) {
				app->pointer_fire_held = false;
				send_pen(app, MRE_PEN_EVENT_RELEASE);
			}
		}
		return true;
	}
	if (!(app->pointer_arrows_withheld & bit) &&
	    (!app->pointer_available || !app->pointer_mode))
		return false;
	if (!key->pressed) {
		app->pointer_arrows_withheld &= ~bit;
		return true;
	}
	app->pointer_arrows_withheld |= bit;
	if (!app->pointer_available || !app->pointer_mode)
		return true;
	/* Without MOVE, keep a held pen's TAP and RELEASE at the same point. */
	if (app->pointer_fire_held)
		return true;
	if (code == -1)
		app->pointer_y -= POINTER_STEP_PX;
	else if (code == -2)
		app->pointer_y += POINTER_STEP_PX;
	else if (code == -3)
		app->pointer_x -= POINTER_STEP_PX;
	else
		app->pointer_x += POINTER_STEP_PX;
	if (app->pointer_x < 0)
		app->pointer_x = 0;
	if (app->pointer_x >= MRE_FRAME_WIDTH)
		app->pointer_x = MRE_FRAME_WIDTH - 1;
	if (app->pointer_y < 0)
		app->pointer_y = 0;
	if (app->pointer_y >= MRE_FRAME_HEIGHT)
		app->pointer_y = MRE_FRAME_HEIGHT - 1;
	layout_pointer(app);
	return true;
}

static void native_key(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context)
{
	struct app *app = context;
	char line[64];
	int code = 0;
	(void)ui;
	(void)screen;
	if (app->picture_retained) {
		if (key->pressed &&
		    (fplinux_ui_is_softkey(key, false) ||
		     key->code == KEY_ESC || key->code == KEY_BACK))
			fplinux_ui_back(app->ui);
		return;
	}

	if (key->code == KEY_UP)
		code = -1;
	else if (key->code == KEY_DOWN)
		code = -2;
	else if (key->code == KEY_LEFT)
		code = -3;
	else if (key->code == KEY_RIGHT)
		code = -4;
	else if (key->code == KEY_OK || key->code == KEY_ENTER ||
		 key->code == KEY_KPENTER)
		code = -5;
	else if (fplinux_ui_is_softkey(key, true) ||
		 (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		  key->code == KEY_MENU))
		code = -6;
	else if (fplinux_ui_is_softkey(key, false) ||
		 (key->source == FPLINUX_UI_KEY_KEYBOARD &&
		  key->code == KEY_ESC))
		code = -7;
	else if (key->source == FPLINUX_UI_KEY_PHONE) {
		if (key->code >= FPLINUX_KEY_0 && key->code <= FPLINUX_KEY_9)
			code = '0' + (int)(key->code - FPLINUX_KEY_0);
		else if (key->code == FPLINUX_KEY_STAR)
			code = '*';
		else if (key->code == FPLINUX_KEY_POUND)
			code = '#';
	} else if ((key->ascii >= '0' && key->ascii <= '9') ||
		   key->ascii == '*' || key->ascii == '#') {
		code = (int)key->ascii;
	}
	if (!code)
		return;
	if (pointer_key(app, key, code))
		return;
	snprintf(line, sizeof(line), "KEY\t%d\t%d\n",
		 key->pressed ? MRE_KEY_EVENT_DOWN : MRE_KEY_EVENT_UP, code);
	if (!send_line(app, line))
		show_error(app, _("The application did not respond."));
	else if (code <= -1 && code >= -5) {
		unsigned int bit = 1U << (-code - 1);
		if (key->pressed)
			app->native_keys_held |= bit;
		else
			app->native_keys_held &= ~bit;
	}
}

static void layout_native(struct app *app)
{
	int width = fplinux_ui_width(app->ui);
	int height = fplinux_ui_height(app->ui);
	struct frame_viewport viewport = frame_viewport(app);

	if (!app->canvas || width <= 0 || height <= 0)
		return;
	if (width != app->display_width || height != app->display_height) {
		uint16_t *pixels =
			realloc(app->display_pixels,
				(size_t)width * height * sizeof(*pixels));
		if (!pixels) {
			show_error(app, _("Not enough memory."));
			return;
		}
		app->display_pixels = pixels;
		app->display_width = width;
		app->display_height = height;
	}
	memset(app->display_pixels, 0,
	       (size_t)width * height * sizeof(*app->display_pixels));
	for (int y = 0; y < viewport.height; ++y)
		for (int x = 0; x < viewport.width; ++x)
			app->display_pixels[(viewport.y + y) * width +
					    viewport.x + x] =
				app->frame_pixels[(y * MRE_FRAME_HEIGHT /
						   viewport.height) *
							  MRE_FRAME_WIDTH +
						  x * MRE_FRAME_WIDTH /
							  viewport.width];
	lv_canvas_set_buffer(app->canvas, app->display_pixels, width, height,
			     LV_COLOR_FORMAT_RGB565);
	lv_obj_invalidate(app->canvas);
	layout_pointer(app);
}

static bool show_native(struct app *app)
{
	struct fplinux_ui_screen *screen;
	lv_obj_t *canvas;

	if (!fplinux_ui_set_fullscreen(app->ui, true))
		goto failed;
	if (app->view == VIEW_NATIVE && app->canvas) {
		layout_native(app);
		return true;
	}
	screen = fplinux_ui_canvas_new(app->ui, NULL);
	if (!screen)
		goto failed;
	/* Stack ownership also covers a failed LVGL child allocation. */
	replace_screen(app, screen, VIEW_NATIVE);
	canvas = lv_canvas_create(fplinux_ui_canvas_object(screen));
	if (!canvas)
		goto failed;
	lv_obj_set_pos(canvas, 0, 0);
	lv_canvas_set_buffer(canvas, app->frame_pixels, MRE_FRAME_WIDTH,
			     MRE_FRAME_HEIGHT, LV_COLOR_FORMAT_RGB565);
	lv_obj_set_scrollable(canvas, false);
	fplinux_ui_canvas_set_key_handler(screen, native_key, app);
	fplinux_ui_canvas_set_keyboard_keys(screen, true);
	app->canvas = canvas;
	app->pointer = lv_obj_create(fplinux_ui_canvas_object(screen));
	if (!app->pointer)
		goto failed;
	lv_obj_remove_style_all(app->pointer);
	lv_obj_set_size(app->pointer, POINTER_SIZE_PX, POINTER_SIZE_PX);
	lv_obj_set_style_radius(app->pointer, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(app->pointer,
				  fplinux_ui_theme(app->ui)->background, 0);
	lv_obj_set_style_bg_opa(app->pointer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(app->pointer,
				      fplinux_ui_theme(app->ui)->foreground, 0);
	lv_obj_set_style_border_width(app->pointer, 2, 0);
	lv_obj_set_scrollable(app->pointer, false);
	lv_obj_set_clickable(app->pointer, false);
	layout_native(app);
	return true;
failed:
	show_error(app, _("Cannot display the application's screen."));
	return false;
}

static bool resume_native(struct app *app)
{
	app->modal = false;
	clear_entries(app);
	if (app->native && app->has_frame)
		return show_native(app);
	show_wait(app);
	return app->child > 0;
}

static void sync_input_commands(struct app *app)
{
	if (app->field)
		add_command(app->screen, CMD_RIGHT, FPLINUX_UI_COMMAND_CANCEL,
			    app->shared->editor_method == 0 ? _("Close") :
			    fplinux_ui_text_is_empty(app->field) ||
					    fplinux_ui_text_is_picker_open(
						    app->field) ?
							      _("Cancel") :
							      _("Clear"),
			    0);
}

static void field_changed(struct fplinux_ui_text *field, void *context)
{
	(void)field;
	sync_input_commands(context);
}

static void submit_editor(struct app *app, bool accepted)
{
	const char *text;
	size_t bytes, units;
	if (!app->shared || !app->field || app->view != VIEW_INPUT)
		return;
	if (accepted) {
		text = fplinux_ui_text_get(app->field);
		bytes = strlen(text);
		if (bytes > MRE_EDITOR_BYTES ||
		    !mre_utf8_decode((const unsigned char *)text, bytes, NULL,
				     MRE_EDITOR_UNITS + 1U, &units) ||
		    units > (size_t)app->shared->editor_max_shorts) {
			show_error(
				app,
				_("The text contains unsupported characters or is too long."));
			return;
		}
		memcpy(app->shared->editor_data, text, bytes + 1U);
		app->shared->editor_bytes = (uint32_t)bytes;
	}
	if (!send_line(app, accepted ? "INPUT_DONE\n" : "INPUT_CANCEL\n")) {
		show_error(app, _("The application did not respond."));
		return;
	}
	resume_native(app);
}

static void editor_key(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
		       const struct fplinux_ui_key *key, void *context)
{
	struct app *app = context;
	(void)ui;
	(void)screen;
	if (!app->field)
		return;
	if (key->source == FPLINUX_UI_KEY_KEYBOARD && key->code == KEY_ESC &&
	    !fplinux_ui_text_is_picker_open(app->field)) {
		if (key->pressed)
			submit_editor(app, false);
		return;
	}
	if (!(app->shared->editor_method & (1 | 32)) &&
	    !fplinux_ui_text_is_picker_open(app->field) &&
	    (key->code == KEY_OK || key->code == KEY_ENTER ||
	     key->code == KEY_KPENTER)) {
		if (key->pressed)
			submit_editor(app, true);
		return;
	}
	fplinux_ui_text_key(app->field, key);
}

static void select_input_mode(struct fplinux_ui_text *field, const char *mode)
{
	struct fplinux_ui_key key = {
		.source = FPLINUX_UI_KEY_PHONE,
		.code = FPLINUX_KEY_POUND,
	};
	char first[FPLINUX_LANGUAGE_TEXT_BYTES];
	snprintf(first, sizeof(first), "%s", fplinux_ui_text_indicator(field));
	for (size_t i = 0; i < FPLINUX_MULTITAP_LANGUAGE_COUNT * 3U + 1U; ++i) {
		if (!strcmp(fplinux_ui_text_indicator(field), mode))
			return;
		key.pressed = true;
		fplinux_ui_text_key(field, &key);
		key.pressed = false;
		fplinux_ui_text_key(field, &key);
		if (!strcmp(fplinux_ui_text_indicator(field), first))
			return;
	}
}

static bool show_editor(struct app *app)
{
	struct fplinux_ui_screen *screen;
	const struct fplinux_ui_metrics *metrics = fplinux_ui_metrics(app->ui);
	struct mre_shared *shared = app->shared;
	size_t units;
	if (!shared || shared->editor_max_shorts < 0 ||
	    shared->editor_max_shorts > (int32_t)MRE_EDITOR_UNITS ||
	    shared->editor_bytes > MRE_EDITOR_BYTES ||
	    shared->editor_data[shared->editor_bytes] ||
	    !memchr(shared->editor_title, 0, sizeof(shared->editor_title)) ||
	    !mre_utf8_decode(shared->editor_data, shared->editor_bytes, NULL,
			     MRE_EDITOR_UNITS + 1U, &units) ||
	    units > (size_t)shared->editor_max_shorts)
		return false;
	fplinux_ui_set_fullscreen(app->ui, false);
	screen = fplinux_ui_canvas_new(app->ui, shared->editor_title);
	if (!screen)
		return false;
	replace_screen(app, screen, VIEW_INPUT);
	app->modal = true;
	lv_obj_t *column = lv_obj_create(fplinux_ui_canvas_object(screen));
	if (!column)
		return false;
	lv_obj_remove_style_all(column);
	lv_obj_set_size(column, lv_pct(100), lv_pct(100));
	lv_obj_set_style_pad_left(column, metrics->margin, 0);
	lv_obj_set_style_pad_right(column, metrics->margin, 0);
	lv_obj_set_style_pad_ver(column, metrics->spacing, 0);
	lv_obj_set_style_pad_row(column, metrics->spacing, 0);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(column, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(column, LV_SCROLLBAR_MODE_AUTO);
	fplinux_ui_style_scrollbar(app->ui, column);
	bool multiline = shared->editor_method == 0 ||
			 (shared->editor_method & (1 | 32)) != 0;
	struct fplinux_ui_text *field = fplinux_ui_text_create(
		app->ui, column, multiline, (size_t)shared->editor_max_shorts);
	if (!field)
		return false;
	app->field = field;
	lv_obj_t *frame = fplinux_ui_text_object(field);
	lv_obj_set_width(frame, lv_pct(100));
	lv_obj_set_height(frame, lv_pct(100));
	enum fplinux_ui_text_constraint constraint = FPLINUX_UI_TEXT_ANY;
	if (shared->editor_method & 2)
		constraint |= FPLINUX_UI_TEXT_PASSWORD;
	if (shared->editor_method == 0 || shared->editor_max_shorts == 0)
		constraint |= FPLINUX_UI_TEXT_UNEDITABLE;
	if (!fplinux_ui_text_set_constraint(field, constraint) ||
	    !fplinux_ui_text_set(field, (const char *)shared->editor_data))
		return false;
	/* SDK method bits select the initial IME, not an app-specific parser.
         * The native field owns Unicode, multi-tap and its symbol picker. */
	if (shared->editor_method & 4)
		select_input_mode(field, "123");
	else if (shared->editor_method & (8 | 16))
		select_input_mode(field, "abc");
	add_command(screen, CMD_ACCEPT, FPLINUX_UI_COMMAND_SCREEN,
		    shared->editor_method == 0 ? _("Close") : _("Done"), 0);
	add_command(screen, CMD_CANCEL, FPLINUX_UI_COMMAND_HELP,
		    _("Cancel input"), 1);
	fplinux_ui_canvas_set_key_handler(screen, editor_key, app);
	fplinux_ui_canvas_set_keyboard_keys(screen, true);
	fplinux_ui_text_set_change_cb(field, field_changed, app);
	sync_input_commands(app);
	accept_document_ui(app);
	return true;
}

static bool append_path(char destination[MRE_VFS_PATH_BYTES],
			const char *folder, const char *name)
{
	size_t bytes = strlen(folder), name_bytes = strlen(name);
	bool slash = bytes && folder[bytes - 1U] != '\\';
	if (!valid_name(name) ||
	    bytes + name_bytes + (slash ? 1U : 0U) >= MRE_VFS_PATH_BYTES)
		return false;
	memcpy(destination, folder, bytes);
	if (slash)
		destination[bytes++] = '\\';
	memcpy(destination + bytes, name, name_bytes + 1U);
	return true;
}

static void show_selector(struct app *app)
{
	struct fplinux_ui_screen *screen;
	int error = read_entries(
		app, mre_vfs_open_directory(app->vfs, app->selector_folder));
	fplinux_ui_set_fullscreen(app->ui, false);
	app->modal = true;
	if (error == 0 && app->entry_count) {
		screen = fplinux_ui_list_new_virtual(
			app->ui, app->selector_folder, FPLINUX_UI_LIST_IMPLICIT,
			app->entry_count, directory_row, app);
		if (screen)
			fplinux_ui_list_set_select_label(screen, _("Select"));
	} else {
		screen = fplinux_ui_form_new(app->ui, app->selector_folder);
		if (screen)
			fplinux_ui_form_append_string(
				screen, NULL,
				error ? _("Cannot read the folder.") :
					_("No matching files."));
	}
	if (!screen) {
		show_error(app, _("Not enough memory."));
		return;
	}
	add_command(screen, CMD_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	if (strlen(app->selector_folder) > 3U)
		add_command(screen, CMD_UP, FPLINUX_UI_COMMAND_SCREEN,
			    _("Parent folder"), 0);
	add_command(screen, CMD_CARD, FPLINUX_UI_COMMAND_SCREEN,
		    _("VXP folder"), 1);
	add_command(screen, CMD_PRIVATE, FPLINUX_UI_COMMAND_SCREEN,
		    _("Application files"), 2);
	replace_screen(app, screen, VIEW_SELECTOR);
	accept_document_ui(app);
}

static void select_file(struct app *app, size_t selected)
{
	char path[MRE_VFS_PATH_BYTES];
	if (selected >= app->entry_count ||
	    !append_path(path, app->selector_folder,
			 app->entries[selected].name))
		return;
	if (app->entries[selected].directory) {
		snprintf(app->selector_folder, sizeof(app->selector_folder),
			 "%s", path);
		show_selector(app);
		return;
	}
	/* Pin the same ordinary file policy used by SDK IO before callback. */
	int fd = mre_vfs_open_readonly(app->vfs, path);
	if (fd < 0) {
		show_selector(app);
		return;
	}
	close(fd);
	if (!send_path(app, "SELECT", path)) {
		show_error(app, _("The application did not respond."));
		return;
	}
	resume_native(app);
}

static bool dispatch_vfs(struct app *app, uint32_t sequence)
{
	struct mre_vfs_request request;
	struct mre_vfs_response response;
	char line[64];
	if (!app->shared || !app->vfs || !sequence ||
	    sequence > VFS_REQUEST_LIMIT || sequence <= app->vfs_sequence)
		return false;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memcpy(&request, &app->shared->vfs_request, sizeof(request));
	if (request.sequence != sequence)
		return false;
	mre_vfs_dispatch(app->vfs, &request, &response);
	app->vfs_sequence = sequence;
	memcpy(&app->shared->vfs_response, &response, sizeof(response));
	snprintf(line, sizeof(line), "VFS_REPLY\t%u\n", sequence);
	return send_line(app, line);
}

static bool publish_http(struct app *app,
			 const struct mre_http_response *response)
{
	char line[64];
	if (!app->shared)
		return false;
	memcpy(&app->shared->http_response, response, sizeof(*response));
	snprintf(line, sizeof(line), "HTTP_RESPONSE\t%u\n", response->id);
	return send_line(app, line);
}

static bool request_http(struct app *app, uint32_t id)
{
	struct mre_http_request request;
	struct mre_http_response failure = { .id = id, .transport = 2 };
	char accepted[64];
	if (!app->shared || !id || id > INT32_MAX || id <= app->http_last_id)
		return false;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memcpy(&request, &app->shared->http_request, sizeof(request));
	if (request.id != id)
		return false;
	app->http_last_id = id;
	/* Release the SDK producer only after its shared slot has been copied.
	 * Acceptance confirms ownership of arguments, not HTTP success. */
	snprintf(accepted, sizeof(accepted), "HTTP_ACCEPTED\t%u\n", id);
	if (!send_line(app, accepted))
		return false;
	if (app->http_active_id || ++app->http_requests > HTTP_REQUEST_LIMIT ||
	    !memchr(request.url, 0, sizeof(request.url)))
		return publish_http(app, &failure);
	/* Each request authorizes precisely the URL copied from that SDK call.
         * The broker does not inspect executable names or fetch credentials. */
	mre_http_broker_destroy(app->http);
	app->http = mre_http_broker_create(request.url, 1);
	if (!app->http || mre_http_broker_start(app->http, &request) != 0) {
		mre_http_broker_destroy(app->http);
		app->http = NULL;
		return publish_http(app, &failure);
	}
	app->http_active_id = id;
	return true;
}

static bool poll_http(struct app *app)
{
	struct mre_http_response response;
	int result;
	if (!app->http || !app->http_active_id)
		return true;
	result = mre_http_broker_poll(app->http, &response);
	if (!result)
		return true;
	if (result < 0) {
		memset(&response, 0, sizeof(response));
		response.id = app->http_active_id;
		response.transport = 2;
	}
	app->http_active_id = 0;
	mre_http_broker_destroy(app->http);
	app->http = NULL;
	return publish_http(app, &response);
}

static void protocol_line(struct app *app, char *line)
{
	uint32_t number;
	char message[PROTOCOL_LIMIT / 2U + 1U];
	if (!strncmp(line, "VFS_REQUEST\t", 12)) {
		if (!parse_number(line + 12, &number) ||
		    !dispatch_vfs(app, number))
			goto malformed;
		return;
	}
	if (!strncmp(line, "HTTP_REQUEST\t", 13)) {
		if (!parse_number(line + 13, &number) ||
		    !request_http(app, number))
			goto malformed;
		return;
	}
	if (!strncmp(line, "HTTP_CANCEL\t", 12)) {
		if (!parse_number(line + 12, &number) || !number ||
		    number > app->http_last_id)
			goto malformed;
		if (number == app->http_active_id) {
			mre_http_broker_cancel(app->http, number);
			mre_http_broker_destroy(app->http);
			app->http = NULL;
			app->http_active_id = 0;
		}
		return;
	}
	if (!strcmp(line, "VIEW_PICTURE") && app->native) {
		app->picture_requested = true;
		return;
	}
	if (!strcmp(line, "POINTER_AVAILABLE\t1") ||
	    !strcmp(line, "POINTER_AVAILABLE\t0")) {
		bool available = line[18] == '1';
		if (!available && !pause_pointer(app))
			return;
		app->pointer_available = available;
		layout_pointer(app);
		return;
	}
	if (!strcmp(line, "DISPLAY\t240\t320") && app->shared) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (app->shared->screen_width != MRE_FRAME_WIDTH ||
		    app->shared->screen_height != MRE_FRAME_HEIGHT)
			goto malformed;
		app->native = true;
		app->viewer_visible = fplinux_ui_is_visible(app->ui);
		if (!app->frame_pixels)
			app->frame_pixels =
				calloc(MRE_FRAME_WIDTH * MRE_FRAME_HEIGHT,
				       sizeof(*app->frame_pixels));
		if (!app->frame_pixels)
			goto no_memory;
		return;
	}
	if (!strncmp(line, "FRAME\t", 6) && app->native && app->shared) {
		char ack[64];
		if (!parse_number(line + 6, &number))
			goto malformed;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (app->shared->frame_sequence != number)
			goto malformed;
		memcpy(app->frame_pixels, app->shared->pixels,
		       sizeof(app->shared->pixels));
		app->has_frame = true;
		/* ACK belongs to transport progress, including modal screens. */
		snprintf(ack, sizeof(ack), "ACK\t%u\n", number);
		if (!send_line(app, ack))
			goto malformed;
		if (!app->modal && !app->decision) {
			if (!show_native(app))
				return;
			lv_obj_invalidate(app->canvas);
			accept_document_ui(app);
		}
		return;
	}
	if (!strcmp(line, "INPUT") && app->shared && !app->modal) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (app->decision) {
			app->deferred_modal = VIEW_INPUT;
			return;
		}
		if (!pause_pointer(app))
			return;
		if (!show_editor(app))
			goto malformed;
		return;
	}
	if (!strcmp(line, "SELECTOR") && app->shared && !app->modal) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (!memchr(app->shared->selector_folder, 0,
			    sizeof(app->shared->selector_folder)) ||
		    app->shared->selector_type < 0 ||
		    app->shared->selector_type > 2)
			goto malformed;
		if (!pause_pointer(app))
			return;
		snprintf(app->selector_folder, sizeof(app->selector_folder),
			 "%s",
			 app->shared->selector_folder[0] ?
				 app->shared->selector_folder :
				 "E:\\");
		app->selector_type = app->shared->selector_type;
		if (app->decision)
			app->deferred_modal = VIEW_SELECTOR;
		else
			show_selector(app);
		return;
	}
	if (!strncmp(line, "SNAPSHOT\t", 9) && app->shared) {
		char path[MRE_VFS_PATH_BYTES];
		uint32_t bytes = 0;
		if (!decode_text(line + 9, path, sizeof(path)))
			goto malformed;
		app->shared->file_bytes = 0;
		app->shared->file_path[0] = 0;
		bool success = mre_vfs_snapshot(app->vfs, path,
						app->shared->file_data,
						MRE_FILE_BYTES, &bytes) == 0;
		if (success) {
			memcpy(app->shared->file_path, path, strlen(path) + 1U);
			app->shared->file_bytes = bytes;
		}
		if (!send_line(app, success ? "SNAPSHOT_DONE\n" :
					      "SNAPSHOT_ERROR\n"))
			goto malformed;
		return;
	}
	if (!strcmp(line, "END")) {
		app->saw_end = true;
		return;
	}
	if (!strncmp(line, "ERROR\t", 6) &&
	    decode_text(line + 6, message, sizeof(message))) {
		show_error(app, message);
		return;
	}
malformed:
	show_error(app, _("Invalid application response."));
	return;
no_memory:
	show_error(app, _("Not enough memory."));
}

static void engine_output(struct fplinux_ui *ui, int fd, short events,
			  void *context)
{
	struct app *app = context;
	(void)ui;
	(void)events;
	while (app->output_fd == fd) {
		ssize_t count = read(fd, app->protocol + app->protocol_used,
				     PROTOCOL_LIMIT - app->protocol_used);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			show_error(
				app,
				_("Cannot read the application's response."));
			return;
		}
		if (count == 0) {
			fplinux_ui_watch_fd(app->ui, -1, 0, NULL, NULL);
			close(app->output_fd);
			app->output_fd = -1;
			if (!app->transport_failure_at)
				app->transport_failure_at = fplinux_ui_now_ms();
			if (app->protocol_used)
				show_error(
					app,
					_("The application ended with an incomplete response."));
			return;
		}
		app->protocol_used += (size_t)count;
		app->protocol[app->protocol_used] = '\0';
		char *newline;
		while ((newline = memchr(app->protocol, '\n',
					 app->protocol_used))) {
			size_t used = (size_t)(newline - app->protocol) + 1;
			*newline = '\0';
			protocol_line(app, app->protocol);
			if (app->output_fd != fd)
				return;
			memmove(app->protocol, app->protocol + used,
				app->protocol_used - used);
			app->protocol_used -= used;
		}
		if (app->protocol_used == PROTOCOL_LIMIT) {
			show_error(
				app,
				_("The application's response is too long."));
			return;
		}
	}
}

static void close_inherited_fds(rlim_t limit)
{
#ifdef SYS_close_range
	if (syscall(SYS_close_range, MRE_ORIGINAL_FD + 1, ~0U, 0U) == 0)
		return;
#endif
	for (rlim_t fd = MRE_ORIGINAL_FD + 1; fd < limit; ++fd)
		close((int)fd);
}

static int open_shared(struct app *app)
{
	int fd = memfd_create("mre-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	void *mapping;
	if (fd < 0)
		return -1;
	if (ftruncate(fd, sizeof(struct mre_shared)) < 0 ||
	    fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) <
		    0) {
		close(fd);
		return -1;
	}
	mapping = mmap(NULL, sizeof(struct mre_shared), PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return -1;
	}
	app->shared = mapping;
	memset(app->shared, 0, sizeof(*app->shared));
	app->shared->screen_width = MRE_FRAME_WIDTH;
	app->shared->screen_height = MRE_FRAME_HEIGHT;
	return fd;
}

static void load_device_fixtures(struct app *app)
{
	uint32_t bytes;
	/* Optional virtual devices are regular global files. These snapshots
         * supply test data; they do not claim live camera/microphone capture. */
	if (mre_vfs_snapshot(app->vfs, "E:\\devices\\camera.uyvy",
			     app->shared->camera_data, MRE_CAMERA_BYTES,
			     &bytes) == 0 &&
	    bytes == MRE_CAMERA_BYTES)
		app->shared->camera_bytes = bytes;
	if (mre_vfs_snapshot(app->vfs, "E:\\devices\\microphone.pcm",
			     app->shared->media_data, MRE_MEDIA_BYTES,
			     &bytes) == 0 &&
	    bytes > 0 && !(bytes & 1U))
		app->shared->media_bytes = bytes;
}

static void start_engine_document(struct app *app)
{
	int card = -1, private = -1, original = -1, shared = -1;
	int incoming[2] = { -1, -1 }, outgoing[2] = { -1, -1 };
	struct rlimit limit;
	pid_t parent = getpid();
	stop_engine(app);
	snprintf(app->filename, sizeof(app->filename), "%s",
		 app->pending_document.name);
	if (getrlimit(RLIMIT_NOFILE, &limit) < 0)
		goto failed;
	card = app->pending_document.directory_fd;
	original = app->pending_document.snapshot_fd;
	app->pending_document.directory_fd = app->pending_document.snapshot_fd =
		-1;
	if (card < 0 || original < 0 ||
	    (private = mre_document_open_state(&app->pending_document,
					       app->state_root_fd)) < 0 ||
	    (shared = open_shared(app)) < 0 || pipe2(incoming, O_CLOEXEC) < 0 ||
	    pipe2(outgoing, O_CLOEXEC) < 0)
		goto failed;
	app->vfs = mre_vfs_create(card, private, app->state_root_fd,
				  MRE_FILE_BUDGET);
	if (!app->vfs)
		goto failed;
	snprintf(app->shared->exec_path, sizeof(app->shared->exec_path),
		 "E:\\%s", app->filename);
	load_device_fixtures(app);
	struct mre_http_broker *probe =
		mre_http_broker_create("http://127.0.0.1/", 1);
	app->shared->http_available = probe ? 1U : 0U;
	mre_http_broker_destroy(probe);
	/* Move sources above the fixed inherited descriptors before dup2:
         * startup remains correct even when the UI owned fd 3 or 4. */
	int source[4] = { incoming[0], outgoing[1], shared, original };
	int inherited[4];
	for (size_t i = 0; i < 4; ++i) {
		inherited[i] = fcntl(source[i], F_DUPFD_CLOEXEC, 5);
		if (inherited[i] < 0) {
			for (size_t j = 0; j < i; ++j)
				close(inherited[j]);
			goto failed;
		}
	}
	app->child = fork();
	if (app->child == 0) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 ||
		    getppid() != parent ||
		    dup2(inherited[0], STDIN_FILENO) < 0 ||
		    dup2(inherited[1], STDOUT_FILENO) < 0 ||
		    dup2(inherited[2], MRE_SHARED_FD) < 0 ||
		    dup2(inherited[3], MRE_ORIGINAL_FD) < 0)
			_exit(126);
		close_inherited_fds(limit.rlim_cur);
		execl(ENGINE, ENGINE, MRE_ORIGINAL_PATH, "--shared",
		      (char *)NULL);
		_exit(127);
	}
	for (size_t i = 0; i < 4; ++i)
		close(inherited[i]);
	if (app->child < 0) {
		app->child = 0;
		goto failed;
	}
	close(original);
	original = -1;
	close(shared);
	shared = -1;
	close(private);
	private = -1;
	close(card);
	card = -1;
	close(incoming[0]);
	incoming[0] = -1;
	close(outgoing[1]);
	outgoing[1] = -1;
	app->input_fd = incoming[1];
	incoming[1] = -1;
	app->output_fd = outgoing[0];
	outgoing[0] = -1;
	if (fcntl(app->input_fd, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(app->output_fd, F_SETFL, O_NONBLOCK) < 0)
		goto failed;
	app->saw_end = false;
	update_tick(app);
	show_wait(app);
	if (app->child > 0)
		fplinux_ui_watch_fd(app->ui, app->output_fd, POLLIN,
				    engine_output, app);
	return;
failed:
	if (card >= 0)
		close(card);
	if (private >= 0)
		close(private);
	if (original >= 0)
		close(original);
	if (shared >= 0)
		close(shared);
	for (size_t i = 0; i < 2; ++i) {
		if (incoming[i] >= 0)
			close(incoming[i]);
		if (outgoing[i] >= 0)
			close(outgoing[i]);
	}
	show_error(app, _("Cannot open or start this VXP."));
}

static void remove_decision(struct app *app)
{
	if (app->decision) {
		app->decision = NULL;
		fplinux_ui_pop(app->ui);
	}
}

static void restore_guest_ui(struct app *app)
{
	enum view pending = app->deferred_modal;

	app->deferred_modal = VIEW_IDLE;
	if (pending == VIEW_INPUT)
		show_editor(app);
	else if (pending == VIEW_SELECTOR)
		show_selector(app);
	else if (!app->modal && (app->native || app->picture_retained) &&
		 app->has_frame) {
		show_native(app);
		accept_document_ui(app);
	}
}

static void finish_document(struct app *app,
			    enum fplinux_ui_document_status status, int error)
{
	uint32_t id = app->document_id;

	if (!id)
		return;
	app->document_id = 0;
	app->replacing = false;
	if (app->validator > 0) {
		kill(app->validator, SIGKILL);
		while (waitpid(app->validator, NULL, 0) < 0 && errno == EINTR) {
		}
		app->validator = 0;
	}
	mre_document_release(&app->pending_document);
	fplinux_ui_document_reply(app->ui, id, status, error);
	update_tick(app);
}

static bool show_decision(struct app *app, bool replace)
{
	struct fplinux_ui_screen *screen = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_CONFIRMATION, _("Close application?"),
		replace ?
			_("The current program will close. Open the new VXP?") :
			_("Close the current program?"),
		0);

	if (!screen)
		return false;
	add_command(screen, replace ? CMD_REPLACE : CMD_ACCEPT,
		    FPLINUX_UI_COMMAND_OK, _("Close"), 0);
	add_command(screen, CMD_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	fplinux_ui_screen_set_listener(screen, screen_command, NULL, app);
	app->decision = screen;
	fplinux_ui_push(app->ui, screen);
	return true;
}

static bool begin_validation(struct app *app)
{
	struct rlimit descriptors;

	if (getrlimit(RLIMIT_NOFILE, &descriptors) < 0)
		return false;
	int original =
		fcntl(app->pending_document.snapshot_fd, F_DUPFD_CLOEXEC, 5);
	if (original < 0)
		return false;
	pid_t parent = getpid();
	app->validator = fork();
	if (app->validator == 0) {
		struct rlimit cpu = { 5, 5 };
		struct rlimit memory = { 64U * 1024U * 1024U,
					 64U * 1024U * 1024U };
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 ||
		    getppid() != parent ||
		    dup2(original, MRE_ORIGINAL_FD) < 0 ||
		    dup2(STDERR_FILENO, STDOUT_FILENO) < 0 ||
		    setrlimit(RLIMIT_CPU, &cpu) < 0 ||
		    setrlimit(RLIMIT_AS, &memory) < 0 ||
		    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
			_exit(126);
		close(MRE_SHARED_FD);
		close_inherited_fds(descriptors.rlim_cur);
		alarm(10);
		execl(ENGINE, ENGINE, MRE_ORIGINAL_PATH, "--validate",
		      (char *)NULL);
		_exit(127);
	}
	int error = errno;
	close(original);
	errno = error;
	if (app->validator < 0) {
		app->validator = 0;
		return false;
	}
	app->validation_started = fplinux_ui_now_ms();
	update_tick(app);
	struct fplinux_ui_screen *screen = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_INFO, "MRE", _("Checking VXP…"), 0);
	if (!screen)
		return false;
	fplinux_ui_alert_set_progress(screen, 0, 0);
	add_command(screen, CMD_CANCEL, FPLINUX_UI_COMMAND_CANCEL, NULL, 0);
	fplinux_ui_screen_set_listener(screen, screen_command, NULL, app);
	app->decision = screen;
	fplinux_ui_push(app->ui, screen);
	return true;
}

static struct fplinux_ui_document_result
document_received(struct fplinux_ui *ui,
		  const struct fplinux_ui_document_request *request,
		  void *context)
{
	struct app *app = context;
	struct fplinux_ui_document_result result = {
		.request_id = request->request_id,
		.status = FPLINUX_UI_DOCUMENT_ERROR,
		.error = EBUSY,
	};
	(void)ui;
	if (app->document_id || app->close_pending)
		return result;
	if (app->state_root_fd < 0) {
		result.error = EIO;
		return result;
	}
	if (!mre_document_prepare(&app->pending_document, request->path)) {
		result.error = errno;
		return result;
	}
	struct mre_vfs *preflight = mre_vfs_create(
		app->pending_document.directory_fd, app->state_root_fd,
		app->state_root_fd, MRE_FILE_BUDGET);
	if (!preflight) {
		result.error = errno;
		mre_document_release(&app->pending_document);
		return result;
	}
	mre_vfs_destroy(preflight);
	app->document_id = request->request_id;
	if (!begin_validation(app)) {
		result.error = errno ? errno : ENOMEM;
		if (app->validator > 0) {
			kill(app->validator, SIGKILL);
			while (waitpid(app->validator, NULL, 0) < 0 &&
			       errno == EINTR) {
			}
			app->validator = 0;
		}
		app->document_id = 0;
		mre_document_release(&app->pending_document);
		update_tick(app);
		return result;
	}
	result.status = FPLINUX_UI_DOCUMENT_WAIT;
	result.error = 0;
	return result;
}

static enum fplinux_ui_close_response prepare_close(struct fplinux_ui *ui,
						    void *context)
{
	struct app *app = context;
	(void)ui;

	if (app->decision)
		return FPLINUX_UI_CLOSE_CANCEL;
	if (app->child <= 0)
		return FPLINUX_UI_CLOSE_READY;
	app->close_pending = true;
	if (!show_decision(app, false)) {
		app->close_pending = false;
		return FPLINUX_UI_CLOSE_CANCEL;
	}
	return FPLINUX_UI_CLOSE_WAIT;
}

static void validation_tick(struct app *app)
{
	int status;
	pid_t ended;

	if (app->validator <= 0)
		return;
	do {
		ended = waitpid(app->validator, &status, WNOHANG);
	} while (ended < 0 && errno == EINTR);
	if (ended == 0 &&
	    fplinux_ui_now_ms() - app->validation_started < 12000U)
		return;
	remove_decision(app);
	if (ended > 0)
		app->validator = 0;
	if (ended <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		finish_document(app, FPLINUX_UI_DOCUMENT_ERROR, ENOEXEC);
		restore_guest_ui(app);
		return;
	}
	app->validator = 0;
	if (app->child > 0 || app->picture_retained) {
		app->replacing = true;
		if (!show_decision(app, true))
			finish_document(app, FPLINUX_UI_DOCUMENT_ERROR, ENOMEM);
	} else {
		start_engine_document(app);
	}
}

static void accept_document_ui(struct app *app)
{
	if (!app->document_id || app->decision)
		return;
	bool fullscreen = fplinux_ui_is_fullscreen(app->ui);
	bool native_ready = app->view == VIEW_NATIVE && app->has_frame &&
			    fullscreen;
	bool modal_ready = !fullscreen &&
			   ((app->view == VIEW_INPUT && app->field) ||
			    app->view == VIEW_SELECTOR);

	if (native_ready || modal_ready)
		finish_document(app, FPLINUX_UI_DOCUMENT_ACCEPTED, 0);
}

static void poll_children(struct app *app)
{
	int status = 0;
	pid_t ended;
	validation_tick(app);
	if (app->child <= 0)
		return;
	/* Drain cancellations/END before advancing HTTP. The watcher and timer
         * share the UI thread; neither invokes a guest callback directly. */
	if (app->output_fd >= 0)
		engine_output(app->ui, app->output_fd, POLLIN, app);
	if (app->child <= 0)
		return;
	if (!flush_outgoing(app) || (!app->saw_end && !poll_http(app))) {
		show_error(app, _("The application did not respond."));
		return;
	}
	do {
		ended = waitpid(app->child, &status, WNOHANG);
	} while (ended < 0 && errno == EINTR);
	if (ended == 0) {
		if (app->transport_failure_at &&
		    fplinux_ui_now_ms() - app->transport_failure_at >= 100U)
			show_error(
				app,
				_("The application closed its input channel."));
		return;
	}
	if (ended < 0 && errno != ECHILD)
		return;
	app->child = 0;
	if (app->output_fd >= 0)
		engine_output(app->ui, app->output_fd, POLLIN, app);
	if (app->view == VIEW_ERROR || app->closing)
		return;
	bool normal = ended > 0 && WIFEXITED(status) &&
		      WEXITSTATUS(status) == 0 && app->saw_end;
	if (normal && app->document_id && !app->validator && !app->replacing &&
	    !app->has_frame) {
		show_error(app, _("The VXP ended without a frame."));
		return;
	}
	bool retain =
		normal && app->has_frame &&
		(app->picture_requested ||
		 (app->document_id && !app->validator && !app->replacing));
	release_runtime(app);
	if (normal) {
		if (retain) {
			app->picture_retained = app->has_frame = true;
			if (!app->decision) {
				show_native(app);
				accept_document_ui(app);
			}
		} else {
			if (!app->document_id)
				fplinux_ui_back(app->ui);
		}
	} else {
		char message[256];
		if (ended > 0 && WIFSIGNALED(status))
			snprintf(message, sizeof(message),
				 _("The application stopped (signal %d)."),
				 WTERMSIG(status));
		else
			snprintf(
				message, sizeof(message),
				_("The application ended with an error (code %d)."),
				ended > 0 && WIFEXITED(status) ?
					WEXITSTATUS(status) :
					-1);
		show_error(app, message);
	}
}

static void child_tick(lv_timer_t *timer)
{
	struct app *app = lv_timer_get_user_data(timer);

	poll_children(app);
	update_tick(app);
}

static void screen_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct app *app = context;
	if (app->decision == screen) {
		app->decision = NULL;
		if (app->validator > 0) {
			finish_document(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		} else if (app->replacing) {
			app->replacing = false;
			if (id == CMD_REPLACE)
				start_engine_document(app);
			else
				finish_document(
					app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		} else if (app->close_pending) {
			app->close_pending = false;
			fplinux_ui_close_reply(
				ui, id == CMD_ACCEPT ? FPLINUX_UI_CLOSE_READY :
						       FPLINUX_UI_CLOSE_CANCEL);
		}
		if (!app->document_id && !app->close_pending)
			restore_guest_ui(app);
		return;
	}
	if (app->view == VIEW_INPUT) {
		if (id == CMD_ACCEPT)
			submit_editor(app, true);
		else if (id == CMD_CANCEL)
			submit_editor(app, false);
		else if (id == CMD_RIGHT && app->field) {
			struct fplinux_ui_key key = {
				.source = FPLINUX_UI_KEY_PHONE,
				.code = FPLINUX_KEY_SOFT_RIGHT,
				.pressed = true,
			};
			if (app->shared->editor_method == 0) {
				submit_editor(app, true);
				return;
			}
			/* A Canvas command is routed before its raw key handler.
			 * Let the common field clear text or close its symbol picker. */
			bool used = fplinux_ui_text_key(app->field, &key);
			key.pressed = false;
			fplinux_ui_text_key(app->field, &key);
			if (!used)
				submit_editor(app, false);
		}
		return;
	}
	if (app->view == VIEW_SELECTOR) {
		if (id == FPLINUX_UI_SELECT_COMMAND) {
			select_file(app, fplinux_ui_list_get_focused(screen));
		} else if (id == CMD_CANCEL) {
			if (!send_line(app, "SELECT_CANCEL\n"))
				show_error(
					app,
					_("The application did not respond."));
			else
				resume_native(app);
		} else if (id == CMD_CARD || id == CMD_PRIVATE) {
			snprintf(app->selector_folder,
				 sizeof(app->selector_folder), "%c:\\",
				 id == CMD_CARD ? 'E' : 'C');
			show_selector(app);
		} else if (id == CMD_UP) {
			size_t bytes = strlen(app->selector_folder);
			while (bytes > 3U &&
			       app->selector_folder[bytes - 1U] == '\\')
				app->selector_folder[--bytes] = 0;
			char *slash = strrchr(app->selector_folder, '\\');
			if (slash)
				slash[slash == app->selector_folder + 2 ? 1 : 0] =
					0;
			show_selector(app);
		}
		return;
	}
	if (id == CMD_BACK || id == CMD_CANCEL) {
		if (app->document_id)
			finish_document(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		fplinux_ui_back(ui);
	}
}

static void event(struct fplinux_ui *ui, const struct fplinux_ui_event *event,
		  void *context)
{
	struct app *app = context;
	if (event->type == FPLINUX_UI_EVENT_CLOSE) {
		app->closing = true;
		finish_document(app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
		stop_engine(app);
		fplinux_ui_quit(ui);
	} else if (event->type == FPLINUX_UI_EVENT_RESIZE) {
		layout_native(app);
		accept_document_ui(app);
	} else if (event->type == FPLINUX_UI_EVENT_INPUT_CANCEL) {
		if (app->pointer_fire_held)
			pause_pointer(app);
	} else if (event->type == FPLINUX_UI_EVENT_VISIBILITY && app->native &&
		   app->input_fd >= 0 &&
		   event->visible != app->viewer_visible) {
		char line[32];
		app->viewer_visible = event->visible;
		snprintf(line, sizeof(line), "VISIBILITY\t%d\n",
			 event->visible ? 1 : 0);
		if (!send_line(app, line))
			show_error(app, _("The application did not respond."));
	}
}

int main(int argc, char **argv)
{
	struct app app = {
		.input_fd = -1,
		.output_fd = -1,
		.state_root_fd = -1,
		.pending_document = { .directory_fd = -1, .snapshot_fd = -1 },
	};
	struct fplinux_cli cli = {
		.program = "fplinux-mre",
		.description = "Run VXP documents opened from Files.",
	};
	int result = fplinux_cli_parse(&cli, argc, argv);
	char state_path[PATH_MAX];

	if (result != FPLINUX_CLI_READY)
		return result;
	signal(SIGPIPE, SIG_IGN);
	app.ui = fplinux_ui_open(UI_ID, event, &app);
	if (!app.ui) {
		perror("MRE: UI");
		return EXIT_FAILURE;
	}
	fplinux_ui_set_title(app.ui, "MRE");
	fplinux_ui_set_document_handler(app.ui, document_received, &app);
	fplinux_ui_set_close_handler(app.ui, prepare_close, &app);
	if (fplinux_ui_state_dir(UI_ID, state_path, sizeof(state_path)))
		app.state_root_fd =
			open(state_path,
			     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	app.tick = lv_timer_create(child_tick, 25, &app);
	update_tick(&app);
	if (!app.tick || app.state_root_fd < 0) {
		result = -1;
		goto cleanup;
	}
	show_idle(&app);
	result = fplinux_ui_run(app.ui);
cleanup:
	finish_document(&app, FPLINUX_UI_DOCUMENT_CANCELLED, 0);
	stop_engine(&app);
	if (app.tick)
		lv_timer_delete(app.tick);
	if (app.state_root_fd >= 0)
		close(app.state_root_fd);
	fplinux_ui_close(app.ui);
	clear_entries(&app);
	free(app.frame_pixels);
	free(app.display_pixels);
	return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
