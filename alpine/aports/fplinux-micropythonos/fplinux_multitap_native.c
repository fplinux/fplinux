// SPDX-License-Identifier: MIT
/* MicroPython binding for FPLinux's shared numeric-keypad composition core. */
/* fplinux-check: package-embedded */

#include <stdint.h>

#include "fplinux-multitap.h"
#include "py/obj.h"
#include "py/runtime.h"

struct multitap_engine {
	mp_obj_base_t base;
	struct fplinux_multitap state;
};

struct emitted_character {
	unsigned char character;
	bool present;
};

static enum fplinux_multitap_emit_result emit_character(void *context,
							unsigned char character)
{
	struct emitted_character *emitted = context;

	emitted->character = character;
	emitted->present = true;
	return FPLINUX_MULTITAP_EMIT_ACCEPTED;
}

static struct multitap_engine *engine_from_obj(mp_obj_t self_in)
{
	return MP_OBJ_TO_PTR(self_in);
}

static uint32_t elapsed_from_obj(mp_obj_t elapsed_in)
{
	mp_int_t elapsed = mp_obj_get_int(elapsed_in);

	if (elapsed < 0 || (uint64_t)elapsed > UINT32_MAX)
		mp_raise_ValueError(
			MP_ERROR_TEXT("elapsed milliseconds must fit uint32"));
	return (uint32_t)elapsed;
}

static unsigned char key_from_obj(mp_obj_t key_in)
{
	size_t length;
	const char *key = mp_obj_str_get_data(key_in, &length);

	if (length != 1U)
		mp_raise_ValueError(
			MP_ERROR_TEXT("key must be one ASCII digit"));
	return (unsigned char)key[0];
}

static mp_obj_t emitted_to_obj(const struct emitted_character *emitted)
{
	if (!emitted->present)
		return mp_const_none;
	return mp_obj_new_str((const char *)&emitted->character, 1);
}

static mp_obj_t candidate_to_obj(const struct multitap_engine *engine)
{
	unsigned char character = fplinux_multitap_candidate(&engine->state);

	if (character == '\0')
		return mp_obj_new_str("", 0);
	return mp_obj_new_str((const char *)&character, 1);
}

static mp_obj_t multitap_engine_make_new(const mp_obj_type_t *type,
					 size_t n_args, size_t n_kw,
					 const mp_obj_t *args)
{
	struct multitap_engine *engine;

	mp_arg_check_num(n_args, n_kw, 0, 0, false);
	engine = mp_obj_malloc(struct multitap_engine, type);
	fplinux_multitap_init(&engine->state);
	return MP_OBJ_FROM_PTR(engine);
}

static mp_obj_t multitap_engine_press(mp_obj_t self_in, mp_obj_t key_in,
				      mp_obj_t elapsed_in)
{
	struct multitap_engine *engine = engine_from_obj(self_in);
	struct emitted_character emitted = {};

	(void)fplinux_multitap_press(&engine->state, key_from_obj(key_in),
				     elapsed_from_obj(elapsed_in),
				     emit_character, &emitted);
	return emitted_to_obj(&emitted);
}
static MP_DEFINE_CONST_FUN_OBJ_3(multitap_engine_press_obj,
				 multitap_engine_press);

static mp_obj_t multitap_engine_expire(mp_obj_t self_in, mp_obj_t elapsed_in)
{
	struct multitap_engine *engine = engine_from_obj(self_in);
	struct emitted_character emitted = {};

	(void)fplinux_multitap_expire(&engine->state,
				      elapsed_from_obj(elapsed_in),
				      emit_character, &emitted);
	return emitted_to_obj(&emitted);
}
static MP_DEFINE_CONST_FUN_OBJ_2(multitap_engine_expire_obj,
				 multitap_engine_expire);

static mp_obj_t multitap_engine_commit(mp_obj_t self_in)
{
	struct multitap_engine *engine = engine_from_obj(self_in);
	struct emitted_character emitted = {};

	(void)fplinux_multitap_commit(&engine->state, emit_character, &emitted);
	return emitted_to_obj(&emitted);
}
static MP_DEFINE_CONST_FUN_OBJ_1(multitap_engine_commit_obj,
				 multitap_engine_commit);

static mp_obj_t multitap_engine_cancel(mp_obj_t self_in)
{
	struct multitap_engine *engine = engine_from_obj(self_in);

	fplinux_multitap_cancel(&engine->state);
	return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(multitap_engine_cancel_obj,
				 multitap_engine_cancel);

static mp_obj_t multitap_engine_candidate(mp_obj_t self_in)
{
	return candidate_to_obj(engine_from_obj(self_in));
}
static MP_DEFINE_CONST_FUN_OBJ_1(multitap_engine_candidate_obj,
				 multitap_engine_candidate);

static mp_obj_t multitap_engine_pending_key(mp_obj_t self_in)
{
	const struct multitap_engine *engine = engine_from_obj(self_in);
	unsigned char key;

	if (!fplinux_multitap_pending(&engine->state))
		return mp_obj_new_str("", 0);
	key = fplinux_multitap_pending_key(&engine->state);
	return mp_obj_new_str((const char *)&key, 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(multitap_engine_pending_key_obj,
				 multitap_engine_pending_key);

static const mp_rom_map_elem_t multitap_engine_locals_table[] = {
	{ MP_ROM_QSTR(MP_QSTR_press), MP_ROM_PTR(&multitap_engine_press_obj) },
	{ MP_ROM_QSTR(MP_QSTR_expire),
	  MP_ROM_PTR(&multitap_engine_expire_obj) },
	{ MP_ROM_QSTR(MP_QSTR_commit),
	  MP_ROM_PTR(&multitap_engine_commit_obj) },
	{ MP_ROM_QSTR(MP_QSTR_cancel),
	  MP_ROM_PTR(&multitap_engine_cancel_obj) },
	{ MP_ROM_QSTR(MP_QSTR_candidate),
	  MP_ROM_PTR(&multitap_engine_candidate_obj) },
	{ MP_ROM_QSTR(MP_QSTR_pending_key),
	  MP_ROM_PTR(&multitap_engine_pending_key_obj) },
};
static MP_DEFINE_CONST_DICT(multitap_engine_locals,
			    multitap_engine_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(multitap_engine_type, MP_QSTR_Engine,
			 MP_TYPE_FLAG_NONE, make_new, multitap_engine_make_new,
			 locals_dict, &multitap_engine_locals);

static mp_obj_t multitap_handles(mp_obj_t key_in)
{
	return mp_obj_new_bool(fplinux_multitap_handles(key_from_obj(key_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(multitap_handles_obj, multitap_handles);

static const mp_rom_map_elem_t multitap_module_globals_table[] = {
	{ MP_ROM_QSTR(MP_QSTR___name__),
	  MP_ROM_QSTR(MP_QSTR_fplinux_multitap_native) },
	{ MP_ROM_QSTR(MP_QSTR_Engine), MP_ROM_PTR(&multitap_engine_type) },
	{ MP_ROM_QSTR(MP_QSTR_handles), MP_ROM_PTR(&multitap_handles_obj) },
};
static MP_DEFINE_CONST_DICT(multitap_module_globals,
			    multitap_module_globals_table);

const mp_obj_module_t multitap_native_module = {
	.base = { &mp_type_module },
	.globals = (mp_obj_dict_t *)&multitap_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fplinux_multitap_native, multitap_native_module);
