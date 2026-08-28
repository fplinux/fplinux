// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>

#include "ums9117-sdio-core.h"

#define UMS9117_SDIO_RESET_POLLS 64U
#define UMS9117_SDIO_INHIBIT_POLLS 100U
#define UMS9117_SDIO_CLOCK_TIMEOUT_US 10000U
#define UMS9117_SDIO_POLL_DELAY_US 10U
#define UMS9117_SDIO_INITIAL_CLOCKS_DELAY_US 10000U

static u32 ums9117_sdio_read(const struct ums9117_sdio_io *io,
			     enum ums9117_sdio_reg reg)
{
	return io->read(io->context, reg);
}

static void ums9117_sdio_write(const struct ums9117_sdio_io *io,
			       enum ums9117_sdio_reg reg, u32 value)
{
	io->write(io->context, reg, value);
}

static bool ums9117_sdio_timed_out(const struct ums9117_sdio_io *io,
				   u64 started, u32 timeout_us)
{
	return io->time_us(io->context) - started >= timeout_us;
}

static void ums9117_sdio_reset_physical_state(struct ums9117_sdio_state *state)
{
	state->card_clock_on = false;
	state->physical_width4 = false;
	state->actual_clock_hz = 0;
}

int ums9117_sdio_snapshot_controller(const struct ums9117_sdio_io *io,
				     struct ums9117_sdio_state *state)
{
	u32 version;

	state->clock_snapshot =
		ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	state->host_control1_snapshot =
		ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	state->status_enable_snapshot =
		ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE);
	version = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_VERSION);
	state->snapshot_valid = true;
	if ((version & UMS9117_SDIO_HOST_VERSION_MASK) !=
		    UMS9117_SDIO_HOST_VERSION_EXPECTED ||
	    state->host_control1_snapshot ||
	    (state->clock_snapshot &
	     (UMS9117_SDIO_HOST_RESET | UMS9117_SDIO_CLOCK_CARD_EN)))
		return -EPROTONOSUPPORT;
	ums9117_sdio_reset_physical_state(state);
	return 0;
}

void ums9117_sdio_mask_and_ack(const struct ums9117_sdio_io *io)
{
	u32 status;

	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	status = ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_STATUS) &
		 UMS9117_SDIO_STATUS_ENABLE_MASK;
	if (status)
		ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS,
				   status);
}

int ums9117_sdio_reset_controller(const struct ums9117_sdio_io *io)
{
	u32 before = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	u32 value;
	unsigned int index;

	if (before & UMS9117_SDIO_HOST_RESET)
		return -EBUSY;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET,
			   before | UMS9117_SDIO_HOST_RESET);
	for (index = 0; index < UMS9117_SDIO_RESET_POLLS; ++index) {
		value = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
		if ((value & ~UMS9117_SDIO_HOST_RESET) !=
		    (before & ~UMS9117_SDIO_HOST_RESET))
			return -EIO;
		if (!(value & UMS9117_SDIO_HOST_RESET))
			return 0;
		io->delay_us(io->context, 1);
	}
	return -ETIMEDOUT;
}

int ums9117_sdio_set_1bit(const struct ums9117_sdio_io *io,
			  struct ums9117_sdio_state *state)
{
	u32 control;
	u32 readback;

	control = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	control = (control & ~UMS9117_SDIO_HOST_CTRL1_OWNED_MASK) |
		  UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_HOST_CONTROL1, control);
	readback = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	if (readback != control ||
	    (readback & UMS9117_SDIO_HOST_CTRL1_OWNED_MASK) !=
		    UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2)
		return -EIO;
	ums9117_sdio_reset_physical_state(state);
	return 0;
}

int ums9117_sdio_configure_1bit(const struct ums9117_sdio_io *io,
				struct ums9117_sdio_state *state)
{
	int ret = ums9117_sdio_set_1bit(io, state);

	if (ret)
		return ret;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE,
			   UMS9117_SDIO_STATUS_ENABLE_MASK);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	return 0;
}

