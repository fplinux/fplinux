/* SPDX-License-Identifier: 0BSD */
/* One SDK namespace for every guest. Compiler differences select an ABI
 * trampoline only after resolving the same implementation. */
#include "native-bridge-map.inc"
struct sdk_api {
	const char *name;
	uintptr_t address;
};
static const struct sdk_api sdk_apis[] = {
	{ "vm_res_init_with_language", (uintptr_t)host_res_init_with_language },
	{ "get_http_head", (uintptr_t)host_get_http_head },
	{ "rand", (uintptr_t)host_rand },
	{ "strtoi", (uintptr_t)utility_strtoi },
	{ "vm_app_log", (uintptr_t)host_app_log },
	{ "vm_ascii_to_ucs2", (uintptr_t)host_ascii_to_wide },
	{ "vm_asyn_http_req", (uintptr_t)host_asyn_http_req },
	{ "vm_audio_play_bytes", (uintptr_t)host_audio_play_bytes },
	{ "vm_audio_stop", (uintptr_t)host_audio_stop },
	{ "vm_calloc", (uintptr_t)host_calloc },
	{ "vm_cancel_all_http_sessions",
	  (uintptr_t)host_cancel_all_http_sessions },
	{ "vm_cancel_asyn_http_req", (uintptr_t)host_cancel_asyn_http_req },
	{ "vm_chset_convert", (uintptr_t)host_charset },
	{ "vm_create_service", (uintptr_t)utility_create_service },
	{ "vm_create_timer", (uintptr_t)host_create_timer },
	{ "vm_create_timer_ex", (uintptr_t)host_create_timer_ex },
	{ "vm_delete_timer", (uintptr_t)host_delete_timer },
	{ "vm_delete_timer_ex", (uintptr_t)host_delete_timer_ex },
	{ "vm_ends_with", (uintptr_t)utility_ends_with },
	{ "vm_exit_app", (uintptr_t)host_exit },
	{ "vm_file_close", (uintptr_t)utility_file_close },
	{ "vm_file_commit", (uintptr_t)utility_file_commit },
	{ "vm_file_copy", (uintptr_t)utility_file_copy },
	{ "vm_file_delete", (uintptr_t)utility_file_delete },
	{ "vm_file_get_attributes", (uintptr_t)utility_file_get_attributes },
	{ "vm_file_getfilesize", (uintptr_t)utility_file_getfilesize },
	{ "vm_file_is_eof", (uintptr_t)utility_file_eof },
	{ "vm_file_mkdir", (uintptr_t)utility_file_mkdir },
	{ "vm_file_open", (uintptr_t)utility_file_open },
	{ "vm_file_read", (uintptr_t)utility_file_read },
	{ "vm_file_rename", (uintptr_t)utility_file_rename },
	{ "vm_file_rmdir", (uintptr_t)utility_file_rmdir },
	{ "vm_file_seek", (uintptr_t)utility_file_seek },
	{ "vm_file_set_attributes", (uintptr_t)utility_file_set_attributes },
	{ "vm_file_tell", (uintptr_t)utility_file_tell },
	{ "vm_file_write", (uintptr_t)utility_file_write },
	{ "vm_find_close", (uintptr_t)utility_find_close_ext },
	{ "vm_find_close_ext", (uintptr_t)utility_find_close_ext },
	{ "vm_find_first", (uintptr_t)utility_find_first },
	{ "vm_find_first_ext", (uintptr_t)utility_find_first_ext },
	{ "vm_find_next", (uintptr_t)utility_find_next },
	{ "vm_find_next_ext", (uintptr_t)utility_find_next_ext },
	{ "vm_font_set_font_size", (uintptr_t)host_font_size },
	{ "vm_font_set_font_style", (uintptr_t)host_font_style },
	{ "vm_free", (uintptr_t)host_free },
	{ "vm_fs_async_commit", (uintptr_t)utility_file_commit },
	{ "vm_gb2312_to_ucs2", (uintptr_t)host_gb2312_to_wide },
	{ "vm_get_asyn_http_req_handle",
	  (uintptr_t)host_get_asyn_http_req_handle },
	{ "vm_get_exec_filename", (uintptr_t)host_exec_filename },
	{ "vm_get_filename", (uintptr_t)host_get_filename },
	{ "vm_get_gmobi_language", (uintptr_t)host_gmobi_language },
	{ "vm_get_malloc_stat", (uintptr_t)host_get_malloc_stat },
	{ "vm_get_path", (uintptr_t)utility_get_path },
	{ "vm_get_removeable_driver", (uintptr_t)host_removable },
	{ "vm_get_system_driver", (uintptr_t)host_system_drive },
	{ "vm_get_tick_count", (uintptr_t)host_tick_count },
	{ "vm_get_time", (uintptr_t)host_get_time },
	{ "vm_graphic_active_layer", (uintptr_t)host_active_layer },
	{ "vm_graphic_blt", (uintptr_t)host_blt },
	{ "vm_graphic_blt_ex", (uintptr_t)host_blt_ex },
	{ "vm_graphic_canvas_set_trans_color",
	  (uintptr_t)host_canvas_set_trans_color },
	{ "vm_graphic_clear_layer_bg", (uintptr_t)host_clear_layer },
	{ "vm_graphic_create_canvas", (uintptr_t)host_create_canvas },
	{ "vm_graphic_create_layer", (uintptr_t)host_create_layer },
	{ "vm_graphic_create_layer_ex",
	  (uintptr_t)host_handheld_create_layer_ex },
	{ "vm_graphic_delete_layer", (uintptr_t)host_delete_layer },
	{ "vm_graphic_draw_image_from_memory", (uintptr_t)host_draw_image },
	{ "vm_graphic_draw_resized_image_from_memory",
	  (uintptr_t)host_draw_resized_image },
	{ "vm_graphic_fill_ellipse_ex", (uintptr_t)host_fill_ellipse_ex },
	{ "vm_graphic_fill_polygon", (uintptr_t)host_fill_polygon },
	{ "vm_graphic_fill_rect", (uintptr_t)host_fill },
	{ "vm_graphic_fill_rect_ex", (uintptr_t)host_fill_ex },
	{ "vm_graphic_fill_roundrect", (uintptr_t)host_fill_roundrect },
	{ "vm_graphic_fill_roundrect_ex", (uintptr_t)host_fill_roundrect_ex },
	{ "vm_graphic_flush_layer", (uintptr_t)host_flush },
	{ "vm_graphic_flush_layer_ex", (uintptr_t)host_flush_region },
	{ "vm_graphic_flush_screen", (uintptr_t)host_flush_screen },
	{ "vm_graphic_get_canvas_buffer", (uintptr_t)host_canvas_buffer },
	{ "vm_graphic_get_character_height", (uintptr_t)host_character_height },
	{ "vm_graphic_get_character_width", (uintptr_t)host_character_width },
	{ "vm_graphic_get_frame_number", (uintptr_t)host_frame_number },
	{ "vm_graphic_get_img_property", (uintptr_t)host_img_property },
	{ "vm_graphic_get_img_property_ex", (uintptr_t)host_img_property_ex },
	{ "vm_graphic_get_layer_buffer", (uintptr_t)host_layer_buffer },
	{ "vm_graphic_get_pixel", (uintptr_t)host_get_pixel },
	{ "vm_graphic_get_screen_height", (uintptr_t)host_screen_height },
	{ "vm_graphic_get_screen_width", (uintptr_t)host_screen_width },
	{ "vm_graphic_get_string_baseline", (uintptr_t)host_string_baseline },
	{ "vm_graphic_get_string_height", (uintptr_t)host_string_height },
	{ "vm_graphic_get_string_width", (uintptr_t)host_string_width },
	{ "vm_graphic_is_r2l_state", (uintptr_t)host_is_r2l },
	{ "vm_graphic_line", (uintptr_t)host_line },
	{ "vm_graphic_line_ex", (uintptr_t)host_line_ex },
	{ "vm_graphic_load_image", (uintptr_t)host_load_image },
	{ "vm_graphic_load_image_resized", (uintptr_t)host_load_image_resized },
	{ "vm_graphic_mirror", (uintptr_t)host_mirror },
	{ "vm_graphic_polygon", (uintptr_t)host_polygon },
	{ "vm_graphic_rect", (uintptr_t)host_rect },
	{ "vm_graphic_rect_ex", (uintptr_t)host_rect_ex },
	{ "vm_graphic_release_canvas", (uintptr_t)host_release_canvas },
	{ "vm_graphic_rotate", (uintptr_t)host_rotate },
	{ "vm_graphic_roundrect", (uintptr_t)host_roundrect },
	{ "vm_graphic_roundrect_ex", (uintptr_t)host_roundrect_ex },
	{ "vm_graphic_set_alpha_blending_layer", (uintptr_t)host_alpha_layer },
	{ "vm_graphic_set_clip", (uintptr_t)host_clip },
	{ "vm_graphic_set_font", (uintptr_t)host_set_font },
	{ "vm_graphic_set_layer_opacity", (uintptr_t)host_layer_opacity },
	{ "vm_graphic_set_pixel", (uintptr_t)host_set_pixel },
	{ "vm_graphic_set_pixel_ex", (uintptr_t)host_set_pixel_ex },
	{ "vm_graphic_setcolor", (uintptr_t)host_set_color },
	{ "vm_graphic_textout", (uintptr_t)host_textout },
	{ "vm_graphic_textout_by_baseline", (uintptr_t)host_text_baseline },
	{ "vm_graphic_textout_to_layer", (uintptr_t)host_text_to_layer },
	{ "vm_input_set_editor_title", (uintptr_t)host_editor_title },
	{ "vm_input_text3", (uintptr_t)host_input_text },
	{ "vm_is_support_pen_touch", (uintptr_t)host_pen_supported },
	{ "vm_load_resource", (uintptr_t)host_load_resource },
	{ "vm_lower_case", (uintptr_t)host_lower_case },
	{ "vm_malloc", (uintptr_t)host_malloc },
	{ "vm_midi_pause", (uintptr_t)host_midi_pause },
	{ "vm_midi_play", (uintptr_t)host_midi_play_resource },
	{ "vm_midi_play_by_bytes", (uintptr_t)host_midi_play },
	{ "vm_midi_resume", (uintptr_t)host_midi_resume },
	{ "vm_midi_stop", (uintptr_t)host_midi_stop },
	{ "vm_midi_stop_all", (uintptr_t)host_midi_stop_all },
	{ "vm_mul_lang_deinit", (uintptr_t)host_mul_lang_deinit },
	{ "vm_mul_lang_get_string", (uintptr_t)host_mul_lang_get_string },
	{ "vm_mul_lang_init", (uintptr_t)host_mul_lang_init },
	{ "vm_pmng_get_current_handle", (uintptr_t)host_current_handle },
	{ "vm_pmng_state", (uintptr_t)host_process_state },
	{ "vm_realloc", (uintptr_t)host_realloc },
	{ "vm_reg_keyboard_callback", (uintptr_t)host_register_key },
	{ "vm_reg_pen_callback", (uintptr_t)host_register_pen },
	{ "vm_reg_res_provider", (uintptr_t)host_reg_res_provider },
	{ "vm_reg_sysevt_callback", (uintptr_t)host_register_system },
	{ "vm_res_deinit", (uintptr_t)host_res_deinit },
	{ "vm_res_get_string", (uintptr_t)host_res_string },
	{ "vm_res_init", (uintptr_t)host_res_init },
	{ "vm_selector_run", (uintptr_t)host_selector },
	{ "vm_safe_wstrcpy", (uintptr_t)host_safe_wstrcpy },
	{ "vm_set_volume", (uintptr_t)host_volume },
	{ "vm_sim_card_count", (uintptr_t)utility_sim_count },
	{ "vm_storage_close", (uintptr_t)host_storage_close },
	{ "vm_storage_open", (uintptr_t)host_storage_open },
	{ "vm_storage_read", (uintptr_t)host_storage_read },
	{ "vm_storage_write", (uintptr_t)host_storage_write },
	{ "vm_string_equals_ignore_case", (uintptr_t)utility_string_equals },
	{ "vm_switch_power_saving_mode", (uintptr_t)host_power },
	{ "vm_ucs2_to_ascii", (uintptr_t)host_wide_to_ascii },
	{ "vm_ucs2_to_gb2312", (uintptr_t)utility_wide_to_gb2312 },
	{ "vm_vibrator_once", (uintptr_t)utility_vibrator_once },
	{ "vm_vsprintf", (uintptr_t)host_vsprintf },
	{ "vm_wstrcat", (uintptr_t)host_wstrcat },
	{ "vm_wstrcmp", (uintptr_t)host_wstrcmp },
	{ "vm_wstrcpy", (uintptr_t)host_wstrcpy },
	{ "vm_wstrlen", (uintptr_t)host_wstrlen },
	{ "vm_wstrncpy", (uintptr_t)host_wstrncpy },
};
uintptr_t MRE_ABI host_resolve(const char *name)
{
	check_guest_api_stack();
	size_t length;
	for (length = 0; length < 128; ++length) {
		if (!guest_data(name + length, 1, PF_R))
			fail("resolver name is outside guest memory");
		if (!name[length])
			break;
	}
	if (length == 128)
		fail("resolver name is not bounded/NUL terminated");
	uintptr_t address = handheld_resolve(name);
	if (!address)
		address = camera_resolve(name);
	if (!address)
		address = media_resolve(name);
	if (!address)
		address = editor_resolve(name);
	for (size_t i = 0;
	     !address && i < sizeof(sdk_apis) / sizeof(sdk_apis[0]); ++i)
		if (!strcmp(name, sdk_apis[i].name))
			address = sdk_apis[i].address;
	if (!address) {
		fprintf(stderr, "IMPORT %s -> NULL (SDK API unavailable)\n",
			name);
		return 0;
	}
	uintptr_t adapted = adapt_api_address(address);
	debug_trace("IMPORT %s -> 0x%08" PRIxPTR " (SDK %s ABI)\n", name,
		    adapted, guest_uses_ads_abi() ? "ADS" : "AAPCS");
	return adapted;
}
