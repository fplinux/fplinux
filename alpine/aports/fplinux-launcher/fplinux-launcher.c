/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-cli.h"
#include "fplinux-ui-locale.h"
#include "fplinux-ui-menu.h"
#include "fplinux-ui-profile.h"
#include "fplinux-ui-screen.h"
#include "fplinux-ui-session.h"
#include "fplinux-ui.h"
#include "launcher-catalogue.h"
#include "launcher-layout.h"
#include "launcher-launch.h"
#include "launcher-power.h"
#include "launcher-tasks.h"
#include "launcher-text.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libintl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define _(text) gettext(text)

#define FPLINUX_LAUNCHER_APP_COUNT 64
/* Room for the icon sizes of every profile (launcher-layout.h). */
#define FPLINUX_LAUNCHER_ICON_SIZE_COUNT 8
#define FPLINUX_LAUNCHER_ICON_FILE_BYTES 32768
#define FPLINUX_LAUNCHER_RELOAD_DELAY_MS 250
#define FPLINUX_LAUNCHER_WATCH_EVENTS \
	(IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)
/* The marquee of a focused text that does not fit. */
#define FPLINUX_LAUNCHER_MARQUEE_DELAY_MS 400
#define FPLINUX_LAUNCHER_MARQUEE_PAUSE_MS 1000
#define FPLINUX_LAUNCHER_MARQUEE_PIXELS_PER_SECOND 30
/* Bounds of one pass, so that short and very long texts stay readable. */
#define FPLINUX_LAUNCHER_MARQUEE_MIN_MS 300
#define FPLINUX_LAUNCHER_MARQUEE_MAX_MS 10000

struct application {
	char id[FPLINUX_UI_SESSION_ID_BYTES];
	char name[FPLINUX_UI_SESSION_TEXT_BYTES];
	enum fplinux_launcher_group group;
	/*
	 * Tiles decoded once per catalogue load at the grid's and the list's
	 * size; NULL shows the generic app glyph instead.
	 */
	lv_draw_buf_t *tile;
	lv_draw_buf_t *row_tile;
};

/* Hashes of icon problems logged by the current and previous catalogue load. */
struct icon_reports {
	uint32_t current[2 * FPLINUX_LAUNCHER_APP_COUNT];
	uint32_t previous[2 * FPLINUX_LAUNCHER_APP_COUNT];
	size_t current_count;
	size_t previous_count;
};

static const uint8_t png_signature[8] = { 0x89, 'P',  'N',  'G',
					  '\r', '\n', 0x1a, '\n' };

/* What the launcher surface shows, derived from the manager's state. */
enum launcher_screen {
	/* Empty before the first ACTIVE, so a locked start never shows Home. */
	SCREEN_BLANK,
	SCREEN_LOCK,
	/* An application covers the body; only the bars are visible. */
	SCREEN_APPLICATION,
	SCREEN_HOME,
	SCREEN_CATALOGUE,
};

struct battery_icon {
	lv_obj_t *bolt;
	/* lv_line keeps a pointer to its points. */
	lv_point_precise_t bolt_points[4];
	lv_obj_t *body;
	lv_obj_t *cap;
	lv_obj_t *bars[FPLINUX_LAUNCHER_BATTERY_STEP_COUNT];
	lv_obj_t *low_stem;
	lv_obj_t *low_dot;
	/* The drawn state; unchanged readings never invalidate the bar. */
	bool drawn;
	bool shown;
	bool low;
	bool charging;
	int steps;
};

struct launcher {
	struct fplinux_ui *ui;
	/* The launcher owns one canvas, including its system notices. */
	struct fplinux_ui_screen *canvas;
	struct fplinux_launcher_tasks tasks;
	bool reported_tasks;
	const struct fplinux_ui_theme *theme;
	/* The display's profile and the surface regions derived from it. */
	const struct fplinux_ui_profile_spec *spec;
	struct fplinux_launcher_frame frame;
	struct fplinux_launcher_battery battery_layout;
	/* Sizes of the icon directories, ascending. */
	int icon_sizes[FPLINUX_LAUNCHER_ICON_SIZE_COUNT];
	size_t icon_size_count;
	/* The generic app glyph for apps without a tile; may be NULL. */
	const void *app_glyph;
	struct application apps[FPLINUX_LAUNCHER_APP_COUNT];
	size_t app_count;
	size_t selected;
	/* The catalogue viewport's scroll offset in pixels. */
	int scroll_offset;
	enum fplinux_launcher_view view;
	/* Grid or list; kept while Home is shown. */
	enum fplinux_launcher_view presentation;
	lv_obj_t *status_clock;
	lv_obj_t *lock_glyph;
	struct battery_icon battery_icon;
	lv_obj_t *body;
	lv_obj_t *softkeys;
	lv_obj_t *clock;
	lv_obj_t *date;
	lv_obj_t *hint;
	/* The shown notice dialog, which Up and Down scroll. */
	lv_obj_t *notice_box;
	lv_anim_t title_animation;
	const char *desktop_dir;
	int inotify_fd;
	bool watching;
	lv_timer_t *reload_timer;
	lv_timer_t *clock_timer;
	struct icon_reports icon_reports;
	bool session_known;
	bool active;
	/* The system choice belongs to this foreground application instance. */
	bool end_prompt;
	struct fplinux_launcher_backdrop backdrop;
	lv_image_dsc_t backdrop_image;
	uint32_t backdrop_token;
	uint32_t active_token;
	char active_id[FPLINUX_UI_SESSION_ID_BYTES];
	char active_title[FPLINUX_UI_SESSION_TEXT_BYTES];
	bool locked;
	bool visible;
	/* Display off or surface hidden: no timers, polling or animation. */
	bool paused;
	/* HOME arrived while an application was still closing. */
	bool home_pending;
	enum fplinux_ui_session_display display;
	char active_left[FPLINUX_UI_SESSION_TEXT_BYTES];
	char active_centre[FPLINUX_UI_SESSION_TEXT_BYTES];
	char active_right[FPLINUX_UI_SESSION_TEXT_BYTES];
	/* The last reason other than an unlock hint. */
	char notice_reason[FPLINUX_UI_SESSION_TEXT_BYTES];
	/* An application notice waiting for Close, on Home and in the catalogue. */
	enum fplinux_launcher_notice notice;
	/*
	 * The manager's battery notice from ACTIVE, shown also above an
	 * application, and the one closed here until the manager confirms.
	 */
	enum fplinux_launcher_notice battery_notice;
	enum fplinux_launcher_notice dismissed_notice;
	/* The manager's current unlock hint for the lock screen. */
	enum fplinux_launcher_notice hint_notice;
	/* The view and selection the manager last learned about. */
	enum fplinux_launcher_view reported_view;
	char reported_selection[FPLINUX_UI_SESSION_ID_BYTES];
	/* The manager's battery level in percent, -1 without a battery. */
	int battery;
	bool charging;
};

/* The theme's colour of a profile table colour token. */
static lv_color_t token_colour(const struct fplinux_ui_theme *theme, int colour)
{
	switch (colour) {
	case FPLINUX_UI_COLOUR_BACKGROUND:
		return theme->background;
	case FPLINUX_UI_COLOUR_SURFACE:
		return theme->surface;
	case FPLINUX_UI_COLOUR_MUTED:
		return theme->muted;
	case FPLINUX_UI_COLOUR_ACCENT:
		return theme->accent;
	case FPLINUX_UI_COLOUR_ACCENT_TEXT:
		return theme->accent_text;
	case FPLINUX_UI_COLOUR_BORDER:
		return theme->border;
	case FPLINUX_UI_COLOUR_POPUP:
		return theme->popup;
	case FPLINUX_UI_COLOUR_BAR:
		return theme->bar;
	case FPLINUX_UI_COLOUR_BAR_TEXT:
		return theme->bar_text;
	case FPLINUX_UI_COLOUR_DISABLED:
		return theme->disabled;
	case FPLINUX_UI_COLOUR_FOCUS:
		return theme->focus;
	case FPLINUX_UI_COLOUR_WARNING:
		return theme->warning;
	case FPLINUX_UI_COLOUR_DANGER:
		return theme->danger;
	case FPLINUX_UI_COLOUR_TILE_RING:
		return theme->tile_ring;
	case FPLINUX_UI_COLOUR_BRAND:
		return theme->brand;
	default:
		return theme->foreground;
	}
}

static lv_obj_t *plain(lv_obj_t *parent, struct fplinux_launcher_rect rect)
{
	lv_obj_t *object = lv_obj_create(parent);

	lv_obj_remove_style_all(object);
	lv_obj_set_pos(object, rect.x, rect.y);
	lv_obj_set_size(object, rect.width, rect.height);
	lv_obj_set_scrollable(object, false);
	return object;
}

/* A filled rectangle, rounded by radius. */
static lv_obj_t *fill(lv_obj_t *parent, struct fplinux_launcher_rect rect,
		      int radius, lv_color_t colour)
{
	lv_obj_t *object = plain(parent, rect);

	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_bg_color(object, colour, 0);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
	return object;
}

/* An outline only, rounded by radius. */
static lv_obj_t *outline(lv_obj_t *parent, struct fplinux_launcher_rect rect,
			 int radius, int width, lv_color_t colour)
{
	lv_obj_t *object = plain(parent, rect);

	lv_obj_set_style_radius(object, radius, 0);
	lv_obj_set_style_border_color(object, colour, 0);
	lv_obj_set_style_border_width(object, width, 0);
	return object;
}