void ums9117_sdio_restore_controller(const struct ums9117_sdio_io *io,
				     const struct ums9117_sdio_state *state)
{
	if (!state->snapshot_valid)
		return;
	ums9117_sdio_mask_and_ack(io);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE,
			   state->status_enable_snapshot);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_HOST_CONTROL1,
			   state->host_control1_snapshot);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET,
			   state->clock_snapshot & ~UMS9117_SDIO_HOST_RESET);
}

int ums9117_sdio_enable_ident_clock(const struct ums9117_sdio_io *io,
				    struct ums9117_sdio_state *state)
{
	u64 started;
	u32 target;
	u32 value;

	value = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	target = value &
		 ~(UMS9117_SDIO_CLOCK_DIVIDER_MASK |
		   UMS9117_SDIO_CLOCK_TIMEOUT_MASK |
		   UMS9117_SDIO_CLOCK_PROG_MODE | UMS9117_SDIO_CLOCK_PLL_EN |
		   UMS9117_SDIO_CLOCK_CARD_EN | UMS9117_SDIO_CLOCK_INT_STABLE |
		   UMS9117_SDIO_CLOCK_INT_EN);
	target |= UMS9117_SDIO_IDENT_DIVIDER | UMS9117_SDIO_CLOCK_INT_EN;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, target);
	started = io->time_us(io->context);
	for (;;) {
		value = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
		if (!(value & UMS9117_SDIO_CLOCK_INT_EN) ||
		    (value & ~UMS9117_SDIO_CLOCK_INT_STABLE) !=
			    (target & ~UMS9117_SDIO_CLOCK_INT_STABLE))
			return -EIO;
		if (value & UMS9117_SDIO_CLOCK_INT_STABLE)
			break;
		if (ums9117_sdio_timed_out(io, started,
					   UMS9117_SDIO_CLOCK_TIMEOUT_US))
			return -ETIMEDOUT;
		io->delay_us(io->context, UMS9117_SDIO_POLL_DELAY_US);
	}
	target = value | UMS9117_SDIO_CLOCK_CARD_EN;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, target);
	if (ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET) != target)
		return -EIO;
	state->card_clock_on = true;
	state->actual_clock_hz = UMS9117_SDIO_IDENT_CLOCK_HZ;
	io->sleep_us(io->context, UMS9117_SDIO_INITIAL_CLOCKS_DELAY_US,
		     UMS9117_SDIO_INITIAL_CLOCKS_DELAY_US);
	return 0;
}

int ums9117_sdio_disable_card_clock(const struct ums9117_sdio_io *io,
				    struct ums9117_sdio_state *state)
{
	u32 target = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET) &
		     ~UMS9117_SDIO_CLOCK_CARD_EN;

	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, target);
	if (ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET) != target)
		return -EIO;
	state->card_clock_on = false;
	state->actual_clock_hz = 0;
	return 0;
}

int ums9117_sdio_wait_inhibit(const struct ums9117_sdio_io *io, bool data)
{
	u32 mask = UMS9117_SDIO_CMD_INHIBIT;
	unsigned int index;

	if (data)
		mask |= UMS9117_SDIO_DAT_INHIBIT;
	for (index = 0; index < UMS9117_SDIO_INHIBIT_POLLS; ++index) {
		if (!(ums9117_sdio_read(io, UMS9117_SDIO_REG_PRESENT_STATE) &
		      mask))
			return 0;
		io->delay_us(io->context, UMS9117_SDIO_POLL_DELAY_US);
	}
	return -EBUSY;
}

int ums9117_sdio_wait_quiescent(const struct ums9117_sdio_io *io,
				u32 timeout_us, u32 *last_present)
{
	u64 started = io->time_us(io->context);
	u32 present;

	for (;;) {
		present = ums9117_sdio_read(io, UMS9117_SDIO_REG_PRESENT_STATE);
		if (!(present & UMS9117_SDIO_DATA_ACTIVE_MASK) &&
		    (present & UMS9117_SDIO_DAT0_LEVEL)) {
			if (last_present)
				*last_present = present;
			return 0;
		}
		if (ums9117_sdio_timed_out(io, started, timeout_us)) {
			if (last_present)
				*last_present = present;
			return -ETIMEDOUT;
		}
		io->sleep_us(io->context, UMS9117_SDIO_POLL_DELAY_US,
			     UMS9117_SDIO_POLL_DELAY_US + 20U);
	}
}

