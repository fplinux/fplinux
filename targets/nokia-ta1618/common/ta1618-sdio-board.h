/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_TA1618_SDIO_BOARD_H
#define FPLINUX_TA1618_SDIO_BOARD_H

#include "ums9117-sdio-core.h"

enum ta1618_sdio_board_reg {
	TA1618_SDIO_REG_GATE_STATE,
	TA1618_SDIO_REG_GATE_SET,
	TA1618_SDIO_REG_GATE_CLEAR,
	TA1618_SDIO_REG_RESET_STATE,
	TA1618_SDIO_REG_RESET_SET,
	TA1618_SDIO_REG_RESET_CLEAR,
	TA1618_SDIO_REG_CLOCK_SELECTOR,
	TA1618_SDIO_REG_PWR_PAD_CTL,
	TA1618_SDIO_REG_CMD_MUX,
	TA1618_SDIO_REG_CMD_PAD,
	TA1618_SDIO_REG_D0_MUX,
	TA1618_SDIO_REG_D0_PAD,
	TA1618_SDIO_REG_CLK_MUX,
	TA1618_SDIO_REG_CLK_PAD,
	TA1618_SDIO_REG_D3_MUX,
	TA1618_SDIO_REG_D3_PAD,
	TA1618_SDIO_REG_D2_MUX,
	TA1618_SDIO_REG_D2_PAD,
	TA1618_SDIO_REG_D1_MUX,
	TA1618_SDIO_REG_D1_PAD,
	TA1618_SDIO_REG_CARD_DETECT_DATA,
	TA1618_SDIO_REG_CARD_DETECT_MASK,
	TA1618_SDIO_REG_BOARD_COUNT,
};

enum ta1618_sdio_analog_reg {
	TA1618_SDIO_ANALOG_CORE_PD,
	TA1618_SDIO_ANALOG_CORE_VOLT,
	TA1618_SDIO_ANALOG_IO_PD,
	TA1618_SDIO_ANALOG_IO_VOLT,
	TA1618_SDIO_ANALOG_COUNT,
};

#define TA1618_SDIO_PIN_COUNT 12U
#define TA1618_SDIO0_SPI 57U
#define TA1618_SDIO0_INTID (TA1618_SDIO0_SPI + 32U)

struct ta1618_sdio_resource {
	const char *name;
	u32 address;
	u32 size;
};

struct ta1618_sdio_io {
	void *context;
	struct ums9117_sdio_io controller;
	u32 (*read)(void *context, enum ta1618_sdio_board_reg reg);
	void (*write)(void *context, enum ta1618_sdio_board_reg reg, u32 value);
	int (*adi_begin)(void *context);
	int (*adi_read)(void *context, enum ta1618_sdio_analog_reg reg,
			u16 *value);
	int (*adi_write)(void *context, enum ta1618_sdio_analog_reg reg,
			 u16 value);
	int (*adi_end)(void *context);
	void (*sleep_ms)(void *context, u32 msec);
};

struct ta1618_sdio_state {
	struct ums9117_sdio_state controller;
	u32 gate_snapshot;
	u32 reset_snapshot;
	u32 selector_snapshot;
	u32 pin_snapshot[TA1618_SDIO_PIN_COUNT];
	u16 analog_snapshot[TA1618_SDIO_ANALOG_COUNT];
	bool snapshots_valid;
	bool card_detect_owned;
	bool platform_active;
	bool rails_on;
};

struct ta1618_sdio_activation_record {
	u32 selector_after;
	u32 clock_after;
};

struct ta1618_sdio_transition_record {
	u32 selector_before;
	u32 selector_after;
	struct ums9117_sdio_transition_record controller;
};

const struct ta1618_sdio_resource *
ta1618_sdio_controller_resource(enum ums9117_sdio_reg reg);
const struct ta1618_sdio_resource *
ta1618_sdio_board_resource(enum ta1618_sdio_board_reg reg);
const struct ta1618_sdio_resource *ta1618_sdio_adi_resource(void);
const struct ta1618_sdio_resource *ta1618_sdio_analog_resource(void);
u32 ta1618_sdio_analog_address(enum ta1618_sdio_analog_reg reg);
u32 ta1618_sdio_analog_offset(enum ta1618_sdio_analog_reg reg);

int ta1618_sdio_snapshot(const struct ta1618_sdio_io *io,
			 struct ta1618_sdio_state *state);
int ta1618_sdio_enable_card_detect(const struct ta1618_sdio_io *io,
				   struct ta1618_sdio_state *state);
int ta1618_sdio_restore_card_detect(const struct ta1618_sdio_io *io,
				    struct ta1618_sdio_state *state);
int ta1618_sdio_card_present(const struct ta1618_sdio_io *io,
			     const struct ta1618_sdio_state *state);
int ta1618_sdio_activate(const struct ta1618_sdio_io *io,
			 struct ta1618_sdio_state *state);
int ta1618_sdio_enable_ident_clock(const struct ta1618_sdio_io *io,
				   struct ta1618_sdio_state *state,
				   struct ta1618_sdio_activation_record *record);
int ta1618_sdio_set_slot_power(const struct ta1618_sdio_io *io,
			       struct ta1618_sdio_state *state, bool enable);
int ta1618_sdio_validate_active(const struct ta1618_sdio_io *io,
				const struct ta1618_sdio_state *state);
int ta1618_sdio_set_operational_clock(
	const struct ta1618_sdio_io *io, struct ta1618_sdio_state *state,
	enum ums9117_sdio_clock_profile profile,
	struct ta1618_sdio_transition_record *record);
int ta1618_sdio_restore_platform(const struct ta1618_sdio_io *io,
				 struct ta1618_sdio_state *state,
				 u32 quiesce_timeout_us);
int ta1618_sdio_cleanup(const struct ta1618_sdio_io *io,
			struct ta1618_sdio_state *state,
			u32 quiesce_timeout_us);

#endif
