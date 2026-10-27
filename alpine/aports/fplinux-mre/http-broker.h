/* SPDX-License-Identifier: 0BSD */
#ifndef FPLINUX_MRE_HTTP_BROKER_H
#define FPLINUX_MRE_HTTP_BROKER_H

#include "mre-http.h"

struct mre_http_broker;

/* The parent derives this exact HTTP URL from its private approved input.
 * Requests must keep the URL bytes unchanged and use increasing positive
 * 31-bit IDs. max_requests includes retries and the initial suffix probe.
 * Creation requires a libcurl asynchronous DNS backend. */
struct mre_http_broker *mre_http_broker_create(const char *approved_url,
					       uint32_t max_requests);
void mre_http_broker_destroy(struct mre_http_broker *broker);

/* start copies the bounded request before returning. No network work or guest
 * callback runs here. Return 0 accepted, -1 invalid, -2 busy, -3 request budget
 * exhausted, or -4 transport initialization failed. */
int mre_http_broker_start(struct mre_http_broker *broker,
			  const struct mre_http_request *request);

/* Return 0 only for the active matching ID. Cancellation suppresses completion
 * and never touches files; an absent ID returns -1 without changing the slot. */
int mre_http_broker_cancel(struct mre_http_broker *broker, uint32_t id);

/* Call from the existing UI timer: no wait, sleep, or guest callback occurs.
 * Return 1 with a copied response and an already-retired active slot, 0 while
 * pending/idle, or -1 on a multi-stack failure (the active slot is cancelled).
 * transport is a CURLcode: 0 for actual HTTP responses, including errors such
 * as 500; nonzero for timeout, network or buffer overflow. Already received
 * bounded headers/body remain available on failure. Headers are the final
 * response's initial block, including CRLF; interim blocks and trailers are
 * excluded. curl unfolds obsolete folded lines before its header callback. */
int mre_http_broker_poll(struct mre_http_broker *broker,
			 struct mre_http_response *response);

#endif