static void ums9117_sdio_clock_values(enum ums9117_sdio_clock_profile profile,
				      u32 *divider, u32 *expected,
				      u32 *frequency)
{
	if (profile == UMS9117_SDIO_CLOCK_HIGH_SPEED) {
		*divider = UMS9117_SDIO_HS_DIVIDER;
		*expected = UMS9117_SDIO_HS_CLOCK_EXPECTED;
		*frequency = UMS9117_SDIO_HS_CLOCK_HZ;
	} else {
		*divider = UMS9117_SDIO_LEGACY_DIVIDER;
		*expected = UMS9117_SDIO_LEGACY_CLOCK_EXPECTED;
		*frequency = UMS9117_SDIO_LEGACY_CLOCK_HZ;
	}
}

int ums9117_sdio_set_operational_clock(
	const struct ums9117_sdio_io *io, struct ums9117_sdio_state *state,
	enum ums9117_sdio_clock_profile profile,
	struct ums9117_sdio_transition_record *record)
{
	u64 started;
	u32 clock;
	u32 control;
	u32 divider;
	u32 expected;
	u32 frequency;
	u32 readback;
	bool first_width = !state->physical_width4;
	int ret;

	ums9117_sdio_clock_values(profile, &divider, &expected, &frequency);
	if (record) {
		record->clock_before = 0;
		record->clock_stopped = 0;
		record->clock_candidate = 0;
		record->clock_readback = 0;
		record->clock_after = 0;
		record->control_before = 0;
		record->control_after = 0;
	}
	if (!first_width && state->actual_clock_hz == frequency)
		return 0;
	ret = ums9117_sdio_wait_inhibit(io, true);
	if (ret)
		return ret;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	if (ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE))
		return -EIO;

	clock = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	control = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	if (record) {
		record->clock_before = clock;
		record->control_before = control;
		record->control_after = control;
	}
	if (!(clock & UMS9117_SDIO_CLOCK_INT_EN) ||
	    !(clock & UMS9117_SDIO_CLOCK_INT_STABLE) ||
	    !(clock & UMS9117_SDIO_CLOCK_CARD_EN) ||
	    (clock & (UMS9117_SDIO_CLOCK_PROG_MODE | UMS9117_SDIO_CLOCK_PLL_EN |
		      UMS9117_SDIO_HOST_RESET)) ||
	    (first_width &&
	     ((clock & UMS9117_SDIO_CLOCK_DIVIDER_MASK) !=
		      UMS9117_SDIO_IDENT_DIVIDER ||
	      (clock & UMS9117_SDIO_CLOCK_TIMEOUT_MASK) ||
	      control != UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2 ||
	      state->actual_clock_hz != UMS9117_SDIO_IDENT_CLOCK_HZ)) ||
	    (!first_width &&
	     ((clock & UMS9117_SDIO_CLOCK_TIMEOUT_MASK) !=
		      UMS9117_SDIO_TIMEOUT_ENCODED ||
	      control != UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2 ||
	      (state->actual_clock_hz == UMS9117_SDIO_LEGACY_CLOCK_HZ &&
	       (clock & UMS9117_SDIO_CLOCK_DIVIDER_MASK) !=
		       UMS9117_SDIO_LEGACY_DIVIDER) ||
	      (state->actual_clock_hz == UMS9117_SDIO_HS_CLOCK_HZ &&
	       (clock & UMS9117_SDIO_CLOCK_DIVIDER_MASK) !=
		       UMS9117_SDIO_HS_DIVIDER) ||
	      (state->actual_clock_hz != UMS9117_SDIO_LEGACY_CLOCK_HZ &&
	       state->actual_clock_hz != UMS9117_SDIO_HS_CLOCK_HZ))))
		return -EPROTO;

	clock &= ~UMS9117_SDIO_CLOCK_CARD_EN;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, clock);
	if (ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET) != clock)
		return -EIO;
	clock &= ~(UMS9117_SDIO_CLOCK_INT_EN | UMS9117_SDIO_CLOCK_INT_STABLE);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, clock);
	started = io->time_us(io->context);
	for (;;) {
		u32 readback =
			ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);

		if (readback == clock) {
			if (record)
				record->clock_stopped = readback;
			break;
		}
		if (ums9117_sdio_timed_out(io, started,
					   UMS9117_SDIO_CLOCK_TIMEOUT_US))
			return -ETIMEDOUT;
		io->delay_us(io->context, UMS9117_SDIO_POLL_DELAY_US);
	}
	if (first_width) {
		control = (control & ~UMS9117_SDIO_HOST_CTRL1_OWNED_MASK) |
			  UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2;
		ums9117_sdio_write(io, UMS9117_SDIO_REG_HOST_CONTROL1, control);
		if (ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1) !=
		    control)
			return -EIO;
		if (record)
			record->control_after = control;
	}
	clock = (clock & ~(UMS9117_SDIO_CLOCK_DIVIDER_MASK |
			   UMS9117_SDIO_CLOCK_TIMEOUT_MASK)) |
		divider | UMS9117_SDIO_TIMEOUT_ENCODED;
	if (record)
		record->clock_candidate = clock;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, clock);
	readback = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	if (record)
		record->clock_readback = readback;
	if (readback != clock)
		return -EIO;
	clock |= UMS9117_SDIO_CLOCK_INT_EN;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, clock);
	started = io->time_us(io->context);
	for (;;) {
		u32 readback =
			ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);

		if (!(readback & UMS9117_SDIO_CLOCK_INT_EN) ||
		    (readback & ~UMS9117_SDIO_CLOCK_INT_STABLE) !=
			    (clock & ~UMS9117_SDIO_CLOCK_INT_STABLE))
			return -EIO;
		if (readback & UMS9117_SDIO_CLOCK_INT_STABLE) {
			clock = readback;
			break;
		}
		if (ums9117_sdio_timed_out(io, started,
					   UMS9117_SDIO_CLOCK_TIMEOUT_US))
			return -ETIMEDOUT;
		io->delay_us(io->context, UMS9117_SDIO_POLL_DELAY_US);
	}
	clock |= UMS9117_SDIO_CLOCK_CARD_EN;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_CLOCK_RESET, clock);
	clock = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	control = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	if (record) {
		record->clock_after = clock;
		record->control_after = control;
	}
	if (clock != expected || control != UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2)
		return -EIO;
	state->physical_width4 = true;
	state->card_clock_on = true;
	state->actual_clock_hz = frequency;
	return 0;
}

