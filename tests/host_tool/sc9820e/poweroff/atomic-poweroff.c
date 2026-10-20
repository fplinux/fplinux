// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <poweroff-test-kernel.h>
#include <linux/spi/sprd-adi.h>

extern struct platform_driver *test_driver_sprd_adi_driver;

/* Only MMIO and the hardware semaphore carry behavior in this host peer. */
static struct {
	u32 mmio[0x10000 / 4];
	u32 pmic[0x1000 / 4];
	u32 read_command;
	struct hwspinlock lock;
	bool local_irqs_disabled;
	bool no_unlock_ack;
	bool bad_unlock_readback;
	bool bad_readback;
	bool final_fifo_busy;
	bool protection_acked;
	unsigned int ack_after;
	unsigned int unlock_reads;
	unsigned int writes;
	unsigned int unlock_writes;
	unsigned int power_writes;
	unsigned int unprotected_power_writes;
	unsigned int final_fifo_reads;
	int lock_error;
} hardware;

static struct spi_controller *registered_controller;
static struct resource adi_resource = { .start = 0x40380000,
					.end = 0x4038ffff };

const void *of_device_get_match_data(const struct device *dev)
{
	return dev->match_data;
}

int of_alias_get_id(struct device_node *node, const char *alias)
{
	assert(!strcmp(alias, "spi"));
	return 0;
}

unsigned int of_get_child_count(struct device_node *node)
{
	return 1;
}

const void *of_get_property(struct device_node *node, const char *name,
			    int *size)
{
	assert(!strcmp(name, "sprd,hw-channels"));
	*size = 0;
	return NULL;
}

bool of_device_is_compatible(const struct device_node *node,
			     const char *compatible)
{
	return node && !strcmp(node->compatible, compatible);
}

u32 be32_to_cpu(__be32 value)
{
	return __builtin_bswap32(value);
}

struct spi_controller *devm_spi_alloc_host(struct device *dev,
					   size_t private_size)
{
	struct spi_controller *controller =
		calloc(1, sizeof(*controller) + private_size);

	assert(controller != NULL);
	controller->private = controller + 1;
	controller->dev.parent = dev;
	return controller;
}

void *spi_controller_get_devdata(struct spi_controller *controller)
{
	return controller->private;
}

void dev_set_drvdata(struct device *dev, void *data)
{
	dev->drvdata = data;
}

void *devm_platform_get_and_ioremap_resource(struct platform_device *pdev,
					     unsigned int index,
					     struct resource **resource)
{
	assert(index == 0);
	*resource = &adi_resource;
	return hardware.mmio;
}

resource_size_t resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

int of_hwspin_lock_get_id(struct device_node *node, unsigned int index)
{
	assert(index == 0);
	return 0;
}

struct hwspinlock *devm_hwspin_lock_request_specific(struct device *dev, int id)
{
	assert(id == 0);
	return &hardware.lock;
}

int devm_spi_register_controller(struct device *dev,
				 struct spi_controller *controller)
{
	assert(registered_controller == NULL);
	registered_controller = controller;
	return 0;
}

struct clk *devm_clk_get_enabled(struct device *dev, const char *name)
{
	assert(!"r3p0 probe must not request the other transport's clock");
	return ERR_PTR(-EIO);
}

int devm_register_restart_handler(struct device *dev,
				  int (*callback)(struct sys_off_data *),
				  void *data)
{
	assert(!"r3p0 probe must not register the other transport's restart handler");
	return -EIO;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}

int hwspin_lock_timeout_irqsave(struct hwspinlock *lock, unsigned int timeout,
				unsigned long *flags)
{
	assert(!hardware.local_irqs_disabled);
	assert(!lock->held);
	*flags = 0;
	hardware.local_irqs_disabled = true;
	lock->held = true;
	return 0;
}

void hwspin_unlock_irqrestore(struct hwspinlock *lock, unsigned long *flags)
{
	assert(lock->held);
	lock->held = false;
	hardware.local_irqs_disabled = !!*flags;
}

int hwspin_lock_timeout_in_atomic(struct hwspinlock *lock, unsigned int timeout)
{
	assert(hardware.local_irqs_disabled && !lock->held);
	assert(timeout > 0 && timeout <= 5);
	if (hardware.lock_error)
		return hardware.lock_error;
	lock->held = true;
	return 0;
}

