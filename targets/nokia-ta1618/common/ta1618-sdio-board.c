// SPDX-License-Identifier: GPL-2.0-only
#include "ta1618-sdio-board.h"

static const struct ums9117_sdio_resource
	ta1618_sdio_resources[UMS9117_SDIO_SLOT_REG_COUNT] = {
		[UMS9117_SDIO_SLOT_REG_GATE_STATE] = { "gate-state",
						       0x20e00000U, 4U },
		[UMS9117_SDIO_SLOT_REG_GATE_SET] = { "gate-set", 0x20e01000U,
						     4U },
		[UMS9117_SDIO_SLOT_REG_GATE_CLEAR] = { "gate-clear",
						       0x20e02000U, 4U },
		[UMS9117_SDIO_SLOT_REG_RESET_STATE] = { "ap-reset-state",
							0x20e00004U, 4U },
		[UMS9117_SDIO_SLOT_REG_RESET_SET] = { "ap-reset-set",
						      0x20e01004U, 4U },
		[UMS9117_SDIO_SLOT_REG_RESET_CLEAR] = { "ap-reset-clear",
							0x20e02004U, 4U },
		[UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR] = { "ap-clock-selector",
							   0x2150006cU, 4U },
		[UMS9117_SDIO_SLOT_REG_PWR_PAD_CTL] = { "pwr-pad-ctl",
							0x402a0000U, 4U },
		[UMS9117_SDIO_SLOT_REG_CMD_MUX] = { "cmd-mux", 0x402a0128U,
						    4U },
		[UMS9117_SDIO_SLOT_REG_CMD_PAD] = { "cmd-pad", 0x402a0528U,
						    4U },
		[UMS9117_SDIO_SLOT_REG_D0_MUX] = { "d0-mux", 0x402a012cU, 4U },
		[UMS9117_SDIO_SLOT_REG_D0_PAD] = { "d0-pad", 0x402a052cU, 4U },
		[UMS9117_SDIO_SLOT_REG_CLK_MUX] = { "clk-mux", 0x402a0134U,
						    4U },
		[UMS9117_SDIO_SLOT_REG_CLK_PAD] = { "clk-pad", 0x402a0534U,
						    4U },
		[UMS9117_SDIO_SLOT_REG_D3_MUX] = { "d3-mux", 0x402a0120U, 4U },
		[UMS9117_SDIO_SLOT_REG_D3_PAD] = { "d3-pad", 0x402a0520U, 4U },
		[UMS9117_SDIO_SLOT_REG_D2_MUX] = { "d2-mux", 0x402a0124U, 4U },
		[UMS9117_SDIO_SLOT_REG_D2_PAD] = { "d2-pad", 0x402a0524U, 4U },
		[UMS9117_SDIO_SLOT_REG_D1_MUX] = { "d1-mux", 0x402a0130U, 4U },
		[UMS9117_SDIO_SLOT_REG_D1_PAD] = { "d1-pad", 0x402a0530U, 4U },
		[UMS9117_SDIO_SLOT_REG_CARD_DETECT_DATA] = { "card-detect-data",
							     0x402c0000U, 4U },
		[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] = { "card-detect-mask",
							     0x402c0004U, 4U },
	};

static const struct ums9117_sdio_slot_pin
	ta1618_sdio_pins[UMS9117_SDIO_SLOT_PIN_COUNT] = {
		{ UMS9117_SDIO_SLOT_REG_CMD_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_CMD_PAD, 0x00182084U, false },
		{ UMS9117_SDIO_SLOT_REG_D0_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_D0_PAD, 0x00182084U, false },
		{ UMS9117_SDIO_SLOT_REG_CLK_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_CLK_PAD, 0x00302001U, false },
		{ UMS9117_SDIO_SLOT_REG_D3_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_D3_PAD, 0x00182084U, false },
		{ UMS9117_SDIO_SLOT_REG_D2_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_D2_PAD, 0x00182084U, false },
		{ UMS9117_SDIO_SLOT_REG_D1_MUX, 0x00000000U, true },
		{ UMS9117_SDIO_SLOT_REG_D1_PAD, 0x00182084U, false },
	};

const struct ums9117_sdio_slot_board ta1618_sdio_board = {
	.resources = ta1618_sdio_resources,
	.pins = ta1618_sdio_pins,
	.core_voltage = 0x0050U,
	.io_voltage = 0x006fU,
	.rail_delay_ms = 300U,
	.enable_card_detect = ums9117_sdio_eic_enable_card_detect,
	.restore_card_detect = ums9117_sdio_eic_restore_card_detect,
	.card_present = ums9117_sdio_eic_card_present,
};