int ums9117_sdio_validate_active(const struct ums9117_sdio_io *io,
				 const struct ums9117_sdio_state *state)
{
	u32 clock = ums9117_sdio_read(io, UMS9117_SDIO_REG_CLOCK_RESET);
	u32 control = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);

	if (!state->card_clock_on || !(clock & UMS9117_SDIO_CLOCK_INT_EN) ||
	    !(clock & UMS9117_SDIO_CLOCK_INT_STABLE) ||
	    !(clock & UMS9117_SDIO_CLOCK_CARD_EN) ||
	    (clock & (UMS9117_SDIO_CLOCK_PROG_MODE | UMS9117_SDIO_CLOCK_PLL_EN |
		      UMS9117_SDIO_HOST_RESET)))
		return -EPROTO;
	if (!state->physical_width4)
		return control == UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2 &&
				       (clock &
					UMS9117_SDIO_CLOCK_DIVIDER_MASK) ==
					       UMS9117_SDIO_IDENT_DIVIDER &&
				       !(clock &
					 UMS9117_SDIO_CLOCK_TIMEOUT_MASK) &&
				       state->actual_clock_hz ==
					       UMS9117_SDIO_IDENT_CLOCK_HZ ?
			       0 :
			       -EPROTO;
	if (control != UMS9117_SDIO_HOST_CTRL1_4BIT_ADMA2 ||
	    (clock & UMS9117_SDIO_CLOCK_TIMEOUT_MASK) !=
		    UMS9117_SDIO_TIMEOUT_ENCODED)
		return -EPROTO;
	if (state->actual_clock_hz == UMS9117_SDIO_LEGACY_CLOCK_HZ &&
	    (clock & UMS9117_SDIO_CLOCK_DIVIDER_MASK) ==
		    UMS9117_SDIO_LEGACY_DIVIDER)
		return 0;
	if (state->actual_clock_hz == UMS9117_SDIO_HS_CLOCK_HZ &&
	    (clock & UMS9117_SDIO_CLOCK_DIVIDER_MASK) ==
		    UMS9117_SDIO_HS_DIVIDER)
		return 0;
	return -EPROTO;
}

