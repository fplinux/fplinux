// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kernel.h>
#ifdef __UBOOT__
#include <linux/compat.h>
#endif

#include "ums9117-sdio-slot.h"

#define UMS9117_SDIO_SLOT_GATE_MASK 0x00000080U
#define UMS9117_SDIO_SLOT_RESET_MASK 0x00000800U
#define UMS9117_SDIO_SLOT_SELECTOR_MASK 0x00000007U
#define UMS9117_SDIO_SLOT_SELECTOR_SOURCE 0x00000004U
#define UMS9117_SDIO_SLOT_PIN_ADMISSIBLE_MASK 0x00000030U
#define UMS9117_SDIO_SLOT_PD_MASK 0x0001U
#define UMS9117_SDIO_SLOT_CORE_VOLTAGE 0x0050U
#define UMS9117_SDIO_SLOT_IO_VOLTAGE 0x006fU
#define UMS9117_SDIO_SLOT_RAIL_DELAY_MS 300U
#define UMS9117_SDIO_EIC_CARD_DETECT_BIT 0x00000001U
/* Settling interval used for the polled EIC input. */
#define UMS9117_SDIO_EIC_CARD_DETECT_SETTLE_MS 3U

struct ums9117_sdio_slot_pin {
	enum ums9117_sdio_slot_reg reg;
	u32 target;
	bool mux;
};

static const struct ums9117_sdio_resource
	ums9117_sdio_slot_controller_resources[UMS9117_SDIO_REG_COUNT] = {
		[UMS9117_SDIO_REG_BLOCK_COUNT] = { "block-count", 0x20300000U,
						   4U },
		[UMS9117_SDIO_REG_BLOCK_SIZE] = { "block-size", 0x20300004U,
						  4U },
		[UMS9117_SDIO_REG_ARGUMENT] = { "argument", 0x20300008U, 4U },
		[UMS9117_SDIO_REG_TRANSFER_COMMAND] = { "transfer-command",
							0x2030000cU, 4U },
		[UMS9117_SDIO_REG_RESPONSE0] = { "response0", 0x20300010U, 4U },
		[UMS9117_SDIO_REG_RESPONSE1] = { "response1", 0x20300014U, 4U },
		[UMS9117_SDIO_REG_RESPONSE2] = { "response2", 0x20300018U, 4U },
		[UMS9117_SDIO_REG_RESPONSE3] = { "response3", 0x2030001cU, 4U },
		[UMS9117_SDIO_REG_PRESENT_STATE] = { "present-state",
						     0x20300024U, 4U },
		[UMS9117_SDIO_REG_HOST_CONTROL1] = { "host-control1",
						     0x20300028U, 4U },
		[UMS9117_SDIO_REG_CLOCK_RESET] = { "clock-timeout-reset",
						   0x2030002cU, 4U },
		[UMS9117_SDIO_REG_INTERRUPT_STATUS] = { "interrupt-status",
							0x20300030U, 4U },
		[UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE] = { "interrupt-status-enable",
							       0x20300034U,
							       4U },
		[UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE] = { "interrupt-signal-enable",
							       0x20300038U,
							       4U },
		[UMS9117_SDIO_REG_HOST_CONTROL2] = { "host-control2",
						     0x2030003cU, 4U },
		[UMS9117_SDIO_REG_ADMA_ERROR] = { "adma-error", 0x20300054U,
						  4U },
		[UMS9117_SDIO_REG_ADMA_ADDRESS_LOW] = { "adma2-address-low",
							0x20300058U, 4U },
		[UMS9117_SDIO_REG_ADMA_ADDRESS_HIGH] = { "adma2-address-high",
							 0x2030005cU, 4U },
		[UMS9117_SDIO_REG_HOST_VERSION] = { "host-version-slot-int",
						    0x203000fcU, 4U },
	};

