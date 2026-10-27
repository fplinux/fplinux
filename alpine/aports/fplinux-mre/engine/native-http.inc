/* SPDX-License-Identifier: 0BSD */
/* Native base-AAPCS SDK HTTP boundary.
 * The parent owns networking. Guest pointers are copied before submit returns;
 * only copied bytes and positive opaque IDs enter the shared broker record. */
typedef void(MRE_ABI *sdk_http_hook_fn)(int32_t, void *);
typedef void(MRE_ABI *sdk_http_notify_fn)(int32_t, int32_t, void *);

struct sdk_asyn_http_request {
	uint8_t method, proxy, padding[2];
	uint32_t request;
};
struct sdk_http_request {
	char url[255];
	uint8_t padding;
	int32_t header_count;
	uint32_t headers;
	int32_t body_bytes;
	uint32_t body;
};
struct sdk_http_header {
	char name[20], value[100];
};
struct sdk_http_session {
	int32_t socket;
	char url[255], host[100];
	uint8_t ip_address[4], ip_address_bytes;
	int32_t port, request_bytes;
	char request[4096];
	int32_t status, header_bytes;
	unsigned char headers[8192];
	int32_t body_bytes;
	uint32_t body, hook;
	uint16_t last_error[50];
};
_Static_assert(sizeof(struct sdk_asyn_http_request) == 8 &&
		       offsetof(struct sdk_asyn_http_request, proxy) == 1 &&
		       offsetof(struct sdk_asyn_http_request, request) == 4,
	       "original HTTP short-enum request ABI");
_Static_assert(sizeof(struct sdk_http_request) == 272 &&
		       offsetof(struct sdk_http_request, header_count) == 256 &&
		       offsetof(struct sdk_http_request, headers) == 260 &&
		       offsetof(struct sdk_http_request, body_bytes) == 264 &&
		       offsetof(struct sdk_http_request, body) == 268 &&
		       sizeof(struct sdk_http_header) == 120,
	       "original HTTP request/header ABI");
_Static_assert(sizeof(struct sdk_http_session) == 12780 &&
		       offsetof(struct sdk_http_session, host) == 259 &&
		       offsetof(struct sdk_http_session, port) == 364 &&
		       offsetof(struct sdk_http_session, status) == 4468 &&
		       offsetof(struct sdk_http_session, header_bytes) ==
			       4472 &&
		       offsetof(struct sdk_http_session, headers) == 4476 &&
		       offsetof(struct sdk_http_session, body_bytes) == 12668 &&
		       offsetof(struct sdk_http_session, body) == 12672 &&
		       offsetof(struct sdk_http_session, hook) == 12676 &&
		       offsetof(struct sdk_http_session, last_error) == 12680,
	       "original full HTTP session ABI");

static struct {
	uint32_t next_id, active_id;
	uintptr_t request_identity;
	struct sdk_http_session *session, *callback_session;
	sdk_http_hook_fn hook;
	sdk_http_notify_fn notify;
	int response_ready, response_failure, callback_cancelled;
} native_http;

static void native_http_emit(const char *command, uint32_t id)
{
	char line[64];
	int length =
		snprintf(line, sizeof(line), "%s\t%" PRIu32 "\n", command, id);
	if (length < 0 || (size_t)length >= sizeof(line))
		fail("HTTP protocol line bound");
	write_protocol(line, (size_t)length);
}

static int native_http_copy_string(char *out, size_t capacity,
				   const char *guest)
{
	for (size_t i = 0; i < capacity; ++i) {
		if (!guest_data(guest + i, 1, PF_R))
			return 0;
		out[i] = guest[i];
		if (!out[i])
			return 1;
	}
	return 0;
}

static int native_http_header_token(unsigned char byte)
{
	return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
	       (byte >= '0' && byte <= '9') ||
	       (byte && strchr("!#$%&'*+-.^_`|~", byte));
}