/* A colour image, such as a tile or the brand mark. */
static lv_obj_t *image(lv_obj_t *parent, const void *source,
		       struct fplinux_launcher_rect rect)
{
	lv_obj_t *object = lv_image_create(parent);

	lv_obj_remove_style_all(object);
	lv_obj_set_pos(object, rect.x, rect.y);
	lv_obj_set_size(object, rect.width, rect.height);
	lv_image_set_src(object, source);
	return object;
}

/* A system glyph: an alpha-only image drawn in ink. */
static lv_obj_t *glyph(lv_obj_t *parent, const void *source,
		       struct fplinux_launcher_rect rect, lv_color_t ink)
{
	lv_obj_t *object = image(parent, source, rect);

	lv_obj_set_style_image_recolor(object, ink, 0);
	lv_obj_set_style_image_recolor_opa(object, LV_OPA_COVER, 0);
	return object;
}

static lv_obj_t *label(lv_obj_t *parent, struct fplinux_launcher_rect rect,
		       const lv_font_t *font, lv_color_t colour,
		       const char *text)
{
	lv_obj_t *object = lv_label_create(parent);

	lv_obj_set_pos(object, rect.x, rect.y);
	lv_obj_set_width(object, rect.width);
	/* A one-line label: LVGL ends a long text only at a fixed height. */
	if (rect.height > 0)
		lv_obj_set_height(object, rect.height);
	lv_obj_set_style_text_font(object, font, 0);
	lv_obj_set_style_text_color(object, colour, 0);
	lv_label_set_text(object, text);
	return object;
}

static void set_label(lv_obj_t *object, const char *text)
{
	if (strcmp(lv_label_get_text(object), text))
		lv_label_set_text(object, text);
}

static int text_width(const char *value, const lv_font_t *font)
{
	lv_point_t size;

	if (!value[0])
		return 0;
	lv_text_get_size(&size, value, font, 0, 0, LV_COORD_MAX,
			 LV_TEXT_FLAG_NONE);
	return size.x;
}

/* Height of a text wrapped at width. */
static int text_height(const char *value, const lv_font_t *font, int width)
{
	lv_point_t size;

	lv_text_get_size(&size, value, font, 0, 0, width, LV_TEXT_FLAG_NONE);
	return size.y;
}

static void copy_message_text(char *destination, const char *text)
{
	/* Session strings are NUL-terminated within the same capacity. */
	snprintf(destination, FPLINUX_UI_SESSION_TEXT_BYTES, "%s", text);
}

static void begin_icon_reports(struct icon_reports *reports)
{
	memcpy(reports->previous, reports->current, sizeof(reports->current));
	reports->previous_count = reports->current_count;
	reports->current_count = 0;
}

/*
 * Every catalogue load checks the icons again, and the session log lives in
 * RAM. A problem is logged when it was not reported by the previous load.
 */
static void report_icon_problem(struct icon_reports *reports,
				const char *app_id, const char *problem)
{
	char line[PATH_MAX + 128];
	uint32_t hash = 2166136261U;
	size_t capacity = sizeof(reports->current) / sizeof(*reports->current);

	snprintf(line, sizeof(line), "fplinux-launcher: %s: %s\n", app_id,
		 problem);
	for (const char *character = line; *character; ++character)
		hash = (hash ^ (unsigned char)*character) * 16777619U;
	/* Both icon sizes may meet the same file. */
	for (size_t i = 0; i < reports->current_count; ++i)
		if (reports->current[i] == hash)
			return;
	if (reports->current_count < capacity)
		reports->current[reports->current_count++] = hash;
	for (size_t i = 0; i < reports->previous_count; ++i)
		if (reports->previous[i] == hash)
			return;
	fputs(line, stderr);
}

static uint32_t read_be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
	       (uint32_t)bytes[2] << 8 | bytes[3];
}

/* Icons follow the trust rule that the session applies to desktop entries. */
static const char *untrusted_icon_reason(const struct stat *status)
{
	if (!S_ISREG(status->st_mode))
		return "is not a regular file";
	if (status->st_uid != 0)
		return "is not owned by root";
	if (status->st_mode & (S_IWGRP | S_IWOTH))
		return "is writable by group or others";
	if (status->st_size > FPLINUX_LAUNCHER_ICON_FILE_BYTES)
		return "is larger than 32768 bytes";
	return NULL;
}

/* NULL with reason; *missing tells that the file does not exist. */
static uint8_t *read_icon_file(const char *path, size_t *bytes, bool *missing,
			       char *reason, size_t capacity)
{
	struct stat status;
	const char *untrusted;
	uint8_t *png;
	size_t size;
	size_t done = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);

	*missing = fd < 0 && errno == ENOENT;
	if (fd < 0) {
		snprintf(reason, capacity, "%s",
			 errno == ELOOP ? "is a symbolic link" :
					  strerror(errno));
		return NULL;
	}
	if (fstat(fd, &status) < 0) {
		snprintf(reason, capacity, "%s", strerror(errno));
		close(fd);
		return NULL;
	}
	untrusted = untrusted_icon_reason(&status);
	if (untrusted) {
		snprintf(reason, capacity, "%s", untrusted);
		close(fd);
		return NULL;
	}
	size = (size_t)status.st_size;
	png = malloc(size ? size : 1);
	if (!png) {
		snprintf(reason, capacity, "%s", strerror(ENOMEM));
		close(fd);
		return NULL;
	}
	while (done < size) {
		ssize_t count = read(fd, png + done, size - done);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		done += (size_t)count;
	}
	close(fd);
	if (done != size) {
		snprintf(reason, capacity, "changed or failed while reading");
		free(png);
		return NULL;
	}
	*bytes = size;
	return png;
}

/* The PNG header must give the size of the directory the file is in. */
static bool check_png_header(const uint8_t *png, size_t bytes, int size,
			     char *reason, size_t capacity)
{
	uint32_t width;
	uint32_t height;

	/* Signature, then IHDR: 13-byte length, type, width and height. */
	if (bytes < 24 || memcmp(png, png_signature, sizeof(png_signature)) ||
	    read_be32(png + 8) != 13 || memcmp(png + 12, "IHDR", 4)) {
		snprintf(reason, capacity, "is not a PNG file");
		return false;
	}
	width = read_be32(png + 16);
	height = read_be32(png + 20);
	if (width != (uint32_t)size || height != (uint32_t)size) {
		snprintf(reason, capacity,
			 "is %" PRIu32 "x%" PRIu32 ", expected %dx%d", width,
			 height, size, size);
		return false;
	}
	return true;
}

/*
 * The runtime decodes the checked PNG bytes once, scaling a larger file down
 * with its area filter, into an ARGB8888 buffer that redraws reuse; LVGL
 * keeps no reference to the PNG bytes.
 */
static lv_draw_buf_t *decode_icon(const uint8_t *png, size_t bytes,
				  int file_size, int size)
{
	const lv_image_dsc_t source = {
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = LV_COLOR_FORMAT_RAW_ALPHA,
			.w = (uint32_t)file_size,
			.h = (uint32_t)file_size,
		},
		.data_size = (uint32_t)bytes,
		.data = png,
	};

	return fplinux_ui_image_load(&source, size, size);
}

/*
 * The tile for size pixels: the file of exactly that size, else the next
 * larger one that is valid, scaled down. Problems are logged once.
 */
static lv_draw_buf_t *load_icon(struct launcher *launcher, const char *app_id,
				const char *name, int size)
{
	char path[PATH_MAX];
	char reason[64];
	char problem[PATH_MAX + 80];
	bool found = false;

	for (size_t i = 0; i < launcher->icon_size_count; ++i) {
		int file_size = launcher->icon_sizes[i];
		lv_draw_buf_t *icon = NULL;
		uint8_t *png;
		size_t bytes;
		bool missing;

		if (file_size < size ||
		    snprintf(path, sizeof(path),
			     "/usr/share/icons/hicolor/%dx%d/apps/%s.png",
			     file_size, file_size, name) >= (int)sizeof(path))
			continue;
		png = read_icon_file(path, &bytes, &missing, reason,
				     sizeof(reason));
		if (missing)
			continue;
		found = true;
		if (png && check_png_header(png, bytes, file_size, reason,
					    sizeof(reason))) {
			icon = decode_icon(png, bytes, file_size, size);
			if (!icon)
				snprintf(
					reason, sizeof(reason), "%s",
					errno == EINVAL ?
						"cannot be decoded or is fully transparent" :
						strerror(errno));
		}
		free(png);
		if (icon)
			return icon;
		snprintf(problem, sizeof(problem), "ignoring icon %s: %s", path,
			 reason);
		report_icon_problem(&launcher->icon_reports, app_id, problem);
	}
	if (!found) {
		snprintf(problem, sizeof(problem),
			 "no icon %s.png of %d pixels or larger", name, size);
		report_icon_problem(&launcher->icon_reports, app_id, problem);
	}
	return NULL;
}

static void release_icons(struct launcher *launcher)
{
	for (size_t i = 0; i < launcher->app_count; ++i) {
		lv_draw_buf_destroy(launcher->apps[i].tile);
		lv_draw_buf_destroy(launcher->apps[i].row_tile);
		launcher->apps[i].tile = NULL;
		launcher->apps[i].row_tile = NULL;
	}
}

