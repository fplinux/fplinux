/* SPDX-License-Identifier: GPL-2.0-only */
/* fplinux-check: package-embedded */
/*
 * Natives of javax.microedition.lcdui.FormPeer: LCDUI Form and TextBox peers
 * on libfplinux-ui forms and text boxes (FACTORY.md, T1.4). A screen arrives
 * as its handle (DisplayableLFImpl.nativeId, a pointer) and an item as its
 * index. They run inside a VM slice on the runtime thread. Headless, when
 * fplinux_pm_app_ui() is NULL, no screen is ever made, every handle is 0 and
 * every native does nothing.
 * MIDP capacities are UTF-16 char units. The runtime counts code points,
 * so its limit is only an upper bound; TextFieldLFImpl clips whole pairs
 * and writes corrected text back when it synchronizes a peer.
 */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fplinux/fplinux-ui-screen.h>
#include <kni.h>

#include "../app/fplinux-pm-app.h"
#include "../lf-screens/fplinux-pm-lf.h"

/* FormPeer.PEER_ITEM_COMMAND: index = item, hint = command id. */
#define PEER_ITEM_COMMAND 16
/* Command labels are printable text shorter than 128 bytes. */
#define LABEL_MAX 127

static struct fplinux_ui_screen *screen_of(jint handle)
{
	return (struct fplinux_ui_screen *)(intptr_t)handle;
}

static jint handle_of(struct fplinux_ui_screen *screen)
{
	return (jint)(intptr_t)screen;
}

static jint index_result(size_t index)
{
	return index == SIZE_MAX || index > INT_MAX ? -1 : (jint)index;
}

/* The form and item of parameters 1 and 2; false for no form or a bad index. */
static bool item_args(struct fplinux_ui_screen **form, size_t *index)
{
	jint item = KNI_GetParameterAsInt(2);

	*form = screen_of(KNI_GetParameterAsInt(1));
	*index = (size_t)item;
	return *form && item >= 0;
}

/* The code point at *text, which moves past it; U+FFFD for invalid UTF-8. */
static uint32_t next_code_point(const unsigned char **text,
				const unsigned char *end)
{
	const unsigned char *p = *text;
	uint32_t c = *p++;
	uint32_t minimum;
	int extra;

	if (c < 0x80) {
		*text = p;
		return c;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		extra = 1;
		minimum = 0x80;
		c &= 0x1f;
	} else if (c >= 0xe0 && c <= 0xef) {
		extra = 2;
		minimum = 0x800;
		c &= 0x0f;
	} else if (c >= 0xf0 && c <= 0xf4) {
		extra = 3;
		minimum = 0x10000;
		c &= 0x07;
	} else {
		*text = p;
		return 0xfffd;
	}
	for (; extra; --extra, ++p) {
		if (p == end || (*p & 0xc0) != 0x80) {
			*text = p;
			return 0xfffd;
		}
		c = c << 6 | (*p & 0x3f);
	}
	*text = p;
	if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
		return 0xfffd;
	return c;
}

/* A Java string from UTF-8 text; result stays null without memory. */
static void new_string(const char *text, jstring result)
{
	const unsigned char *s = (const unsigned char *)text;
	const unsigned char *end = s + strlen(text);
	/* Never more UTF-16 units than bytes. */
	jchar *chars = malloc(((size_t)(end - s) + 1) * sizeof(*chars));
	jsize length = 0;
	uint32_t c;

	if (!chars)
		return;
	while (s < end) {
		c = next_code_point(&s, end);
		if (c >= 0x10000) {
			c -= 0x10000;
			chars[length++] = (jchar)(0xd800 | c >> 10);
			chars[length++] = (jchar)(0xdc00 | (c & 0x3ff));
		} else {
			chars[length++] = (jchar)c;
		}
	}
	KNI_NewString(chars, length, result);
	free(chars);
}

