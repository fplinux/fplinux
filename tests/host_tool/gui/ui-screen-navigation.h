/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UI_TEST_SCREEN_NAVIGATION_H
#define FPLINUX_UI_TEST_SCREEN_NAVIGATION_H

struct fplinux_ui_frame_peer;

int fplinux_ui_test_navigation_client(int control, const char *scenario);
void fplinux_ui_test_navigation_check(struct fplinux_ui_frame_peer *peer,
				      int session, int control,
				      const char *scenario);

#endif
