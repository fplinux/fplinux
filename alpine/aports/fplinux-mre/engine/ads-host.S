/* SPDX-License-Identifier: 0BSD */
.syntax unified
.arch armv7-a
.fpu vfpv3-d16
.text
.include "ads_api.inc"
ADS_API ads_camera_create, camera_create, 0
ADS_API ads_camera_get_frame, camera_get_frame, 0
ADS_API ads_camera_get_status, camera_get_status, 0
ADS_API ads_camera_main_id, camera_main_id, 0
ADS_API ads_camera_preview_start, camera_preview_start, 0
ADS_API ads_camera_preview_stop, camera_preview_stop, 0
ADS_API ads_camera_register_notify, camera_register_notify, 0
ADS_API ads_camera_release, camera_release, 0
ADS_API ads_camera_set_fps, camera_set_fps, 0
ADS_API ads_camera_set_size, camera_set_size, 0
ADS_API ads_camera_support_size, camera_support_size, 0
ADS_API ads_host_active_layer, host_active_layer, 0
ADS_API ads_host_alpha_layer, host_alpha_layer, 0
ADS_API ads_host_app_log, host_app_log, 0
ADS_API ads_host_ascii_to_wide, host_ascii_to_wide, 0
ADS_API ads_host_asyn_http_req, host_asyn_http_req, 0
.ltorg
ADS_API ads_host_audio_play_bytes, host_audio_play_bytes, 2
ADS_API ads_host_audio_stop, host_audio_stop, 0
ADS_API ads_host_blt, host_blt, 5
ADS_API ads_host_blt_ex, host_blt_ex, 6
ADS_API ads_host_calloc, host_calloc, 0
ADS_API ads_host_cancel_all_http_sessions, host_cancel_all_http_sessions, 0
ADS_API ads_host_cancel_asyn_http_req, host_cancel_asyn_http_req, 0
ADS_API ads_host_canvas_buffer, host_canvas_buffer, 0
ADS_API ads_host_canvas_set_trans_color, host_canvas_set_trans_color, 0
ADS_API ads_host_character_height, host_character_height, 0
ADS_API ads_host_character_width, host_character_width, 0
ADS_API ads_host_charset, host_charset, 1
ADS_API ads_host_clear_layer, host_clear_layer, 0
ADS_API ads_host_clip, host_clip, 0
ADS_API ads_host_create_canvas, host_create_canvas, 0
ADS_API ads_host_create_layer, host_create_layer, 1
.ltorg
ADS_API ads_host_create_timer, host_create_timer, 0
ADS_API ads_host_create_timer_ex, host_create_timer_ex, 0
ADS_API ads_host_current_handle, host_current_handle, 0
ADS_API ads_host_delete_layer, host_delete_layer, 0
ADS_API ads_host_delete_timer, host_delete_timer, 0
ADS_API ads_host_delete_timer_ex, host_delete_timer_ex, 0
ADS_API ads_host_draw_image, host_draw_image, 1
ADS_API ads_host_draw_resized_image, host_draw_resized_image, 3
ADS_API ads_host_editor_activate, host_editor_activate, 0
ADS_API ads_host_editor_border, host_editor_border, 0
ADS_API ads_host_editor_close, host_editor_close, 0
ADS_API ads_host_editor_create, host_editor_create, 5
ADS_API ads_host_editor_cursor, host_editor_cursor, 0
ADS_API ads_host_editor_deactivate, host_editor_deactivate, 0
ADS_API ads_host_editor_font, host_editor_font, 0
ADS_API ads_host_editor_get_text, host_editor_get_text, 0
.ltorg
ADS_API ads_host_editor_pos, host_editor_pos, 0
ADS_API ads_host_editor_redraw_ime, host_editor_redraw_ime, 0
ADS_API ads_host_editor_save, host_editor_save, 0
ADS_API ads_host_editor_set_ime, host_editor_set_ime, 1
ADS_API ads_host_editor_set_softkey, host_editor_set_softkey, 0
ADS_API ads_host_editor_set_text, host_editor_set_text, 0
ADS_API ads_host_editor_set_update_text, host_editor_set_update_text, 0
ADS_API ads_host_editor_show, host_editor_show, 0
ADS_API ads_host_editor_size, host_editor_size, 0
ADS_API ads_host_editor_softkey_height, host_editor_softkey_height, 0
ADS_API ads_host_editor_tcp_close, host_editor_tcp_close, 0
ADS_API ads_host_editor_tcp_connect, host_editor_tcp_connect, 0
ADS_API ads_host_editor_tcp_read, host_editor_tcp_read, 0
ADS_API ads_host_editor_tcp_write, host_editor_tcp_write, 0
ADS_API ads_host_editor_title, host_editor_title, 0
ADS_API ads_host_exec_filename, host_exec_filename, 0
.ltorg
ADS_API ads_host_exit, host_exit, 0
ADS_API ads_host_fill, host_fill, 3
ADS_API ads_host_fill_ellipse_ex, host_fill_ellipse_ex, 1
ADS_API ads_host_fill_ex, host_fill_ex, 1
ADS_API ads_host_fill_polygon, host_fill_polygon, 0
ADS_API ads_host_fill_roundrect, host_fill_roundrect, 3
ADS_API ads_host_fill_roundrect_ex, host_fill_roundrect_ex, 2
ADS_API ads_host_flush, host_flush, 0
ADS_API ads_host_flush_region, host_flush_region, 2
ADS_API ads_host_flush_screen, host_flush_screen, 0
ADS_API ads_host_font_size, host_font_size, 0
ADS_API ads_host_font_style, host_font_style, 0
ADS_API ads_host_frame_number, host_frame_number, 0
ADS_API ads_host_free, host_free, 0
ADS_API ads_host_gb2312_to_wide, host_gb2312_to_wide, 0
ADS_API ads_host_get_asyn_http_req_handle, host_get_asyn_http_req_handle, 0
.ltorg
ADS_API ads_host_get_filename, host_get_filename, 0
ADS_API ads_host_get_http_head, host_get_http_head, 0
ADS_API ads_host_get_malloc_stat, host_get_malloc_stat, 0
ADS_API ads_host_get_pixel, host_get_pixel, 0
ADS_API ads_host_get_time, host_get_time, 0
ADS_API ads_host_gmobi_language, host_gmobi_language, 0
ADS_API ads_host_handheld_canvas_buffer_size, host_handheld_canvas_buffer_size, 0
ADS_API ads_host_handheld_create_canvas_cf, host_handheld_create_canvas_cf, 0
ADS_API ads_host_handheld_create_layer_cf, host_handheld_create_layer_cf, 5
ADS_API ads_host_handheld_create_layer_ex, host_handheld_create_layer_ex, 3
ADS_API ads_host_handheld_ellipse, host_handheld_ellipse, 2
ADS_API ads_host_handheld_kbd_mode, host_handheld_kbd_mode, 0
ADS_API ads_host_handheld_utc, host_handheld_utc, 0
ADS_API ads_host_img_property, host_img_property, 0
ADS_API ads_host_img_property_ex, host_img_property_ex, 0
ADS_API ads_host_input_text, host_input_text, 0
.ltorg
ADS_API ads_host_is_r2l, host_is_r2l, 0
ADS_API ads_host_layer_buffer, host_layer_buffer, 0
ADS_API ads_host_layer_opacity, host_layer_opacity, 0
ADS_API ads_host_line, host_line, 2
ADS_API ads_host_line_ex, host_line_ex, 1
ADS_API ads_host_load_image, host_load_image, 0
ADS_API ads_host_load_image_resized, host_load_image_resized, 0
ADS_API ads_host_load_resource, host_load_resource, 0
ADS_API ads_host_lower_case, host_lower_case, 0
ADS_API ads_host_malloc, host_malloc, 0
ADS_API ads_host_midi_pause, host_midi_pause, 0
ADS_API ads_host_midi_play, host_midi_play, 0
ADS_API ads_host_midi_play_resource, host_midi_play_resource, 0
ADS_API ads_host_midi_resume, host_midi_resume, 0
ADS_API ads_host_midi_stop, host_midi_stop, 0
ADS_API ads_host_midi_stop_all, host_midi_stop_all, 0
.ltorg
ADS_API ads_host_mirror, host_mirror, 2
ADS_API ads_host_mul_lang_deinit, host_mul_lang_deinit, 0
ADS_API ads_host_mul_lang_get_string, host_mul_lang_get_string, 0
ADS_API ads_host_mul_lang_init, host_mul_lang_init, 0
ADS_API ads_host_pen_supported, host_pen_supported, 0
ADS_API ads_host_polygon, host_polygon, 0
ADS_API ads_host_power, host_power, 0
ADS_API ads_host_process_state, host_process_state, 0
ADS_API ads_host_rand, host_rand, 0
ADS_API ads_host_realloc, host_realloc, 0
ADS_API ads_host_rect, host_rect, 2
ADS_API ads_host_rect_ex, host_rect_ex, 1
ADS_API ads_host_reg_res_provider, host_reg_res_provider, 0
ADS_API ads_host_register_key, host_register_key, 0
ADS_API ads_host_register_pen, host_register_pen, 0
ADS_API ads_host_register_system, host_register_system, 0
.ltorg
ADS_API ads_host_release_canvas, host_release_canvas, 0
ADS_API ads_host_removable, host_removable, 0
ADS_API ads_host_res_deinit, host_res_deinit, 0
ADS_API ads_host_res_init, host_res_init, 0
ADS_API ads_host_res_init_with_language, host_res_init_with_language, 0
ADS_API ads_host_res_string, host_res_string, 0
ADS_API ads_host_resolve, host_resolve, 0
ADS_API ads_host_rotate, host_rotate, 2
ADS_API ads_host_roundrect, host_roundrect, 3
ADS_API ads_host_roundrect_ex, host_roundrect_ex, 2
ADS_API ads_host_safe_wstrcpy, host_safe_wstrcpy, 0
ADS_API ads_host_screen_height, host_screen_height, 0
ADS_API ads_host_screen_width, host_screen_width, 0
ADS_API ads_host_selector, host_selector, 0
ADS_API ads_host_set_color, host_set_color, 0
ADS_API ads_host_set_font, host_set_font, 0
.ltorg
ADS_API ads_host_set_pixel, host_set_pixel, 0
ADS_API ads_host_set_pixel_ex, host_set_pixel_ex, 0
ADS_API ads_host_storage_close, host_storage_close, 0
ADS_API ads_host_storage_open, host_storage_open, 0
ADS_API ads_host_storage_read, host_storage_read, 1
ADS_API ads_host_storage_write, host_storage_write, 1
ADS_API ads_host_string_baseline, host_string_baseline, 0
ADS_API ads_host_string_height, host_string_height, 0
ADS_API ads_host_string_width, host_string_width, 0
ADS_API ads_host_system_drive, host_system_drive, 0
ADS_API ads_host_text_baseline, host_text_baseline, 3
ADS_API ads_host_text_to_layer, host_text_to_layer, 1
ADS_API ads_host_textout, host_textout, 2
ADS_API ads_host_tick_count, host_tick_count, 0
ADS_API ads_host_volume, host_volume, 0
ADS_API ads_host_vsprintf, host_vsprintf, 0
.ltorg
ADS_API ads_host_wide_to_ascii, host_wide_to_ascii, 0
ADS_API ads_host_wstrcat, host_wstrcat, 0
ADS_API ads_host_wstrcmp, host_wstrcmp, 0
ADS_API ads_host_wstrcpy, host_wstrcpy, 0
ADS_API ads_host_wstrlen, host_wstrlen, 0
ADS_API ads_host_wstrncpy, host_wstrncpy, 0
ADS_API ads_media_getreadbuffer, media_getreadbuffer, 0
ADS_API ads_media_readdatadone, media_readdatadone, 0
ADS_API ads_media_record, media_record, 0
ADS_API ads_media_setbuffer, media_setbuffer, 0
ADS_API ads_media_stop, media_stop, 0
ADS_API ads_utility_create_service, utility_create_service, 0
ADS_API ads_utility_ends_with, utility_ends_with, 0
ADS_API ads_utility_file_close, utility_file_close, 0
ADS_API ads_utility_file_commit, utility_file_commit, 0
ADS_API ads_utility_file_copy, utility_file_copy, 0
.ltorg
ADS_API ads_utility_file_delete, utility_file_delete, 0
ADS_API ads_utility_file_eof, utility_file_eof, 0
ADS_API ads_utility_file_get_attributes, utility_file_get_attributes, 0
ADS_API ads_utility_file_getfilesize, utility_file_getfilesize, 0
ADS_API ads_utility_file_mkdir, utility_file_mkdir, 0
ADS_API ads_utility_file_open, utility_file_open, 0
ADS_API ads_utility_file_read, utility_file_read, 0
ADS_API ads_utility_file_rename, utility_file_rename, 0
ADS_API ads_utility_file_rmdir, utility_file_rmdir, 0
ADS_API ads_utility_file_seek, utility_file_seek, 0
ADS_API ads_utility_file_set_attributes, utility_file_set_attributes, 0
ADS_API ads_utility_file_tell, utility_file_tell, 0
ADS_API ads_utility_file_write, utility_file_write, 0
ADS_API ads_utility_find_close, utility_find_close, 0
ADS_API ads_utility_find_close_ext, utility_find_close_ext, 0
ADS_API ads_utility_find_first, utility_find_first, 0
.ltorg
ADS_API ads_utility_find_first_ext, utility_find_first_ext, 0
ADS_API ads_utility_find_next, utility_find_next, 0
ADS_API ads_utility_find_next_ext, utility_find_next_ext, 0
ADS_API ads_utility_get_path, utility_get_path, 0
ADS_API ads_utility_pen_supported, utility_pen_supported, 0
ADS_API ads_utility_sim_count, utility_sim_count, 0
ADS_API ads_utility_string_equals, utility_string_equals, 0
ADS_API ads_utility_strtoi, utility_strtoi, 0
ADS_API ads_utility_vibrator_once, utility_vibrator_once, 0
ADS_API ads_utility_viewer_add_ref, utility_viewer_add_ref, 0
ADS_API ads_utility_viewer_close, utility_viewer_close, 0
ADS_API ads_utility_viewer_create, utility_viewer_create, 0
ADS_API ads_utility_viewer_query_interface, utility_viewer_query_interface, 0
ADS_API ads_utility_viewer_release, utility_viewer_release, 0
ADS_API ads_utility_viewer_run, utility_viewer_run, 0
ADS_API ads_utility_viewer_set_direction, utility_viewer_set_direction, 0
.ltorg
ADS_API ads_utility_viewer_set_filename, utility_viewer_set_filename, 0
ADS_API ads_utility_viewer_set_title, utility_viewer_set_title, 0
ADS_API ads_utility_wide_to_gb2312, utility_wide_to_gb2312, 0
.ltorg
/* Four base-AAPCS arguments for a nested SDK callback. The Linux
 * callee-saved integer/VFP state and both aligned managed stacks are retained. */
.arm
.align 2
.global arm_guest_call4
.type arm_guest_call4, %function
arm_guest_call4:
    push {r4-r11, lr}
    sub sp, sp, #4
    vpush {d8-d15}
    mov r5, sp
    mov r6, r0
    mov r7, r1
    mov r8, r2
    mov r9, r3
    ldr r10, [sp, #104]
    ldr r11, [sp, #108]
    mov sp, r7
    mov r0, r8
    mov r1, r9
    mov r2, r10
    mov r3, r11
    blx r6
    mov sp, r5
    vpop {d8-d15}
    add sp, sp, #4
    pop {r4-r11, pc}
.size arm_guest_call4, . - arm_guest_call4
.section .note.GNU-stack,"",%progbits