static bool read_application(struct launcher *launcher, const char *filename,
			     struct application *app)
{
	struct fplinux_launcher_entry entry;

	memset(app, 0, sizeof(*app));
	if (!fplinux_launcher_read_entry(launcher->desktop_dir, filename,
					 fplinux_ui_locale(launcher->ui), 0,
					 &entry))
		return false;
	strcpy(app->id, entry.id);
	strcpy(app->name, entry.name);
	app->group = entry.group;
	if (!entry.icon_valid) {
		report_icon_problem(&launcher->icon_reports, app->id,
				    "ignoring invalid Icon");
	} else if (entry.icon[0]) {
		app->tile = load_icon(launcher, app->id, entry.icon,
				      launcher->spec->app_icon_size);
		app->row_tile = load_icon(launcher, app->id, entry.icon,
					  launcher->spec->row_icon_size);
	}
	return true;
}

/* Declared group first, then the localized name, then the stable ID. */
static int compare_application(const void *first, const void *second)
{
	const struct application *left = first;
	const struct application *right = second;
	int order;

	if (left->group != right->group)
		return left->group < right->group ? -1 : 1;
	order = fplinux_launcher_compare_names(left->name, right->name);
	return order ? order : strcmp(left->id, right->id);
}

static const char *selected_id(const struct launcher *launcher)
{
	return launcher->app_count ? launcher->apps[launcher->selected].id : "";
}

static void select_id(struct launcher *launcher, const char *id)
{
	for (size_t i = 0; i < launcher->app_count; ++i)
		if (!strcmp(launcher->apps[i].id, id))
			launcher->selected = i;
}

/*
 * SELECT reports the selection, base view and catalogue presentation;
 * a restarted launcher receives them back in ACTIVE.
 */
static void report_view(struct launcher *launcher, bool force)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SELECT,
	};

	if (!launcher->session_known ||
	    (!force && launcher->view == launcher->reported_view &&
	     !strcmp(selected_id(launcher), launcher->reported_selection)))
		return;
	strcpy(message.selected_id, selected_id(launcher));
	strcpy(message.value, fplinux_launcher_view_value(launcher->view));
	strcpy(message.name,
	       fplinux_launcher_presentation_name(launcher->presentation));
	if (!fplinux_ui_send(launcher->ui, &message))
		return;
	launcher->reported_view = launcher->view;
	strcpy(launcher->reported_selection, message.selected_id);
}

static void tasks_view_changed(void *context)
{
	struct launcher *launcher = context;
	bool open = fplinux_launcher_tasks_is_open(&launcher->tasks);
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_SELECT,
		.enabled = open,
		.value = "tasks",
	};

	if (!launcher->session_known || open == launcher->reported_tasks)
		return;
	if (fplinux_ui_send(launcher->ui, &message))
		launcher->reported_tasks = open;
}

static const void *task_icon(const char *app_id, void *context)
{
	const struct launcher *launcher = context;

	for (size_t i = 0; i < launcher->app_count; ++i)
		if (!strcmp(launcher->apps[i].id, app_id))
			return launcher->apps[i].row_tile ?
				       launcher->apps[i].row_tile :
				       launcher->app_glyph;
	return launcher->app_glyph;
}

static void clear_view(struct launcher *launcher)
{
	lv_obj_clean(launcher->body);
	launcher->clock = NULL;
	launcher->date = NULL;
	launcher->hint = NULL;
	launcher->notice_box = NULL;
}

/* Callers redraw with render() after the catalogue has been replaced. */
static void scan_applications(struct launcher *launcher)
{
	char previous_id[FPLINUX_UI_SESSION_ID_BYTES];
	DIR *directory;
	struct dirent *entry;

	strcpy(previous_id, selected_id(launcher));
	/* Images borrow the tile buffers; delete them before freeing. */
	if (launcher->body)
		clear_view(launcher);
	release_icons(launcher);
	begin_icon_reports(&launcher->icon_reports);
	launcher->app_count = 0;
	launcher->selected = 0;
	directory = opendir(launcher->desktop_dir);
	if (!directory)
		return;
	while ((entry = readdir(directory)) &&
	       launcher->app_count < FPLINUX_LAUNCHER_APP_COUNT) {
		if (read_application(launcher, entry->d_name,
				     &launcher->apps[launcher->app_count]))
			++launcher->app_count;
	}
	closedir(directory);
	qsort(launcher->apps, launcher->app_count, sizeof(*launcher->apps),
	      compare_application);
	select_id(launcher, previous_id);
}

/*
 * A tile, or the generic app glyph centred on its place in ink when the app
 * has none.
 */
static void draw_tile(struct launcher *launcher, lv_obj_t *parent,
		      const lv_draw_buf_t *tile,
		      struct fplinux_launcher_rect place, lv_color_t ink)
{
	int size = launcher->spec->row_icon_size;

	if (tile) {
		image(parent, tile, place);
		return;
	}
	if (!launcher->app_glyph)
		return;
	glyph(parent, launcher->app_glyph,
	      (struct fplinux_launcher_rect){
		      .x = place.x + fplinux_launcher_centre(place.width, size),
		      .y = place.y +
			   fplinux_launcher_centre(place.height, size),
		      .width = size,
		      .height = size,
	      },
	      ink);
}

static enum launcher_screen current_screen(const struct launcher *launcher)
{
	if (!launcher->session_known)
		return SCREEN_BLANK;
	if (launcher->locked)
		return SCREEN_LOCK;
	if (launcher->active)
		return SCREEN_APPLICATION;
	return launcher->view == FPLINUX_LAUNCHER_VIEW_HOME ? SCREEN_HOME :
							      SCREEN_CATALOGUE;
}

/*
 * The dialog to show: the manager's battery notice, also above an
 * application where the manager raises the launcher for it, then an
 * application notice on Home and in the catalogue. The lock screen shows
 * none.
 */
static enum fplinux_launcher_notice
shown_notice(const struct launcher *launcher)
{
	enum launcher_screen screen = current_screen(launcher);

	if (screen == SCREEN_BLANK || screen == SCREEN_LOCK)
		return FPLINUX_LAUNCHER_NOTICE_NONE;
	if (launcher->battery_notice != FPLINUX_LAUNCHER_NOTICE_NONE)
		return launcher->battery_notice;
	if (screen == SCREEN_HOME || screen == SCREEN_CATALOGUE)
		return launcher->notice;
	return FPLINUX_LAUNCHER_NOTICE_NONE;
}

/*
 * The battery outline with its cap, five bars and the "!" of the low state,
 * and a zigzag bolt beside it while charging: state is never shown by
 * colour alone.
 */
static void create_battery_icon(struct launcher *launcher, lv_obj_t *bar)
{
	struct battery_icon *icon = &launcher->battery_icon;
	const struct fplinux_launcher_battery *layout =
		&launcher->battery_layout;
	lv_color_t ink = launcher->theme->bar_text;
	lv_color_t danger = launcher->theme->danger;
	size_t points = sizeof(icon->bolt_points) / sizeof(*icon->bolt_points);

	for (size_t i = 0; i < points; ++i)
		icon->bolt_points[i] = (lv_point_precise_t){
			.x = layout->bolt_points[i][0],
			.y = layout->bolt_points[i][1],
		};
	icon->bolt = lv_line_create(bar);
	lv_obj_remove_style_all(icon->bolt);
	lv_obj_set_pos(icon->bolt, layout->bolt.x, layout->bolt.y);
	lv_line_set_points(icon->bolt, icon->bolt_points, (uint32_t)points);
	lv_obj_set_style_line_width(icon->bolt, layout->bolt_line_width, 0);
	lv_obj_set_style_line_color(icon->bolt, ink, 0);
	icon->body = outline(bar, layout->body, layout->outline_radius,
			     layout->outline_width, ink);
	icon->cap = fill(bar, layout->cap, 0, ink);
	for (int i = 0; i < FPLINUX_LAUNCHER_BATTERY_STEP_COUNT; ++i)
		icon->bars[i] = fill(bar, layout->bars[i], 0, ink);
	icon->low_stem = fill(bar, layout->low_stem, 0, danger);
	icon->low_dot = fill(bar, layout->low_dot, 0, danger);
}

static void update_battery_icon(struct launcher *launcher)
{
	struct battery_icon *icon = &launcher->battery_icon;
	bool shown = launcher->battery >= 0;
	int percent = launcher->battery;
	int steps = shown ? fplinux_launcher_battery_steps(percent) : 0;
	bool low = shown && percent < FPLINUX_LAUNCHER_BATTERY_LOW_PERCENT;
	bool charging = shown && launcher->charging;
	lv_color_t ink = low ? launcher->theme->danger :
			       launcher->theme->bar_text;

	if (icon->drawn && icon->shown == shown && icon->steps == steps &&
	    icon->low == low && icon->charging == charging)
		return;
	lv_obj_set_hidden(icon->body, !shown);
	lv_obj_set_hidden(icon->cap, !shown);
	lv_obj_set_hidden(icon->bolt, !charging);
	lv_obj_set_hidden(icon->low_stem, !low);
	lv_obj_set_hidden(icon->low_dot, !low);
	for (int i = 0; i < FPLINUX_LAUNCHER_BATTERY_STEP_COUNT; ++i)
		lv_obj_set_hidden(icon->bars[i], i >= steps);
	lv_obj_set_style_border_color(icon->body, ink, 0);
	lv_obj_set_style_bg_color(icon->cap, ink, 0);
	icon->drawn = true;
	icon->shown = shown;
	icon->steps = steps;
	icon->low = low;
	icon->charging = charging;
}

