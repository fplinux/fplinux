// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ums9117-sdio-core.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct fake_write {
	enum ums9117_sdio_reg reg;
	u32 value;
};

struct fake_mmio {
	u32 regs[UMS9117_SDIO_REG_COUNT];
	struct fake_write writes[64];
	u64 now_us;
	unsigned int write_count;
	bool reset_stuck;
	bool status_stuck;
	bool clock_auto_stable;
	bool quiesce_after_reset;
};

static u32 fake_read(void *context, enum ums9117_sdio_reg reg)
{
	struct fake_mmio *fake = context;
	u32 value = fake->regs[reg];

	if (reg == UMS9117_SDIO_REG_CLOCK_RESET &&
	    (value & UMS9117_SDIO_HOST_RESET) && !fake->reset_stuck) {
		value &= ~UMS9117_SDIO_HOST_RESET;
		fake->regs[reg] = value;
		if (fake->quiesce_after_reset)
			fake->regs[UMS9117_SDIO_REG_PRESENT_STATE] =
				UMS9117_SDIO_DAT0_LEVEL;
	}
	if (reg == UMS9117_SDIO_REG_CLOCK_RESET && fake->clock_auto_stable &&
	    (value & UMS9117_SDIO_CLOCK_INT_EN) &&
	    !(value & UMS9117_SDIO_CLOCK_INT_STABLE)) {
		value |= UMS9117_SDIO_CLOCK_INT_STABLE;
		fake->regs[reg] = value;
	}
	return value;
}

static void fake_write(void *context, enum ums9117_sdio_reg reg, u32 value)
{
	struct fake_mmio *fake = context;

	if (fake->write_count < ARRAY_SIZE(fake->writes)) {
		fake->writes[fake->write_count].reg = reg;
		fake->writes[fake->write_count].value = value;
	}
	fake->write_count++;
	if (reg == UMS9117_SDIO_REG_INTERRUPT_STATUS) {
		if (!fake->status_stuck)
			fake->regs[reg] &= ~value;
		return;
	}
	fake->regs[reg] = value;
}

static u64 fake_time_us(void *context)
{
	return ((struct fake_mmio *)context)->now_us;
}

static void fake_delay_us(void *context, u32 usec)
{
	struct fake_mmio *fake = context;

	fake->now_us += usec;
}

static void fake_sleep_us(void *context, u32 min, u32 max)
{
	(void)max;
	fake_delay_us(context, min);
}

static void fake_barrier(void *context)
{
	(void)context;
}

static struct ums9117_sdio_io fake_io(struct fake_mmio *fake)
{
	struct ums9117_sdio_io io = {
		.context = fake,
		.read = fake_read,
		.write = fake_write,
		.time_us = fake_time_us,
		.delay_us = fake_delay_us,
		.sleep_us = fake_sleep_us,
		.data_barrier = fake_barrier,
	};

	return io;
}

static int expect(bool condition, const char *message)
{
	if (condition)
		return 0;
	fprintf(stderr, "%s\n", message);
	return 1;
}

static int test_response_and_status_decoding(void)
{
	u16 flags = 0;
	int crc_error;
	int failed = 0;

	failed |= expect(!ums9117_sdio_response_flags(
				 UMS9117_SDIO_RESPONSE_SHORT_BUSY, &flags),
			 "busy response was rejected");
	failed |= expect(flags == 0x001bU, "busy response encoding changed");
	failed |= expect(ums9117_sdio_status_error(0x00010000U) == -ETIMEDOUT,
			 "command timeout decoding changed");
	crc_error = ums9117_sdio_status_error(0x00020000U);
	failed |=
		expect(crc_error < 0 && crc_error != -ETIMEDOUT,
		       "command CRC error was reported as success or timeout");
	failed |= expect(ums9117_sdio_r1_error(0x04000000U) == -EROFS,
			 "write-protect response decoding changed");
	return failed;
}

static int test_reset_success(void)
{
	struct fake_mmio fake = { 0 };
	struct ums9117_sdio_io io = fake_io(&fake);
	int failed = 0;

	fake.regs[UMS9117_SDIO_REG_CLOCK_RESET] = 0x00000400U;
	failed |= expect(!ums9117_sdio_reset_controller(&io),
			 "controller reset did not complete");
	failed |= expect(fake.write_count == 1U,
			 "controller reset performed extra writes");
	failed |= expect(fake.writes[0].reg == UMS9117_SDIO_REG_CLOCK_RESET &&
				 fake.writes[0].value == 0x01000400U,
			 "controller reset write changed");
	return failed;
}

