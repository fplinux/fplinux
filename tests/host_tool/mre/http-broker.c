/* SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "brokers/http-broker.h"

#include <assert.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	struct mre_http_request request = {
		.id = 1,
		.method = 1,
		.proxy = 2,
		.header_count = 1,
		.header_name = "Range",
		.header_value = "bytes=3-6",
	};
	struct mre_http_response response;
	int result;

	assert(argc == 2 && strlen(argv[1]) < sizeof(request.url));
	assert(setlocale(LC_CTYPE, "C.UTF-8"));
	strcpy(request.url, argv[1]);
	struct mre_http_broker *broker = mre_http_broker_create(request.url, 1);
	assert(broker);
	assert(mre_http_broker_start(broker, &request) == 0);
	while ((result = mre_http_broker_poll(broker, &response)) == 0)
		nanosleep(&(struct timespec){ .tv_nsec = 1000000 }, NULL);
	mre_http_broker_destroy(broker);
	assert(result == 1);
	printf("%" PRIu32 " %" PRId32 " %" PRId32 "\n", response.id,
	       response.transport, response.status);
	fwrite(response.headers, 1, response.header_bytes, stdout);
	fwrite(response.body, 1, response.body_bytes, stdout);
	return 0;
}