/*
 * The long date on one line when it fits the text width, else split after
 * the weekday when both lines fit, else the short date.
 */
static void format_home_date(struct launcher *launcher, time_t now, char *date,
			     size_t capacity)
{
	const lv_font_t *font = launcher->theme->font;
	int width = fplinux_launcher_text_width(launcher->spec,
						launcher->frame.width);
	char split[128];

	if (!fplinux_ui_format_date(launcher->ui, now, FPLINUX_UI_DATE_LONG,
				    date, capacity))
		date[0] = '\0';
	if (text_width(date, font) <= width)
		return;
	if (fplinux_launcher_split_date(date, split, sizeof(split)) &&
	    text_width(split, font) <= width) {
		snprintf(date, capacity, "%s", split);
		return;
	}
	if (!fplinux_ui_format_date(launcher->ui, now, FPLINUX_UI_DATE_SHORT,
				    date, capacity))
		date[0] = '\0';
}

/* Centres the clock, date and hint stack in the body. */
static void place_home(struct launcher *launcher)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	int width = fplinux_launcher_text_width(launcher->spec,
						launcher->frame.width);
	/* Sizes from the fonts: the layout has not run on new labels yet. */
	int clock = lv_font_get_line_height(
		lv_obj_get_style_text_font(launcher->clock, LV_PART_MAIN));
	int date = launcher->date ?
			   text_height(lv_label_get_text(launcher->date),
				       theme->font, width) :
			   0;
	int hint = launcher->hint ?
			   text_height(lv_label_get_text(launcher->hint),
				       theme->font, width) :
			   0;
	struct fplinux_launcher_home home;

	fplinux_launcher_home_layout(launcher->spec,
				     launcher->frame.body_height, clock, date,
				     hint, &home);
	lv_obj_set_y(launcher->clock, home.clock_y);
	if (launcher->date)
		lv_obj_set_y(launcher->date, home.date_y);
	if (launcher->hint)
		lv_obj_set_y(launcher->hint, home.hint_y);
}

/* The clock in the status bar, centred on the full width at its own width. */
static void set_status_clock(struct launcher *launcher, const char *text)
{
	int width = text_width(text, launcher->theme->font);

	set_label(launcher->status_clock, text);
	lv_obj_set_x(launcher->status_clock,
		     fplinux_launcher_centre(launcher->frame.width, width));
	lv_obj_set_width(launcher->status_clock, width);
	lv_obj_set_hidden(launcher->status_clock, !width);
}

/* Home and the lock screen show the large clock instead of the status one. */
static void update_clock(struct launcher *launcher)
{
	enum launcher_screen screen = current_screen(launcher);
	bool status_clock = screen == SCREEN_APPLICATION ||
			    screen == SCREEN_CATALOGUE;
	time_t now = time(NULL);
	bool clock_ready = fplinux_launcher_clock_is_ready(now);
	char clock_text[16] = "--:--";
	char date[128] = "";

	if (clock_ready &&
	    !fplinux_ui_format_date(launcher->ui, now, FPLINUX_UI_DATE_CLOCK,
				    clock_text, sizeof(clock_text)))
		clock_text[0] = '\0';
	set_status_clock(launcher, status_clock ? clock_text : "");
	if (!launcher->clock)
		return;
	set_label(launcher->clock, clock_text);
	if (launcher->date) {
		if (clock_ready)
			format_home_date(launcher, now, date, sizeof(date));
		if (!strcmp(lv_label_get_text(launcher->date), date))
			return;
		set_label(launcher->date, date);
	}
	place_home(launcher);
}

static void schedule_clock(struct launcher *launcher)
{
	struct timespec now = { 0 };

	clock_gettime(CLOCK_REALTIME, &now);
	lv_timer_set_period(launcher->clock_timer,
			    fplinux_launcher_clock_delay(&now));
	lv_timer_reset(launcher->clock_timer);
}

/*
 * A focused text that does not fit scrolls to its end and back after
 * 400 ms, with 1 s pauses, while it keeps focus; a paused launcher keeps it
 * still.
 */
static void set_marquee(struct launcher *launcher, lv_obj_t *name)
{
	if (launcher->paused) {
		lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_CLIP);
		return;
	}
	lv_obj_set_style_anim(name, &launcher->title_animation, 0);
	lv_obj_set_style_anim_duration(
		name,
		lv_anim_speed_clamped(FPLINUX_LAUNCHER_MARQUEE_PIXELS_PER_SECOND,
				      FPLINUX_LAUNCHER_MARQUEE_MIN_MS,
				      FPLINUX_LAUNCHER_MARQUEE_MAX_MS),
		0);
	lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_SCROLL);
}

/*
 * Clock, date and, on the lock screen, the unlock hint, centred one under
 * the other; where the profile says so the hint takes the date's place.
 */
static void draw_clock_face(struct launcher *launcher)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const lv_font_t *clock_font = theme->font_clock ? theme->font_clock :
							  theme->font_large;
	struct fplinux_launcher_rect text = {
		.x = launcher->spec->margin,
		.width = fplinux_launcher_text_width(launcher->spec,
						     launcher->frame.width),
	};
	enum fplinux_launcher_notice hint = launcher->hint_notice;
	bool hinted = current_screen(launcher) == SCREEN_LOCK &&
		      hint != FPLINUX_LAUNCHER_NOTICE_NONE;

	launcher->clock =
		label(launcher->body,
		      (struct fplinux_launcher_rect){
			      .x = text.x,
			      .width = text.width,
			      .height = lv_font_get_line_height(clock_font),
		      },
		      clock_font, theme->foreground, "");
	lv_obj_set_style_text_align(launcher->clock, LV_TEXT_ALIGN_CENTER, 0);
	if (!hinted || !launcher->spec->home_hint_replaces_date) {
		launcher->date = label(launcher->body, text, theme->font,
				       theme->foreground, "");
		lv_label_set_long_mode(launcher->date, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(launcher->date,
					    LV_TEXT_ALIGN_CENTER, 0);
	}
	if (hinted) {
		launcher->hint = label(launcher->body, text, theme->font,
				       theme->muted,
				       _(fplinux_launcher_notice_body(hint)));
		lv_label_set_long_mode(launcher->hint, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(launcher->hint,
					    LV_TEXT_ALIGN_CENTER, 0);
	}
	place_home(launcher);
}

/* The header band component with a centred title. */
static void draw_band(struct launcher *launcher, const char *title,
		      bool marquee)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	int width = launcher->frame.width;
	int height = launcher->frame.band_height;
	lv_obj_t *band = fill(launcher->body,
			      (struct fplinux_launcher_rect){
				      .width = width,
				      .height = height,
			      },
			      0, theme->surface);
	lv_obj_t *name = label(
		band,
		(struct fplinux_launcher_rect){
			.x = spec->margin,
			.y = fplinux_launcher_centre(
				height,
				lv_font_get_line_height(theme->font_large)),
			.width = fplinux_launcher_text_width(spec, width),
			.height = lv_font_get_line_height(theme->font_large),
		},
		theme->font_large, theme->foreground, title);

	fill(band,
	     (struct fplinux_launcher_rect){
		     .y = height - spec->divider_width,
		     .width = width,
		     .height = spec->divider_width,
	     },
	     0, theme->border);
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	if (marquee)
		set_marquee(launcher, name);
	else
		lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
}

/*
 * A tile with its selection: a transparent ring of grid_ring_width in
 * the ring colour, the page showing through the gap inside it, and the tile.
 * Where the pitch leaves no room outside the tile, the selected tile shows
 * only its inner part, its edge given to the gap.
 */
static void draw_grid_cell(struct launcher *launcher, lv_obj_t *viewport,
			   const struct fplinux_launcher_scroll *scroll,
			   size_t index, bool selected)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	const struct application *app = &launcher->apps[index];
	size_t columns = fplinux_launcher_columns(spec, true);
	struct fplinux_launcher_cell cell;
	struct fplinux_launcher_rect shown;
	lv_obj_t *visible;

	fplinux_launcher_grid_cell(spec, launcher->frame.width, scroll,
				   index % columns, index / columns, &cell);
	if (!selected) {
		draw_tile(launcher, viewport, app->tile, cell.tile.box,
			  theme->foreground);
		return;
	}
	outline(viewport, cell.ring.box, cell.ring.radius,
		spec->grid_ring_width,
		token_colour(theme, spec->grid_ring_colour));
	if (!cell.inset || !app->tile) {
		draw_tile(launcher, viewport, app->tile, cell.tile.box,
			  theme->foreground);
		return;
	}
	shown = cell.tile.box;
	shown.x += cell.inset;
	shown.y += cell.inset;
	shown.width -= 2 * cell.inset;
	shown.height -= 2 * cell.inset;
	visible = plain(viewport, shown);
	lv_obj_set_style_radius(visible, cell.tile.radius - cell.inset, 0);
	lv_obj_set_style_clip_corner(visible, true, 0);
	image(visible, app->tile,
	      (struct fplinux_launcher_rect){
		      .x = -cell.inset,
		      .y = -cell.inset,
		      .width = cell.tile.box.width,
		      .height = cell.tile.box.height,
	      });
}