static const struct ums9117_sdio_resource
	ums9117_sdio_slot_resources[UMS9117_SDIO_SLOT_REG_COUNT] = {
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
	ums9117_sdio_slot_pins[UMS9117_SDIO_SLOT_PIN_COUNT] = {
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

static const u32
	ums9117_sdio_slot_analog_addresses[UMS9117_SDIO_SLOT_ANALOG_COUNT] = {
		[UMS9117_SDIO_SLOT_ANALOG_CORE_PD] = 0x40608d00U,
		[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] = 0x40608d04U,
		[UMS9117_SDIO_SLOT_ANALOG_IO_PD] = 0x40608d0cU,
		[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] = 0x40608d10U,
	};

static const struct ums9117_sdio_resource ums9117_sdio_slot_adi = {
	"adi-controller", 0x40600000U, 0x0400U
};

static const struct ums9117_sdio_resource ums9117_sdio_slot_analog = {
	"analog-slave", 0x40608000U, 0x1000U
};

static const u32
	ums9117_sdio_slot_analog_offsets[UMS9117_SDIO_SLOT_ANALOG_COUNT] = {
		[UMS9117_SDIO_SLOT_ANALOG_CORE_PD] = 0x0d00U,
		[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] = 0x0d04U,
		[UMS9117_SDIO_SLOT_ANALOG_IO_PD] = 0x0d0cU,
		[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] = 0x0d10U,
	};

const struct ums9117_sdio_resource *
ums9117_sdio_slot_controller_resource(enum ums9117_sdio_reg reg)
{
	return reg < UMS9117_SDIO_REG_COUNT ?
		       &ums9117_sdio_slot_controller_resources[reg] :
		       NULL;
}

const struct ums9117_sdio_resource *
ums9117_sdio_slot_resource(enum ums9117_sdio_slot_reg reg)
{
	return reg < UMS9117_SDIO_SLOT_REG_COUNT ?
		       &ums9117_sdio_slot_resources[reg] :
		       NULL;
}

const struct ums9117_sdio_resource *ums9117_sdio_slot_adi_resource(void)
{
	return &ums9117_sdio_slot_adi;
}

const struct ums9117_sdio_resource *ums9117_sdio_slot_analog_resource(void)
{
	return &ums9117_sdio_slot_analog;
}

u32 ums9117_sdio_slot_analog_address(enum ums9117_sdio_slot_analog_reg reg)
{
	return reg < UMS9117_SDIO_SLOT_ANALOG_COUNT ?
		       ums9117_sdio_slot_analog_addresses[reg] :
		       0;
}

u32 ums9117_sdio_slot_analog_offset(enum ums9117_sdio_slot_analog_reg reg)
{
	return reg < UMS9117_SDIO_SLOT_ANALOG_COUNT ?
		       ums9117_sdio_slot_analog_offsets[reg] :
		       0;
}

static u32 ums9117_sdio_slot_read(const struct ums9117_sdio_slot_io *io,
				  enum ums9117_sdio_slot_reg reg)
{
	return io->read(io->context, reg);
}

static void ums9117_sdio_slot_write(const struct ums9117_sdio_slot_io *io,
				    enum ums9117_sdio_slot_reg reg, u32 value)
{
	io->write(io->context, reg, value);
}

static int ums9117_sdio_slot_set_rails(const struct ums9117_sdio_slot_io *io,
				       struct ums9117_sdio_slot_state *state,
				       bool enable)
{
	static const enum ums9117_sdio_slot_analog_reg order[] = {
		UMS9117_SDIO_SLOT_ANALOG_CORE_PD,
		UMS9117_SDIO_SLOT_ANALOG_IO_PD,
	};
	u16 current;
	u16 target;
	unsigned int index;
	bool changed = false;
	int ret;
	int end_ret;

	ret = io->adi_begin(io->context);
	if (ret)
		return ret;
	for (index = 0; index < 2U; ++index) {
		ret = io->adi_read(io->context, order[index], &current);
		if (ret)
			break;
		target = enable ? current & ~UMS9117_SDIO_SLOT_PD_MASK :
				  current | UMS9117_SDIO_SLOT_PD_MASK;
		if (target == current)
			continue;
		ret = io->adi_write(io->context, order[index], target);
		if (ret)
			break;
		changed = true;
	}
	end_ret = io->adi_end(io->context);
	if (!ret && end_ret)
		ret = end_ret;
	if (!ret) {
		state->rails_on = enable;
		if (changed)
			io->sleep_ms(io->context,
				     UMS9117_SDIO_SLOT_RAIL_DELAY_MS);
	}
	return ret;
}

int ums9117_sdio_slot_set_slot_power(const struct ums9117_sdio_slot_io *io,
				     struct ums9117_sdio_slot_state *state,
				     bool enable)
{
	if (!state->platform_active)
		return -EINVAL;
	return ums9117_sdio_slot_set_rails(io, state, enable);
}

static int
ums9117_sdio_slot_restore_rails(const struct ums9117_sdio_slot_io *io,
				const struct ums9117_sdio_slot_state *state)
{
	static const enum ums9117_sdio_slot_analog_reg order[] = {
		UMS9117_SDIO_SLOT_ANALOG_IO_PD,
		UMS9117_SDIO_SLOT_ANALOG_CORE_PD,
	};
	u16 current;
	u16 target;
	unsigned int index;
	int first_error = 0;
	int ret;

	ret = io->adi_begin(io->context);
	if (ret)
		return ret;
	for (index = 0; index < 2U; ++index) {
		ret = io->adi_read(io->context, order[index], &current);
		if (ret) {
			if (!first_error)
				first_error = ret;
			continue;
		}
		target = (current & ~UMS9117_SDIO_SLOT_PD_MASK) |
			 (state->analog_snapshot[order[index]] &
			  UMS9117_SDIO_SLOT_PD_MASK);
		if (target == current)
			continue;
		ret = io->adi_write(io->context, order[index], target);
		if (ret && !first_error)
			first_error = ret;
	}
	ret = io->adi_end(io->context);
	if (ret && !first_error)
		first_error = ret;
	return first_error;
}

int ums9117_sdio_slot_snapshot(const struct ums9117_sdio_slot_io *io,
			       struct ums9117_sdio_slot_state *state)
{
	u16 analog;
	u32 value;
	unsigned int pass;
	unsigned int index;
	int ret;
	int end_ret;

	state->gate_snapshot =
		ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_GATE_STATE);
	state->reset_snapshot =
		ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_RESET_STATE);
	if ((state->gate_snapshot & UMS9117_SDIO_SLOT_GATE_MASK) ||
	    (state->reset_snapshot & UMS9117_SDIO_SLOT_RESET_MASK))
		return -EBUSY;
	state->selector_snapshot = ums9117_sdio_slot_read(
		io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR);
	(void)ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_PWR_PAD_CTL);
	for (index = 0; index < UMS9117_SDIO_SLOT_PIN_COUNT; ++index) {
		const struct ums9117_sdio_slot_pin *pin =
			&ums9117_sdio_slot_pins[index];

		value = ums9117_sdio_slot_read(io, pin->reg);
		state->pin_snapshot[index] = value;
		if (ums9117_sdio_slot_read(io, pin->reg) != value)
			return -EAGAIN;
		if (pin->mux &&
		    (value & ~UMS9117_SDIO_SLOT_PIN_ADMISSIBLE_MASK))
			return -ERANGE;
	}
	ret = io->adi_begin(io->context);
	if (ret)
		return ret;
	for (pass = 0; pass < 2U && !ret; ++pass) {
		for (index = 0; index < UMS9117_SDIO_SLOT_ANALOG_COUNT;
		     ++index) {
			ret = io->adi_read(io->context, index, &analog);
			if (ret)
				break;
			if (!pass)
				state->analog_snapshot[index] = analog;
			else if (state->analog_snapshot[index] != analog) {
				ret = -EAGAIN;
				break;
			}
		}
	}
	end_ret = io->adi_end(io->context);
	if (!ret && end_ret)
		ret = end_ret;
	if (ret)
		return ret;
	if (state->analog_snapshot[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] !=
		    UMS9117_SDIO_SLOT_CORE_VOLTAGE ||
	    state->analog_snapshot[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] !=
		    UMS9117_SDIO_SLOT_IO_VOLTAGE)
		return -ERANGE;
	state->snapshots_valid = true;
	return 0;
}

