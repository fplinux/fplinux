/* SPDX-License-Identifier: 0BSD */
#include "http-broker.h"

#include <curl/curl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct mre_http_broker {
	CURLM *multi;
	CURL *easy;
	struct curl_slist *request_headers;
	struct mre_http_response response;
	char approved_url[MRE_HTTP_URL_BYTES];
	uint32_t last_id, requests_started, max_requests;
	int in_multi, headers_complete;
};

static int valid_url(const char *url)
{
	if (!url)
		return 0;
	size_t length = 0;
	while (length < MRE_HTTP_URL_BYTES && url[length])
		++length;
	if (length == MRE_HTTP_URL_BYTES || strncmp(url, "http://", 7))
		return 0;
	for (size_t i = 0; url[i]; ++i) {
		unsigned char byte = (unsigned char)url[i];
		if (byte < 33 || byte > 126 || byte == '#')
			return 0;
	}
	const char *path = strchr(url + 7, '/');
	const char *query = strchr(url + 7, '?');
	if (!path || (query && query < path))
		return 0;
	CURLU *parsed = curl_url();
	if (!parsed)
		return 0;
	CURLUcode result = curl_url_set(parsed, CURLUPART_URL, url,
					CURLU_PATH_AS_IS | CURLU_DISALLOW_USER);
	curl_url_cleanup(parsed);
	return result == CURLUE_OK;
}

static int range_header_name(const char *name)
{
	static const char wanted[] = "range";
	if (strlen(name) != sizeof(wanted) - 1)
		return 0;
	for (size_t i = 0; i < sizeof(wanted) - 1; ++i) {
		unsigned char byte = (unsigned char)name[i];
		if (byte >= 'A' && byte <= 'Z')
			byte += 'a' - 'A';
		if (byte != (unsigned char)wanted[i])
			return 0;
	}
	return 1;
}

static int valid_request(const struct mre_http_broker *broker,
			 const struct mre_http_request *request)
{
	if (!request->id || request->id > INT32_MAX ||
	    request->id <= broker->last_id || request->method != 1 ||
	    request->proxy != 2 || request->header_count > 1 ||
	    request->body_bytes ||
	    !memchr(request->url, 0, sizeof(request->url)) ||
	    strcmp(request->url, broker->approved_url) ||
	    !memchr(request->header_name, 0, sizeof(request->header_name)) ||
	    !memchr(request->header_value, 0, sizeof(request->header_value)))
		return 0;
	if (!request->header_count)
		return 1;
	if (!range_header_name(request->header_name))
		return 0;
	for (size_t i = 0; request->header_value[i]; ++i) {
		unsigned char byte = (unsigned char)request->header_value[i];
		if ((byte < 32 && byte != '\t') || byte >= 127)
			return 0;
	}
	return 1;
}

static void retire_transfer(struct mre_http_broker *broker)
{
	if (broker->in_multi) {
		(void)curl_multi_remove_handle(broker->multi, broker->easy);
		broker->in_multi = 0;
	}
	if (broker->easy) {
		curl_easy_cleanup(broker->easy);
		broker->easy = NULL;
	}
	curl_slist_free_all(broker->request_headers);
	broker->request_headers = NULL;
}

static size_t receive_headers(char *data, size_t size, size_t count,
			      void *context)
{
	struct mre_http_broker *broker = context;
	struct mre_http_response *response = &broker->response;
	if (size && count > SIZE_MAX / size)
		return 0;
	size_t bytes = size * count;
	/* libcurl reports informational responses and trailers through this same
	 * callback. The SDK session needs the initial headers of its final status,
	 * so an interim field cannot fill a field missing from the final response. */
	if (bytes >= 5 && !memcmp(data, "HTTP/", 5)) {
		response->header_bytes = 0;
		broker->headers_complete = 0;
	} else if (broker->headers_complete) {
		return bytes;
	}
	if (bytes > sizeof(response->headers) - response->header_bytes)
		return 0;
	memcpy(response->headers + response->header_bytes, data, bytes);
	response->header_bytes += (uint32_t)bytes;
	if ((bytes == 2 && !memcmp(data, "\r\n", 2)) ||
	    (bytes == 1 && data[0] == '\n'))
		broker->headers_complete = 1;
	return bytes;
}

static size_t receive_body(char *data, size_t size, size_t count, void *context)
{
	struct mre_http_broker *broker = context;
	struct mre_http_response *response = &broker->response;
	if (size && count > SIZE_MAX / size)
		return 0;
	size_t bytes = size * count;
	if (bytes > sizeof(response->body) - response->body_bytes)
		return 0;
	memcpy(response->body + response->body_bytes, data, bytes);
	response->body_bytes += (uint32_t)bytes;
	return bytes;
}