int ums9117_sdio_abort(const struct ums9117_sdio_io *io,
		       struct ums9117_sdio_state *state, u32 timeout_us)
{
	u32 present = 0;
	int first_error = 0;
	int ret;

	ums9117_sdio_mask_and_ack(io);
	ret = ums9117_sdio_wait_quiescent(io, timeout_us, &present);
	if (ret) {
		ret = ums9117_sdio_reset_controller(io);
		if (!ret)
			ret = ums9117_sdio_wait_quiescent(io, timeout_us,
							  &present);
		if (ret)
			first_error = ret;
	}
	ret = ums9117_sdio_disable_card_clock(io, state);
	if (ret && !first_error)
		first_error = ret;
	if ((ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE) ||
	     (present & UMS9117_SDIO_DATA_ACTIVE_MASK)) &&
	    !first_error)
		first_error = -EBUSY;
	return first_error;
}

int ums9117_sdio_response_flags(enum ums9117_sdio_response_type type,
				u16 *flags)
{
	switch (type) {
	case UMS9117_SDIO_RESPONSE_NONE:
		*flags = UMS9117_SDIO_RESP_NONE;
		break;
	case UMS9117_SDIO_RESPONSE_LONG:
		*flags = UMS9117_SDIO_RESP_LONG | UMS9117_SDIO_CMD_CRC;
		break;
	case UMS9117_SDIO_RESPONSE_SHORT_BUSY:
		*flags = UMS9117_SDIO_RESP_SHORT_BUSY | UMS9117_SDIO_CMD_CRC |
			 UMS9117_SDIO_CMD_INDEX;
		break;
	case UMS9117_SDIO_RESPONSE_OCR:
		*flags = UMS9117_SDIO_RESP_SHORT;
		break;
	case UMS9117_SDIO_RESPONSE_SHORT:
		*flags = UMS9117_SDIO_RESP_SHORT | UMS9117_SDIO_CMD_CRC |
			 UMS9117_SDIO_CMD_INDEX;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

int ums9117_sdio_status_error(u32 status)
{
	if (status & (UMS9117_SDIO_INT_TIMEOUT | UMS9117_SDIO_INT_DATA_TIMEOUT))
		return -ETIMEDOUT;
	if (status & (UMS9117_SDIO_INT_CRC | UMS9117_SDIO_INT_END_BIT |
		      UMS9117_SDIO_INT_INDEX | UMS9117_SDIO_INT_DATA_CRC |
		      UMS9117_SDIO_INT_DATA_END_BIT))
		return -EILSEQ;
	if (status & (UMS9117_SDIO_DETAIL_ERROR_MASK | UMS9117_SDIO_INT_ERROR))
		return -EIO;
	return 0;
}

int ums9117_sdio_r1_error(u32 response)
{
	u32 status = response & UMS9117_SDIO_R1_STATUS_MASK;

	if (!status)
		return 0;
	if (status &
	    (UMS9117_SDIO_R1_WP_VIOLATION | UMS9117_SDIO_R1_CARD_IS_LOCKED |
	     UMS9117_SDIO_R1_WP_ERASE_SKIP))
		return -EROFS;
	if (status &
	    (UMS9117_SDIO_R1_OUT_OF_RANGE | UMS9117_SDIO_R1_ADDRESS_ERROR |
	     UMS9117_SDIO_R1_BLOCK_LEN_ERROR))
		return -EINVAL;
	if (status &
	    (UMS9117_SDIO_R1_COM_CRC_ERROR | UMS9117_SDIO_R1_CARD_ECC_FAILED))
		return -EILSEQ;
	return -EIO;
}

bool ums9117_sdio_status_terminal(u32 status, u32 terminal_bit)
{
	return status & (UMS9117_SDIO_DETAIL_ERROR_MASK |
			 UMS9117_SDIO_INT_ERROR | terminal_bit);
}

int ums9117_sdio_prepare_request(const struct ums9117_sdio_io *io,
				 const struct ums9117_sdio_data_setup *data)
{
	u32 stale;
	u32 control;

	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0);
	stale = ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_STATUS) &
		UMS9117_SDIO_STATUS_ENABLE_MASK;
	if (stale)
		ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS,
				   stale);
	if (ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_STATUS) & stale)
		return -EIO;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE,
			   UMS9117_SDIO_STATUS_ENABLE_MASK);
	if (!data)
		return 0;
	control = ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL1);
	control = (control & ~UMS9117_SDIO_HOST_CTRL1_DMA_MASK) |
		  UMS9117_SDIO_HOST_CTRL1_1BIT_ADMA2;
	ums9117_sdio_write(io, UMS9117_SDIO_REG_HOST_CONTROL1, control);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_BLOCK_COUNT, data->blocks);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_BLOCK_SIZE, data->block_size);
	io->data_barrier(io->context);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_ADMA_ADDRESS_HIGH, 0);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_ADMA_ADDRESS_LOW,
			   data->adma_address);
	return 0;
}

