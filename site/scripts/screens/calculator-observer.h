/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CALCULATOR_OBSERVER_H
#define FPLINUX_CALCULATOR_OBSERVER_H

enum calculator_observation_kind {
	CALCULATOR_NUMBER,
	CALCULATOR_ALERT,
	CALCULATOR_STATUS,
};

struct calculator_number_observation {
	enum calculator_observation_kind kind;
	char text[256];
	char title[128];
	char detail[128];
	int width;
	int height;
	int text_width;
	int text_height;
	int left;
	int top;
	int parent_width;
	int parent_height;
};

#endif
