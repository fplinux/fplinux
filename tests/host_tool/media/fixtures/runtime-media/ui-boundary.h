/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RUNTIME_MEDIA_TEST_UI_BOUNDARY_H
#define RUNTIME_MEDIA_TEST_UI_BOUNDARY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fplinux_ui;

struct fplinux_ui *runtime_media_test_ui_open(void);
void runtime_media_test_ui_close(void);
void runtime_media_test_tick(void);
unsigned int runtime_media_test_active_timers(void);
void runtime_media_test_set_drain(bool drain);
void runtime_media_test_set_policy_hold(bool held);
void runtime_media_test_set_output_failure(bool failed);
void runtime_media_test_set_open_error(int error);
void runtime_media_test_set_write_limit(size_t frames);
size_t runtime_media_test_accepted(void);
uint64_t runtime_media_test_heard(void);
void runtime_media_test_reset_samples(void);
size_t runtime_media_test_samples(int16_t *samples, size_t capacity);
unsigned int runtime_media_test_opened(void);
unsigned int runtime_media_test_closed(void);
size_t runtime_media_test_stream_samples(unsigned int id, int16_t *samples,
					 size_t capacity);
size_t runtime_media_test_stream_accepted(unsigned int id);
uint64_t runtime_media_test_stream_heard(unsigned int id);
void runtime_media_test_assert_drained(void);
void runtime_media_test_assert_owner(void);

#endif