static int native_http_valid_request_header(const char *name, const char *value)
{
	if (!name[0])
		return 0;
	for (size_t i = 0; name[i]; ++i)
		if (!native_http_header_token((unsigned char)name[i]))
			return 0;
	for (size_t i = 0; value[i]; ++i) {
		unsigned char byte = (unsigned char)value[i];
		if ((byte < 32 && byte != '\t') || byte >= 127)
			return 0;
	}
	return 1;
}

static int native_http_url_parts(const char *url, char host[100], int32_t *port)
{
	if (strncmp(url, "http://", 7))
		return 0;
	for (size_t i = 0; url[i]; ++i)
		if ((unsigned char)url[i] < 33 || (unsigned char)url[i] > 126 ||
		    url[i] == '#')
			return 0;
	const char *authority = url + 7;
	const char *end = strchr(authority, '/');
	if (!end || end == authority)
		return 0;
	const char *colon = NULL;
	for (const char *p = authority; p < end; ++p) {
		unsigned char byte = (unsigned char)*p;
		if (byte == ':') {
			if (colon)
				return 0;
			colon = p;
		} else if (!((byte >= 'a' && byte <= 'z') ||
			     (byte >= 'A' && byte <= 'Z') ||
			     (byte >= '0' && byte <= '9') || byte == '.' ||
			     byte == '-')) {
			return 0;
		}
	}
	size_t host_bytes = (size_t)((colon ? colon : end) - authority);
	if (!host_bytes || host_bytes >= 100)
		return 0;
	*port = 80;
	if (colon) {
		uint32_t number = 0;
		if (colon + 1 == end)
			return 0;
		for (const char *p = colon + 1; p < end; ++p) {
			if (*p < '0' || *p > '9' || number > 65535U / 10U)
				return 0;
			number = number * 10U + (unsigned)(*p - '0');
			if (number > 65535U)
				return 0;
		}
		if (!number)
			return 0;
		*port = (int32_t)number;
	}
	memcpy(host, authority, host_bytes);
	host[host_bytes] = 0;
	return 1;
}

static int native_http_name_equal(const unsigned char *name, size_t bytes,
				  const char *wanted)
{
	if (strlen(wanted) != bytes)
		return 0;
	for (size_t i = 0; i < bytes; ++i) {
		unsigned char left = name[i], right = (unsigned char)wanted[i];
		if (left >= 'A' && left <= 'Z')
			left += 'a' - 'A';
		if (right >= 'A' && right <= 'Z')
			right += 'a' - 'A';
		if (left != right)
			return 0;
	}
	return 1;
}

/* Bounded raw header lookup, independent of NUL termination in resbuf.
 * 0 found, 1 absent, -1 found value exceeds the original caller's 255 bytes. */
static int native_http_header_value(const unsigned char *headers, size_t bytes,
				    const char *name,
				    const unsigned char **value,
				    size_t *value_bytes)
{
	size_t start = 0;
	while (start < bytes) {
		size_t end = start;
		while (end < bytes && headers[end] != '\n')
			end++;
		size_t content_end = end;
		if (content_end > start && headers[content_end - 1] == '\r')
			content_end--;
		const unsigned char *colon =
			memchr(headers + start, ':', content_end - start);
		if (colon && native_http_name_equal(
				     headers + start,
				     (size_t)(colon - headers - start), name)) {
			size_t first = (size_t)(colon - headers) + 1;
			while (first < content_end && (headers[first] == ' ' ||
						       headers[first] == '\t'))
				first++;
			while (content_end > first &&
			       (headers[content_end - 1] == ' ' ||
				headers[content_end - 1] == '\t'))
				content_end--;
			*value = headers + first;
			*value_bytes = content_end - first;
			return *value_bytes <= 254 ? 0 : -1;
		}
		start = end < bytes ? end + 1 : bytes;
	}
	return 1;
}

