/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * LCDUI look and feel natives shared by every Displayable peer: the screen
 * stack, titles, commands and the screen listener, which turns commands and
 * changes into MIDP events for the Java event thread. The helpers of
 * fplinux-pm-lf.h serve the List, Alert, Form and Canvas natives.
 *
 * The listener only queues events: Java never runs from a runtime callback.
 * A small table remembers the stacked peers, so that Java can tell an alert
 * that the runtime removed after a command from one still on the stack.
 */
#include "fplinux-pm-lf.h"
#include "fplinux-pm-lf-backend.h"

#include <libintl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gxj_putpixel.h>
#include <kni.h>
#include <midpEvents.h>

#include "../app/fplinux-pm-app.h"
#include "fplinux-pm-vm.h"

/* More than the base, an alert and a peer in transition. */
#define PEERS_MAX 16
/* Command labels are printable text shorter than this many bytes. */
#define LABEL_BYTES 128
/* LFNative.CMD_DISMISS: an Alert's own dismiss command. */
#define CMD_DISMISS 8

struct lf_peer {
	struct fplinux_ui_screen *screen;
	int serial;
	bool alert;
};

static struct lf_peer peers[PEERS_MAX];

static const struct lf_backend runtime_backend = {
	.replace = fplinux_ui_replace,
	.push = fplinux_ui_push,
	.pop = fplinux_ui_pop,
	.current = fplinux_ui_current,
	.set_listener = fplinux_ui_screen_set_listener,
	.set_data = fplinux_ui_screen_set_data,
	.get_data = fplinux_ui_screen_get_data,
	.set_title = fplinux_ui_screen_set_title,
	.add_command = fplinux_ui_screen_add_command,
	.remove_command = fplinux_ui_screen_remove_command,
	.content_size = fplinux_ui_screen_get_content_size,
	.command_item = fplinux_ui_screen_get_command_item,
	.list_new = fplinux_ui_list_new,
	.list_insert = fplinux_ui_list_insert,
	.list_delete = fplinux_ui_list_delete,
	.list_delete_all = fplinux_ui_list_delete_all,
	.list_set = fplinux_ui_list_set,
	.list_set_icon = fplinux_ui_list_set_icon,
	.list_size = fplinux_ui_list_size,
	.list_get_focused = fplinux_ui_list_get_focused,
	.list_set_focused = fplinux_ui_list_set_focused,
	.list_is_selected = fplinux_ui_list_is_selected,
	.list_set_selected = fplinux_ui_list_set_selected,
	.list_set_select_label = fplinux_ui_list_set_select_label,
	.alert_new = fplinux_ui_alert_new,
	.alert_set_text = fplinux_ui_alert_set_text,
	.alert_set_progress = fplinux_ui_alert_set_progress,
};

const struct lf_backend *lf_backend(void)
{
	static struct lf_backend runtime;
	struct fplinux_ui *ui = fplinux_pm_app_ui();

	if (ui) {
		runtime = runtime_backend;
		runtime.ui = ui;
		return &runtime;
	}
	return lf_trace_enabled() ? lf_trace_backend() : NULL;
}

static struct lf_peer *find_peer(const struct fplinux_ui_screen *screen)
{
	for (size_t i = 0; i < PEERS_MAX; ++i)
		if (peers[i].screen == screen)
			return &peers[i];
	return NULL;
}

static void forget_screen(const struct fplinux_ui_screen *screen)
{
	struct lf_peer *peer = screen ? find_peer(screen) : NULL;

	if (peer)
		*peer = (struct lf_peer){ 0 };
}

/* A screen passed from Java as an int; NULL without a backend. */
static struct fplinux_ui_screen *screen_parameter(int index)
{
	if (!lf_backend())
		return NULL;
	return (struct fplinux_ui_screen *)(intptr_t)KNI_GetParameterAsInt(
		index);
}

/* Text */