void hwspin_unlock_in_atomic(struct hwspinlock *lock)
{
	assert(lock->held && hardware.local_irqs_disabled);
	lock->held = false;
}

bool irqs_disabled(void)
{
	return hardware.local_irqs_disabled;
}

u32 readl_relaxed(const void *address)
{
	uintptr_t offset = (uintptr_t)address - (uintptr_t)hardware.mmio;
	u32 reg, value, tag;

	assert(offset < sizeof(hardware.mmio) && !(offset & 3));
	if (offset == 0x30) {
		if (hardware.power_writes) {
			hardware.final_fifo_reads++;
			if (hardware.final_fifo_busy)
				return 0;
		}
		return 0x400;
	}
	if (offset != 0x2c)
		return hardware.mmio[offset / 4];

	assert(hardware.lock.held);
	assert(hardware.read_command >= 0x40388000 &&
	       hardware.read_command < 0x40389000);
	reg = hardware.read_command - 0x40388000;
	assert(!(reg & 3));
	if (reg == 0xefc && hardware.unlock_writes && !hardware.no_unlock_ack &&
	    hardware.unlock_reads++ >= hardware.ack_after) {
		hardware.pmic[0xefc / 4] |= 0x8000;
		hardware.protection_acked = true;
	}
	value = hardware.pmic[reg / 4] & 0xffff;
	/* The fake endpoint returns the r3p0 physical-address echo. */
	tag = ((hardware.read_command & 0xffff) >> 2) << 16;
	if (hardware.bad_readback ||
	    (reg == 0xefc && hardware.bad_unlock_readback))
		tag ^= 0x10000;
	return tag | value;
}

void writel_relaxed(u32 value, void *address)
{
	uintptr_t offset = (uintptr_t)address - (uintptr_t)hardware.mmio;
	u32 reg;

	assert(offset < sizeof(hardware.mmio) && !(offset & 3));
	if (offset == 0x28) {
		assert(hardware.lock.held);
		hardware.read_command = value;
		return;
	}
	if (offset < 0x8000 || offset >= 0x9000) {
		hardware.mmio[offset / 4] = value;
		return;
	}
	assert(hardware.lock.held);
	reg = offset - 0x8000;
	hardware.pmic[reg / 4] = value;
	hardware.writes++;
	if (reg == 0xefc) {
		hardware.unlock_writes++;
		if (value != 0x6e7f)
			hardware.no_unlock_ack = true;
	}
	if (reg == 0xc20) {
		hardware.power_writes++;
		if (!hardware.protection_acked)
			hardware.unprotected_power_writes++;
	}
}

void udelay(unsigned long delay)
{
	assert(delay <= 10000);
}

void mdelay(unsigned long delay)
{
	assert(!"watchdog restart is outside the tested poweroff path");
}

void cpu_relax(void)
{
}

static struct spi_device prepare(struct platform_device *pdev)
{
	static struct device_node adi_node = { .compatible = "sprd,r3p0-adi" };
	static struct device_node pmic_node = { .compatible = "sprd,sc2721" };
	const struct of_device_id *match =
		test_driver_sprd_adi_driver->driver.of_match_table;

	memset(&hardware, 0, sizeof(hardware));
	memset(pdev, 0, sizeof(*pdev));
	hardware.pmic[0xc00 / 4] = 0xa005;
	hardware.pmic[0xc04 / 4] = 0x2721;
	hardware.pmic[0xc20 / 4] = 0x2468;
	hardware.pmic[0xd00 / 4] = 0x1357;
	pdev->dev.of_node = &adi_node;
	for (; match->compatible; match++) {
		if (!strcmp(match->compatible, "sprd,r3p0-adi")) {
			pdev->dev.match_data = match->data;
			break;
		}
	}
	assert(pdev->dev.match_data != NULL);
	assert(test_driver_sprd_adi_driver->probe(pdev) == 0);
	assert(registered_controller == pdev->dev.drvdata);
	hardware.local_irqs_disabled = true;
	return (struct spi_device){
		.dev = { .parent = &registered_controller->dev,
			 .of_node = &pmic_node },
		.controller = registered_controller,
	};
}