static int native_http_response_bounds(const struct mre_http_response *response)
{
	if (response->header_bytes > MRE_HTTP_HEADER_BYTES ||
	    response->body_bytes > MRE_HTTP_BODY_BYTES)
		return 0;
	for (size_t i = 0; i < response->header_bytes; ++i) {
		unsigned char byte = response->headers[i];
		if (!byte || byte == 127 ||
		    (byte < 32 && byte != '\t' && byte != '\r' &&
		     byte != '\n') ||
		    (byte == '\r' && (i + 1 == response->header_bytes ||
				      response->headers[i + 1] != '\n')))
			return 0;
	}
	const unsigned char *value;
	size_t bytes;
	int found = native_http_header_value(response->headers,
					     response->header_bytes,
					     "Content-Range", &value, &bytes);
	/* Missing/malformed Content-Range belongs to the original's own branch.
 * Reject only a numeric total that demonstrably exceeds the file budget. */
	if (!found) {
		const unsigned char *slash = memchr(value, '/', bytes);
		const unsigned char *end = value + bytes;
		if (slash && slash + 1 < end && slash[1] >= '0' &&
		    slash[1] <= '9') {
			uint32_t total = 0;
			for (const unsigned char *p = slash + 1;
			     p < end && *p >= '0' && *p <= '9'; ++p) {
				unsigned digit = (unsigned)(*p - '0');
				if (total > (MRE_FILE_BYTES - digit) / 10U)
					return 0;
				total = total * 10U + digit;
			}
		}
	}
	return 1;
}

static void native_http_free_session(struct sdk_http_session *session)
{
	if (!session)
		return;
	size_t released;
	if (mre_memory_free(&app.memory, session, &released) !=
	    MRE_MEMORY_RELEASED)
		fail("HTTP session allocation was released by the guest");
	service_memory_refresh();
}

static void native_http_cancel_all(void)
{
	if (native_http.active_id)
		native_http_emit("HTTP_CANCEL", native_http.active_id);
	native_http.active_id = 0;
	native_http.request_identity = 0;
	native_http.response_ready = 0;
	native_http.hook = NULL;
	native_http.notify = NULL;
	native_http_free_session(native_http.session);
	native_http.session = NULL;
	/* A callback's session remains readable until that callback returns. */
	if (native_http.callback_session)
		native_http.callback_cancelled = 1;
}

