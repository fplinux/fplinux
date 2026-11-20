/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_HTTP_H
#define FPLINUX_MRE_HTTP_H

#include <stdint.h>

#define MRE_HTTP_URL_BYTES 255U
#define MRE_HTTP_HEADER_NAME_BYTES 20U
#define MRE_HTTP_HEADER_VALUE_BYTES 100U
#define MRE_HTTP_HEADER_BYTES 8192U
#define MRE_HTTP_BODY_BYTES 200U

/* Copies of a bounded SDK GET/Range request, not guest pointers
 * or Linux socket descriptors. The parent publishes a complete response before
 * sending HTTP_RESPONSE. transport == 0 preserves actual HTTP status/headers. */
struct mre_http_request {
	uint32_t id, method, proxy, header_count, body_bytes;
	char url[MRE_HTTP_URL_BYTES];
	char header_name[MRE_HTTP_HEADER_NAME_BYTES];
	char header_value[MRE_HTTP_HEADER_VALUE_BYTES];
	unsigned char body[MRE_HTTP_BODY_BYTES];
};

struct mre_http_response {
	uint32_t id;
	int32_t transport, status;
	uint32_t header_bytes, body_bytes;
	unsigned char headers[MRE_HTTP_HEADER_BYTES];
	unsigned char body[MRE_HTTP_BODY_BYTES];
};

#endif
