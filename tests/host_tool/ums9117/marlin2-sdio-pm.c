/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdio.h>
#include <stdlib.h>

#include "bq2830l-radio.h"

/*
 * The driver is linked unchanged. These doubles replace kernel device state
 * and the SDIO PM API. Firmware boot and hardware I/O are not exercised and
 * their external APIs fail if suspend or resume attempts to use them.
 */
unsigned long jiffies;

void sdio_set_drvdata(struct sdio_func *func, void *data)
{
	func->dev.driver_data = data;
}

void *sdio_get_drvdata(struct sdio_func *func)
{
	return func->dev.driver_data;
}

int sdio_set_host_pm_flags(struct sdio_func *func, unsigned int flags)
{
	struct mmc_host *host = func->card->host;

	if (flags & ~host->pm_caps)
		return -EINVAL;
	host->pm_flags |= flags;
	return 0;
}

static _Noreturn void unexpected_hardware(const char *name)
{
	fprintf(stderr, "PM callback unexpectedly used %s\n", name);
	abort();
}

/* Unused probe/remove dependencies keep the complete driver normally linked. */
#define HARDWARE_STUB(result, name, arguments) \
	result name arguments                  \
	{                                      \
		unexpected_hardware(__func__); \
	}

HARDWARE_STUB(void, msleep, (unsigned int milliseconds))
HARDWARE_STUB(void *, kmalloc, (size_t bytes, int flags))
HARDWARE_STUB(void, kfree, (void *pointer))
HARDWARE_STUB(void *, devm_kzalloc,
	      (struct device * dev, size_t bytes, int flags))
HARDWARE_STUB(int, device_property_read_string,
	      (struct device * dev, const char *property, const char **value))
HARDWARE_STUB(int, request_firmware_direct,
	      (const struct firmware **firmware, const char *name,
	       struct device *dev))
HARDWARE_STUB(void, release_firmware, (const struct firmware *firmware))
HARDWARE_STUB(void, sha256, (const u8 *data, size_t bytes, u8 *digest))
HARDWARE_STUB(bool, of_device_is_compatible, (void *node, const char *name))
HARDWARE_STUB(int, dev_err_probe,
	      (struct device * dev, int error, const char *format, ...))
HARDWARE_STUB(void, sdio_claim_host, (struct sdio_func * func))
HARDWARE_STUB(void, sdio_release_host, (struct sdio_func * func))
HARDWARE_STUB(int, sdio_enable_func, (struct sdio_func * func))
HARDWARE_STUB(int, sdio_disable_func, (struct sdio_func * func))
HARDWARE_STUB(int, sdio_set_block_size,
	      (struct sdio_func * func, unsigned int bytes))
HARDWARE_STUB(int, sdio_memcpy_toio,
	      (struct sdio_func * func, unsigned int address, void *data,
	       int bytes))
HARDWARE_STUB(int, sdio_memcpy_toio_fixed_port,
	      (struct sdio_func * func, unsigned int address, void *data,
	       int bytes))
HARDWARE_STUB(int, sdio_memcpy_fromio,
	      (struct sdio_func * func, void *data, unsigned int address,
	       int bytes))
HARDWARE_STUB(u32, sdio_readl,
	      (struct sdio_func * func, unsigned int address, int *error))
HARDWARE_STUB(void, sdio_writel,
	      (struct sdio_func * func, u32 value, unsigned int address,
	       int *error))
HARDWARE_STUB(u8, sdio_f0_readb,
	      (struct sdio_func * func, unsigned int address, int *error))
HARDWARE_STUB(void, sdio_f0_writeb,
	      (struct sdio_func * func, u8 value, unsigned int address,
	       int *error))
HARDWARE_STUB(bool, bq2830l_radio_runtime_requested, (struct sdio_func * func))
HARDWARE_STUB(unsigned long, bq2830l_radio_deadline, (struct sdio_func * func))
HARDWARE_STUB(struct gpio_desc *, bq2830l_radio_rx_request,
	      (struct sdio_func * func))
HARDWARE_STUB(void, bq2830l_radio_report,
	      (struct sdio_func * func, int error,
	       const struct marlin2_boot_result *result))
HARDWARE_STUB(void, bq2830l_radio_runtime_cleanup_error,
	      (struct sdio_func * func, int error))
HARDWARE_STUB(int, marlin2_packet_get_info_locked,
	      (struct sdio_func * func, struct gpio_desc *request, u32 token,
	       unsigned long deadline, struct marlin2_packet_result *result))

/* A kernel generic PM dispatch with no registered method returns success. */
static int suspend_device(struct sdio_func *func)
{
	const struct dev_pm_ops *pm = marlin2_sdio_driver.drv.pm;

	return pm && pm->suspend ? pm->suspend(&func->dev) : 0;
}

static int resume_device(struct sdio_func *func)
{
	const struct dev_pm_ops *pm = marlin2_sdio_driver.drv.pm;

	return pm && pm->resume ? pm->resume(&func->dev) : 0;
}

static void check_persistent_cycles(struct sdio_func *func)
{
	struct mmc_host *host = func->card->host;
	unsigned int cycle;
	int persistent_runtime;

	sdio_set_drvdata(func, &persistent_runtime);
	host->pm_caps = 1;
	for (cycle = 0; cycle < 2; cycle++) {
		assert(!suspend_device(func));
		/* Bit 0 is the SDIO PM API's retained-power request. */
		assert(host->pm_flags == 1);
		/* The card PM core consumes this request during resume. */
		host->pm_flags = 0;
		assert(!resume_device(func));
		assert(!host->pm_flags);
	}
}

static void check_unsupported_retention(struct sdio_func *func)
{
	struct mmc_host *host = func->card->host;
	int persistent_runtime;

	sdio_set_drvdata(func, &persistent_runtime);
	host->pm_caps = 0;
	host->pm_flags = 0;
	assert(suspend_device(func) == -EINVAL);
	assert(!host->pm_flags);
}

static void check_nonpersistent_function(struct sdio_func *func)
{
	struct mmc_host *host = func->card->host;

	sdio_set_drvdata(func, NULL);
	host->pm_caps = 0;
	assert(!suspend_device(func));
	assert(!host->pm_flags);
	assert(!resume_device(func));
}

int main(int argc, char **argv)
{
	struct mmc_host host = {};
	struct mmc_card card = { .host = &host };
	struct sdio_func func = { .card = &card, .num = 1 };

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "persistent") &&
			 strcmp(argv[1], "unsupported") &&
			 strcmp(argv[1], "nonpersistent")))
		return 2;
	if (argc == 1 || !strcmp(argv[1], "persistent"))
		check_persistent_cycles(&func);
	if (argc == 1 || !strcmp(argv[1], "unsupported"))
		check_unsupported_retention(&func);
	if (argc == 1 || !strcmp(argv[1], "nonpersistent"))
		check_nonpersistent_function(&func);
	if (argc == 1)
		puts("Marlin2 SDIO PM host component cases passed");
	else
		printf("Marlin2 SDIO %s PM case passed\n", argv[1]);
	return 0;
}