/* A copy of a command label the runtime takes: printable, under 128 bytes. */
static char *command_label(jobject string)
{
	char *label = fplinux_pm_lf_utf8(string);
	size_t length;

	if (!label)
		return NULL;
	for (char *c = label; *c; ++c)
		if ((unsigned char)*c < ' ' || *c == 0x7f)
			*c = ' ';
	length = strlen(label);
	if (length > LABEL_MAX) {
		length = LABEL_MAX;
		while (length && ((unsigned char)label[length] & 0xc0) == 0x80)
			--length;
		label[length] = '\0';
	}
	return label;
}

/* Local wall-clock time from its fields; the current time if invalid. */
static time_t local_time(jint year, jint month, jint day, jint hour,
			 jint minute)
{
	struct tm tm = {
		.tm_year = year - 1900,
		.tm_mon = month,
		.tm_mday = day,
		.tm_hour = hour,
		.tm_min = minute,
		.tm_isdst = -1,
	};
	time_t value = mktime(&tm);

	return value == (time_t)-1 ? time(NULL) : value;
}

static void item_command(struct fplinux_ui *ui, struct fplinux_ui_screen *form,
			 size_t index, int command_id, void *context)
{
	int serial = fplinux_pm_lf_serial(form);

	(void)ui;
	(void)context;
	if (serial && index <= INT_MAX)
		fplinux_pm_lf_post_peer_changed(serial, PEER_ITEM_COMMAND,
						(int)index, command_id);
}