int32_t MRE_ABI host_asyn_http_req(struct sdk_asyn_http_request *request,
				   sdk_http_hook_fn hook,
				   sdk_http_notify_fn notify)
{
	check_guest_api_stack();
	if (!request)
		return 0; /* Documented successful no-op. */
	if (!app.shared || !app.shared->http_available || app.exited)
		return 2;
	if (native_http.active_id || native_http.next_id == INT32_MAX)
		return 4;
	if (((uintptr_t)request & 3) ||
	    !guest_data(request, sizeof(*request), PF_R) ||
	    !guest_code((uintptr_t)hook) ||
	    (notify && !guest_code((uintptr_t)notify)))
		return 4;
	struct sdk_asyn_http_request outer;
	memcpy(&outer, request, sizeof(outer));
	if (outer.method != 1)
		return 1;
	if (outer.proxy != 2)
		return 3;
	const struct sdk_http_request *guest =
		(const void *)(uintptr_t)outer.request;
	if (((uintptr_t)guest & 3) || !guest_data(guest, sizeof(*guest), PF_R))
		return 4;
	struct sdk_http_request copied;
	memcpy(&copied, guest, sizeof(copied));
	struct mre_http_request wire = { 0 };
	if (!native_http_copy_string(wire.url, sizeof(wire.url), guest->url))
		return 5;
	char host[100];
	int32_t port;
	if (!native_http_url_parts(wire.url, host, &port))
		return 5;
	if (copied.header_count < 0 || copied.header_count > 1 ||
	    copied.body_bytes < 0 ||
	    copied.body_bytes > (int32_t)sizeof(wire.body))
		return 4;
	if (copied.header_count) {
		const struct sdk_http_header *header =
			(const void *)(uintptr_t)copied.headers;
		if (!guest_data(header, sizeof(*header), PF_R) ||
		    !native_http_copy_string(wire.header_name,
					     sizeof(wire.header_name),
					     header->name) ||
		    !native_http_copy_string(wire.header_value,
					     sizeof(wire.header_value),
					     header->value) ||
		    !native_http_valid_request_header(wire.header_name,
						      wire.header_value))
			return 4;
	}
	if (copied.body_bytes) {
		const void *body = (const void *)(uintptr_t)copied.body;
		if (!guest_data(body, (size_t)copied.body_bytes, PF_R))
			return 4;
		memcpy(wire.body, body, (size_t)copied.body_bytes);
	}
	size_t session_bytes =
		sizeof(struct sdk_http_session) + MRE_HTTP_BODY_BYTES;
	if (app.canvas_bytes > app.ram_limit ||
	    app.memory.bytes > app.ram_limit - app.canvas_bytes ||
	    session_bytes > app.ram_limit - app.canvas_bytes - app.memory.bytes)
		return 4;
	struct sdk_http_session *session =
		mre_memory_calloc(&app.memory, (int32_t)session_bytes);
	if (!session)
		return 4;
	service_memory_refresh();
	wire.id = ++native_http.next_id;
	wire.method = outer.method;
	wire.proxy = outer.proxy;
	wire.header_count = (uint32_t)copied.header_count;
	wire.body_bytes = (uint32_t)copied.body_bytes;
	session->socket =
		(int32_t)wire.id; /* Never a parent socket descriptor. */
	memcpy(session->url, wire.url, sizeof(session->url));
	memcpy(session->host, host, strlen(host) + 1);
	session->port = port;
	session->body = (uint32_t)(uintptr_t)(session + 1);
	session->hook = (uint32_t)(uintptr_t)hook;
	native_http.active_id = wire.id;
	native_http.request_identity = (uintptr_t)request;
	native_http.session = session;
	native_http.hook = hook;
	native_http.notify = notify;
	native_http.response_ready = 0;
	memcpy(&app.shared->http_request, &wire, sizeof(wire));
	__atomic_thread_fence(__ATOMIC_RELEASE);
	native_http_emit("HTTP_REQUEST", wire.id);
	utility_http_accept(wire.id);
	return 0;
}

int32_t MRE_ABI host_get_asyn_http_req_handle(
	struct sdk_asyn_http_request *request, int32_t *handle)
{
	check_guest_api_stack();
	if (!request || ((uintptr_t)request & 3) ||
	    !guest_data(request, sizeof(*request), PF_R) || !handle ||
	    ((uintptr_t)handle & 3) ||
	    !guest_data(handle, sizeof(*handle), PF_W))
		return -2;
	if (!native_http.active_id ||
	    native_http.request_identity != (uintptr_t)request)
		return -1;
	*handle = (int32_t)native_http.active_id;
	return 0;
}

int32_t MRE_ABI host_get_http_head(struct sdk_http_session *session,
				   const char *name, char *value)
{
	check_guest_api_stack();
	if (session != native_http.callback_session ||
	    !guest_data(session, sizeof(*session), PF_R) ||
	    session->header_bytes < 0 ||
	    session->header_bytes > (int32_t)sizeof(session->headers))
		return -1;
	char wanted[128];
	if (!native_http_copy_string(wanted, sizeof(wanted), name) ||
	    !wanted[0])
		return -1;
	const unsigned char *found;
	size_t bytes;
	if (native_http_header_value(session->headers,
				     (size_t)session->header_bytes, wanted,
				     &found, &bytes) ||
	    !guest_data(value, bytes + 1, PF_W))
		return -1;
	memcpy(value, found, bytes);
	value[bytes] = 0;
	return 0;
}

