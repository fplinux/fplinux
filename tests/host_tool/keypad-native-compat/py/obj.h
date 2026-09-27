/* SPDX-License-Identifier: GPL-2.0-only */
/* Native-module API double; objects live only until the next test call. */
#ifndef FPLINUX_TEST_MP_OBJ_H
#define FPLINUX_TEST_MP_OBJ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *mp_obj_t;
typedef struct {
	const char *key;
	const void *value;
} mp_rom_map_elem_t;
typedef struct {
	const mp_rom_map_elem_t *table;
	size_t count;
} mp_obj_dict_t;
typedef struct {
	struct {
		const void *type;
	} base;
	mp_obj_dict_t *globals;
} mp_obj_module_t;
struct test_mp_function {
	mp_obj_t (*call)(void);
};
struct test_mp_tuple {
	mp_obj_t values[4];
};

extern const char mp_type_module;
extern const char mp_type_OSError;

#define mp_const_none NULL
#define MP_ROM_QSTR(name) #name
#define MP_ROM_INT(value) ((void *)(uintptr_t)(value))
#define MP_ROM_PTR(value) (value)
#define MP_OBJ_NEW_SMALL_INT(value) ((void *)(uintptr_t)((value) * 2U + 1U))
#define MP_DEFINE_CONST_FUN_OBJ_0(name, function) \
	const struct test_mp_function name = { function }
#define MP_DEFINE_CONST_DICT(name, entries) \
	mp_obj_dict_t name = { entries, sizeof(entries) / sizeof(entries[0]) }
#define MP_REGISTER_MODULE(name, module)
#define MP_ERROR_TEXT(text) (text)

mp_obj_t mp_obj_new_int_from_uint(unsigned int value);
mp_obj_t mp_obj_new_bool(bool value);
mp_obj_t mp_obj_new_str(const char *text, size_t bytes);
mp_obj_t mp_obj_new_tuple(size_t count, const mp_obj_t *values);
_Noreturn void mp_raise_OSError(int error);
_Noreturn void mp_raise_msg_varg(const void *type, const char *format, ...);

#endif