/*
 * The list row component with the app's tile: a selected row is
 * inverted over its pitch and its tile gets the tile_ring around it, a
 * shape filled under the tile.
 */
static void draw_list_row(struct launcher *launcher, lv_obj_t *viewport,
			  size_t index, bool selected)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	const struct application *app = &launcher->apps[index];
	lv_color_t ink = selected ? theme->accent_text : theme->foreground;
	struct fplinux_launcher_row layout;
	lv_obj_t *row;
	lv_obj_t *icon;
	lv_obj_t *name;

	fplinux_launcher_list_row(spec, launcher->frame.width,
				  lv_font_get_line_height(theme->font), index,
				  &layout);
	row = plain(viewport, layout.row);
	lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(row, spec->divider_width, 0);
	lv_obj_set_style_border_color(
		row, selected ? theme->accent : theme->border, 0);
	if (selected) {
		lv_obj_set_style_bg_color(row, theme->accent, 0);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
	}
	/*
	 * The icon box starts every row's content; the ring is filled in it
	 * under the tile and reaches out of it by the box's extra draw size,
	 * to which LVGL 9 clips the children of an overflowing object.
	 */
	icon = plain(row, layout.icon.box);
	if (selected && app->row_tile) {
		struct fplinux_launcher_rect ring = layout.ring.box;
		int reach = layout.icon.box.x - ring.x;

		lv_obj_set_overflow_visible(icon, true);
		lv_obj_set_ext_draw_size(icon, reach);
		lv_obj_refresh_ext_draw_size(icon);
		ring.x = -reach;
		ring.y = -reach;
		fill(icon, ring, layout.ring.radius,
		     token_colour(theme, spec->list_tile_ring_colour));
	}
	draw_tile(launcher, icon, app->row_tile,
		  (struct fplinux_launcher_rect){
			  .width = layout.icon.box.width,
			  .height = layout.icon.box.height,
		  },
		  ink);
	name = label(row, layout.text, theme->font, ink, app->name);
	if (selected)
		set_marquee(launcher, name);
	else
		lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
}

/* The catalogue's scrolling content in the current presentation. */
static void catalogue_scroll(const struct launcher *launcher,
			     struct fplinux_launcher_scroll *scroll)
{
	fplinux_launcher_scroll_layout(
		launcher->spec, launcher->frame.width,
		launcher->frame.viewport_height,
		launcher->view == FPLINUX_LAUNCHER_VIEW_GRID, scroll);
}

/*
 * The grid's scroll indicator track: border under the thumb,
 * which the viewport draws after its content.
 */
static void draw_grid_track(lv_event_t *event)
{
	const struct launcher *launcher = lv_event_get_user_data(event);
	struct fplinux_launcher_rect track =
		fplinux_launcher_grid_track(launcher->spec, &launcher->frame);
	lv_draw_rect_dsc_t style;
	lv_area_t area;

	lv_obj_get_coords(lv_event_get_target_obj(event), &area);
	area.x1 += track.x;
	area.y1 += track.y;
	area.x2 = area.x1 + track.width - 1;
	area.y2 = area.y1 + track.height - 1;
	lv_draw_rect_dsc_init(&style);
	style.bg_color = launcher->theme->border;
	style.bg_opa = LV_OPA_COVER;
	lv_draw_rect(lv_event_get_layer(event), &style, &area);
}

/*
 * The catalogue: the band with the selected app's name (grid) or Apps
 * (list), and a viewport that scrolls by the fewest pixels that keep the
 * selection visible, with the scroll indicator of the runtime's lists, in
 * the grid over a track and clear of the selection ring.
 */
