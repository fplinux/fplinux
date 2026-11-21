/* SPDX-License-Identifier: 0BSD */
/* Modal UI and event delivery use SDK callbacks, independently of executable
 * name, compiler, application category, or expected callback output. */
static uint32_t invoke_guest(uintptr_t callback, uint32_t first,
			     uint32_t second, uint32_t third, uint32_t fourth)
{
	if (!guest_code(callback))
		fail("callback is outside guest executable memory");
	int outer_phase = app.phase;
	app.phase = 2;
	uint32_t result = guest_uses_ads_abi() ?
				  ads_guest_call(callback, app.stack_top,
						 app.ads.data_base, first,
						 second, third, fourth) :
				  arm_guest_call4(callback, app.stack_top,
						  first, second, third, fourth);
	app.phase = outer_phase;
	return result;
}

static int dispatch_editor(void)
{
	call_event(app.system_callback, 3, 0, "INACTIVE for editor");
	call_event(app.system_callback, 6, 0, "HIDE for editor");
	if (!publish_frame())
		return 0;
	size_t bytes = strlen(app.editor_default);
	memcpy(app.shared->editor_data, app.editor_default, bytes + 1);
	app.shared->editor_bytes = (uint32_t)bytes;
	app.shared->editor_max_shorts = app.editor_max;
	app.shared->editor_method = app.editor_method;
	memcpy(app.shared->editor_title, app.editor_title,
	       strlen(app.editor_title) + 1);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	send_control(
		(struct mre_control_message){ .opcode = MRE_CONTROL_INPUT });
	struct mre_control_message reply;

	if (!wait_for_viewer_reply(&reply,
				   (struct mre_control_message){
					   .opcode = MRE_CONTROL_INPUT_DONE,
				   },
				   MRE_CONTROL_INPUT_CANCEL))
		return 0;
	int32_t state = reply.opcode == MRE_CONTROL_INPUT_DONE;

	app.input_text[0] = 0;
	if (state == 1) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		bytes = app.shared->editor_bytes;
		size_t units = 0;
		if (bytes > MRE_EDITOR_BYTES ||
		    app.shared->editor_data[bytes] ||
		    !mre_utf8_decode(
			    app.shared->editor_data, bytes, app.input_text,
			    app.input_text_bytes / sizeof(*app.input_text),
			    &units) ||
		    units > (size_t)app.editor_max)
			fail("editor reply exceeds SDK text capacity or UCS2 encoding");
	}
	/* SDK screen restoration precedes the completion callback. A repeated
	 * request for the still-active editor during PAINT is idempotent. */
	app.editor_resuming = 1;
	call_event(app.system_callback, 2, 0, "ACTIVE after editor");
	call_event(app.system_callback, 1, 0, "PAINT after editor");
	app.editor_resuming = 0;
	input_fn callback = app.input_callback;
	app.pending = 0;
	app.input_callback = NULL;
	if (!app.exited)
		(void)invoke_guest((uintptr_t)callback, (uint32_t)state,
				   (uint32_t)(uintptr_t)app.input_text, 0, 0);
	return publish_frame();
}

static int dispatch_selector(void)
{
	call_event(app.system_callback, 3, 0, "INACTIVE for selector");
	call_event(app.system_callback, 6, 0, "HIDE for selector");
	if (!publish_frame())
		return 0;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	send_control(
		(struct mre_control_message){ .opcode = MRE_CONTROL_SELECTOR });
	struct mre_control_message reply;
	uint16_t *selected = NULL;
	size_t units = 0;

	if (!wait_for_viewer_reply(&reply,
				   (struct mre_control_message){
					   .opcode = MRE_CONTROL_SELECT,
				   },
				   MRE_CONTROL_SELECT_CANCEL))
		return 0;
	if (reply.opcode == MRE_CONTROL_SELECT) {
		const char *path = reply.text.data;

		if (reply.text.length < 3 || path[1] != ':' ||
		    path[2] != '\\' || (path[0] != 'E' && path[0] != 'C') ||
		    !mre_utf8_decode((const unsigned char *)path,
				     reply.text.length, app.input_text,
				     MRE_VFS_PATH_BYTES, &units))
			fail("selected virtual path is not bounded UTF8/UCS2");
		selected = app.input_text;
	}
	selector_fn callback = app.selector_callback;
	app.selector_pending = 0;
	app.selector_callback = NULL;
	if (!app.exited)
		(void)invoke_guest((uintptr_t)callback,
				   (uint32_t)(uintptr_t)selected,
				   (uint32_t)units, 0, 0);
	call_event(app.system_callback, 2, 0, "ACTIVE after selector");
	call_event(app.system_callback, 1, 0, "PAINT after selector");
	return publish_frame();
}

