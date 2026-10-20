/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The shared scrollbar appearance; private, not installed. */
#ifndef FPLINUX_UI_STYLE_INTERNAL_H
#define FPLINUX_UI_STYLE_INTERNAL_H

#include "fplinux-ui-internal.h"

/* Mode, scroll direction and vertical padding belong to the caller. */
static inline void fplinux_ui_style_scrollbar(struct fplinux_ui *ui,
					      lv_obj_t *object)
{
	const struct fplinux_ui_profile_spec *spec = fplinux_ui_spec(ui);

	lv_obj_set_style_bg_color(object, fplinux_ui_theme(ui)->focus,
				  LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(object, spec->scrollbar_width,
			       LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(object, spec->scrollbar_offset,
				   LV_PART_SCROLLBAR);
}

#endif