static int test_reset_timeout_stops(void)
{
	struct fake_mmio fake = { .reset_stuck = true };
	struct ums9117_sdio_io io = fake_io(&fake);
	int failed = 0;

	failed |= expect(ums9117_sdio_reset_controller(&io) == -ETIMEDOUT,
			 "stuck reset did not return ETIMEDOUT");
	failed |= expect(fake.write_count == 1U,
			 "stuck reset performed a later write");
	return failed;
}

static int test_stale_status_stops_request(void)
{
	struct fake_mmio fake = { .status_stuck = true };
	struct ums9117_sdio_io io = fake_io(&fake);
	int failed = 0;

	fake.regs[UMS9117_SDIO_REG_INTERRUPT_STATUS] = 0x00020000U;
	failed |= expect(ums9117_sdio_prepare_request(&io, NULL) == -EIO,
			 "uncleared status did not return EIO");
	failed |= expect(fake.write_count == 2U,
			 "failed request preparation performed later writes");
	failed |= expect(
		fake.writes[0].reg ==
				UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE &&
			fake.writes[0].value == 0 &&
			fake.writes[1].reg ==
				UMS9117_SDIO_REG_INTERRUPT_STATUS &&
			fake.writes[1].value == 0x00020000U,
		"failed request preparation write boundary changed");
	return failed;
}

static int test_data_request_registers(void)
{
	struct fake_mmio fake = { 0 };
	struct ums9117_sdio_io io = fake_io(&fake);
	static const struct fake_write expected[] = {
		{ UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0x00000000U },
		{ UMS9117_SDIO_REG_INTERRUPT_STATUS_ENABLE, 0x1b7f0003U },
		{ UMS9117_SDIO_REG_HOST_CONTROL1, 0x00000012U },
		{ UMS9117_SDIO_REG_BLOCK_COUNT, 0x00000001U },
		{ UMS9117_SDIO_REG_BLOCK_SIZE, 0x00000200U },
		{ UMS9117_SDIO_REG_ADMA_ADDRESS_HIGH, 0x00000000U },
		{ UMS9117_SDIO_REG_ADMA_ADDRESS_LOW, 0x81234000U },
		{ UMS9117_SDIO_REG_ARGUMENT, 0x12345678U },
		{ UMS9117_SDIO_REG_TRANSFER_COMMAND, 0x113a0013U },
	};
	const struct ums9117_sdio_data_setup data = {
		.blocks = 1,
		.block_size = 512,
		.adma_address = 0x81234000U,
	};
	int failed = 0;
	unsigned int index;

	fake.regs[UMS9117_SDIO_REG_HOST_CONTROL1] = 0x00000012U;
	failed |= expect(!ums9117_sdio_prepare_request(&io, &data),
			 "data request preparation failed");
	ums9117_sdio_issue_request(&io, 0x12345678U, 0x113aU, 0x0013U, 0);
	failed |= expect(fake.regs[UMS9117_SDIO_REG_BLOCK_COUNT] == 1U &&
				 fake.regs[UMS9117_SDIO_REG_BLOCK_SIZE] == 512U,
			 "block shape programming changed");
	failed |= expect(fake.regs[UMS9117_SDIO_REG_ADMA_ADDRESS_HIGH] == 0 &&
				 fake.regs[UMS9117_SDIO_REG_ADMA_ADDRESS_LOW] ==
					 0x81234000U,
			 "ADMA high-then-low programming changed");
	failed |= expect(fake.regs[UMS9117_SDIO_REG_ARGUMENT] == 0x12345678U &&
				 fake.regs[UMS9117_SDIO_REG_TRANSFER_COMMAND] ==
					 0x113a0013U,
			 "final request issue changed");
	failed |= expect(fake.write_count == ARRAY_SIZE(expected),
			 "data request write count changed");
	for (index = 0; index < ARRAY_SIZE(expected) &&
			fake.write_count == ARRAY_SIZE(expected);
	     ++index)
		failed |=
			expect(fake.writes[index].reg == expected[index].reg &&
				       fake.writes[index].value ==
					       expected[index].value,
			       "data request write order changed");
	return failed;
}

static int expect_writes(const struct fake_mmio *fake,
			 const struct fake_write *expected, size_t count,
			 const char *message)
{
	size_t index;

	if (fake->write_count != count)
		return expect(false, message);
	for (index = 0; index < count; ++index)
		if (fake->writes[index].reg != expected[index].reg ||
		    fake->writes[index].value != expected[index].value)
			return expect(false, message);
	return 0;
}

