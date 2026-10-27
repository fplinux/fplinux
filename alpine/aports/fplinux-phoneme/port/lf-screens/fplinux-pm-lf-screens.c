/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fplinux-check: package-embedded
 *
 * LCDUI List and Alert natives of javax.microedition.lcdui.LFNative: thin
 * wrappers of the runtime's list and alert calls for ListLFImpl and
 * AlertLFImpl. Screens are created unstacked; DisplayableLFImpl stacks them.
 */
#include "fplinux-pm-lf.h"
#include "fplinux-pm-lf-backend.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <kni.h>

static struct fplinux_ui_screen *screen_parameter(int index)
{
	if (!lf_backend())
		return NULL;
	return (struct fplinux_ui_screen *)(intptr_t)KNI_GetParameterAsInt(
		index);
}

/* The UTF-8 text of a String; keep_newlines leaves '\n' for alerts. */
static char *text_of(jobject string, bool keep_newlines)
{
	char *text = fplinux_pm_lf_utf8(string);

	for (char *c = text; c && *c; ++c)
		if (((unsigned char)*c < 0x20 &&
		     !(keep_newlines && *c == '\n')) ||
		    *c == 0x7f)
			*c = ' ';
	return text;
}

static size_t index_parameter(int parameter)
{
	jint index = KNI_GetParameterAsInt(parameter);

	return index < 0 ? SIZE_MAX : (size_t)index;
}

/* List */

/* static native int listNew0(String title, int kind); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_listNew0(void)
{
	const struct lf_backend *lf = lf_backend();
	jint kind = KNI_GetParameterAsInt(2);
	struct fplinux_ui_screen *list = NULL;

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(1, string);
	if (lf) {
		char *title = text_of(string, false);

		if (kind < FPLINUX_UI_LIST_IMPLICIT ||
		    kind > FPLINUX_UI_LIST_MULTIPLE)
			kind = FPLINUX_UI_LIST_IMPLICIT;
		list = lf->list_new(lf->ui, title,
				    (enum fplinux_ui_list_kind)kind);
		free(title);
	}
	KNI_EndHandles();
	KNI_ReturnInt((jint)(intptr_t)list);
}

/* static native void listInsert0(int list, int index, String text); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listInsert0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(3, string);
	if (list) {
		char *text = text_of(string, false);

		lf->list_insert(list, index, text ? text : "", NULL);
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/* static native void listDelete0(int list, int index); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listDelete0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);

	if (list)
		lf->list_delete(list, index_parameter(2));
	KNI_ReturnVoid();
}

/* static native void listDeleteAll0(int list); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listDeleteAll0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);

	if (list)
		lf->list_delete_all(list);
	KNI_ReturnVoid();
}

/* static native void listSet0(int list, int index, String text); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listSet0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(3, string);
	if (list) {
		char *text = text_of(string, false);

		lf->list_set(list, index, text ? text : "", NULL);
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/* static native void listSetIcon0(int list, int index, ImageData image); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listSetIcon0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);

	KNI_StartHandles(1);
	KNI_DeclareHandle(image);
	KNI_GetParameterAsObject(3, image);
	if (list) {
		lv_image_dsc_t dsc;
		void *pixels = fplinux_pm_lf_image(image, &dsc);

		lf->list_set_icon(list, index, pixels ? &dsc : NULL,
				  FPLINUX_UI_LIST_ICON_IMAGE);
		free(pixels);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/* static native int listGetFocused0(int list); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_listGetFocused0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t focused = list ? lf->list_get_focused(list) : SIZE_MAX;

	KNI_ReturnInt(focused > INT32_MAX ? -1 : (jint)focused);
}

/* static native void listSetFocused0(int list, int index); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listSetFocused0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);

	if (list && index < lf->list_size(list))
		lf->list_set_focused(list, index);
	KNI_ReturnVoid();
}

/* static native boolean listIsSelected0(int list, int index); */
KNIEXPORT KNI_RETURNTYPE_BOOLEAN
Java_javax_microedition_lcdui_LFNative_listIsSelected0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);

	KNI_ReturnBoolean(list && index < lf->list_size(list) &&
			  lf->list_is_selected(list, index));
}

/*
 * static native void listSetSelected0(int list, int index,
 *                                     boolean selected);
 */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listSetSelected0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);
	size_t index = index_parameter(2);
	bool selected = KNI_GetParameterAsBoolean(3);

	if (list && index < lf->list_size(list))
		lf->list_set_selected(list, index, selected);
	KNI_ReturnVoid();
}

/* static native void listSetSelectLabel0(int list, String label); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_listSetSelectLabel0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *list = screen_parameter(1);

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (list) {
		char *label = text_of(string, false);

		/* An unusable label (too long) keeps the default "Select". */
		if (!label || !label[0] ||
		    !lf->list_set_select_label(list, label))
			lf->list_set_select_label(list, NULL);
		free(label);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/* Alert */

/* static native int alertNew0(int kind, String title, String text); */
KNIEXPORT KNI_RETURNTYPE_INT
Java_javax_microedition_lcdui_LFNative_alertNew0(void)
{
	const struct lf_backend *lf = lf_backend();
	jint kind = KNI_GetParameterAsInt(1);
	struct fplinux_ui_screen *alert = NULL;

	KNI_StartHandles(2);
	KNI_DeclareHandle(title_string);
	KNI_DeclareHandle(text_string);
	KNI_GetParameterAsObject(2, title_string);
	KNI_GetParameterAsObject(3, text_string);
	if (lf) {
		char *title = text_of(title_string, false);
		char *text = text_of(text_string, true);

		if (kind < FPLINUX_UI_ALERT_INFO ||
		    kind > FPLINUX_UI_ALERT_ERROR)
			kind = FPLINUX_UI_ALERT_INFO;
		/* Timeout 0: AlertLFImpl times the alert in Java. */
		alert = lf->alert_new(lf->ui, (enum fplinux_ui_alert_kind)kind,
				      title, text, 0);
		free(title);
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnInt((jint)(intptr_t)alert);
}

/* static native void alertSetText0(int alert, String text); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_alertSetText0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *alert = screen_parameter(1);

	KNI_StartHandles(1);
	KNI_DeclareHandle(string);
	KNI_GetParameterAsObject(2, string);
	if (alert) {
		char *text = text_of(string, true);

		lf->alert_set_text(alert, text);
		free(text);
	}
	KNI_EndHandles();
	KNI_ReturnVoid();
}

/* static native void alertSetProgress0(int alert, int value, int maximum); */
KNIEXPORT KNI_RETURNTYPE_VOID
Java_javax_microedition_lcdui_LFNative_alertSetProgress0(void)
{
	const struct lf_backend *lf = lf_backend();
	struct fplinux_ui_screen *alert = screen_parameter(1);
	jint value = KNI_GetParameterAsInt(2);
	jint maximum = KNI_GetParameterAsInt(3);

	if (alert)
		lf->alert_set_progress(alert, value, maximum < 0 ? 0 : maximum);
	KNI_ReturnVoid();
}