static int configure_transfer(struct mre_http_broker *broker)
{
	CURL *easy = broker->easy;
	/* PATH_AS_IS retains dot segments, but curl's URL parser still normalizes
	 * percent hex case. This validated raw path/query preserves original bytes. */
	const char *request_target = strchr(broker->approved_url + 7, '/');
	/* proxy=2 selects the original SDK's CMNET policy, not a host HTTP proxy.
	 * An ambient proxy or redirect must not replace the privately granted URL. */
	if (curl_easy_setopt(easy, CURLOPT_URL, broker->approved_url) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_REQUEST_TARGET, request_target) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION,
			     (long)CURL_HTTP_VERSION_1_1) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http") != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_PATH_AS_IS, 1L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_PROXY, "") != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 5000L) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 15000L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HTTP09_ALLOWED, 0L) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 0L) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, receive_headers) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HEADERDATA, broker) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, receive_body) !=
		    CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_WRITEDATA, broker) != CURLE_OK ||
	    curl_easy_setopt(easy, CURLOPT_HTTPHEADER,
			     broker->request_headers) != CURLE_OK)
		return 0;
	return 1;
}

struct mre_http_broker *mre_http_broker_create(const char *approved_url,
					       uint32_t max_requests)
{
	if (!max_requests || max_requests > INT32_MAX ||
	    curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
		return NULL;
	const curl_version_info_data *version =
		curl_version_info(CURLVERSION_NOW);
	if (!version || !(version->features & CURL_VERSION_ASYNCHDNS) ||
	    !valid_url(approved_url)) {
		curl_global_cleanup();
		return NULL;
	}
	struct mre_http_broker *broker = calloc(1, sizeof(*broker));
	if (!broker) {
		curl_global_cleanup();
		return NULL;
	}
	memcpy(broker->approved_url, approved_url, strlen(approved_url) + 1);
	broker->max_requests = max_requests;
	broker->multi = curl_multi_init();
	if (!broker->multi ||
	    curl_multi_setopt(broker->multi, CURLMOPT_MAXCONNECTS, 1L) !=
		    CURLM_OK) {
		mre_http_broker_destroy(broker);
		return NULL;
	}
	return broker;
}

void mre_http_broker_destroy(struct mre_http_broker *broker)
{
	if (!broker)
		return;
	retire_transfer(broker);
	if (broker->multi)
		curl_multi_cleanup(broker->multi);
	free(broker);
	curl_global_cleanup();
}

int mre_http_broker_start(struct mre_http_broker *broker,
			  const struct mre_http_request *request)
{
	if (!broker || !request)
		return -1;
	struct mre_http_request copied = *request;
	if (!valid_request(broker, &copied))
		return -1;
	if (broker->easy)
		return -2;
	if (broker->requests_started == broker->max_requests)
		return -3;
	broker->easy = curl_easy_init();
	if (!broker->easy)
		return -4;
	if (copied.header_count) {
		char header[MRE_HTTP_HEADER_NAME_BYTES +
			    MRE_HTTP_HEADER_VALUE_BYTES + 2];
		size_t name_bytes = strlen(copied.header_name);
		size_t value_bytes = strlen(copied.header_value);
		memcpy(header, copied.header_name, name_bytes);
		memcpy(header + name_bytes, ": ", 2);
		memcpy(header + name_bytes + 2, copied.header_value,
		       value_bytes + 1);
		broker->request_headers = curl_slist_append(NULL, header);
		if (!broker->request_headers) {
			retire_transfer(broker);
			return -4;
		}
	}
	memset(&broker->response, 0, sizeof(broker->response));
	broker->response.id = copied.id;
	broker->headers_complete = 0;
	if (!configure_transfer(broker) ||
	    curl_multi_add_handle(broker->multi, broker->easy) != CURLM_OK) {
		retire_transfer(broker);
		return -4;
	}
	broker->in_multi = 1;
	broker->last_id = copied.id;
	++broker->requests_started;
	return 0;
}

int mre_http_broker_cancel(struct mre_http_broker *broker, uint32_t id)
{
	if (!broker || !broker->easy || broker->response.id != id)
		return -1;
	retire_transfer(broker);
	memset(&broker->response, 0, sizeof(broker->response));
	return 0;
}

int mre_http_broker_poll(struct mre_http_broker *broker,
			 struct mre_http_response *response)
{
	if (!broker || !response)
		return -1;
	if (!broker->easy)
		return 0;
	int running;
	if (curl_multi_perform(broker->multi, &running) != CURLM_OK) {
		retire_transfer(broker);
		return -1;
	}
	int remaining;
	CURLMsg *message;
	while ((message = curl_multi_info_read(broker->multi, &remaining))) {
		if (message->msg != CURLMSG_DONE ||
		    message->easy_handle != broker->easy)
			continue;
		long status = 0;
		broker->response.transport = (int32_t)message->data.result;
		if (curl_easy_getinfo(broker->easy, CURLINFO_RESPONSE_CODE,
				      &status) == CURLE_OK &&
		    status >= 0 && status <= INT32_MAX)
			broker->response.status = (int32_t)status;
		*response = broker->response;
		retire_transfer(broker);
		return 1;
	}
	return 0;
}