static char *put_utf8(char *out, uint32_t code)
{
	if (code < 0x80) {
		*out++ = (char)code;
	} else if (code < 0x800) {
		*out++ = (char)(0xc0 | code >> 6);
		*out++ = (char)(0x80 | (code & 0x3f));
	} else if (code < 0x10000) {
		*out++ = (char)(0xe0 | code >> 12);
		*out++ = (char)(0x80 | (code >> 6 & 0x3f));
		*out++ = (char)(0x80 | (code & 0x3f));
	} else {
		*out++ = (char)(0xf0 | code >> 18);
		*out++ = (char)(0x80 | (code >> 12 & 0x3f));
		*out++ = (char)(0x80 | (code >> 6 & 0x3f));
		*out++ = (char)(0x80 | (code & 0x3f));
	}
	return out;
}

char *fplinux_pm_lf_utf8(jobject string)
{
	jsize length;
	jchar *chars;
	char *text;
	char *out;

	if (KNI_IsNullHandle(string))
		return NULL;
	length = KNI_GetStringLength(string);
	if (length < 0)
		return NULL;
	chars = malloc(((size_t)length + 1) * sizeof(*chars));
	/* A UTF-16 unit takes at most 3 bytes, a surrogate pair 4. */
	text = malloc((size_t)length * 3 + 1);
	if (!chars || !text) {
		free(chars);
		free(text);
		return NULL;
	}
	KNI_GetStringRegion(string, 0, length, chars);
	out = text;
	for (jsize i = 0; i < length; ++i) {
		uint32_t code = chars[i];

		if (code >= 0xd800 && code <= 0xdbff && i + 1 < length &&
		    chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff) {
			code = 0x10000 + ((code - 0xd800) << 10) +
			       (uint32_t)(chars[i + 1] - 0xdc00);
			++i;
		} else if (code >= 0xd800 && code <= 0xdfff) {
			code = 0xfffd;
		} else if (code == 0) {
			code = ' ';
		}
		out = put_utf8(out, code);
	}
	*out = '\0';
	free(chars);
	return text;
}

/* The UTF-8 text of a String on one line: control characters as spaces. */
static char *line_of(jobject string)
{
	char *text = fplinux_pm_lf_utf8(string);

	for (char *c = text; c && *c; ++c)
		if ((unsigned char)*c < 0x20 || *c == 0x7f)
			*c = ' ';
	return text;
}

/* Shortens text below LABEL_BYTES without cutting a character. */
static void fit_label(char *text)
{
	size_t length = strlen(text);

	if (length < LABEL_BYTES)
		return;
	length = LABEL_BYTES - 1;
	while (length && ((unsigned char)text[length] & 0xc0) == 0x80)
		--length;
	text[length] = '\0';
}

/* Events */

static bool vm_takes_events(void)
{
	return fplinux_pm_vm_get_state() == FPLINUX_PM_VM_RUNNING;
}

void fplinux_pm_lf_post_peer_changed(int serial, int sub_type, int index,
				     int hint)
{
	MidpEvent event;

	if (!serial || !vm_takes_events())
		return;
	MIDP_EVENT_INITIALIZE(event);
	event.type = MIDP_PEER_CHANGED_EVENT;
	event.intParam1 = serial;
	event.intParam2 = sub_type;
	event.intParam3 = index;
	event.intParam5 = hint;
	event.DISPLAY = FPLINUX_PM_LF_DISPLAY_ID;
	StoreMIDPEventInVmThread(event, 0);
}

/* Display.handleCommandEvent() finds the command by its id. */
static void post_command(int id)
{
	MidpEvent event;

	if (!vm_takes_events())
		return;
	MIDP_EVENT_INITIALIZE(event);
	event.type = MIDP_COMMAND_EVENT;
	event.COMMAND = id;
	event.DISPLAY = FPLINUX_PM_LF_DISPLAY_ID;
	StoreMIDPEventInVmThread(event, 0);
}

static void peer_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	const struct lf_backend *lf = lf_backend();
	int serial = (int)(intptr_t)context;
	struct lf_peer *peer = find_peer(screen);
	size_t focused;

	if (!lf)
		return;
	/* The runtime took the alert off the stack; it deletes it next. */
	if (peer && peer->alert && lf->current(ui) != screen)
		*peer = (struct lf_peer){ 0 };
	if (id == FPLINUX_UI_SELECT_COMMAND) {
		focused = lf->list_get_focused(screen);
		if (focused != SIZE_MAX)
			fplinux_pm_lf_post_peer_changed(
				serial, FPLINUX_PM_LF_PEER_LIST_SELECT,
				(int)focused, 0);
		return;
	}
	/* Item commands go to the Form LF's own item listener. */
	if (lf->command_item(screen) != SIZE_MAX)
		return;
	post_command(id);
}