/*
 * U-Boot widens the bus at the legacy clock, then raises a 4-bit bus to high
 * speed. Expected words follow the SDHCI Clock Control register (02Ch): bit 0
 * internal clock enable, bit 1 stable, bit 2 SD clock enable and bits 15:8 the
 * divisor N, giving SDCLK = 195 MHz / (2 * N). N = 4 is 24.375 MHz and N = 2
 * is 48.75 MHz. Bits 19:16 hold timeout value 8; fixture bit 27 is preserved.
 */
static int test_width_then_high_speed_transition(void)
{
	struct fake_mmio fake = { .clock_auto_stable = true };
	struct ums9117_sdio_io io = fake_io(&fake);
	struct ums9117_sdio_state state = {
		.card_clock_on = true,
		.physical_width4 = false,
		.actual_clock_hz = 399590U,
	};
	static const struct fake_write legacy[] = {
		{ UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0x00000000U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x0800f403U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x0800f400U },
		{ UMS9117_SDIO_REG_HOST_CONTROL1, 0x00000012U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080400U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080401U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080407U },
	};
	static const struct fake_write high_speed[] = {
		{ UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE, 0x00000000U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080403U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080400U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080200U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080201U },
		{ UMS9117_SDIO_REG_CLOCK_RESET, 0x08080207U },
	};
	int failed = 0;

	fake.regs[UMS9117_SDIO_REG_CLOCK_RESET] = 0x0800f407U;
	fake.regs[UMS9117_SDIO_REG_HOST_CONTROL1] = 0x00000010U;
	failed |= expect(!ums9117_sdio_set_operational_clock(
				 &io, &state, UMS9117_SDIO_CLOCK_LEGACY, NULL),
			 "first-width legacy transition failed");
	failed |= expect(state.physical_width4 && state.card_clock_on &&
				 state.actual_clock_hz == 24375000U,
			 "legacy state publication changed");
	failed |= expect_writes(&fake, legacy, ARRAY_SIZE(legacy),
				"legacy transition writes changed");
	fake.write_count = 0;
	failed |= expect(!ums9117_sdio_set_operational_clock(
				 &io, &state, UMS9117_SDIO_CLOCK_HIGH_SPEED,
				 NULL),
			 "legacy to high-speed transition failed");
	failed |= expect(state.physical_width4 && state.card_clock_on &&
				 state.actual_clock_hz == 48750000U,
			 "high-speed state publication changed");
	failed |= expect_writes(&fake, high_speed, ARRAY_SIZE(high_speed),
				"high-speed transition writes changed");
	return failed;
}

static int test_abort_reset_fallback(void)
{
	struct fake_mmio fake = {
		.quiesce_after_reset = true,
	};
	struct ums9117_sdio_io io = fake_io(&fake);
	struct ums9117_sdio_state state = {
		.card_clock_on = true,
		.physical_width4 = true,
		.actual_clock_hz = UMS9117_SDIO_HS_CLOCK_HZ,
	};
	int failed = 0;

	fake.regs[UMS9117_SDIO_REG_PRESENT_STATE] = UMS9117_SDIO_DAT_INHIBIT;
	fake.regs[UMS9117_SDIO_REG_CLOCK_RESET] =
		UMS9117_SDIO_HS_CLOCK_EXPECTED;
	failed |= expect(!ums9117_sdio_abort(&io, &state, 20U),
			 "abort reset fallback failed");
	failed |= expect(!state.card_clock_on && !state.actual_clock_hz,
			 "abort did not publish card-clock off");
	failed |= expect(
		fake.write_count == 3U &&
			fake.writes[0].reg ==
				UMS9117_SDIO_REG_INTERRUPT_SIGNAL_ENABLE &&
			fake.writes[0].value == 0 &&
			fake.writes[1].reg == UMS9117_SDIO_REG_CLOCK_RESET &&
			(fake.writes[1].value & UMS9117_SDIO_HOST_RESET) &&
			fake.writes[2].reg == UMS9117_SDIO_REG_CLOCK_RESET &&
			!(fake.writes[2].value & UMS9117_SDIO_CLOCK_CARD_EN),
		"abort reset-fallback write boundary changed");
	return failed;
}

int main(void)
{
	int failed = 0;

	failed |= test_response_and_status_decoding();
	failed |= test_reset_success();
	failed |= test_reset_timeout_stops();
	failed |= test_stale_status_stops_request();
	failed |= test_data_request_registers();
	failed |= test_width_then_high_speed_transition();
	failed |= test_abort_reset_fallback();
	return failed ? 1 : 0;
}