/* Active-low EIC DATA0; acquire an idle mask and release only bit0. */
int ums9117_sdio_slot_enable_card_detect(const struct ums9117_sdio_slot_io *io,
					 struct ums9117_sdio_slot_state *state)
{
	u32 baseline =
		io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK);
	u32 data;

	if (baseline ||
	    io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK) !=
		    baseline)
		return -EBUSY;
	state->card_detect_owned = true;
	io->write(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK,
		  UMS9117_SDIO_EIC_CARD_DETECT_BIT);
	if (io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK) !=
	    UMS9117_SDIO_EIC_CARD_DETECT_BIT)
		return -EIO;
	io->sleep_ms(io->context, UMS9117_SDIO_EIC_CARD_DETECT_SETTLE_MS);
	data = io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_DATA);
	if (data & ~UMS9117_SDIO_EIC_CARD_DETECT_BIT)
		return -EUCLEAN;
	return 0;
}

int ums9117_sdio_slot_restore_card_detect(const struct ums9117_sdio_slot_io *io,
					  struct ums9117_sdio_slot_state *state)
{
	u32 before;
	u32 target;

	if (!state->card_detect_owned)
		return 0;
	before = io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK);
	target = before & ~UMS9117_SDIO_EIC_CARD_DETECT_BIT;
	if (before & UMS9117_SDIO_EIC_CARD_DETECT_BIT)
		io->write(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK,
			  target);
	if (io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK) !=
	    target)
		return -EIO;
	state->card_detect_owned = false;
	return 0;
}