int32_t MRE_ABI host_cancel_asyn_http_req(int32_t handle)
{
	check_guest_api_stack();
	if (handle <= 0 || (uint32_t)handle != native_http.active_id)
		return -1;
	native_http_cancel_all();
	return 0;
}

void MRE_ABI host_cancel_all_http_sessions(void)
{
	check_guest_api_stack();
	native_http_cancel_all();
}

/* Called on a managed host stack, including while awaiting a frame/RPC ACK.
 * Copy/queue only: callbacks run later, after the current guest invocation. */
static int native_http_receive_response(uint32_t id)
{
	if (!id || id != native_http.active_id || app.exited ||
	    native_http.response_ready)
		return 0;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	const struct mre_http_response *response = &app.shared->http_response;
	if (response->id != id)
		return 0;
	struct sdk_http_session *session = native_http.session;
	if (!session ||
	    !guest_data(session, sizeof(*session) + MRE_HTTP_BODY_BYTES, PF_R))
		fail("HTTP response session outside its SDK allocation");
	int valid = native_http_response_bounds(response);
	native_http.response_failure = response->transport != 0 || !valid;
	session->status = native_http.response_failure ? -1 : response->status;
	if (valid) {
		session->header_bytes = (int32_t)response->header_bytes;
		session->body_bytes = (int32_t)response->body_bytes;
		memcpy(session->headers, response->headers,
		       response->header_bytes);
		memcpy(session + 1, response->body, response->body_bytes);
	}
	if (native_http.response_failure) {
		const char *error = valid ? "HTTP transport failed" :
					    "HTTP response exceeds limits";
		for (size_t i = 0; error[i] && i + 1 < 50; ++i)
			session->last_error[i] = (unsigned char)error[i];
	}
	native_http.response_ready = 1;
	return 1;
}

static int native_http_response_pending(void)
{
	return native_http.response_ready;
}

static void native_http_dispatch_response(void)
{
	if (app.phase)
		fail("HTTP callback attempted guest reentry");
	if (app.exited) {
		native_http_cancel_all();
		return;
	}
	if (!native_http.response_ready)
		return;
	struct sdk_http_session *session = native_http.session;
	sdk_http_hook_fn hook = native_http.hook;
	sdk_http_notify_fn notify = native_http.notify;
	int failure = native_http.response_failure;
	uint32_t id = native_http.active_id;
	/* Retire before callback: original tail-submit acquires a fresh slot, while
 * the former session remains alive independently until this callback ends. */
	native_http.active_id = 0;
	native_http.request_identity = 0;
	native_http.session = NULL;
	native_http.hook = NULL;
	native_http.notify = NULL;
	native_http.response_ready = 0;
	native_http.callback_session = session;
	native_http.callback_cancelled = 0;
	if (!failure && notify) {
		/* These events describe the already received response. No synthetic
		 * hostname/connection/send progress is reported. */
		for (int32_t state = 4; state <= 6 && !app.exited &&
					!native_http.callback_cancelled;
		     ++state) {
			app.phase = 2;
			callback_call((uintptr_t)notify, (uint32_t)state, 0,
				      (uint32_t)(uintptr_t)session, 0);
			app.phase = 0;
		}
	}
	if (!app.exited && !native_http.callback_cancelled) {
		debug_trace("DISPATCH HTTP id=%" PRIu32
			    " failure=%d status=%d bytes=%d\n",
			    id, failure, session->status, session->body_bytes);
		app.phase = 2;
		callback_call((uintptr_t)hook, (uint32_t)failure,
			      (uint32_t)(uintptr_t)session, 0, 0);
		app.phase = 0;
	}
	native_http.callback_session = NULL;
	native_http.callback_cancelled = 0;
	native_http_free_session(session);
	if (app.exited)
		native_http_cancel_all();
}
