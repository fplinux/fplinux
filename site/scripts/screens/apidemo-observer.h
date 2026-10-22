/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_APIDEMO_OBSERVER_H
#define FPLINUX_APIDEMO_OBSERVER_H

enum apidemo_observation_kind {
	APIDEMO_LABEL,
	APIDEMO_LIST,
	APIDEMO_FORM,
	APIDEMO_ALERT,
	APIDEMO_CANVAS,
	APIDEMO_TEXTAREA,
	APIDEMO_PREFERENCE,
};

struct apidemo_observation {
	enum apidemo_observation_kind kind;
	char title[128];
	char text[512];
};

#endif