static void peer_change(struct fplinux_ui *ui, struct fplinux_ui_screen *screen,
			size_t index, void *context)
{
	(void)ui;
	(void)screen;
	fplinux_pm_lf_post_peer_changed((int)(intptr_t)context,
					FPLINUX_PM_LF_PEER_ITEM_CHANGED,
					(int)index, 0);
}

int fplinux_pm_lf_serial(const struct fplinux_ui_screen *screen)
{
	const struct lf_backend *lf = lf_backend();

	return lf && screen ? (int)(intptr_t)lf->get_data(screen) : 0;
}

/* Images */

void *fplinux_pm_lf_image(jobject image_data, lv_image_dsc_t *dsc)
{
	gxj_screen_buffer buffer;
	size_t pixels;
	size_t size;
	unsigned char *data;

	if (KNI_IsNullHandle(image_data) ||
	    !gxj_get_image_screen_buffer_impl(
		    IMGAPI_GET_IMAGEDATA_PTR(image_data), &buffer, NULL) ||
	    buffer.width <= 0 || buffer.height <= 0 || !buffer.pixelData)
		return NULL;
	pixels = (size_t)buffer.width * (size_t)buffer.height;
	size = pixels * sizeof(gxj_pixel_type) +
	       (buffer.alphaData ? pixels : 0);
	data = malloc(size);
	if (!data)
		return NULL;
	/* LVGL's RGB565A8 is the RGB565 plane followed by the alpha plane. */
	memcpy(data, buffer.pixelData, pixels * sizeof(gxj_pixel_type));
	if (buffer.alphaData)
		memcpy(data + pixels * sizeof(gxj_pixel_type), buffer.alphaData,
		       pixels);
	*dsc = (lv_image_dsc_t){
		.header = {
			.magic = LV_IMAGE_HEADER_MAGIC,
			.cf = buffer.alphaData ? LV_COLOR_FORMAT_RGB565A8 :
						 LV_COLOR_FORMAT_RGB565,
			.w = (uint32_t)buffer.width,
			.h = (uint32_t)buffer.height,
			.stride = (uint32_t)buffer.width *
				  sizeof(gxj_pixel_type),
		},
		.data_size = (uint32_t)size,
		.data = data,
	};
	return data;
}

/* javax.microedition.lcdui.LFNative: the screen stack */

/* private static native boolean headless0(); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_headless0(void)
{
	KNI_ReturnBoolean(lf_backend() == NULL);
}

/* static native boolean replace0(int screen); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_replace0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);
	struct fplinux_ui_screen *old = lf ? lf->current(lf->ui) : NULL;

	if (!lf || !screen || !lf->replace(lf->ui, screen))
		KNI_ReturnBoolean(false);
	forget_screen(old);
	/* A Canvas becomes usable only when it publishes its first frame. */
	if (fplinux_pm_app_ui() && !fplinux_ui_canvas_object(screen))
		fplinux_pm_app_loaded();
	KNI_ReturnBoolean(true);
}

