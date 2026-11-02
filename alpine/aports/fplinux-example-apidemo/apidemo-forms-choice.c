/* SPDX-License-Identifier: 0BSD */
#include "apidemo-forms.h"

#include <errno.h>
#include <stdint.h>

static const char *const sizes[] = { N_("Small"), N_("Medium"), N_("Large") };
static const char *const toppings[] = { N_("Cheese"), N_("Olives"),
					N_("Mushrooms"), N_("Peppers") };
static const char *const crusts[] = { N_("Thin"), N_("Classic"), N_("Thick") };

#define CHOICES_MAX 4

static size_t choice_status;
static size_t choice_size;
static size_t choice_toppings;
static size_t choice_crust;

/* The runtime copies the choice texts. */
static size_t append_choice(struct fplinux_ui_screen *form, const char *label,
			    enum fplinux_ui_choice_kind kind,
			    const char *const *choices, size_t count)
{
	const char *texts[CHOICES_MAX];

	if (count > CHOICES_MAX) {
		errno = EINVAL;
		return SIZE_MAX;
	}
	for (size_t i = 0; i < count; ++i)
		texts[i] = _(choices[i]);
	return fplinux_ui_form_append_choice(form, label, kind, texts, count);
}

static size_t chosen_toppings(const struct fplinux_ui_screen *form)
{
	size_t count = 0;

	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		count += fplinux_ui_form_is_chosen(form, choice_toppings, i);
	return count;
}

/* A choice reports OK on a row, a popup the choice made in its list. */
static void choices_changed(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, size_t index,
			    void *context)
{
	size_t count;

	(void)ui;
	(void)context;
	if (index == choice_size)
		apidemo_forms_show_status(
			form, choice_status, _("Item %zu: %s"), index,
			_(sizes[fplinux_ui_form_get_choice(form, index)]));
	else if (index == choice_crust)
		apidemo_forms_show_status(
			form, choice_status, _("Item %zu: %s"), index,
			_(crusts[fplinux_ui_form_get_choice(form, index)]));
	else if (index == choice_toppings && (count = chosen_toppings(form)))
		apidemo_forms_show_status(form, choice_status,
					  ngettext("Item %zu: %zu chosen",
						   "Item %zu: %zu chosen",
						   count),
					  index, count);
	else if (index == choice_toppings)
		apidemo_forms_show_status(form, choice_status,
					  _("Item %zu: none chosen"), index);
}

/* EXCLUSIVE and POPUP: one index; MULTIPLE: one flag per choice. */
static void show_choices(struct apidemo *app, struct fplinux_ui_screen *form)
{
	char text[384] = "";

	apidemo_forms_append_text(
		text, sizeof(text), "get_choice(%zu) = %zu\n", choice_size,
		fplinux_ui_form_get_choice(form, choice_size));
	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		apidemo_forms_append_text(
			text, sizeof(text), "is_chosen(%zu, %zu) = %s\n",
			choice_toppings, i,
			fplinux_ui_form_is_chosen(form, choice_toppings, i) ?
				"true" :
				"false");
	apidemo_forms_append_text(
		text, sizeof(text), "get_choice(%zu) = %zu", choice_crust,
		fplinux_ui_form_get_choice(form, choice_crust));
	apidemo_show(app, _("Values"), text);
}

/* Changes by the application do not call the listener. */
static void choose_all(struct fplinux_ui_screen *form, bool chosen)
{
	for (size_t i = 0; i < APIDEMO_COUNT(toppings); ++i)
		fplinux_ui_form_set_chosen(form, choice_toppings, i, chosen);
}

static void choices_command(struct fplinux_ui *ui,
			    struct fplinux_ui_screen *form, int command_id,
			    void *context)
{
	switch (command_id) {
	case APIDEMO_COMMAND_BACK:
		fplinux_ui_go_back(ui);
		break;
	case COMMAND_SHOW:
		show_choices(context, form);
		break;
	case COMMAND_CHOOSE_ALL:
		choose_all(form, true);
		break;
	case COMMAND_RESET:
		fplinux_ui_form_set_choice(form, choice_size, 0);
		choose_all(form, false);
		fplinux_ui_form_set_choice(form, choice_crust, 0);
		break;
	default:
		break;
	}
}

/*
 * EXCLUSIVE shows radio rows, MULTIPLE check rows; a POPUP choice is one
 * line with the current choice, and OK opens the list of choices.
 */
void apidemo_forms_open_choices(struct apidemo *app,
				const struct apidemo_entry *entry)
{
	struct fplinux_ui_screen *form =
		fplinux_ui_form_new(app->ui, _(entry->title));

	if (!form) {
		apidemo_error(app, "fplinux_ui_form_new");
		return;
	}
	choice_status = apidemo_forms_append_status(form);
	choice_size = append_choice(form, _("Size"),
				    FPLINUX_UI_CHOICE_EXCLUSIVE, sizes,
				    APIDEMO_COUNT(sizes));
	choice_toppings = append_choice(form, _("Toppings"),
					FPLINUX_UI_CHOICE_MULTIPLE, toppings,
					APIDEMO_COUNT(toppings));
	choice_crust = append_choice(form, _("Crust"), FPLINUX_UI_CHOICE_POPUP,
				     crusts, APIDEMO_COUNT(crusts));
	/* The application's own choices at the start. */
	fplinux_ui_form_set_choice(form, choice_size, 1);
	fplinux_ui_form_set_chosen(form, choice_toppings, 0, true);
	fplinux_ui_form_set_choice(form, choice_crust, 1);
	apidemo_forms_add_command(form, COMMAND_SHOW, FPLINUX_UI_COMMAND_SCREEN,
				  _("Show values"), 1,
				  apidemo_glyph(app, "view"));
	apidemo_forms_add_command(form, COMMAND_CHOOSE_ALL,
				  FPLINUX_UI_COMMAND_SCREEN,
				  _("Choose all toppings"), 2,
				  apidemo_glyph(app, "check"));
	apidemo_forms_add_command(form, COMMAND_RESET,
				  FPLINUX_UI_COMMAND_SCREEN, _("Reset"), 3,
				  apidemo_glyph(app, "refresh"));
	apidemo_add_back(form);
	fplinux_ui_screen_set_listener(form, choices_command, choices_changed,
				       app);
	apidemo_push(app, form, "fplinux_ui_form_new");
}