void ums9117_sdio_issue_request(const struct ums9117_sdio_io *io, u32 argument,
				u16 command, u16 transfer, u32 signal)
{
	ums9117_sdio_write(io, UMS9117_SDIO_REG_ARGUMENT, argument);
	if (signal)
		ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE,
				   signal);
	ums9117_sdio_write(io, UMS9117_SDIO_REG_TRANSFER_COMMAND,
			   ((u32)command << 16) | transfer);
}

void ums9117_sdio_capture_completion(const struct ums9117_sdio_io *io,
				     u32 status, bool response_136,
				     bool mask_signal,
				     enum ums9117_sdio_completion_order order,
				     struct ums9117_sdio_completion *completion)
{
	u32 raw[4];

	completion->status = status;
	completion->owned_status = status & UMS9117_SDIO_STATUS_ENABLE_MASK;
	if (mask_signal)
		ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE,
				   0);
	completion->auto_command_status =
		ums9117_sdio_read(io, UMS9117_SDIO_REG_HOST_CONTROL2);
	if (order == UMS9117_SDIO_RESPONSE_BEFORE_ACK) {
		raw[0] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE0);
		raw[1] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE1);
		raw[2] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE2);
		raw[3] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE3);
	}
	if (completion->owned_status)
		ums9117_sdio_write(io, UMS9117_SDIO_REG_INTERRUPT_STATUS,
				   completion->owned_status);
	completion->status_readback =
		ums9117_sdio_read(io, UMS9117_SDIO_REG_INTERRUPT_STATUS);
	if (order == UMS9117_SDIO_ACK_BEFORE_RESPONSE) {
		raw[0] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE0);
		raw[1] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE1);
		raw[2] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE2);
		raw[3] = ums9117_sdio_read(io, UMS9117_SDIO_REG_RESPONSE3);
	}
	if (response_136) {
		completion->response[0] = (raw[3] << 8) | (raw[2] >> 24);
		completion->response[1] = (raw[2] << 8) | (raw[1] >> 24);
		completion->response[2] = (raw[1] << 8) | (raw[0] >> 24);
		completion->response[3] = raw[0] << 8;
	} else {
		completion->response[0] = raw[0];
		completion->response[1] = 0;
		completion->response[2] = 0;
		completion->response[3] = 0;
	}
}

int ums9117_sdio_validate_completion(
	const struct ums9117_sdio_completion *completion, u32 required_status)
{
	int ret = ums9117_sdio_status_error(completion->status);

	if (!ret && (completion->status_readback & completion->owned_status))
		ret = -EIO;
	if (!ret && (completion->status & required_status) != required_status)
		ret = -EIO;
	return ret;
}
