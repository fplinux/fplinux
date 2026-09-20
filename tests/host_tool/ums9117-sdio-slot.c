// SPDX-License-Identifier: GPL-2.0-only
/* Characterize slot ownership and power policy with fake MMIO/ADI. */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

#include "ums9117-sdio-slot.h"

struct fake_slot {
	u32 regs[UMS9117_SDIO_SLOT_REG_COUNT];
	u16 analog[UMS9117_SDIO_SLOT_ANALOG_COUNT];
	unsigned int writes;
	u32 sleep_ms;
};

static u32 fake_read(void *context, enum ums9117_sdio_slot_reg reg)
{
	return ((struct fake_slot *)context)->regs[reg];
}

static void fake_write(void *context, enum ums9117_sdio_slot_reg reg, u32 value)
{
	struct fake_slot *fake = context;

	fake->regs[reg] = value;
	fake->writes++;
}

static int fake_adi_transaction(void *context)
{
	(void)context;
	return 0;
}

static int fake_adi_read(void *context, enum ums9117_sdio_slot_analog_reg reg,
			 u16 *value)
{
	*value = ((struct fake_slot *)context)->analog[reg];
	return 0;
}

static int fake_adi_write(void *context, enum ums9117_sdio_slot_analog_reg reg,
			  u16 value)
{
	struct fake_slot *fake = context;

	fake->analog[reg] = value;
	fake->writes++;
	return 0;
}

static void fake_sleep_ms(void *context, u32 msec)
{
	((struct fake_slot *)context)->sleep_ms += msec;
}

static struct ums9117_sdio_slot_io fake_io(struct fake_slot *fake)
{
	struct ums9117_sdio_slot_io io = {
		.context = fake,
		.read = fake_read,
		.write = fake_write,
		.adi_begin = fake_adi_transaction,
		.adi_read = fake_adi_read,
		.adi_write = fake_adi_write,
		.adi_end = fake_adi_transaction,
		.sleep_ms = fake_sleep_ms,
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

static int test_voltage_admission(void)
{
	struct fake_slot fake = { 0 };
	struct ums9117_sdio_slot_io io = fake_io(&fake);
	struct ums9117_sdio_slot_state state = { 0 };
	int failed = 0;

	fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] = 0x0050U;
	fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] = 0x006eU;
	failed |= expect(ums9117_sdio_slot_snapshot(&io, &state) == -ERANGE,
			 "unexpected voltage baseline was admitted");
	failed |= expect(!state.snapshots_valid && !fake.writes,
			 "voltage rejection changed the slot");
	fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] = 0x006fU;
	failed |= expect(!ums9117_sdio_slot_snapshot(&io, &state),
			 "expected voltage baseline was rejected");
	failed |= expect(state.snapshots_valid && !fake.writes,
			 "slot snapshot changed hardware state");
	return failed;
}

static int test_card_detect_ownership(void)
{
	struct fake_slot fake = { 0 };
	struct ums9117_sdio_slot_io io = fake_io(&fake);
	struct ums9117_sdio_slot_state state = { 0 };
	int failed = 0;

	fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] = 2U;
	failed |= expect(ums9117_sdio_slot_enable_card_detect(&io, &state) ==
				 -EBUSY,
			 "card detection took an already owned EIC bank");
	failed |= expect(!fake.writes && !state.card_detect_owned,
			 "failed EIC admission changed ownership");
	fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] = 0;
	failed |= expect(!ums9117_sdio_slot_enable_card_detect(&io, &state),
			 "idle EIC could not be acquired");
	failed |= expect(fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] ==
					 1U &&
				 fake.sleep_ms == 3U,
			 "EIC input was not enabled and settled");
	failed |= expect(ums9117_sdio_slot_card_present(&io, &state) == 1,
			 "active-low card detect missed an inserted card");
	fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_DATA] = 1U;
	failed |= expect(ums9117_sdio_slot_card_present(&io, &state) == 0,
			 "active-low card detect missed card removal");
	fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] |= 4U;
	failed |= expect(!ums9117_sdio_slot_restore_card_detect(&io, &state),
			 "card-detect ownership was not released");
	failed |= expect(fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] ==
					 4U &&
				 !state.card_detect_owned,
			 "EIC release changed a separately owned input");
	failed |= expect(ums9117_sdio_slot_card_present(&io, &state) == -EIO,
			 "released card-detect input was still trusted");
	fake.regs[UMS9117_SDIO_SLOT_REG_CARD_DETECT_MASK] = 0;
	failed |=
		expect(!ums9117_sdio_slot_enable_card_detect(&io, &state) &&
			       ums9117_sdio_slot_card_present(&io, &state) == 0,
		       "empty slot was not admitted with card absent");
	return failed;
}

static int test_power_preserves_voltage_and_other_pmic_bits(void)
{
	struct fake_slot fake = { 0 };
	struct ums9117_sdio_slot_io io = fake_io(&fake);
	struct ums9117_sdio_slot_state state = { 0 };
	int failed = 0;

	fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_PD] = 0x2221U;
	fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_PD] = 0x4441U;
	fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] = 0x0050U;
	fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] = 0x006fU;
	failed |= expect(ums9117_sdio_slot_set_slot_power(&io, &state, true) ==
					 -EINVAL &&
				 !fake.writes,
			 "inactive slot changed its rails");
	state.platform_active = true;
	failed |= expect(!ums9117_sdio_slot_set_slot_power(&io, &state, true),
			 "slot power-on failed");
	failed |= expect(
		fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_PD] == 0x2220U &&
			fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_PD] ==
				0x4440U &&
			state.rails_on && fake.sleep_ms == 300U,
		"power-on changed other PMIC bits or skipped settling");
	failed |= expect(!ums9117_sdio_slot_set_slot_power(&io, &state, true) &&
				 fake.sleep_ms == 300U,
			 "unchanged rails caused another settling delay");
	failed |= expect(!ums9117_sdio_slot_set_slot_power(&io, &state, false),
			 "slot power-off failed");
	failed |= expect(
		fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_PD] == 0x2221U &&
			fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_PD] ==
				0x4441U &&
			!state.rails_on && fake.sleep_ms == 600U,
		"power-off changed other PMIC bits or skipped settling");
	failed |= expect(
		fake.analog[UMS9117_SDIO_SLOT_ANALOG_CORE_VOLT] == 0x0050U &&
			fake.analog[UMS9117_SDIO_SLOT_ANALOG_IO_VOLT] ==
				0x006fU,
		"slot power operation rewrote VSEL");
	return failed;
}

int main(void)
{
	return test_voltage_admission() | test_card_detect_ownership() |
	       test_power_preserves_voltage_and_other_pmic_bits();
}
