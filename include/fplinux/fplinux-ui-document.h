/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_DOCUMENT_H
#define FPLINUX_UI_DOCUMENT_H

#include <stdint.h>

enum fplinux_ui_document_status {
	FPLINUX_UI_DOCUMENT_ACCEPTED,
	FPLINUX_UI_DOCUMENT_WAIT,
	FPLINUX_UI_DOCUMENT_CANCELLED,
	FPLINUX_UI_DOCUMENT_ERROR,
	FPLINUX_UI_DOCUMENT_RESTART_REQUIRED,
};

struct fplinux_ui_document_request {
	uint32_t request_id;
	/* Borrowed during the callback; copy it before returning WAIT. */
	const char *path;
};

struct fplinux_ui_document_result {
	uint32_t request_id;
	enum fplinux_ui_document_status status;
	/* Positive errno for ERROR, zero for other statuses. */
	int error;
};

#endif