int ums9117_sdio_slot_card_present(const struct ums9117_sdio_slot_io *io,
				   const struct ums9117_sdio_slot_state *state)
{
	u32 data;

	if (!state->card_detect_owned ||
	    !(io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK) &
	      UMS9117_SDIO_EIC_CARD_DETECT_BIT))
		return -EIO;
	data = io->read(io->context, UMS9117_SDIO_SLOT_REG_CARD_DETECT_DATA);
	return !(data & UMS9117_SDIO_EIC_CARD_DETECT_BIT);
}

int ums9117_sdio_slot_activate(const struct ums9117_sdio_slot_io *io,
			       struct ums9117_sdio_slot_state *state)
{
	u32 expected;
	u32 value;
	unsigned int index;
	int ret;

	if (state->platform_active)
		return state->rails_on ?
			       0 :
			       ums9117_sdio_slot_set_rails(io, state, true);
	if (!state->snapshots_valid)
		return -EINVAL;
	ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_GATE_SET,
				UMS9117_SDIO_SLOT_GATE_MASK);
	state->platform_active = true;
	if (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_GATE_STATE) !=
	    (state->gate_snapshot | UMS9117_SDIO_SLOT_GATE_MASK))
		return -EIO;
	ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_RESET_SET,
				UMS9117_SDIO_SLOT_RESET_MASK);
	expected = state->reset_snapshot | UMS9117_SDIO_SLOT_RESET_MASK;
	if (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_RESET_STATE) !=
	    expected)
		return -EIO;
	ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_RESET_CLEAR,
				UMS9117_SDIO_SLOT_RESET_MASK);
	if (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_RESET_STATE) !=
	    state->reset_snapshot)
		return -EIO;

	value = state->selector_snapshot & ~UMS9117_SDIO_SLOT_SELECTOR_MASK;
	ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR,
				value);
	if (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR) !=
	    value)
		return -EIO;
	value |= UMS9117_SDIO_SLOT_SELECTOR_SOURCE;
	ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR,
				value);
	if (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR) !=
	    value)
		return -EIO;

	ret = ums9117_sdio_snapshot_controller(&io->controller,
					       &state->controller);
	if (ret)
		return ret;
	ums9117_sdio_mask_and_ack(&io->controller);
	ret = ums9117_sdio_reset_controller(&io->controller);
	if (ret)
		return ret;
	for (index = 0; index < UMS9117_SDIO_SLOT_PIN_COUNT; ++index) {
		const struct ums9117_sdio_slot_pin *pin =
			&ums9117_sdio_slot_pins[index];

		if (ums9117_sdio_slot_read(io, pin->reg) !=
		    state->pin_snapshot[index])
			return -EAGAIN;
		ums9117_sdio_slot_write(io, pin->reg, pin->target);
		if (ums9117_sdio_slot_read(io, pin->reg) != pin->target)
			return -EIO;
	}
	ret = ums9117_sdio_slot_set_rails(io, state, false);
	if (!ret)
		ret = ums9117_sdio_slot_set_rails(io, state, true);
	if (ret)
		return ret;
	return ums9117_sdio_configure_1bit(&io->controller, &state->controller);
}

int ums9117_sdio_slot_enable_ident_clock(
	const struct ums9117_sdio_slot_io *io,
	struct ums9117_sdio_slot_state *state,
	struct ums9117_sdio_slot_activation_record *record)
{
	int ret;

	if (record) {
		record->selector_after = 0;
		record->clock_after = 0;
	}
	if (!state->platform_active || !state->rails_on)
		return -EPROTO;
	ret = ums9117_sdio_enable_ident_clock(&io->controller,
					      &state->controller);
	if (record) {
		record->selector_after = ums9117_sdio_slot_read(
			io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR);
		record->clock_after = io->controller.read(
			io->controller.context, UMS9117_SDIO_REG_CLOCK_RESET);
	}
	return ret;
}