/* ---- screens ---- */

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_createForm0(void)
{
	struct fplinux_ui *ui = fplinux_pm_app_ui();
	struct fplinux_ui_screen *form = NULL;
	char *title;

	KNI_StartHandles(1);
	KNI_DeclareHandle(title_handle);
	KNI_GetParameterAsObject(1, title_handle);
	if (ui) {
		title = fplinux_pm_lf_utf8(title_handle);
		form = fplinux_ui_form_new(ui, title);
		free(title);
		if (form)
			fplinux_ui_screen_set_item_listener(form, item_command,
							    NULL);
	}
	KNI_EndHandles();
	KNI_ReturnInt(handle_of(form));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_createTextBox0(void)
{
	struct fplinux_ui *ui = fplinux_pm_app_ui();
	struct fplinux_ui_screen *box = NULL;
	jint max_chars = KNI_GetParameterAsInt(3);
	enum fplinux_ui_text_constraint constraint =
		(enum fplinux_ui_text_constraint)KNI_GetParameterAsInt(4);
	char *title;
	char *text;

	KNI_StartHandles(2);
	KNI_DeclareHandle(title_handle);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(1, title_handle);
	KNI_GetParameterAsObject(2, text_handle);
	if (ui) {
		title = fplinux_pm_lf_utf8(title_handle);
		text = fplinux_pm_lf_utf8(text_handle);
		box = fplinux_ui_text_box_new(
			ui, title, text ? text : "",
			max_chars > 0 ? (size_t)max_chars : 0);
		/* A text the constraint refuses: start empty rather than fail. */
		if (box &&
		    !fplinux_ui_text_box_set_constraint(box, constraint) &&
		    fplinux_ui_text_box_set(box, ""))
			fplinux_ui_text_box_set_constraint(box, constraint);
		free(text);
		free(title);
	}
	KNI_EndHandles();
	KNI_ReturnInt(handle_of(box));
}

/* ---- appending items ---- */

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendString0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	size_t index = SIZE_MAX;
	char *label;
	char *text;

	KNI_StartHandles(2);
	KNI_DeclareHandle(label_handle);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(2, label_handle);
	KNI_GetParameterAsObject(3, text_handle);
	if (form) {
		label = fplinux_pm_lf_utf8(label_handle);
		text = fplinux_pm_lf_utf8(text_handle);
		index = fplinux_ui_form_append_string(form, label, text);
		free(text);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendTextField0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	jint max_chars = KNI_GetParameterAsInt(4);
	enum fplinux_ui_text_constraint constraint =
		(enum fplinux_ui_text_constraint)KNI_GetParameterAsInt(5);
	size_t index = SIZE_MAX;
	char *label;
	char *text;

	KNI_StartHandles(2);
	KNI_DeclareHandle(label_handle);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(2, label_handle);
	KNI_GetParameterAsObject(3, text_handle);
	if (form) {
		label = fplinux_pm_lf_utf8(label_handle);
		text = fplinux_pm_lf_utf8(text_handle);
		index = fplinux_ui_form_append_text_field(
			form, label, text ? text : "",
			max_chars > 0 ? (size_t)max_chars : 0, constraint);
		free(text);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendChoice0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	enum fplinux_ui_choice_kind kind =
		(enum fplinux_ui_choice_kind)KNI_GetParameterAsInt(3);
	jint count = KNI_GetParameterAsInt(5);
	size_t index = SIZE_MAX;
	char **choices = NULL;
	char *label = NULL;
	jint made = 0;

	KNI_StartHandles(3);
	KNI_DeclareHandle(label_handle);
	KNI_DeclareHandle(array_handle);
	KNI_DeclareHandle(choice_handle);
	KNI_GetParameterAsObject(2, label_handle);
	KNI_GetParameterAsObject(4, array_handle);
	if (form && count > 0 && !KNI_IsNullHandle(array_handle) &&
	    count <= KNI_GetArrayLength(array_handle))
		choices = calloc((size_t)count, sizeof(*choices));
	if (choices) {
		for (; made < count; ++made) {
			KNI_GetObjectArrayElement(array_handle, made,
						  choice_handle);
			choices[made] = fplinux_pm_lf_utf8(choice_handle);
			if (!choices[made])
				break;
		}
		label = fplinux_pm_lf_utf8(label_handle);
		if (made == count)
			index = fplinux_ui_form_append_choice(
				form, label, kind, (const char *const *)choices,
				(size_t)count);
		for (jint i = 0; i < made; ++i)
			free(choices[i]);
		free(choices);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendGauge0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	int minimum = KNI_GetParameterAsInt(3);
	int maximum = KNI_GetParameterAsInt(4);
	int value = KNI_GetParameterAsInt(5);
	bool interactive = KNI_GetParameterAsBoolean(6);
	size_t index = SIZE_MAX;
	char *label;

	KNI_StartHandles(1);
	KNI_DeclareHandle(label_handle);
	KNI_GetParameterAsObject(2, label_handle);
	if (form) {
		label = fplinux_pm_lf_utf8(label_handle);
		index = fplinux_ui_form_append_gauge(
			form, label, minimum, maximum, value, interactive);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendDate0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	enum fplinux_ui_date_mode mode =
		(enum fplinux_ui_date_mode)KNI_GetParameterAsInt(3);
	time_t value =
		local_time(KNI_GetParameterAsInt(4), KNI_GetParameterAsInt(5),
			   KNI_GetParameterAsInt(6), KNI_GetParameterAsInt(7),
			   KNI_GetParameterAsInt(8));
	size_t index = SIZE_MAX;
	char *label;

	KNI_StartHandles(1);
	KNI_DeclareHandle(label_handle);
	KNI_GetParameterAsObject(2, label_handle);
	if (form) {
		label = fplinux_pm_lf_utf8(label_handle);
		index = fplinux_ui_form_append_date(form, label, mode, value);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendImage0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	size_t index = SIZE_MAX;
	lv_image_dsc_t image;
	void *pixels;
	char *label;

	KNI_StartHandles(2);
	KNI_DeclareHandle(label_handle);
	KNI_DeclareHandle(image_handle);
	KNI_GetParameterAsObject(2, label_handle);
	KNI_GetParameterAsObject(3, image_handle);
	if (form) {
		pixels = fplinux_pm_lf_image(image_handle, &image);
		if (pixels) {
			label = fplinux_pm_lf_utf8(label_handle);
			index = fplinux_ui_form_append_image(form, label,
							     &image);
			free(label);
			free(pixels);
		}
	}
	KNI_EndHandles();
	KNI_ReturnInt(index_result(index));
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_appendPlaceholder0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));
	size_t index = SIZE_MAX;

	if (form) {
		index = fplinux_ui_form_append_string(form, NULL, NULL);
		if (index != SIZE_MAX &&
		    !fplinux_ui_form_set_hidden(form, index, true)) {
			fplinux_ui_form_delete(form, index);
			index = SIZE_MAX;
		}
	}
	KNI_ReturnInt(index_result(index));
}

/* ---- rows, focus and item commands ---- */

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_moveItem0(void)
{
	struct fplinux_ui_screen *form;
	size_t from;
	jint to = KNI_GetParameterAsInt(3);
	bool done = item_args(&form, &from) && to >= 0 &&
		    fplinux_ui_form_move_item(form, from, (size_t)to);

	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_deleteItem0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	if (item_args(&form, &index))
		fplinux_ui_form_delete(form, index);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_deleteAll0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));

	if (form)
		fplinux_ui_form_delete_all(form);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_getFocus0(void)
{
	struct fplinux_ui_screen *form = screen_of(KNI_GetParameterAsInt(1));

	KNI_ReturnInt(form ? index_result(fplinux_ui_form_get_focus(form)) :
			     -1);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_setFocus0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	if (item_args(&form, &index))
		fplinux_ui_form_set_focus(form, index);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setAppearance0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	enum fplinux_ui_appearance appearance =
		(enum fplinux_ui_appearance)KNI_GetParameterAsInt(3);
	bool done = item_args(&form, &index) &&
		    fplinux_ui_form_set_appearance(form, index, appearance);

	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_addItemCommand0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	struct fplinux_ui_command command = {
		.id = KNI_GetParameterAsInt(3),
		.type = (enum fplinux_ui_command_type)KNI_GetParameterAsInt(4),
		.priority = KNI_GetParameterAsInt(6),
	};
	bool done = false;
	char *label;

	KNI_StartHandles(1);
	KNI_DeclareHandle(label_handle);
	KNI_GetParameterAsObject(5, label_handle);
	if (item_args(&form, &index)) {
		label = command_label(label_handle);
		command.label = label && label[0] ? label : NULL;
		/* ITEM and SCREEN commands need a label, as in fplinux-pm-lf.c. */
		if (!command.label &&
		    (command.type == FPLINUX_UI_COMMAND_ITEM ||
		     command.type == FPLINUX_UI_COMMAND_SCREEN))
			command.label = "\xe2\x80\xa6";
		done = fplinux_ui_form_add_item_command(form, index, &command);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_removeItemCommand0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	if (item_args(&form, &index))
		fplinux_ui_screen_remove_item_command(form, index,
						      KNI_GetParameterAsInt(3));
	KNI_ReturnVoid();
}

/* ---- item values ---- */

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setString0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	bool done = false;
	char *text;

	KNI_StartHandles(1);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(3, text_handle);
	if (item_args(&form, &index)) {
		text = fplinux_pm_lf_utf8(text_handle);
		done = fplinux_ui_form_set_string(form, index,
						  text ? text : "");
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_OBJECT
Java_javax_microedition_lcdui_FormPeer_getText0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	const char *text;

	KNI_StartHandles(1);
	KNI_DeclareHandle(result);
	if (item_args(&form, &index)) {
		text = fplinux_ui_form_get_text(form, index);
		if (text)
			new_string(text, result);
	}
	KNI_EndHandlesAndReturnObject(result);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setText0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	bool done = false;
	char *text;

	KNI_StartHandles(1);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(3, text_handle);
	if (item_args(&form, &index)) {
		text = fplinux_pm_lf_utf8(text_handle);
		done = fplinux_ui_form_set_text(form, index, text ? text : "");
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setConstraint0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	enum fplinux_ui_text_constraint constraint =
		(enum fplinux_ui_text_constraint)KNI_GetParameterAsInt(3);
	bool done = item_args(&form, &index) &&
		    fplinux_ui_form_set_constraint(form, index, constraint);

	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_getChoice0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	KNI_ReturnInt(
		item_args(&form, &index) ?
			index_result(fplinux_ui_form_get_choice(form, index)) :
			-1);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_setChoice0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	jint choice = KNI_GetParameterAsInt(3);

	if (item_args(&form, &index) && choice >= 0)
		fplinux_ui_form_set_choice(form, index, (size_t)choice);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_isChosen0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	jint choice = KNI_GetParameterAsInt(3);
	bool chosen = item_args(&form, &index) && choice >= 0 &&
		      fplinux_ui_form_is_chosen(form, index, (size_t)choice);

	KNI_ReturnBoolean(chosen);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_setChosen0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	jint choice = KNI_GetParameterAsInt(3);
	bool chosen = KNI_GetParameterAsBoolean(4);

	if (item_args(&form, &index) && choice >= 0)
		fplinux_ui_form_set_chosen(form, index, (size_t)choice, chosen);
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_FormPeer_getValue0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	KNI_ReturnInt(item_args(&form, &index) ?
			      fplinux_ui_form_get_value(form, index) :
			      -1);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_setValue0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	if (item_args(&form, &index))
		fplinux_ui_form_set_value(form, index,
					  KNI_GetParameterAsInt(3));
	KNI_ReturnVoid();
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_getDate0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;
	bool done = false;
	struct tm tm;
	time_t value;

	KNI_StartHandles(1);
	KNI_DeclareHandle(fields);
	KNI_GetParameterAsObject(3, fields);
	if (item_args(&form, &index) && !KNI_IsNullHandle(fields) &&
	    KNI_GetArrayLength(fields) >= 5) {
		value = fplinux_ui_form_get_date(form, index);
		if (value != (time_t)-1 && localtime_r(&value, &tm)) {
			KNI_SetIntArrayElement(fields, 0, tm.tm_year + 1900);
			KNI_SetIntArrayElement(fields, 1, tm.tm_mon);
			KNI_SetIntArrayElement(fields, 2, tm.tm_mday);
			KNI_SetIntArrayElement(fields, 3, tm.tm_hour);
			KNI_SetIntArrayElement(fields, 4, tm.tm_min);
			done = true;
		}
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_FormPeer_setDate0(void)
{
	struct fplinux_ui_screen *form;
	size_t index;

	if (item_args(&form, &index))
		fplinux_ui_form_set_date(form, index,
					 local_time(KNI_GetParameterAsInt(3),
						    KNI_GetParameterAsInt(4),
						    KNI_GetParameterAsInt(5),
						    KNI_GetParameterAsInt(6),
						    KNI_GetParameterAsInt(7)));
	KNI_ReturnVoid();
}

/* ---- text boxes ---- */

KNIEXPORT KNI_RETURNTYPE_OBJECT
Java_javax_microedition_lcdui_FormPeer_getBoxText0(void)
{
	struct fplinux_ui_screen *box = screen_of(KNI_GetParameterAsInt(1));
	const char *text;

	KNI_StartHandles(1);
	KNI_DeclareHandle(result);
	if (box) {
		text = fplinux_ui_text_box_get(box);
		if (text)
			new_string(text, result);
	}
	KNI_EndHandlesAndReturnObject(result);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setBoxText0(void)
{
	struct fplinux_ui_screen *box = screen_of(KNI_GetParameterAsInt(1));
	bool done = false;
	char *text;

	KNI_StartHandles(1);
	KNI_DeclareHandle(text_handle);
	KNI_GetParameterAsObject(2, text_handle);
	if (box) {
		text = fplinux_pm_lf_utf8(text_handle);
		done = fplinux_ui_text_box_set(box, text ? text : "");
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(done);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_FormPeer_setBoxConstraint0(void)
{
	struct fplinux_ui_screen *box = screen_of(KNI_GetParameterAsInt(1));
	enum fplinux_ui_text_constraint constraint =
		(enum fplinux_ui_text_constraint)KNI_GetParameterAsInt(2);
	bool done = box && fplinux_ui_text_box_set_constraint(box, constraint);

	KNI_ReturnBoolean(done);
}
