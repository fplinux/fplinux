// SPDX-License-Identifier: GPL-2.0-only
/* Link the MMC driver and slot/core logic to fake MMIO, ADI and device properties. */
#include <stdlib.h>
#include <string.h>

#include "mmc-boundary.h"
#include "stage0-handoff.h"
#include "ums9117-mmc.h"

extern const struct driver ums9117_mmc;

struct fake_register {
	uintptr_t address;
	u32 value;
};

static struct fake_register registers[96];
static unsigned int register_count;
static uintptr_t adi_read_address;
static u64 elapsed_us;

static u32 *fake_register(uintptr_t address)
{
	unsigned int index;

	for (index = 0; index < register_count; ++index)
		if (registers[index].address == address)
			return &registers[index].value;
	if (register_count == sizeof(registers) / sizeof(registers[0]))
		abort();
	registers[register_count].address = address;
	return &registers[register_count++].value;
}

u32 readl(const void *address)
{
	uintptr_t physical = (uintptr_t)address;

	if (physical == 0x4060002cU)
		return *fake_register(adi_read_address);
	return *fake_register(physical);
}

void writel(u32 value, void *address)
{
	uintptr_t physical = (uintptr_t)address;

	if (physical == 0x40600028U) {
		adi_read_address = value;
		return;
	}
	if (physical == 0x20e01000U || physical == 0x20e01004U) {
		*fake_register(physical - 0x1000U) |= value;
		return;
	}
	if (physical == 0x20e02000U || physical == 0x20e02004U) {
		*fake_register(physical - 0x2000U) &= ~value;
		return;
	}
	if (physical == 0x2030002cU) {
		value &= ~0x01000000U;
		if (value & 1U)
			value |= 2U;
	}
	if (physical == 0x20300030U)
		*fake_register(physical) &= ~value;
	else
		*fake_register(physical) = value;
}

u64 timer_get_us(void)
{
	return elapsed_us;
}

void udelay(u32 usec)
{
	elapsed_us += usec;
}

void mdelay(u32 msec)
{
	elapsed_us += (u64)msec * 1000U;
}

void mb(void)
{
}

void hang(void)
{
	abort();
}

static void fail_storage(u32 code, u32 detail) __attribute__((noreturn));

static void fail_storage(u32 code, u32 detail)
{
	fprintf(stderr, "unexpected storage failure code=%u detail=%u\n", code,
		detail);
	abort();
}

const struct fplinux_stage0_ops *ums9117_stage0_ops(void)
{
	static const struct fplinux_stage0_ops ops = {
		.fail = fail_storage,
	};

	return &ops;
}

bool dev_read_bool(struct udevice *dev, const char *property)
{
	return !strcmp(property, "non-removable") && dev->non_removable;
}

int mmc_bind(struct udevice *dev, struct mmc *mmc, struct mmc_config *cfg)
{
	(void)dev;
	mmc->cfg = cfg;
	return 0;
}

static int expect(bool condition, const char *message)
{
	if (condition)
		return 0;
	fprintf(stderr, "%s\n", message);
	return 1;
}

int main(int argc, char **argv)
{
	const struct dm_mmc_ops *ops = ums9117_mmc.ops;
	struct udevice dev = { .name = "test-slot" };
	bool removable_absent;
	bool invalid_voltage;
	u32 mask;
	int failed = 0;
	int ret;

	if (argc != 2)
		return 2;
	dev.non_removable = !strncmp(argv[1], "fixed-", 6);
	removable_absent = !strcmp(argv[1], "removable-absent");
	invalid_voltage = !strcmp(argv[1], "fixed-invalid-voltage");
	mask = !strcmp(argv[1], "fixed-busy-detect") ? 2U : 0U;
	*fake_register(0x402c0004U) = mask;
	*fake_register(0x402c0000U) =
		dev.non_removable || removable_absent ? 1U : 0U;
	*fake_register(0x40600030U) = 0x400U;
	*fake_register(0x40608d00U) = 1U;
	*fake_register(0x40608d04U) = 0x50U;
	*fake_register(0x40608d0cU) = 1U;
	*fake_register(0x40608d10U) = invalid_voltage ? 0x6eU : 0x6fU;
	*fake_register(0x20300024U) = 0x00100000U;
	*fake_register(0x203000fcU) = 0x00040000U;
	dev.plat = calloc(1, ums9117_mmc.plat_auto);
	dev.priv = calloc(1, ums9117_mmc.priv_auto);
	if (!dev.plat || !dev.priv)
		return 2;
	failed |= expect(!ums9117_mmc.bind(&dev), "driver bind failed");
	ret = ums9117_mmc.probe(&dev);
	if (invalid_voltage) {
		failed |= expect(ret == -ERANGE,
				 "fixed slot bypassed voltage admission");
		goto done;
	}
	failed |= expect(!ret, "driver probe rejected the slot");
	if (ret)
		goto done;
	failed |= expect(ops->get_cd(&dev) == !removable_absent,
			 "card presence does not match the slot policy");
	ret = ops->reinit(&dev);
	failed |=
		expect(ret == (removable_absent ? -ENOMEDIUM : 0),
		       "slot initialization does not match the card presence");
	if (!ret) {
		failed |=
			expect((*fake_register(0x2030002cU) & 4U) &&
				       !(*fake_register(0x40608d00U) & 1U) &&
				       !(*fake_register(0x40608d0cU) & 1U),
			       "present card did not receive clock and power");
		if (!dev.non_removable) {
			*fake_register(0x402c0000U) = 1U;
			failed |= expect(ops->get_cd(&dev) == 0,
					 "removable slot ignored card removal");
		}
	}
	ums9117_mmc_release();
	failed |= expect(*fake_register(0x402c0004U) == mask,
			 "slot changed unrelated card-detect ownership");
	failed |= expect(!*fake_register(0x20e00000U) &&
				 (*fake_register(0x40608d00U) & 1U) &&
				 (*fake_register(0x40608d0cU) & 1U),
			 "release did not restore slot power and gate state");
	failed |= expect(!ums9117_mmc.remove(&dev), "driver remove failed");
done:
	free(dev.priv);
	free(dev.plat);
	return failed;
}