static void release(void)
{
	assert(!hardware.lock.held);
	free(registered_controller);
	registered_controller = NULL;
}

static void check_success(void)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);

	hardware.ack_after = 3;
	assert(sprd_adi_sc2721_power_off(&spi) == 0);
	assert(hardware.writes == 2);
	assert(hardware.unlock_writes == 1 && hardware.power_writes == 1);
	assert(hardware.pmic[0xefc / 4] == 0xee7f);
	assert(hardware.pmic[0xc20 / 4] == 1);
	assert(hardware.pmic[0xd00 / 4] == 0x1357);
	assert(hardware.pmic[0xc00 / 4] == 0xa005);
	assert(hardware.final_fifo_reads > 0);
	assert(hardware.unprotected_power_writes == 0);
	assert(hardware.local_irqs_disabled);
	release();
}

static void check_identity(void)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);

	hardware.local_irqs_disabled = false;
	assert(sprd_adi_sc2721_power_off(&spi) == -EOPNOTSUPP);
	assert(hardware.writes == 0);
	hardware.local_irqs_disabled = true;
	hardware.pmic[0xc04 / 4] = 0x2720;
	assert(sprd_adi_sc2721_power_off(&spi) == -ENODEV);
	assert(hardware.writes == 0);
	release();
}

static void check_lock(void)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);

	hardware.lock_error = -ETIMEDOUT;
	assert(sprd_adi_sc2721_power_off(&spi) == -ETIMEDOUT);
	assert(hardware.writes == 0);
	release();
}

static void check_unlock(bool read_error)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);

	hardware.no_unlock_ack = !read_error;
	hardware.bad_unlock_readback = read_error;
	assert(sprd_adi_sc2721_power_off(&spi) ==
	       (read_error ? -EIO : -ETIMEDOUT));
	assert(hardware.writes == 1);
	assert(hardware.unlock_writes == 1 && hardware.power_writes == 0);
	assert(hardware.pmic[0xc20 / 4] == 0x2468);
	assert(hardware.unprotected_power_writes == 0);
	release();
}

static void check_final_fifo(void)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);

	hardware.final_fifo_busy = true;
	assert(sprd_adi_sc2721_power_off(&spi) == -EBUSY);
	assert(hardware.writes == 2);
	assert(hardware.unlock_writes == 1 && hardware.power_writes == 1);
	assert(hardware.pmic[0xc20 / 4] == 1);
	assert(hardware.unprotected_power_writes == 0);
	release();
}

static void check_spi(void)
{
	struct platform_device pdev;
	struct spi_device spi = prepare(&pdev);
	u32 value = 0xc00;
	u32 write[] = { 0xc10, 0x1234 };
	struct spi_transfer transfer = { .rx_buf = &value };

	hardware.local_irqs_disabled = false;
	assert(spi.controller->transfer_one(spi.controller, &spi, &transfer) ==
	       0);
	assert(value == 0xa005);
	transfer = (struct spi_transfer){ .tx_buf = write };
	assert(spi.controller->transfer_one(spi.controller, &spi, &transfer) ==
	       0);
	assert(hardware.pmic[0xc10 / 4] == 0x1234);
	hardware.bad_readback = true;
	value = 0xc00;
	transfer = (struct spi_transfer){ .rx_buf = &value };
	assert(spi.controller->transfer_one(spi.controller, &spi, &transfer) ==
	       -EIO);
	assert(value == 0xc00);
	assert(!hardware.local_irqs_disabled);
	release();
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "success"))
		check_success();
	else if (!strcmp(argv[1], "identity"))
		check_identity();
	else if (!strcmp(argv[1], "lock"))
		check_lock();
	else if (!strcmp(argv[1], "unlock-timeout"))
		check_unlock(false);
	else if (!strcmp(argv[1], "unlock-error"))
		check_unlock(true);
	else if (!strcmp(argv[1], "final-fifo"))
		check_final_fifo();
	else if (!strcmp(argv[1], "spi"))
		check_spi();
	else
		assert(!"unknown poweroff scenario");
	return 0;
}
