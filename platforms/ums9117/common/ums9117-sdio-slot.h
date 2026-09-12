/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_UMS9117_SDIO_SLOT_H
#define FPLINUX_UMS9117_SDIO_SLOT_H

#include "ums9117-sdio-core.h"

enum ums9117_sdio_slot_reg {
	UMS9117_SDIO_SLOT_REG_GATE_STATE,
	UMS9117_SDIO_SLOT_REG_GATE_SET,
	UMS9117_SDIO_SLOT_REG_GATE_CLEAR,
	UMS9117_SDIO_SLOT_REG_RESET_STATE,
	UMS9117_SDIO_SLOT_REG_RESET_SET,
	UMS9117_SDIO_SLOT_REG_RESET_CLEAR,
	UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR,
	UMS9117_SDIO_SLOT_REG_PWR_PAD_CTL,
	UMS9117_SDIO_SLOT_REG_CMD_MUX,
	UMS9117_SDIO_SLOT_REG_CMD_PAD,
	UMS9117_SDIO_SLOT_REG_D0_MUX,
	UMS9117_SDIO_SLOT_REG_D0_PAD,
	UMS9117_SDIO_SLOT_REG_CLK_MUX,
	UMS9117_SDIO_SLOT_REG_CLK_PAD,
	UMS9117_SDIO_SLOT_REG_D3_MUX,
	UMS9117_SDIO_SLOT_REG_D3_PAD,
	UMS9117_SDIO_SLOT_REG_D2_MUX,
	UMS9117_SDIO_SLOT_REG_D2_PAD,
	UMS9117_SDIO_SLOT_REG_D1_MUX,
	UMS9117_SDIO_SLOT_REG_D1_PAD,
	UMS9117_SDIO_SLOT_REG_CARD_DETECT_DATA,
	UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK,
	UMS9117_SDIO_SLOT_REG_COUNT,
};

enum ums9117_sdio_slot_analog_reg {
	UMS9117_SDIO_SLOT_ANALOG_CORE_PD,
	UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT,
	UMS9117_SDIO_SLOT_ANALOG_IO_PD,
	UMS9117_SDIO_SLOT_ANALOG_IO_VOLT,
	UMS9117_SDIO_SLOT_ANALOG_COUNT,
};

#define UMS9117_SDIO_SLOT_PIN_COUNT 12U
#define UMS9117_SDIO0_SPI 57U
#define UMS9117_SDIO0_INTID (UMS9117_SDIO0_SPI + 32U)

struct ums9117_sdio_resource {
	const char *name;
	u32 address;
	u32 size;
};

struct ums9117_sdio_slot_pin {
	enum ums9117_sdio_slot_reg reg;
	u32 target;
	bool mux;
};

struct ums9117_sdio_slot_io;
struct ums9117_sdio_slot_state;

struct ums9117_sdio_slot_board {
	const struct ums9117_sdio_resource *resources;
	const struct ums9117_sdio_slot_pin *pins;
	u16 core_voltage;
	u16 io_voltage;
	u32 rail_delay_ms;
	int (*enable_card_detect)(const struct ums9117_sdio_slot_io *io,
				  struct ums9117_sdio_slot_state *state);
	int (*restore_card_detect)(const struct ums9117_sdio_slot_io *io,
				   struct ums9117_sdio_slot_state *state);
	int (*card_present)(const struct ums9117_sdio_slot_io *io,
			    const struct ums9117_sdio_slot_state *state);
};

struct ums9117_sdio_slot_io {
	const struct ums9117_sdio_slot_board *board;
	void *context;
	struct ums9117_sdio_io controller;
	u32 (*read)(void *context, enum ums9117_sdio_slot_reg reg);
	void (*write)(void *context, enum ums9117_sdio_slot_reg reg, u32 value);
	int (*adi_begin)(void *context);
	int (*adi_read)(void *context, enum ums9117_sdio_slot_analog_reg reg,
			u16 *value);
	int (*adi_write)(void *context, enum ums9117_sdio_slot_analog_reg reg,
			 u16 value);
	int (*adi_end)(void *context);
	void (*sleep_ms)(void *context, u32 msec);
};