static void draw_catalogue(struct launcher *launcher, bool focus)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	const struct fplinux_launcher_frame *frame = &launcher->frame;
	bool grid = launcher->view == FPLINUX_LAUNCHER_VIEW_GRID;
	size_t columns = fplinux_launcher_columns(spec, grid);
	size_t total_rows = (launcher->app_count + columns - 1) / columns;
	struct fplinux_launcher_rect area = fplinux_launcher_viewport(frame);
	struct fplinux_launcher_scroll scroll;
	lv_obj_t *viewport;
	int content;

	/* The name moves only while the catalogue has the focus. */
	if (grid && launcher->app_count)
		draw_band(launcher, launcher->apps[launcher->selected].name,
			  focus);
	else
		draw_band(launcher, _("Apps"), false);
	if (!launcher->app_count) {
		int width = fplinux_launcher_text_width(spec, frame->width);
		const char *text = _("No apps installed");
		lv_obj_t *empty = label(
			launcher->body,
			(struct fplinux_launcher_rect){
				.x = spec->margin,
				.y = frame->band_height +
				     fplinux_launcher_centre(
					     frame->viewport_height,
					     text_height(text, theme->font,
							 width)),
				.width = width,
			},
			theme->font, theme->muted, text);

		lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		return;
	}
	viewport = plain(launcher->body, area);
	catalogue_scroll(launcher, &scroll);
	lv_obj_set_scrollable(viewport, true);
	lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_bg_color(viewport, theme->focus, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(viewport, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(viewport, spec->scrollbar_width,
			       LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(
		viewport,
		grid ? spec->grid_scrollbar_offset :
		       (spec->margin - spec->scrollbar_width) / 2,
		LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_ver(viewport, spec->list_padding,
				 LV_PART_SCROLLBAR);
	content = fplinux_launcher_content_height(&scroll, total_rows);
	if (grid && content > area.height)
		lv_obj_add_event_cb(viewport, draw_grid_track,
				    LV_EVENT_DRAW_MAIN_END, launcher);
	for (size_t i = 0; i < launcher->app_count; ++i) {
		bool selected = focus && i == launcher->selected;

		if (grid)
			draw_grid_cell(launcher, viewport, &scroll, i,
				       selected);
		else
			draw_list_row(launcher, viewport, i, selected);
	}
	/*
	 * LVGL scrolls to the lowest child plus the bottom padding. A pixel at
	 * the layout's content end, without padding, makes that the content's
	 * height: the selected ring below the last row stays inside it (the
	 * padding is wider than the ring's reach), so it never adds an offset
	 * the layout does not know, nor a scroll indicator without overflow.
	 */
	plain(viewport, (struct fplinux_launcher_rect){
				.y = content - 1,
				.width = 1,
				.height = 1,
			});
	launcher->scroll_offset = fplinux_launcher_scroll_offset(
		&scroll, total_rows, area.height, launcher->scroll_offset,
		launcher->selected / columns);
	lv_obj_scroll_to_y(viewport, launcher->scroll_offset, LV_ANIM_OFF);
}

static void draw_dialog(struct launcher *launcher, const char *heading,
			const char *text, bool critical)
{
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	lv_color_t kind = critical ? theme->danger : theme->focus;
	struct fplinux_launcher_notice_box box;
	/* The notice owns a static body-only scrim; bars remain readable. */
	lv_obj_t *scrim = plain(launcher->body,
				(struct fplinux_launcher_rect){
					.width = launcher->frame.width,
					.height = launcher->frame.body_height,
				});
	lv_obj_t *dialog = lv_obj_create(scrim);
	lv_obj_t *title;
	lv_obj_t *body;

	lv_obj_set_style_bg_color(scrim, theme->scrim, 0);
	lv_obj_set_style_bg_opa(scrim, theme->scrim_opacity, 0);
	fplinux_launcher_notice_layout(spec, launcher->frame.width,
				       launcher->frame.body_height, &box);
	lv_obj_remove_style_all(dialog);
	lv_obj_set_size(dialog, box.width, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(dialog, box.max_height, 0);
	lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(dialog, box.padding, 0);
	lv_obj_set_style_pad_row(dialog, box.gap, 0);
	lv_obj_set_style_bg_color(dialog, theme->popup, 0);
	lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(dialog, kind, 0);
	lv_obj_set_style_border_width(dialog, box.border, 0);
	lv_obj_set_style_text_font(dialog, theme->font, 0);
	lv_obj_set_scrollable(dialog, true);
	lv_obj_set_scroll_dir(dialog, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(dialog, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_bg_color(dialog, theme->focus, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(dialog, spec->scrollbar_width,
			       LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(dialog, box.scrollbar_inset,
				   LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_ver(dialog, spec->content_padding,
				 LV_PART_SCROLLBAR);
	title = lv_label_create(dialog);
	lv_obj_set_width(title, lv_pct(100));
	lv_obj_set_style_text_font(title, theme->font_large, 0);
	lv_obj_set_style_text_color(title, critical ? kind : theme->foreground,
				    0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(title, heading);
	body = lv_label_create(dialog);
	lv_obj_set_width(body, lv_pct(100));
	lv_obj_set_style_text_color(body, theme->foreground, 0);
	lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
	lv_label_set_text(body, text);
	lv_obj_center(dialog);
	launcher->notice_box = dialog;
}

static void render(struct launcher *launcher)
{
	enum launcher_screen screen = current_screen(launcher);
	enum fplinux_launcher_notice notice = shown_notice(launcher);
	const char *legends[FPLINUX_LAUNCHER_LEGEND_COUNT] = { "", "", "" };

	fplinux_launcher_tasks_set_blocked(
		&launcher->tasks,
		(screen != SCREEN_HOME && screen != SCREEN_CATALOGUE) ||
			notice != FPLINUX_LAUNCHER_NOTICE_NONE ||
			launcher->end_prompt);
	clear_view(launcher);
	if (launcher->lock_glyph)
		lv_obj_set_hidden(launcher->lock_glyph, screen != SCREEN_LOCK);
	switch (screen) {
	case SCREEN_BLANK:
		break;
	case SCREEN_LOCK:
		draw_clock_face(launcher);
		legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] = _("Unlock");
		break;
	case SCREEN_APPLICATION:
		if (launcher->end_prompt && launcher->backdrop.pixels)
			image(launcher->body, &launcher->backdrop_image,
			      (struct fplinux_launcher_rect){
				      .y = -launcher->frame.body_y,
				      .width = launcher->frame.width,
				      .height = launcher->frame.height,
			      });
		legends[FPLINUX_LAUNCHER_LEGEND_LEFT] = launcher->active_left;
		legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] =
			launcher->active_centre;
		legends[FPLINUX_LAUNCHER_LEGEND_RIGHT] = launcher->active_right;
		break;
	case SCREEN_HOME:
		draw_clock_face(launcher);
		legends[FPLINUX_LAUNCHER_LEGEND_LEFT] = _("Tasks");
		legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] = _("Menu");
		break;
	case SCREEN_CATALOGUE:
		draw_catalogue(launcher,
			       notice == FPLINUX_LAUNCHER_NOTICE_NONE);
		/* An empty catalogue offers only Back. */
		if (launcher->app_count) {
			legends[FPLINUX_LAUNCHER_LEGEND_LEFT] =
				launcher->view == FPLINUX_LAUNCHER_VIEW_GRID ?
					_("List") :
					_("Icons");
			legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] = _("Open");
		}
		legends[FPLINUX_LAUNCHER_LEGEND_RIGHT] = _("Back");
		break;
	}
	/* The page's texts are final before a notice covers part of them. */
	update_clock(launcher);
	if (launcher->end_prompt) {
		draw_dialog(launcher, launcher->active_title,
			    _("Minimize or close the application?"), false);
		legends[FPLINUX_LAUNCHER_LEGEND_LEFT] = _("Minimize");
		legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] = _("Cancel");
		legends[FPLINUX_LAUNCHER_LEGEND_RIGHT] = _("Close");
	} else if (notice != FPLINUX_LAUNCHER_NOTICE_NONE) {
		draw_dialog(launcher, _(fplinux_launcher_notice_title(notice)),
			    _(fplinux_launcher_notice_body(notice)),
			    notice == FPLINUX_LAUNCHER_NOTICE_BATTERY_CRITICAL);
		legends[FPLINUX_LAUNCHER_LEGEND_LEFT] = "";
		legends[FPLINUX_LAUNCHER_LEGEND_CENTRE] = "";
		legends[FPLINUX_LAUNCHER_LEGEND_RIGHT] = _("Close");
	}
	fplinux_ui_softkey_bar_set(launcher->ui, launcher->softkeys,
				   legends[FPLINUX_LAUNCHER_LEGEND_LEFT],
				   legends[FPLINUX_LAUNCHER_LEGEND_CENTRE],
				   legends[FPLINUX_LAUNCHER_LEGEND_RIGHT]);
}

static void activate_selected(struct launcher *launcher)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_LAUNCH,
	};
	struct fplinux_launcher_launch layout;
	const struct application *app;
	lv_draw_buf_t *snapshot;
	char text[2 * FPLINUX_UI_SESSION_TEXT_BYTES];
	lv_obj_t *name;

	if (!launcher->app_count)
		return;
	app = &launcher->apps[launcher->selected];
	strcpy(message.app_id, app->id);
	/* Capture off the Wayland path; fast apps never show this temporary view. */
	fplinux_launcher_launch_layout(launcher->spec, &launcher->frame,
				       &layout);
	clear_view(launcher);
	draw_tile(launcher, launcher->body, app->tile, layout.icon,
		  launcher->theme->foreground);
	snprintf(text, sizeof(text), _("Starting %s…"), app->name);
	name = label(launcher->body, layout.text, launcher->theme->font,
		     launcher->theme->foreground, text);
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
	fplinux_ui_softkey_bar_set(launcher->ui, launcher->softkeys, "", "",
				   _("Cancel"));
	lv_obj_update_layout(fplinux_ui_screen(launcher->ui));
	snapshot = lv_snapshot_take(fplinux_ui_screen(launcher->ui),
				    LV_COLOR_FORMAT_RGB565);
	if (snapshot && snapshot->header.w == (uint32_t)launcher->frame.width &&
	    snapshot->header.h == (uint32_t)launcher->frame.height) {
		message.frame_fd = fplinux_launcher_launch_fd(
			snapshot->data, snapshot->header.stride,
			launcher->frame.width, launcher->frame.height);
		if (message.frame_fd >= 0) {
			message.has_frame = true;
			message.width = launcher->frame.width;
			message.height = launcher->frame.height;
		}
	}
	if (snapshot)
		lv_draw_buf_destroy(snapshot);
	/* The ordinary catalogue is the real first frame until the 200 ms deadline. */
	render(launcher);
	fplinux_ui_send(launcher->ui, &message);
	if (message.has_frame)
		close(message.frame_fd);
}

/* D-pad keys move the selection; digits 1-9 pick from the page. */
static void move_selection(struct launcher *launcher,
			   const struct fplinux_ui_key *key)
{
	static const struct {
		unsigned int code;
		enum fplinux_launcher_move move;
	} moves[] = {
		{ KEY_UP, FPLINUX_LAUNCHER_MOVE_UP },
		{ KEY_DOWN, FPLINUX_LAUNCHER_MOVE_DOWN },
		{ KEY_LEFT, FPLINUX_LAUNCHER_MOVE_LEFT },
		{ KEY_RIGHT, FPLINUX_LAUNCHER_MOVE_RIGHT },
	};
	size_t columns = fplinux_launcher_columns(
		launcher->spec, launcher->view == FPLINUX_LAUNCHER_VIEW_GRID);
	size_t rows = (launcher->app_count + columns - 1) / columns;
	struct fplinux_launcher_scroll scroll;

	if (!launcher->app_count)
		return;
	catalogue_scroll(launcher, &scroll);
	for (size_t i = 0; i < sizeof(moves) / sizeof(*moves); ++i)
		if (key->code == moves[i].code) {
			launcher->selected = fplinux_launcher_move(
				launcher->selected, launcher->app_count,
				columns,
				fplinux_launcher_page_rows(
					&scroll,
					launcher->frame.viewport_height),
				moves[i].move);
			return;
		}
	if (key->source == FPLINUX_UI_KEY_PHONE && key->code >= KEY_NUMERIC_1 &&
	    key->code <= KEY_NUMERIC_9) {
		size_t index = fplinux_launcher_digit_index(
			fplinux_launcher_first_visible_row(
				&scroll, rows, launcher->scroll_offset),
			columns, (int)(key->code - KEY_NUMERIC_1) + 1,
			launcher->app_count);

		if (index < launcher->app_count)
			launcher->selected = index;
	}
}

/* Close answers a battery notice to the manager, which owns it. */
static void dismiss_battery_notice(struct launcher *launcher)
{
	struct fplinux_ui_session_message message = {
		.type = FPLINUX_UI_SESSION_DISMISS,
	};

	snprintf(message.notice, sizeof(message.notice), "%s",
		 fplinux_launcher_notice_code(launcher->battery_notice));
	/* Unanswered, the manager would keep the launcher on top. */
	if (!fplinux_ui_send(launcher->ui, &message))
		return;
	launcher->dismissed_notice = launcher->battery_notice;
	launcher->battery_notice = FPLINUX_LAUNCHER_NOTICE_NONE;
}

static void handle_key(struct launcher *launcher,
		       const struct fplinux_ui_key *key)
{
	bool confirm = key->code == KEY_OK || key->code == KEY_ENTER ||
		       key->code == KEY_KPENTER;
	bool left = fplinux_ui_is_softkey(key, true);
	bool back = fplinux_ui_is_softkey(key, false) || key->code == KEY_ESC;
	enum launcher_screen screen = current_screen(launcher);
	enum fplinux_launcher_notice notice = shown_notice(launcher);

	if (!key->pressed)
		return;
	if (fplinux_ui_current(launcher->ui) != launcher->canvas)
		return;
	if (launcher->end_prompt) {
		struct fplinux_ui_session_message message = {
			.type = FPLINUX_UI_SESSION_END,
			.token = launcher->active_token,
		};

		if (left)
			strcpy(message.value, "minimize");
		else if (fplinux_ui_is_softkey(key, false))
			strcpy(message.value, "close");
		else if (confirm || key->code == KEY_ESC)
			strcpy(message.value, "cancel");
		else {
			fplinux_ui_scroll_key(launcher->notice_box, key);
			return;
		}
		strcpy(message.app_id, launcher->active_id);
		fplinux_ui_send(launcher->ui, &message);
		return;
	}
	if (notice != FPLINUX_LAUNCHER_NOTICE_NONE) {
		if (!back) {
			/* A long notice scrolls; nothing else reacts. */
			fplinux_ui_scroll_key(launcher->notice_box, key);
			return;
		}
		if (notice == launcher->battery_notice)
			dismiss_battery_notice(launcher);
		else
			launcher->notice = FPLINUX_LAUNCHER_NOTICE_NONE;
	} else if (screen != SCREEN_HOME && screen != SCREEN_CATALOGUE) {
		return;
	} else if (screen == SCREEN_HOME) {
		if (left || key->code == KEY_TAB) {
			fplinux_launcher_tasks_open(&launcher->tasks);
			return;
		}
		if (!confirm)
			return;
		/* Without a working watch, opening the catalogue refreshes it. */
		if (!launcher->watching)
			scan_applications(launcher);
		launcher->view = launcher->presentation;
	} else if (back) {
		launcher->view = FPLINUX_LAUNCHER_VIEW_HOME;
	} else if (left || key->code == KEY_TAB) {
		/* Like its legend, switching needs an app to show. */
		if (!launcher->app_count)
			return;
		launcher->presentation =
			launcher->view == FPLINUX_LAUNCHER_VIEW_GRID ?
				FPLINUX_LAUNCHER_VIEW_LIST :
				FPLINUX_LAUNCHER_VIEW_GRID;
		launcher->view = launcher->presentation;
		launcher->scroll_offset = 0;
	} else if (confirm) {
		activate_selected(launcher);
		return;
	} else {
		move_selection(launcher, key);
	}
	report_view(launcher, false);
	render(launcher);
}

static void canvas_key(struct fplinux_ui *ui, struct fplinux_ui_screen *canvas,
		       const struct fplinux_ui_key *key, void *context)
{
	(void)ui;
	(void)canvas;
	handle_key(context, key);
}

static void clock_tick(lv_timer_t *timer)
{
	struct launcher *launcher = lv_timer_get_user_data(timer);

	update_clock(launcher);
	schedule_clock(launcher);
}

/* Returns whether the paused state changed, which needs a redraw. */
static bool update_paused(struct launcher *launcher)
{
	bool paused = launcher->display == FPLINUX_UI_SESSION_DISPLAY_OFF ||
		      !launcher->visible;

	if (paused == launcher->paused)
		return false;
	launcher->paused = paused;
	if (paused) {
		lv_timer_pause(launcher->clock_timer);
		return true;
	}
	/* The clock may have moved on while the display was off. */
	schedule_clock(launcher);
	lv_timer_resume(launcher->clock_timer);
	return true;
}

/*
 * Unlock hints follow the manager's current reason. Other notices are raised
 * when the reason changes to their code from the previous non-hint reason and
 * stay until Close, so the same code repeated by later ACTIVE messages, also
 * around unlock hints, does not raise them again.
 */
static void receive_reason(struct launcher *launcher, const char *reason)
{
	enum fplinux_launcher_notice notice =
		fplinux_launcher_notice_from_code(reason);

	if (fplinux_launcher_notice_is_unlock_hint(notice)) {
		launcher->hint_notice = notice;
		return;
	}
	launcher->hint_notice = FPLINUX_LAUNCHER_NOTICE_NONE;
	if (notice != FPLINUX_LAUNCHER_NOTICE_NONE &&
	    strcmp(reason, launcher->notice_reason)) {
		if (notice == FPLINUX_LAUNCHER_NOTICE_FAILED)
			fprintf(stderr, "fplinux-launcher: unknown notice %s\n",
				reason);
		launcher->notice = notice;
	}
	copy_message_text(launcher->notice_reason, reason);
}

/*
 * The manager reports the battery and its notice in every ACTIVE. A notice
 * closed here stays closed until the manager has seen DISMISS, that is until
 * an ACTIVE carries another notice or none.
 */
static void receive_battery(struct launcher *launcher,
			    const struct fplinux_ui_session_message *message)
{
	enum fplinux_launcher_notice notice =
		fplinux_launcher_notice_from_code(message->notice);

	launcher->battery = message->battery;
	launcher->charging = message->charging;
	if (notice != FPLINUX_LAUNCHER_NOTICE_BATTERY_LOW &&
	    notice != FPLINUX_LAUNCHER_NOTICE_BATTERY_CRITICAL)
		notice = FPLINUX_LAUNCHER_NOTICE_NONE;
	if (notice == launcher->dismissed_notice &&
	    notice != FPLINUX_LAUNCHER_NOTICE_NONE)
		notice = FPLINUX_LAUNCHER_NOTICE_NONE;
	else
		launcher->dismissed_notice = FPLINUX_LAUNCHER_NOTICE_NONE;
	launcher->battery_notice = notice;
	update_battery_icon(launcher);
}

static void receive_active(struct launcher *launcher,
			   const struct fplinux_ui_session_message *message)
{
	bool first = !launcher->session_known;
	bool was_active = launcher->active;
	bool rescan = false;

	launcher->session_known = true;
	launcher->active = message->active_id[0] != '\0';
	launcher->locked = message->locked;
	launcher->end_prompt = message->end_prompt && launcher->active &&
			       !launcher->locked;
	if (launcher->backdrop.pixels &&
	    (!launcher->end_prompt ||
	     launcher->backdrop_token != message->token)) {
		/* Body images borrow the mapping, so delete them before unmapping. */
		clear_view(launcher);
		fplinux_launcher_backdrop_clear(&launcher->backdrop);
		launcher->backdrop_token = 0;
	}
	if (launcher->end_prompt && message->has_frame &&
	    !launcher->backdrop.pixels &&
	    message->width == launcher->frame.width &&
	    message->height == launcher->frame.height &&
	    fplinux_launcher_backdrop_map(&launcher->backdrop,
					  message->frame_fd, message->width,
					  message->height)) {
		launcher->backdrop_token = message->token;
		launcher->backdrop_image = (lv_image_dsc_t){
			.header = {
				.magic = LV_IMAGE_HEADER_MAGIC,
				.cf = LV_COLOR_FORMAT_RGB565,
				.w = (uint32_t)message->width,
				.h = (uint32_t)message->height,
				.stride = (uint32_t)message->width * 2U,
			},
			.data_size = (uint32_t)launcher->backdrop.bytes,
			.data = launcher->backdrop.pixels,
		};
	}
	launcher->active_token = message->token;
	snprintf(launcher->active_id, sizeof(launcher->active_id), "%s",
		 message->active_id);
	copy_message_text(launcher->active_title, message->title);
	launcher->display = message->display;
	copy_message_text(launcher->active_left, message->left);
	copy_message_text(launcher->active_centre, message->centre);
	copy_message_text(launcher->active_right, message->right);
	receive_reason(launcher, message->reason);
	receive_battery(launcher, message);
	/*
	 * A launcher restarted after a fullscreen application, a crash or a
	 * language change opens the view the manager last learned about.
	 */
	if (first && fplinux_launcher_view_from_names(
			     message->value, message->name, &launcher->view,
			     &launcher->presentation))
		launcher->reported_view = launcher->view;
	/* EXIT returns to the catalogue, HOME to the home view. */
	if (was_active && !launcher->active) {
		launcher->view = launcher->home_pending ?
					 FPLINUX_LAUNCHER_VIEW_HOME :
					 launcher->presentation;
		launcher->home_pending = false;
		rescan = true;
	}
	/* The watch keeps the catalogue current; see handle_key(). */
	if (rescan && !launcher->watching)
		scan_applications(launcher);
	/* The manager keeps the selection across launcher restarts and apps. */
	if (first || (was_active && !launcher->active)) {
		select_id(launcher, message->selected_id);
		strcpy(launcher->reported_selection, selected_id(launcher));
	}
	update_paused(launcher);
	render(launcher);
	/* Draw after this ACTIVE; the runtime binds its ACK to a new commit. */
	lv_refr_now(NULL);
	struct fplinux_ui_session_message ack = {
		.type = FPLINUX_UI_SESSION_ACTIVE_ACK,
		.serial = message->serial,
	};
	fplinux_ui_send(launcher->ui, &ack);
}

static void receive_home(struct launcher *launcher)
{
	fplinux_launcher_tasks_close(&launcher->tasks);
	/* The manager assumes Home once it has sent HOME. */
	launcher->reported_view = FPLINUX_LAUNCHER_VIEW_HOME;
	if (launcher->active) {
		launcher->home_pending = true;
		return;
	}
	launcher->view = FPLINUX_LAUNCHER_VIEW_HOME;
	/* Confirm after any SELECT that crossed HOME on the way. */
	report_view(launcher, true);
	render(launcher);
}

static void ui_event(struct fplinux_ui *ui,
		     const struct fplinux_ui_event *event, void *context)
{
	struct launcher *launcher = context;
	/* Set for session events only. */
	const struct fplinux_ui_session_message *message = event->session;

	(void)ui;
	if (!launcher->body)
		return;
	if (event->type == FPLINUX_UI_EVENT_VISIBILITY) {
		launcher->visible = event->visible;
		if (update_paused(launcher))
			render(launcher);
	} else if (event->type != FPLINUX_UI_EVENT_SESSION) {
		return;
	} else if (fplinux_launcher_tasks_receive(&launcher->tasks, message)) {
		return;
	} else if (message->type == FPLINUX_UI_SESSION_ACTIVE) {
		receive_active(launcher, message);
	} else if (message->type == FPLINUX_UI_SESSION_HOME) {
		receive_home(launcher);
	} else if (message->type == FPLINUX_UI_SESSION_ERROR) {
		/* An error repeats even when its code matches the last one. */
		launcher->notice_reason[0] = '\0';
		receive_reason(launcher, message->reason);
		render(launcher);
	}
}

static void catalogue_changed(struct fplinux_ui *ui, int fd, short events,
			      void *context)
{
	struct launcher *launcher = context;
	char buffer[4096];

	(void)ui;
	if (!(events & POLLIN))
		return;
	while (read(fd, buffer, sizeof(buffer)) > 0)
		;
	/* Coalesce a package's desktop entry and icons into one reload. */
	lv_timer_reset(launcher->reload_timer);
	lv_timer_resume(launcher->reload_timer);
}

static void reload_catalogue(lv_timer_t *timer)
{
	struct launcher *launcher = lv_timer_get_user_data(timer);

	lv_timer_pause(timer);
	scan_applications(launcher);
	report_view(launcher, false);
	render(launcher);
}

static bool watch_directory(int fd, const char *path)
{
	if (inotify_add_watch(fd, path, FPLINUX_LAUNCHER_WATCH_EVENTS) >= 0)
		return true;
	fprintf(stderr, "fplinux-launcher: cannot watch %s: %s\n", path,
		strerror(errno));
	return false;
}

/* The icon directories that can hold this profile's tiles. */
static bool watch_icon_directories(struct launcher *launcher)
{
	bool watched = false;

	for (size_t i = 0; i < launcher->icon_size_count; ++i) {
		int size = launcher->icon_sizes[i];
		char path[PATH_MAX];

		if (size < launcher->spec->row_icon_size)
			continue;
		snprintf(path, sizeof(path),
			 "/usr/share/icons/hicolor/%dx%d/apps", size, size);
		watched = watch_directory(launcher->inotify_fd, path) ||
			  watched;
	}
	return watched;
}

static void create_view(struct launcher *launcher)
{
	lv_obj_t *screen = fplinux_ui_canvas_object(launcher->canvas);
	const struct fplinux_ui_theme *theme = launcher->theme;
	const struct fplinux_ui_profile_spec *spec = launcher->spec;
	const struct fplinux_launcher_frame *frame = &launcher->frame;
	const void *lock =
		fplinux_ui_glyph(launcher->ui, "lock", FPLINUX_UI_GLYPH_STATUS);
	lv_obj_t *status_bar = fill(screen,
				    (struct fplinux_launcher_rect){
					    .width = frame->width,
					    .height = frame->status_height,
				    },
				    0, theme->bar);

	lv_anim_init(&launcher->title_animation);
	lv_anim_set_delay(&launcher->title_animation,
			  FPLINUX_LAUNCHER_MARQUEE_DELAY_MS);
	lv_anim_set_reverse_delay(&launcher->title_animation,
				  FPLINUX_LAUNCHER_MARQUEE_PAUSE_MS);
	lv_anim_set_repeat_delay(&launcher->title_animation,
				 FPLINUX_LAUNCHER_MARQUEE_PAUSE_MS);
	lv_anim_set_repeat_count(&launcher->title_animation,
				 LV_ANIM_REPEAT_INFINITE);

	/* The left slot shows the lock glyph while locked. */
	if (lock)
		launcher->lock_glyph = glyph(
			status_bar, lock, fplinux_launcher_status_glyph(spec),
			theme->bar_text);
	launcher->status_clock =
		label(status_bar,
		      (struct fplinux_launcher_rect){
			      .y = fplinux_launcher_centre(
				      frame->status_height,
				      lv_font_get_line_height(theme->font)),
			      .height = lv_font_get_line_height(theme->font),
		      },
		      theme->font, theme->bar_text, "");
	lv_label_set_long_mode(launcher->status_clock, LV_LABEL_LONG_MODE_CLIP);
	create_battery_icon(launcher, status_bar);
	fill(screen,
	     (struct fplinux_launcher_rect){
		     .y = frame->status_height - spec->divider_width,
		     .width = frame->width,
		     .height = spec->divider_width,
	     },
	     0, theme->border);
	launcher->body = plain(screen, (struct fplinux_launcher_rect){
					       .y = frame->body_y,
					       .width = frame->width,
					       .height = frame->body_height,
				       });
	launcher->softkeys =
		fplinux_ui_softkey_bar_create(launcher->ui, screen);
	/* Centre legends in the full bar, including its top divider. */
	lv_obj_set_style_pad_top(launcher->softkeys, -spec->divider_width, 0);
	launcher->clock_timer = lv_timer_create(clock_tick, 60000, launcher);
	schedule_clock(launcher);
	/* No battery icon until the manager reports the level in ACTIVE. */
	update_battery_icon(launcher);
	render(launcher);
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-launcher",
		.description = "FPLinux keypad home and application catalogue",
	};
	struct launcher launcher = {
		.inotify_fd = -1,
		.view = FPLINUX_LAUNCHER_VIEW_HOME,
		.reported_view = FPLINUX_LAUNCHER_VIEW_HOME,
		.visible = true,
		.display = FPLINUX_UI_SESSION_DISPLAY_ON,
		.battery = -1,
	};
	enum fplinux_cli_result parsed = fplinux_cli_parse(&cli, argc, argv);
	int result;

	if (parsed != FPLINUX_CLI_READY)
		return parsed;
	launcher.desktop_dir = getenv("FPLINUX_UI_DESKTOP_DIR");
	if (!launcher.desktop_dir || !launcher.desktop_dir[0])
		launcher.desktop_dir = "/usr/share/applications";
	launcher.ui =
		fplinux_ui_open("org.fplinux.launcher", ui_event, &launcher);
	if (!launcher.ui) {
		perror("fplinux-launcher: open UI");
		return 1;
	}
	launcher.theme = fplinux_ui_theme(launcher.ui);
	launcher.spec = fplinux_ui_profile_spec(
		fplinux_ui_metrics(launcher.ui)->profile);
	/* A profile this launcher's table does not know: the one that fits. */
	if (!launcher.spec)
		launcher.spec =
			fplinux_ui_profile_spec(fplinux_ui_profile_for_screen(
				fplinux_ui_width(launcher.ui),
				fplinux_ui_height(launcher.ui)));
	fplinux_launcher_frame(launcher.spec, fplinux_ui_width(launcher.ui),
			       fplinux_ui_height(launcher.ui), &launcher.frame);
	fplinux_launcher_battery_layout(launcher.spec, launcher.frame.width,
					&launcher.battery_layout);
	launcher.icon_size_count = fplinux_launcher_icon_sizes(
		launcher.icon_sizes, FPLINUX_LAUNCHER_ICON_SIZE_COUNT);
	if (launcher.icon_size_count > FPLINUX_LAUNCHER_ICON_SIZE_COUNT)
		launcher.icon_size_count = FPLINUX_LAUNCHER_ICON_SIZE_COUNT;
	launcher.app_glyph =
		fplinux_ui_glyph(launcher.ui, "app", FPLINUX_UI_GLYPH_ROW);
	launcher.canvas = fplinux_ui_canvas_new(launcher.ui, NULL);
	if (!launcher.canvas) {
		perror("fplinux-launcher: create Canvas");
		fplinux_ui_close(launcher.ui);
		return 1;
	}
	fplinux_ui_canvas_set_key_handler(launcher.canvas, canvas_key,
					  &launcher);
	fplinux_ui_canvas_set_keyboard_keys(launcher.canvas, true);
	fplinux_ui_push(launcher.ui, launcher.canvas);
	fplinux_launcher_tasks_init(
		&launcher.tasks, launcher.ui, launcher.canvas,
		&(const struct fplinux_launcher_tasks_listener){
			.icon = task_icon,
			.view_changed = tasks_view_changed,
			.context = &launcher,
		});
	/* The profile's presentation until the first ACTIVE names one. */
	launcher.presentation = launcher.spec->catalogue_list ?
					FPLINUX_LAUNCHER_VIEW_LIST :
					FPLINUX_LAUNCHER_VIEW_GRID;
	launcher.reload_timer = lv_timer_create(
		reload_catalogue, FPLINUX_LAUNCHER_RELOAD_DELAY_MS, &launcher);
	if (launcher.reload_timer)
		lv_timer_pause(launcher.reload_timer);
	launcher.inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	/* Watch before the first scan so that no installed file is missed. */
	if (launcher.reload_timer && launcher.inotify_fd >= 0) {
		bool desktop = watch_directory(launcher.inotify_fd,
					       launcher.desktop_dir);
		bool icons = watch_icon_directories(&launcher);

		if (desktop || icons)
			fplinux_ui_watch_fd(launcher.ui, launcher.inotify_fd,
					    POLLIN, catalogue_changed,
					    &launcher);
		launcher.watching = desktop;
	}
	scan_applications(&launcher);
	create_view(&launcher);
	result = fplinux_ui_run(launcher.ui);
	if (launcher.inotify_fd >= 0)
		close(launcher.inotify_fd);
	fplinux_ui_close(launcher.ui);
	/* Closing the UI deleted every image that borrowed these buffers. */
	fplinux_launcher_backdrop_clear(&launcher.backdrop);
	release_icons(&launcher);
	return result < 0 ? 1 : 0;
}
