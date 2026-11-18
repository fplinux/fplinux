/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UI_TEST_FORM_BOUNDARIES_H
#define FPLINUX_UI_TEST_FORM_BOUNDARIES_H

struct fplinux_ui_frame_peer;

int fplinux_ui_test_form_boundaries_client(int control);
void fplinux_ui_test_form_boundaries_check(struct fplinux_ui_frame_peer *peer,
					   int session, int control,
					   const char *directory);

#endif