static int guest_has_callbacks(void)
{
	if (app.system_callback || app.key_callback || app.pen_callback ||
	    app.pending || app.selector_pending)
		return 1;
	for (size_t i = 0; i < ALL_TIMER_CAP; ++i)
		if (app.timers[i].callback)
			return 1;
	uint64_t now = monotonic_ms();
	return camera_timeout_ms(now) >= 0 || media_timeout_ms(now) >= 0 ||
	       editor_timeout_ms(now) >= 0 || native_http.active_id ||
	       native_http_response_pending() || native_audio_pending();
}

static void dispatch_viewer(void)
{
	send_control((struct mre_control_message){
		.opcode = MRE_CONTROL_DISPLAY,
		.display = { .width = MRE_FRAME_WIDTH,
			     .height = MRE_FRAME_HEIGHT },
	});
	call_event(app.system_callback, 4, 0, "CREATE");
	app.paint_pending = 0;
	if (!app.pending && !app.selector_pending)
		call_event(app.system_callback, 1, 0, "PAINT");
	if (!publish_frame())
		return;
	if (!app.exited && !guest_has_callbacks())
		app.exited = 1;
	struct mre_control_message message;
	while (!app.exited) {
		if (app.selector_pending && !dispatch_selector())
			return;
		if (app.pending && !dispatch_editor())
			return;
		if (app.exited)
			break;
		if (app.selector_pending || app.pending)
			continue;
		if (!receive_interactive_command(&message))
			break;
		if (queue_service_message(&message))
			continue;
		switch (message.opcode) {
		case MRE_CONTROL_KEY: {
			int32_t event = message.input.key.event;
			int32_t key = message.input.key.code;

			debug_trace("KEY concurrent mode=%d\n",
				    handheld_two_key_mode());
			if (app.visible) {
				(void)editor_key(event, key);
				call_event(app.key_callback, event, key, "KEY");
			}
			break;
		}
		case MRE_CONTROL_PEN: {
			int32_t x = message.input.pen.x;
			int32_t y = message.input.pen.y;

			if (app.visible && app.pen_callback) {
				struct display_viewport viewport =
					display_viewport();
				x = (x - viewport.x) * guest_screen_width() /
				    viewport.width;
				y = (y - viewport.y) * guest_screen_height() /
				    viewport.height;
				if (x < 0)
					x = 0;
				if (y < 0)
					y = 0;
				if (x >= guest_screen_width())
					x = guest_screen_width() - 1;
				if (y >= guest_screen_height())
					y = guest_screen_height() - 1;
				(void)invoke_guest((uintptr_t)app.pen_callback,
						   message.input.pen.event, x,
						   y, 0);
			}
			break;
		}
		case MRE_CONTROL_VISIBILITY: {
			bool visible = message.input.visible;

			if (visible != app.visible) {
				app.visible = visible;
				call_event(app.system_callback, visible ? 2 : 3,
					   0, visible ? "ACTIVE" : "INACTIVE");
				if (visible)
					call_event(app.system_callback, 1, 0,
						   "PAINT");
				else
					call_event(app.system_callback, 6, 0,
						   "HIDE");
			}
			break;
		}
		default:
			fail("unexpected viewer event opcode %u",
			     message.opcode);
		}
		if (!publish_frame())
			return;
	}
	if (app.exited)
		send_control((struct mre_control_message){
			.opcode = MRE_CONTROL_END });
}