/* static native boolean push0(int screen); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_push0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);

	if (!lf || !screen || !lf->push(lf->ui, screen))
		KNI_ReturnBoolean(false);
	if (fplinux_pm_app_ui() && !fplinux_ui_canvas_object(screen))
		fplinux_pm_app_loaded();
	KNI_ReturnBoolean(true);
}

/* static native boolean pop0(); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_pop0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *top = lf ? lf->current(lf->ui) : NULL;

	if (!top || !lf->pop(lf->ui))
		KNI_ReturnBoolean(false);
	forget_screen(top);
	KNI_ReturnBoolean(true);
}

/* static native int current0(); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_current0(void)
{
	const struct lf_backend *lf = lf_backend();

	KNI_ReturnInt(lf ? (jint)(intptr_t)lf->current(lf->ui) : 0);
}

/* static native boolean attach0(int screen, int serial, boolean alert); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_attach0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);
	int serial = (int)KNI_GetParameterAsInt(2);
	bool alert = KNI_GetParameterAsBoolean(3);
	struct lf_peer *peer;

	if (!screen)
		KNI_ReturnBoolean(false);
	lf->set_listener(screen, peer_command, peer_change,
			 (void *)(intptr_t)serial);
	lf->set_data(screen, (void *)(intptr_t)serial);
	peer = find_peer(screen);
	if (!peer)
		peer = find_peer(NULL);
	if (!peer)
		KNI_ReturnBoolean(false);
	*peer = (struct lf_peer){ screen, serial, alert };
	KNI_ReturnBoolean(true);
}

/* static native boolean alive0(int screen, int serial); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_alive0(void)
{
	struct fplinux_ui_screen *screen = screen_parameter(1);
	int serial = (int)KNI_GetParameterAsInt(2);
	struct lf_peer *peer = screen ? find_peer(screen) : NULL;

	KNI_ReturnBoolean(peer && peer->serial == serial);
}

/* Titles, commands, sizes */

/* static native void setTitle0(int screen, String title); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_setTitle0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (screen) {
		char *title = line_of(string);

		lf->set_title(screen, title);
		free(title);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/*
 * static native boolean addCommand0(int screen, int id, int type,
 *                                   String label, int priority);
 */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_addCommand0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);
	int type = (int)KNI_GetParameterAsInt(3);
	struct fplinux_ui_command command = {
		.id = (int)KNI_GetParameterAsInt(2),
		.type = FPLINUX_UI_COMMAND_SCREEN,
		.priority = (int)KNI_GetParameterAsInt(5),
	};
	char *label = NULL;
	bool added = false;

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(4, string);
	if (screen) {
		label = line_of(string);
		if (label && !label[0]) {
			free(label);
			label = NULL;
		}
		if (label)
			fit_label(label);
		if (type == CMD_DISMISS) {
			command.type = FPLINUX_UI_COMMAND_CANCEL;
			command.label = gettext("Close");
		} else {
			if (type >= FPLINUX_UI_COMMAND_BACK &&
			    type <= FPLINUX_UI_COMMAND_HELP)
				command.type =
					(enum fplinux_ui_command_type)type;
			command.label = label;
			/* ITEM and SCREEN commands need a label. */
			if (!label &&
			    (command.type == FPLINUX_UI_COMMAND_ITEM ||
			     command.type == FPLINUX_UI_COMMAND_SCREEN))
				command.label = "\xe2\x80\xa6";
		}
		added = lf->add_command(screen, &command);
	}
	free(label);
	KNI_EndHandles();
	KNI_ReturnBoolean(added);
}

/* static native void removeCommand0(int screen, int id); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_removeCommand0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);

	if (screen)
		lf->remove_command(screen, (int)KNI_GetParameterAsInt(2));
	KNI_ReturnVoid();
}

static void content_size(int *width, int *height)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *screen = screen_parameter(1);

	if (!screen || !lf->content_size(screen, width, height)) {
		*width = 0;
		*height = 0;
	}
}

/* static native int contentWidth0(int screen); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_contentWidth0(void)
{
	int width;
	int height;

	content_size(&width, &height);
	KNI_ReturnInt(width);
}

/* static native int contentHeight0(int screen); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_contentHeight0(void)
{
	int width;
	int height;

	content_size(&width, &height);
	KNI_ReturnInt(height);
}

/* Trace script (headless tests) */

/* static native int traceNext0(); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_traceNext0(void)
{
	KNI_ReturnInt(fplinux_pm_app_ui() || !lf_trace_enabled() ?
			      -1 :
			      lf_trace_next_delay());
}

/* static native void traceShown0(int screen); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_traceShown0(void)
{
	if (!fplinux_pm_app_ui() && lf_trace_enabled())
		lf_trace_shown((const struct fplinux_ui_screen *)(intptr_t)
				       KNI_GetParameterAsInt(1));
	KNI_ReturnVoid();
}

/* static native void traceRun0(); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_traceRun0(void)
{
	if (!fplinux_pm_app_ui() && lf_trace_enabled())
		lf_trace_run_step();
	KNI_ReturnVoid();
}