struct ums9117_sdio_slot_state {
	struct ums9117_sdio_state controller;
	u32 gate_snapshot;
	u32 reset_snapshot;
	u32 selector_snapshot;
	u32 pin_snapshot[UMS9117_SDIO_SLOT_PIN_COUNT];
	u16 analog_snapshot[UMS9117_SDIO_SLOT_ANALOG_COUNT];
	bool snapshots_valid;
	bool card_detect_owned;
	bool platform_active;
	bool rails_on;
};

struct ums9117_sdio_slot_activation_record {
	u32 selector_after;
	u32 clock_after;
};

struct ums9117_sdio_slot_transition_record {
	u32 selector_before;
	u32 selector_after;
	struct ums9117_sdio_transition_record controller;
};

const struct ums9117_sdio_resource *
ums9117_sdio_slot_controller_resource(enum ums9117_sdio_reg reg);
const struct ums9117_sdio_resource *
ums9117_sdio_slot_board_resource(const struct ums9117_sdio_slot_board *board,
				 enum ums9117_sdio_slot_reg reg);
const struct ums9117_sdio_resource *ums9117_sdio_slot_adi_resource(void);
const struct ums9117_sdio_resource *ums9117_sdio_slot_analog_resource(void);
u32 ums9117_sdio_slot_analog_address(enum ums9117_sdio_slot_analog_reg reg);
u32 ums9117_sdio_slot_analog_offset(enum ums9117_sdio_slot_analog_reg reg);

/* Active-low EIC DATA0; acquire an idle mask and release only bit0. */
int ums9117_sdio_eic_enable_card_detect(const struct ums9117_sdio_slot_io *io,
					struct ums9117_sdio_slot_state *state);
int ums9117_sdio_eic_restore_card_detect(const struct ums9117_sdio_slot_io *io,
					 struct ums9117_sdio_slot_state *state);
int ums9117_sdio_eic_card_present(const struct ums9117_sdio_slot_io *io,
				  const struct ums9117_sdio_slot_state *state);

int ums9117_sdio_slot_snapshot(const struct ums9117_sdio_slot_io *io,
			       struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_enable_card_detect(const struct ums9117_sdio_slot_io *io,
					 struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_restore_card_detect(const struct ums9117_sdio_slot_io *io,
					  struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_card_present(const struct ums9117_sdio_slot_io *io,
				   const struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_activate(const struct ums9117_sdio_slot_io *io,
			       struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_enable_ident_clock(
	const struct ums9117_sdio_slot_io *io,
	struct ums9117_sdio_slot_state *state,
	struct ums9117_sdio_slot_activation_record *record);
int ums9117_sdio_slot_set_slot_power(const struct ums9117_sdio_slot_io *io,
				     struct ums9117_sdio_slot_state *state,
				     bool enable);
int ums9117_sdio_slot_validate_active(
	const struct ums9117_sdio_slot_io *io,
	const struct ums9117_sdio_slot_state *state);
int ums9117_sdio_slot_set_operational_clock(
	const struct ums9117_sdio_slot_io *io,
	struct ums9117_sdio_slot_state *state,
	enum ums9117_sdio_clock_profile profile,
	struct ums9117_sdio_slot_transition_record *record);
int ums9117_sdio_slot_restore_platform(const struct ums9117_sdio_slot_io *io,
				       struct ums9117_sdio_slot_state *state,
				       u32 quiesce_timeout_us);
int ums9117_sdio_slot_cleanup(const struct ums9117_sdio_slot_io *io,
			      struct ums9117_sdio_slot_state *state,
			      u32 quiesce_timeout_us);

#endif