int ums9117_sdio_slot_validate_active(
	const struct ums9117_sdio_slot_io *io,
	const struct ums9117_sdio_slot_state *state)
{
	if (!state->platform_active || !state->rails_on ||
	    (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR) &
	     UMS9117_SDIO_SLOT_SELECTOR_MASK) !=
		    UMS9117_SDIO_SLOT_SELECTOR_SOURCE)
		return -EPROTO;
	return ums9117_sdio_validate_active(&io->controller,
					    &state->controller);
}

int ums9117_sdio_slot_set_operational_clock(
	const struct ums9117_sdio_slot_io *io,
	struct ums9117_sdio_slot_state *state,
	enum ums9117_sdio_clock_profile profile,
	struct ums9117_sdio_slot_transition_record *record)
{
	int ret;

	if (record) {
		*record = (struct ums9117_sdio_slot_transition_record){ 0 };
		record->selector_before = ums9117_sdio_slot_read(
			io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR);
		record->selector_after = record->selector_before;
	}
	if (!state->platform_active || !state->rails_on ||
	    (ums9117_sdio_slot_read(io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR) &
	     UMS9117_SDIO_SLOT_SELECTOR_MASK) !=
		    UMS9117_SDIO_SLOT_SELECTOR_SOURCE)
		return -EPROTO;
	ret = ums9117_sdio_set_operational_clock(
		&io->controller, &state->controller, profile,
		record ? &record->controller : NULL);
	if (record)
		record->selector_after = ums9117_sdio_slot_read(
			io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR);
	return ret;
}

int ums9117_sdio_slot_restore_platform(const struct ums9117_sdio_slot_io *io,
				       struct ums9117_sdio_slot_state *state,
				       u32 quiesce_timeout_us)
{
	u32 value;
	unsigned int index;
	int first_error = 0;
	int ret;

	if (state->platform_active) {
		ret = ums9117_sdio_abort(&io->controller, &state->controller,
					 quiesce_timeout_us);
		if (ret)
			first_error = ret;
		if (state->rails_on) {
			ret = ums9117_sdio_slot_set_rails(io, state, false);
			if (ret && !first_error)
				first_error = ret;
			state->rails_on = false;
		}
		ums9117_sdio_restore_controller(&io->controller,
						&state->controller);
		ret = ums9117_sdio_slot_restore_rails(io, state);
		if (ret && !first_error)
			first_error = ret;
		for (index = 0; index < UMS9117_SDIO_SLOT_PIN_COUNT; ++index)
			ums9117_sdio_slot_write(
				io, ums9117_sdio_slot_pins[index].reg,
				state->pin_snapshot[index]);
		value = ums9117_sdio_slot_read(
			io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR);
		value = (value & ~UMS9117_SDIO_SLOT_SELECTOR_MASK) |
			(state->selector_snapshot &
			 UMS9117_SDIO_SLOT_SELECTOR_MASK);
		ums9117_sdio_slot_write(
			io, UMS9117_SDIO_SLOT_REG_CLOCK_SELECTOR, value);
		if (ums9117_sdio_slot_read(io,
					   UMS9117_SDIO_SLOT_REG_RESET_STATE) !=
		    state->reset_snapshot)
			ums9117_sdio_slot_write(
				io, UMS9117_SDIO_SLOT_REG_RESET_CLEAR,
				UMS9117_SDIO_SLOT_RESET_MASK);
		ums9117_sdio_slot_write(io, UMS9117_SDIO_SLOT_REG_GATE_CLEAR,
					UMS9117_SDIO_SLOT_GATE_MASK);
		if (ums9117_sdio_slot_read(io,
					   UMS9117_SDIO_SLOT_REG_GATE_STATE) !=
			    state->gate_snapshot &&
		    !first_error)
			first_error = -EIO;
		state->platform_active = false;
		state->controller.card_clock_on = false;
		state->controller.physical_width4 = false;
		state->controller.actual_clock_hz = 0;
	}
	return first_error;
}

int ums9117_sdio_slot_cleanup(const struct ums9117_sdio_slot_io *io,
			      struct ums9117_sdio_slot_state *state,
			      u32 quiesce_timeout_us)
{
	int first_error = ums9117_sdio_slot_restore_platform(
		io, state, quiesce_timeout_us);
	int ret = ums9117_sdio_slot_restore_card_detect(io, state);

	return first_error ? first_error : ret;
}
