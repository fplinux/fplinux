// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nokia TA-1618 UMS9117 fixed-command read-only NANDC driver.
 *
 * The driver deliberately exposes no MTD, SPI, arbitrary-command, program or
 * erase path.  Its physical read-only stream becomes visible only after the
 * NAND identity and feature state have been checked and the controller's owned
 * platform state has been restored.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>

#define TA1618_NANDC_MMIO_BYTES 0x1000U
#define TA1618_NANDC_PIN_WINDOW_BYTES 0x0044U

#define TA1618_NANDC_START 0x000U
#define TA1618_NANDC_CFG0 0x004U
#define TA1618_NANDC_CFG1 0x008U
#define TA1618_NANDC_CFG2 0x00cU
#define TA1618_NANDC_INT 0x010U
#define TA1618_NANDC_TIMING0 0x014U
#define TA1618_NANDC_TIMING1 0x018U
#define TA1618_NANDC_TIMEOUT 0x034U
#define TA1618_NANDC_CFG3 0x038U
#define TA1618_NANDC_AXIM_STS 0x03cU
#define TA1618_NANDC_POLY0 0x0b0U
#define TA1618_NANDC_POLY1 0x0b4U
#define TA1618_NANDC_POLY2 0x0b8U
#define TA1618_NANDC_POLY3 0x0bcU
#define TA1618_NANDC_PHY_CFG 0x0dcU
#define TA1618_NANDC_DLL0 0x0e0U
#define TA1618_NANDC_DLL1 0x0e4U
#define TA1618_NANDC_DLL2 0x0e8U
#define TA1618_NANDC_DLL 0x0ecU
#define TA1618_NANDC_CFG4 0x0f8U
#define TA1618_NANDC_SPI_CLK_CFG 0x180U
#define TA1618_NANDC_SPI_WP_HOLD 0x188U
#define TA1618_NANDC_SPI_STATUS 0x194U
#define TA1618_NANDC_SPI_FEATURE 0x19cU
#define TA1618_NANDC_MAIN_ADDR_HIGH 0x200U
#define TA1618_NANDC_MAIN_ADDR_LOW 0x204U
#define TA1618_NANDC_SPARE_ADDR_HIGH 0x208U
#define TA1618_NANDC_SPARE_ADDR_LOW 0x20cU
#define TA1618_NANDC_STATUS_ADDR_HIGH 0x210U
#define TA1618_NANDC_STATUS_ADDR_LOW 0x214U
#define TA1618_NANDC_INSTRUCTION_RAM 0x220U

#define TA1618_NANDC_AP_AHB_GATE BIT(9)
#define TA1618_NANDC_AP_AHB_RESET BIT(7)
#define TA1618_NANDC_AON_APB_EB0 0x000U
#define TA1618_NANDC_AON_APB_EB2 0x0b0U
#define TA1618_NANDC_AON_APB_SHARED_PIN_GATE0 BIT(20)
#define TA1618_NANDC_AON_APB_SHARED_PIN_GATE2 BIT(12)
#define TA1618_NANDC_AON_APB_ARM_BOOT_MODE 0x02cU
#define TA1618_NANDC_ARM_BOOT_MODE_MASK GENMASK(1, 0)
#define TA1618_NANDC_ARM_BOOT_MODE_EXPECTED 2U

#define TA1618_NANDC_CLOCK_ECC_MASK GENMASK(1, 0)
#define TA1618_NANDC_CLOCK_ECC_VALUE 2U
#define TA1618_NANDC_CLOCK_2X_VALUE 3U
#define TA1618_NANDC_PIN_CONTROL_MASK BIT(12)
#define TA1618_NANDC_PIN_CONTROL_VALUE BIT(12)

#define TA1618_NANDC_CFG0_SPI_ONLY 2U
#define TA1618_NANDC_CFG1_SPI 0x00003000U
#define TA1618_NANDC_CFG4_SPI_ENABLE BIT(1)

#define TA1618_NANDC_INT_DONE_RAW BIT(24)
#define TA1618_NANDC_INT_FATAL_RAW GENMASK(31, 25)
/* The SoC specification defines AUTO_POLL_TIMEOUT at raw 31 / clear 15. */
#define TA1618_NANDC_INT_CLEAR_W1C 0x0000ff00U
#define TA1618_NANDC_SPI_BUSY BIT(1)
#define TA1618_NANDC_WAIT_TIMEOUT_MS 100U
#define TA1618_NANDC_PAGE_READY_TIMEOUT_MS 10U
#define TA1618_NANDC_EXPECTED_ID 0x0000b1a1U
#define TA1618_NANDC_INSTRUCTION_WORDS 8U
#define TA1618_NANDC_PINMUX_COUNT 17U
#define TA1618_NANDC_PINCONF_COUNT 16U
#define TA1618_NANDC_PAGE_BYTES 2048U
#define TA1618_NANDC_OOB_BYTES 128U
#define TA1618_NANDC_PAGE_COUNT 65536U
#define TA1618_NANDC_PHYSICAL_PAGE_BYTES \
	(TA1618_NANDC_PAGE_BYTES + TA1618_NANDC_OOB_BYTES)
#define TA1618_NANDC_RAW_BYTES \
	((u64)TA1618_NANDC_PAGE_COUNT * TA1618_NANDC_PHYSICAL_PAGE_BYTES)
#define TA1618_NANDC_BATCH_BYTES (32U * TA1618_NANDC_PAGE_BYTES)
#define TA1618_NANDC_FEATURE_STATUS_OIP BIT(0)
/* One 2 KiB page is two apart 1 KiB sectors in the NANDC ping-pong RAM. */
#define TA1618_NANDC_DATA_CFG0 0x01000058U
#define TA1618_NANDC_DATA_CFG1 0x000033ffU
#define TA1618_NANDC_DATA_CFG4 0x0000003aU
#define TA1618_NANDC_OOB_CFG0 0x00000058U
#define TA1618_NANDC_OOB_CFG1 0x0000307fU
#define TA1618_NANDC_INVALID_DMA_ADDRESS 0xffffffffU

enum ta1618_nandc_resource {
	TA1618_NANDC_RES_NANDC,
	TA1618_NANDC_RES_GATE_STATE,
	TA1618_NANDC_RES_GATE_SET,
	TA1618_NANDC_RES_GATE_CLEAR,
	TA1618_NANDC_RES_RESET_STATE,
	TA1618_NANDC_RES_RESET_SET,
	TA1618_NANDC_RES_RESET_CLEAR,
	TA1618_NANDC_RES_ECC_CLOCK,
	TA1618_NANDC_RES_2X_CLOCK,
	TA1618_NANDC_RES_PIN_CONTROL,
	TA1618_NANDC_RES_PINMUX,
	TA1618_NANDC_RES_PINCONF,
	TA1618_NANDC_RES_COUNT,
};

enum ta1618_nandc_fixed_operation {
	TA1618_NANDC_FIXED_RESET,
	TA1618_NANDC_FIXED_READ_ID,
	TA1618_NANDC_FIXED_GET_FEATURE,
	TA1618_NANDC_FIXED_PAGE_TO_CACHE,
	TA1618_NANDC_FIXED_READ_CACHE_X1,
	TA1618_NANDC_FIXED_READ_CACHE_OOB,
};

static const char *const ta1618_nandc_resource_names[] = {
	[TA1618_NANDC_RES_NANDC] = "nandc",
	[TA1618_NANDC_RES_GATE_STATE] = "ap-ahb-gate-state",
	[TA1618_NANDC_RES_GATE_SET] = "ap-ahb-gate-set",
	[TA1618_NANDC_RES_GATE_CLEAR] = "ap-ahb-gate-clear",
	[TA1618_NANDC_RES_RESET_STATE] = "ap-ahb-reset-state",
	[TA1618_NANDC_RES_RESET_SET] = "ap-ahb-reset-set",
	[TA1618_NANDC_RES_RESET_CLEAR] = "ap-ahb-reset-clear",
	[TA1618_NANDC_RES_ECC_CLOCK] = "nandc-ecc-clock",
	[TA1618_NANDC_RES_2X_CLOCK] = "nandc-2x-clock",
	[TA1618_NANDC_RES_PIN_CONTROL] = "pin-control",
	[TA1618_NANDC_RES_PINMUX] = "pinmux",
	[TA1618_NANDC_RES_PINCONF] = "pinconf",
};

struct ta1618_nandc_pin {
	u32 offset;
	u32 value;
	u32 snapshot;
};

struct ta1618_nandc {
	struct device *dev;
	struct miscdevice raw_misc;
	struct mutex io_mutex;
	void __iomem *regs[TA1618_NANDC_RES_COUNT];
	struct regmap *aon_apb;
	struct ta1618_nandc_pin *pinmux;
	struct ta1618_nandc_pin *pinconf;
	u32 pin_control_snapshot;
	u32 ecc_clock_snapshot;
	u32 clock_2x_snapshot;
	u32 gate_snapshot;
	u32 reset_snapshot;
	u32 arm_boot_mode;
	u32 last_int_raw;
	u32 last_spi_status;
	u32 last_axim_status;
	u32 id_raw;
	u32 feature_raw[3];
	u32 completed_operations;
	u32 runtime_sessions_completed;
	u64 runtime_pages_completed;
	u32 page_status;
	u32 status_poll_count;
	u32 last_page;
	u8 *staging_buffer;
	void *page_buffer;
	dma_addr_t page_dma;
	int last_error;
	int page_read_error;
	int lifecycle_restore_error;
	enum ta1618_nandc_fixed_operation last_operation;
	u8 last_feature_address;
	bool pin_control_snapshot_valid;
	bool clock_snapshots_valid;
	bool pin_snapshots_valid;
	bool gate_touched;
	bool gate_enabled;
	bool failed;
	bool audit_file_created;
	bool lifecycle_restored;
	bool raw_registered;
	bool io_faulted;
	bool shutting_down;
};

static const u32 ta1618_nandc_pinmux_offsets[] = {
	/* Exact Nokia FDL write order in the 0x402a0154..0x402a0197 window. */
	0x40U, 0x20U, 0x0cU, 0x08U, 0x30U, 0x10U, 0x04U, 0x00U, 0x2cU,
	0x38U, 0x3cU, 0x34U, 0x28U, 0x14U, 0x18U, 0x24U, 0x1cU,
};

static const u32 ta1618_nandc_pinconf_offsets[] = {
	/* Exact Nokia FDL write order in the 0x402a0554..0x402a0597 window. */
	0x40U, 0x0cU, 0x08U, 0x30U, 0x10U, 0x00U, 0x2cU, 0x38U,
	0x3cU, 0x34U, 0x28U, 0x14U, 0x18U, 0x1cU, 0x20U, 0x04U,
};

static void __iomem *ta1618_nandc_map_shared(struct platform_device *pdev,
					     const char *name,
					     resource_size_t minimum_size)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < minimum_size)
		return IOMEM_ERR_PTR(-EINVAL);

	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static int ta1618_nandc_map_resources(struct ta1618_nandc *nandc,
				      struct platform_device *pdev)
{
	struct resource *nandc_resource;
	unsigned int i;

	nandc_resource =
		platform_get_resource_byname(pdev, IORESOURCE_MEM, "nandc");
	if (!nandc_resource ||
	    resource_size(nandc_resource) < TA1618_NANDC_MMIO_BYTES)
		return -EINVAL;
	nandc->regs[TA1618_NANDC_RES_NANDC] =
		devm_platform_ioremap_resource_byname(pdev, "nandc");
	if (IS_ERR(nandc->regs[TA1618_NANDC_RES_NANDC]))
		return PTR_ERR(nandc->regs[TA1618_NANDC_RES_NANDC]);

	for (i = TA1618_NANDC_RES_GATE_STATE; i < TA1618_NANDC_RES_COUNT; i++) {
		resource_size_t minimum_size = sizeof(u32);

		if (i == TA1618_NANDC_RES_PINMUX ||
		    i == TA1618_NANDC_RES_PINCONF)
			minimum_size = TA1618_NANDC_PIN_WINDOW_BYTES;
		nandc->regs[i] = ta1618_nandc_map_shared(
			pdev, ta1618_nandc_resource_names[i], minimum_size);
		if (IS_ERR(nandc->regs[i]))
			return PTR_ERR(nandc->regs[i]);
	}

	return 0;
}

static int ta1618_nandc_parse_pins(struct ta1618_nandc *nandc,
				   struct device_node *np,
				   const char *values_name, const u32 *offsets,
				   struct ta1618_nandc_pin **pins,
				   unsigned int expected_count,
				   void __iomem *window)
{
	struct ta1618_nandc_pin *parsed;
	unsigned int i;
	int count;

	count = of_property_count_u32_elems(np, values_name);
	if (count != expected_count)
		return -EINVAL;

	parsed = devm_kcalloc(nandc->dev, expected_count, sizeof(*parsed),
			      GFP_KERNEL);
	if (!parsed)
		return -ENOMEM;
	for (i = 0; i < expected_count; i++) {
		parsed[i].offset = offsets[i];
		if (of_property_read_u32_index(np, values_name, i,
					       &parsed[i].value))
			return -EINVAL;
		if (!IS_ALIGNED(parsed[i].offset, sizeof(u32)) ||
		    parsed[i].offset >
			    TA1618_NANDC_PIN_WINDOW_BYTES - sizeof(u32))
			return -EINVAL;
		for (unsigned int j = 0; j < i; j++)
			if (parsed[j].offset == parsed[i].offset)
				return -EINVAL;
	}

	/* Keep the resource explicit in this verifier; no derived MMIO endpoint. */
	if (!window)
		return -EINVAL;
	*pins = parsed;
	return 0;
}

static int ta1618_nandc_parse_resources(struct ta1618_nandc *nandc,
					struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	nandc->aon_apb = syscon_regmap_lookup_by_phandle(np, "sprd,aon-apb");
	if (IS_ERR(nandc->aon_apb))
		return PTR_ERR(nandc->aon_apb);

	ret = ta1618_nandc_parse_pins(nandc, np, "sprd,pinmux-values",
				      ta1618_nandc_pinmux_offsets,
				      &nandc->pinmux, TA1618_NANDC_PINMUX_COUNT,
				      nandc->regs[TA1618_NANDC_RES_PINMUX]);
	if (ret)
		return ret;
	return ta1618_nandc_parse_pins(nandc, np, "sprd,pinconf-values",
				       ta1618_nandc_pinconf_offsets,
				       &nandc->pinconf,
				       TA1618_NANDC_PINCONF_COUNT,
				       nandc->regs[TA1618_NANDC_RES_PINCONF]);
}

static int ta1618_nandc_update_shared_pin_gates(struct ta1618_nandc *nandc)
{
	int ret;

	ret = regmap_update_bits(nandc->aon_apb, TA1618_NANDC_AON_APB_EB0,
				 TA1618_NANDC_AON_APB_SHARED_PIN_GATE0,
				 TA1618_NANDC_AON_APB_SHARED_PIN_GATE0);
	if (ret)
		return ret;
	return regmap_update_bits(nandc->aon_apb, TA1618_NANDC_AON_APB_EB2,
				  TA1618_NANDC_AON_APB_SHARED_PIN_GATE2,
				  TA1618_NANDC_AON_APB_SHARED_PIN_GATE2);
}

static int ta1618_nandc_check_arm_boot_mode(struct ta1618_nandc *nandc)
{
	int ret;

	ret = regmap_read(nandc->aon_apb, TA1618_NANDC_AON_APB_ARM_BOOT_MODE,
			  &nandc->arm_boot_mode);
	if (ret)
		return ret;
	if ((nandc->arm_boot_mode & TA1618_NANDC_ARM_BOOT_MODE_MASK) !=
	    TA1618_NANDC_ARM_BOOT_MODE_EXPECTED)
		return -ENODEV;
	return 0;
}

static int ta1618_nandc_apply_pin_control(struct ta1618_nandc *nandc)
{
	void __iomem *pin_control = nandc->regs[TA1618_NANDC_RES_PIN_CONTROL];
	u32 value;

	nandc->pin_control_snapshot = readl(pin_control);
	nandc->pin_control_snapshot_valid = true;
	value = nandc->pin_control_snapshot | TA1618_NANDC_PIN_CONTROL_VALUE;
	writel(value, pin_control);
	return (readl(pin_control) & TA1618_NANDC_PIN_CONTROL_MASK) ==
			       TA1618_NANDC_PIN_CONTROL_VALUE ?
		       0 :
		       -EIO;
}

static int ta1618_nandc_apply_clocks(struct ta1618_nandc *nandc)
{
	void __iomem *ecc_clock = nandc->regs[TA1618_NANDC_RES_ECC_CLOCK];
	void __iomem *clock_2x = nandc->regs[TA1618_NANDC_RES_2X_CLOCK];
	u32 value;

	nandc->ecc_clock_snapshot = readl(ecc_clock);
	nandc->clock_2x_snapshot = readl(clock_2x);
	nandc->clock_snapshots_valid = true;
	writel(TA1618_NANDC_CLOCK_2X_VALUE, clock_2x);
	value = (nandc->ecc_clock_snapshot & ~TA1618_NANDC_CLOCK_ECC_MASK) |
		TA1618_NANDC_CLOCK_ECC_VALUE;
	writel(value, ecc_clock);
	if ((readl(ecc_clock) & TA1618_NANDC_CLOCK_ECC_MASK) !=
		    TA1618_NANDC_CLOCK_ECC_VALUE ||
	    readl(clock_2x) != TA1618_NANDC_CLOCK_2X_VALUE)
		return -EIO;
	return 0;
}

static int ta1618_nandc_apply_pins(struct ta1618_nandc *nandc)
{
	void __iomem *pinmux = nandc->regs[TA1618_NANDC_RES_PINMUX];
	void __iomem *pinconf = nandc->regs[TA1618_NANDC_RES_PINCONF];
	unsigned int i;

	for (i = 0; i < TA1618_NANDC_PINMUX_COUNT; i++)
		nandc->pinmux[i].snapshot =
			readl(pinmux + nandc->pinmux[i].offset);
	for (i = 0; i < TA1618_NANDC_PINCONF_COUNT; i++)
		nandc->pinconf[i].snapshot =
			readl(pinconf + nandc->pinconf[i].offset);
	nandc->pin_snapshots_valid = true;

	for (i = 0; i < TA1618_NANDC_PINMUX_COUNT; i++)
		writel(nandc->pinmux[i].value,
		       pinmux + nandc->pinmux[i].offset);
	for (i = 0; i < TA1618_NANDC_PINCONF_COUNT; i++)
		writel(nandc->pinconf[i].value,
		       pinconf + nandc->pinconf[i].offset);
	for (i = 0; i < TA1618_NANDC_PINMUX_COUNT; i++)
		if (readl(pinmux + nandc->pinmux[i].offset) !=
		    nandc->pinmux[i].value)
			return -EIO;
	for (i = 0; i < TA1618_NANDC_PINCONF_COUNT; i++)
		if (readl(pinconf + nandc->pinconf[i].offset) !=
		    nandc->pinconf[i].value)
			return -EIO;
	return 0;
}

static int ta1618_nandc_assert_reset(struct ta1618_nandc *nandc)
{
	void __iomem *reset_set = nandc->regs[TA1618_NANDC_RES_RESET_SET];
	void __iomem *reset_state = nandc->regs[TA1618_NANDC_RES_RESET_STATE];

	writel(TA1618_NANDC_AP_AHB_RESET, reset_set);
	usleep_range(1000, 2000);
	return readl(reset_state) & TA1618_NANDC_AP_AHB_RESET ? 0 : -EIO;
}

static int ta1618_nandc_controller_reset(struct ta1618_nandc *nandc)
{
	void __iomem *reset_clear = nandc->regs[TA1618_NANDC_RES_RESET_CLEAR];
	void __iomem *reset_state = nandc->regs[TA1618_NANDC_RES_RESET_STATE];
	int ret;

	ret = ta1618_nandc_assert_reset(nandc);
	if (ret)
		return ret;
	writel(TA1618_NANDC_AP_AHB_RESET, reset_clear);
	return readl(reset_state) & TA1618_NANDC_AP_AHB_RESET ? -EIO : 0;
}

static int ta1618_nandc_enable_controller(struct ta1618_nandc *nandc)
{
	void __iomem *gate_state = nandc->regs[TA1618_NANDC_RES_GATE_STATE];
	void __iomem *gate_set = nandc->regs[TA1618_NANDC_RES_GATE_SET];
	void __iomem *reset_state = nandc->regs[TA1618_NANDC_RES_RESET_STATE];
	int ret;

	nandc->gate_snapshot = readl(gate_state);
	nandc->reset_snapshot = readl(reset_state);
	writel(TA1618_NANDC_AP_AHB_GATE, gate_set);
	nandc->gate_touched = true;
	if (!(readl(gate_state) & TA1618_NANDC_AP_AHB_GATE))
		return -EIO;
	nandc->gate_enabled = true;
	ret = ta1618_nandc_controller_reset(nandc);
	if (ret)
		return ret;
	return 0;
}

static void ta1618_nandc_write_instructions(struct ta1618_nandc *nandc,
					    const u16 *instructions,
					    unsigned int instruction_count)
{
	void __iomem *instruction_ram = nandc->regs[TA1618_NANDC_RES_NANDC] +
					TA1618_NANDC_INSTRUCTION_RAM;
	unsigned int i;

	for (i = 0; i < TA1618_NANDC_INSTRUCTION_WORDS / 2; i++)
		writel(0, instruction_ram + i * sizeof(u32));
	for (i = 0; i < instruction_count; i += 2) {
		u32 value = instructions[i];

		if (i + 1 < instruction_count)
			value |= (u32)instructions[i + 1] << 16;
		writel(value, instruction_ram + (i / 2) * sizeof(u32));
	}
	readl(instruction_ram + ((instruction_count - 1) / 2) * sizeof(u32));
}

static void ta1618_nandc_abort_command(struct ta1618_nandc *nandc)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];

	writel(BIT(1), regs + TA1618_NANDC_START);
	readl(regs + TA1618_NANDC_START);
	writel(TA1618_NANDC_INT_CLEAR_W1C, regs + TA1618_NANDC_INT);
	readl(regs + TA1618_NANDC_INT);
}

static int ta1618_nandc_wait(struct ta1618_nandc *nandc)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	unsigned long deadline =
		jiffies + msecs_to_jiffies(TA1618_NANDC_WAIT_TIMEOUT_MS);
	u32 raw;
	u32 spi_status;

	for (;;) {
		raw = readl(regs + TA1618_NANDC_INT);
		spi_status = readl(regs + TA1618_NANDC_SPI_STATUS);
		nandc->last_int_raw = raw;
		nandc->last_spi_status = spi_status;
		nandc->last_axim_status = readl(regs + TA1618_NANDC_AXIM_STS);
		if (raw & TA1618_NANDC_INT_FATAL_RAW) {
			ta1618_nandc_abort_command(nandc);
			return -EIO;
		}
		if ((raw & TA1618_NANDC_INT_DONE_RAW) &&
		    !(spi_status & TA1618_NANDC_SPI_BUSY)) {
			writel(TA1618_NANDC_INT_CLEAR_W1C,
			       regs + TA1618_NANDC_INT);
			readl(regs + TA1618_NANDC_INT);
			return 0;
		}
		if (time_after_eq(jiffies, deadline))
			break;
		usleep_range(50, 100);
	}
	ta1618_nandc_abort_command(nandc);
	return -ETIMEDOUT;
}

static bool ta1618_nandc_feature_address_allowed(u8 address)
{
	return address == 0xa0U || address == 0xb0U || address == 0xc0U;
}

static int ta1618_nandc_run_job(struct ta1618_nandc *nandc,
				const u16 *instructions,
				unsigned int instruction_count)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	u32 raw;
	int ret;

	if (nandc->failed)
		return -EINVAL;
	writel(TA1618_NANDC_INT_CLEAR_W1C, regs + TA1618_NANDC_INT);
	raw = readl(regs + TA1618_NANDC_INT);
	if (raw & (TA1618_NANDC_INT_DONE_RAW | TA1618_NANDC_INT_FATAL_RAW)) {
		nandc->last_int_raw = raw;
		nandc->last_spi_status = readl(regs + TA1618_NANDC_SPI_STATUS);
		nandc->last_axim_status = readl(regs + TA1618_NANDC_AXIM_STS);
		ta1618_nandc_abort_command(nandc);
		return -EIO;
	}

	ta1618_nandc_write_instructions(nandc, instructions, instruction_count);
	/* Publish the instruction RAM before starting the controller. */
	wmb();
	writel(1, regs + TA1618_NANDC_START);
	ret = ta1618_nandc_wait(nandc);
	if (!ret)
		nandc->completed_operations++;
	return ret;
}

static int
ta1618_nandc_execute_fixed(struct ta1618_nandc *nandc,
			   enum ta1618_nandc_fixed_operation operation,
			   u8 feature_address)
{
	static const u16 reset[] = {
		0x00ff, 0xe000, 0x000f, 0x00c0, 0x1000, 0xe001, 0xf000,
	};
	static const u16 read_id[] = {
		0x009f, 0x0000, 0x1000, 0x1000, 0xe000, 0xf000,
	};
	u16 get_feature[] = { 0x000f, 0x0000, 0x1000, 0xe000, 0xf000 };
	const u16 *instructions;
	unsigned int instruction_count;
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];

	if (nandc->failed)
		return -EINVAL;
	switch (operation) {
	case TA1618_NANDC_FIXED_RESET:
		instructions = reset;
		instruction_count = ARRAY_SIZE(reset);
		break;
	case TA1618_NANDC_FIXED_READ_ID:
		instructions = read_id;
		instruction_count = ARRAY_SIZE(read_id);
		break;
	case TA1618_NANDC_FIXED_GET_FEATURE:
		if (!ta1618_nandc_feature_address_allowed(feature_address))
			return -EOPNOTSUPP;
		get_feature[1] = feature_address;
		instructions = get_feature;
		instruction_count = ARRAY_SIZE(get_feature);
		break;
	default:
		return -EOPNOTSUPP;
	}
	nandc->last_operation = operation;
	nandc->last_feature_address = feature_address;

	writel(TA1618_NANDC_CFG0_SPI_ONLY, regs + TA1618_NANDC_CFG0);
	writel(TA1618_NANDC_CFG1_SPI, regs + TA1618_NANDC_CFG1);
	writel(readl(regs + TA1618_NANDC_CFG4) | TA1618_NANDC_CFG4_SPI_ENABLE,
	       regs + TA1618_NANDC_CFG4);
	return ta1618_nandc_run_job(nandc, instructions, instruction_count);
}

static int ta1618_nandc_page_to_cache(struct ta1618_nandc *nandc, u32 page)
{
	u16 instructions[] = {
		0x0013, 0x0000, 0x0000, 0x0000, 0xe000, 0xf000,
	};
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];

	if (page >= TA1618_NANDC_PAGE_COUNT)
		return -ERANGE;
	instructions[2] = (page >> 8) & 0xffU;
	instructions[3] = page & 0xffU;
	nandc->last_operation = TA1618_NANDC_FIXED_PAGE_TO_CACHE;
	nandc->last_feature_address = 0;
	writel(TA1618_NANDC_CFG0_SPI_ONLY, regs + TA1618_NANDC_CFG0);
	writel(TA1618_NANDC_CFG1_SPI, regs + TA1618_NANDC_CFG1);
	writel(readl(regs + TA1618_NANDC_CFG4) | TA1618_NANDC_CFG4_SPI_ENABLE,
	       regs + TA1618_NANDC_CFG4);
	return ta1618_nandc_run_job(nandc, instructions,
				    ARRAY_SIZE(instructions));
}

static int ta1618_nandc_wait_page_ready(struct ta1618_nandc *nandc)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	ktime_t deadline =
		ktime_add_ms(ktime_get(), TA1618_NANDC_PAGE_READY_TIMEOUT_MS);
	u32 status;
	bool final_poll = false;
	int ret;

	for (;;) {
		ret = ta1618_nandc_execute_fixed(
			nandc, TA1618_NANDC_FIXED_GET_FEATURE, 0xc0);
		if (ret)
			return ret;
		status = readl(regs + TA1618_NANDC_SPI_FEATURE);
		nandc->page_status = status;
		nandc->status_poll_count++;
		if (!(status & TA1618_NANDC_FEATURE_STATUS_OIP)) {
			if (status & (BIT(2) | BIT(3)))
				return -EIO;
			return 0;
		}
		if (final_poll)
			return -ETIMEDOUT;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			final_poll = true;
		else
			usleep_range(50, 100);
	}
}

static int ta1618_nandc_read_page_x1(struct ta1618_nandc *nandc)
{
	static const u16 instructions[] = {
		0x000b, 0x0000, 0x0000, 0x6000, 0x3000, 0xe000, 0xf000,
	};
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	int ret;

	if (upper_32_bits(nandc->page_dma) || !IS_ALIGNED(nandc->page_dma, 64U))
		return -ERANGE;
	memset(nandc->page_buffer, 0xa5, TA1618_NANDC_PAGE_BYTES);
	writel(0, regs + TA1618_NANDC_MAIN_ADDR_HIGH);
	writel(lower_32_bits(nandc->page_dma),
	       regs + TA1618_NANDC_MAIN_ADDR_LOW);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_SPARE_ADDR_HIGH);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_SPARE_ADDR_LOW);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_STATUS_ADDR_HIGH);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_STATUS_ADDR_LOW);
	writel(TA1618_NANDC_DATA_CFG0, regs + TA1618_NANDC_CFG0);
	writel(0, regs + TA1618_NANDC_CFG2);
	writel(TA1618_NANDC_DATA_CFG1, regs + TA1618_NANDC_CFG1);
	writel(TA1618_NANDC_DATA_CFG4, regs + TA1618_NANDC_CFG4);
	writel(0, regs + TA1618_NANDC_START);
	nandc->last_operation = TA1618_NANDC_FIXED_READ_CACHE_X1;
	nandc->last_feature_address = 0;
	dma_wmb();
	ret = ta1618_nandc_run_job(nandc, instructions,
				   ARRAY_SIZE(instructions));
	if (ret)
		return ret;
	dma_rmb();
	if (nandc->last_axim_status)
		return -EIO;
	return 0;
}

static int ta1618_nandc_read_oob_cache(struct ta1618_nandc *nandc)
{
	static const u16 instructions[] = {
		0x000b, 0x0008, 0x0000, 0x6000, 0x3000, 0xe000, 0xf000,
	};
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	int ret;

	if (upper_32_bits(nandc->page_dma) || !IS_ALIGNED(nandc->page_dma, 64U))
		return -ERANGE;
	memset(nandc->page_buffer, 0xa5, TA1618_NANDC_OOB_BYTES);
	writel(0, regs + TA1618_NANDC_MAIN_ADDR_HIGH);
	writel(lower_32_bits(nandc->page_dma),
	       regs + TA1618_NANDC_MAIN_ADDR_LOW);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_SPARE_ADDR_HIGH);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_SPARE_ADDR_LOW);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_STATUS_ADDR_HIGH);
	writel(TA1618_NANDC_INVALID_DMA_ADDRESS,
	       regs + TA1618_NANDC_STATUS_ADDR_LOW);
	writel(TA1618_NANDC_OOB_CFG0, regs + TA1618_NANDC_CFG0);
	writel(0, regs + TA1618_NANDC_CFG2);
	writel(TA1618_NANDC_OOB_CFG1, regs + TA1618_NANDC_CFG1);
	writel(TA1618_NANDC_DATA_CFG4, regs + TA1618_NANDC_CFG4);
	writel(0, regs + TA1618_NANDC_START);
	nandc->last_operation = TA1618_NANDC_FIXED_READ_CACHE_OOB;
	nandc->last_feature_address = 0;
	dma_wmb();
	ret = ta1618_nandc_run_job(nandc, instructions,
				   ARRAY_SIZE(instructions));
	if (ret)
		return ret;
	dma_rmb();
	return nandc->last_axim_status ? -EIO : 0;
}

static int ta1618_nandc_read_raw_slice(struct ta1618_nandc *nandc, u32 page,
				       size_t offset, size_t length,
				       u8 *destination)
{
	size_t end;
	size_t main_end;
	size_t oob_start;
	int ret;

	if (page >= TA1618_NANDC_PAGE_COUNT ||
	    offset >= TA1618_NANDC_PHYSICAL_PAGE_BYTES ||
	    length > TA1618_NANDC_PHYSICAL_PAGE_BYTES - offset)
		return -ERANGE;
	end = offset + length;
	nandc->last_page = page;
	ret = ta1618_nandc_page_to_cache(nandc, page);
	if (ret)
		return ret;
	ret = ta1618_nandc_wait_page_ready(nandc);
	if (ret)
		return ret;
	ret = ta1618_nandc_read_page_x1(nandc);
	if (ret)
		return ret;

	main_end = min_t(size_t, end, TA1618_NANDC_PAGE_BYTES);
	if (offset < main_end)
		memcpy(destination, nandc->page_buffer + offset,
		       main_end - offset);

	ret = ta1618_nandc_read_oob_cache(nandc);
	if (ret)
		return ret;
	oob_start = max_t(size_t, offset, TA1618_NANDC_PAGE_BYTES);
	if (oob_start < end)
		memcpy(destination + oob_start - offset,
		       nandc->page_buffer + oob_start - TA1618_NANDC_PAGE_BYTES,
		       end - oob_start);
	return 0;
}

static int ta1618_nandc_apply_fdl_recipe(struct ta1618_nandc *nandc)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];

	writel(0x3a493146U, regs + TA1618_NANDC_TIMING0);
	writel(0x192a0000U, regs + TA1618_NANDC_TIMING1);
	writel(0x81000000U, regs + TA1618_NANDC_TIMEOUT);
	writel(0x0003000cU, regs + TA1618_NANDC_CFG3);
	writel(0x00001004U, regs + TA1618_NANDC_POLY0);
	writel(0x00001004U, regs + TA1618_NANDC_POLY1);
	writel(0x00004013U, regs + TA1618_NANDC_POLY2);
	writel(0x00400010U, regs + TA1618_NANDC_POLY3);
	writel(6, regs + TA1618_NANDC_PHY_CFG);
	writel(0x00000100U, regs + TA1618_NANDC_DLL0);
	writel(0x00000100U, regs + TA1618_NANDC_DLL1);
	writel(0x000000bfU, regs + TA1618_NANDC_DLL2);
	writel(0x00000c80U, regs + TA1618_NANDC_DLL);
	writel(0x00000680U, regs + TA1618_NANDC_SPI_CLK_CFG);
	writel(3, regs + TA1618_NANDC_SPI_WP_HOLD);
	writel(0x0000548aU, regs + 0x12cU);
	writel(TA1618_NANDC_CFG0_SPI_ONLY, regs + TA1618_NANDC_CFG0);
	writel(TA1618_NANDC_CFG1_SPI, regs + TA1618_NANDC_CFG1);
	writel(0x00040002U, regs + TA1618_NANDC_DLL);
	writel(readl(regs + TA1618_NANDC_CFG4) | TA1618_NANDC_CFG4_SPI_ENABLE,
	       regs + TA1618_NANDC_CFG4);
	readl(regs + TA1618_NANDC_CFG4);
	return 0;
}

static int ta1618_nandc_fail(struct ta1618_nandc *nandc, int error,
			     const char *stage)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	u32 int_raw;
	u32 axim_sts;
	u32 spi_status;
	u32 feature;
	u32 start;
	u32 cfg0;
	u32 cfg1;
	u32 cfg4;
	int reset_error;

	nandc->failed = true;
	nandc->last_error = error;
	int_raw = readl(regs + TA1618_NANDC_INT);
	axim_sts = readl(regs + TA1618_NANDC_AXIM_STS);
	spi_status = readl(regs + TA1618_NANDC_SPI_STATUS);
	feature = readl(regs + TA1618_NANDC_SPI_FEATURE);
	start = readl(regs + TA1618_NANDC_START);
	cfg0 = readl(regs + TA1618_NANDC_CFG0);
	cfg1 = readl(regs + TA1618_NANDC_CFG1);
	cfg4 = readl(regs + TA1618_NANDC_CFG4);
	ta1618_nandc_abort_command(nandc);
	reset_error = ta1618_nandc_assert_reset(nandc);
	dev_err(nandc->dev,
		"%s failed: %d page=%u operation=%u feature_address=0x%02x completed=%u\n",
		stage, error, nandc->last_page, nandc->last_operation,
		nandc->last_feature_address, nandc->completed_operations);
	dev_err(nandc->dev,
		"observed_int=0x%08x observed_axim=0x%08x observed_spi=0x%08x\n",
		nandc->last_int_raw, nandc->last_axim_status,
		nandc->last_spi_status);
	dev_err(nandc->dev, "post_abort_int=0x%08x axim_sts=0x%08x\n", int_raw,
		axim_sts);
	dev_err(nandc->dev, "spi_status=0x%08x feature=0x%08x start=0x%08x\n",
		spi_status, feature, start);
	dev_err(nandc->dev,
		"cfg0=0x%08x cfg1=0x%08x cfg4=0x%08x reset_error=%d\n", cfg0,
		cfg1, cfg4, reset_error);
	if (reset_error)
		dev_err(nandc->dev,
			"NANDC reset did not assert after failure\n");
	return error;
}

static int ta1618_nandc_run_diagnostics(struct ta1618_nandc *nandc)
{
	static const u8 feature_addresses[] = { 0xa0, 0xb0, 0xc0 };
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	unsigned int i;
	int ret;

	ret = ta1618_nandc_execute_fixed(nandc, TA1618_NANDC_FIXED_RESET, 0);
	if (ret)
		return ta1618_nandc_fail(nandc, ret, "fixed NAND reset");

	ret = ta1618_nandc_execute_fixed(nandc, TA1618_NANDC_FIXED_READ_ID, 0);
	if (ret)
		return ta1618_nandc_fail(nandc, ret, "fixed NAND read-id");
	nandc->id_raw = readl(regs + TA1618_NANDC_SPI_FEATURE);
	if (nandc->id_raw != TA1618_NANDC_EXPECTED_ID)
		return ta1618_nandc_fail(nandc, -ENODEV, "unexpected NAND id");

	for (i = 0; i < ARRAY_SIZE(feature_addresses); i++) {
		ret = ta1618_nandc_execute_fixed(nandc,
						 TA1618_NANDC_FIXED_GET_FEATURE,
						 feature_addresses[i]);
		if (ret)
			return ta1618_nandc_fail(nandc, ret,
						 "fixed NAND get-feature");
		nandc->feature_raw[i] = readl(regs + TA1618_NANDC_SPI_FEATURE);
	}
	if ((nandc->feature_raw[1] & 0xffU) != 0 ||
	    (nandc->feature_raw[2] & 0xffU) != 0)
		return ta1618_nandc_fail(nandc, -EUCLEAN,
					 "unexpected NAND feature state");
	return 0;
}

static void ta1618_nandc_begin_session(struct ta1618_nandc *nandc)
{
	nandc->pin_control_snapshot_valid = false;
	nandc->clock_snapshots_valid = false;
	nandc->pin_snapshots_valid = false;
	nandc->gate_touched = false;
	nandc->gate_enabled = false;
	nandc->failed = false;
	nandc->lifecycle_restored = false;
	nandc->last_error = 0;
	nandc->page_read_error = 0;
	nandc->lifecycle_restore_error = 0;
	nandc->last_int_raw = 0;
	nandc->last_spi_status = 0;
	nandc->last_axim_status = 0;
	nandc->status_poll_count = 0;
}

static int ta1618_nandc_activate(struct ta1618_nandc *nandc)
{
	int ret;

	ta1618_nandc_begin_session(nandc);
	ret = ta1618_nandc_update_shared_pin_gates(nandc);
	if (ret)
		return ret;
	ret = ta1618_nandc_apply_pin_control(nandc);
	if (ret)
		return ret;
	ret = ta1618_nandc_apply_pins(nandc);
	if (ret)
		return ret;
	ret = ta1618_nandc_apply_clocks(nandc);
	if (ret)
		return ret;
	ret = ta1618_nandc_enable_controller(nandc);
	if (ret) {
		if (nandc->gate_enabled)
			return ta1618_nandc_fail(nandc, ret,
						 "NANDC enable/reset");
		return ret;
	}
	ret = ta1618_nandc_apply_fdl_recipe(nandc);
	if (ret)
		return ta1618_nandc_fail(nandc, ret, "NANDC recipe");
	return ta1618_nandc_run_diagnostics(nandc);
}

static int ta1618_nandc_restore_lifecycle(struct ta1618_nandc *nandc)
{
	void __iomem *regs = nandc->regs[TA1618_NANDC_RES_NANDC];
	void __iomem *gate_state = nandc->regs[TA1618_NANDC_RES_GATE_STATE];
	void __iomem *gate_set = nandc->regs[TA1618_NANDC_RES_GATE_SET];
	void __iomem *gate_clear = nandc->regs[TA1618_NANDC_RES_GATE_CLEAR];
	void __iomem *reset_state = nandc->regs[TA1618_NANDC_RES_RESET_STATE];
	void __iomem *reset_set = nandc->regs[TA1618_NANDC_RES_RESET_SET];
	void __iomem *reset_clear = nandc->regs[TA1618_NANDC_RES_RESET_CLEAR];
	void __iomem *pin_control = nandc->regs[TA1618_NANDC_RES_PIN_CONTROL];
	void __iomem *ecc_clock = nandc->regs[TA1618_NANDC_RES_ECC_CLOCK];
	void __iomem *clock_2x = nandc->regs[TA1618_NANDC_RES_2X_CLOCK];
	void __iomem *pinmux = nandc->regs[TA1618_NANDC_RES_PINMUX];
	void __iomem *pinconf = nandc->regs[TA1618_NANDC_RES_PINCONF];
	u32 value;
	unsigned int i;
	int ret = 0;

	if (nandc->lifecycle_restored)
		return 0;
	if (nandc->gate_touched) {
		if (nandc->gate_enabled)
			ta1618_nandc_abort_command(nandc);
		if (ta1618_nandc_assert_reset(nandc))
			ret = -EIO;
		if (nandc->gate_enabled &&
		    readl(regs + TA1618_NANDC_SPI_STATUS) &
			    TA1618_NANDC_SPI_BUSY &&
		    !ret)
			ret = -EBUSY;
		if (nandc->gate_enabled) {
			nandc->last_axim_status =
				readl(regs + TA1618_NANDC_AXIM_STS);
			if (nandc->last_axim_status && !ret)
				ret = -EIO;
		}
		writel(TA1618_NANDC_AP_AHB_GATE, gate_clear);
		value = readl(gate_state);
		if (value & TA1618_NANDC_AP_AHB_GATE && !ret)
			ret = -EIO;
	}
	if (nandc->pin_snapshots_valid) {
		for (i = 0; i < TA1618_NANDC_PINCONF_COUNT; i++) {
			writel(nandc->pinconf[i].snapshot,
			       pinconf + nandc->pinconf[i].offset);
			if (readl(pinconf + nandc->pinconf[i].offset) !=
				    nandc->pinconf[i].snapshot &&
			    !ret)
				ret = -EIO;
		}
		for (i = 0; i < TA1618_NANDC_PINMUX_COUNT; i++) {
			writel(nandc->pinmux[i].snapshot,
			       pinmux + nandc->pinmux[i].offset);
			if (readl(pinmux + nandc->pinmux[i].offset) !=
				    nandc->pinmux[i].snapshot &&
			    !ret)
				ret = -EIO;
		}
	}
	if (nandc->clock_snapshots_valid) {
		writel(nandc->ecc_clock_snapshot, ecc_clock);
		writel(nandc->clock_2x_snapshot, clock_2x);
		if ((readl(ecc_clock) != nandc->ecc_clock_snapshot ||
		     readl(clock_2x) != nandc->clock_2x_snapshot) &&
		    !ret)
			ret = -EIO;
	}
	if (nandc->pin_control_snapshot_valid) {
		writel(nandc->pin_control_snapshot, pin_control);
		if (readl(pin_control) != nandc->pin_control_snapshot && !ret)
			ret = -EIO;
	}
	if (ret) {
		nandc->lifecycle_restore_error = ret;
		return ret;
	}

	if (nandc->gate_touched) {
		if (nandc->reset_snapshot & TA1618_NANDC_AP_AHB_RESET)
			writel(TA1618_NANDC_AP_AHB_RESET, reset_set);
		else
			writel(TA1618_NANDC_AP_AHB_RESET, reset_clear);
		value = readl(reset_state);
		if (!!(value & TA1618_NANDC_AP_AHB_RESET) !=
			    !!(nandc->reset_snapshot &
			       TA1618_NANDC_AP_AHB_RESET) &&
		    !ret)
			ret = -EIO;
		if (ret) {
			nandc->lifecycle_restore_error = ret;
			return ret;
		}

		if (nandc->gate_snapshot & TA1618_NANDC_AP_AHB_GATE)
			writel(TA1618_NANDC_AP_AHB_GATE, gate_set);
		else
			writel(TA1618_NANDC_AP_AHB_GATE, gate_clear);
		value = readl(gate_state);
		if (!!(value & TA1618_NANDC_AP_AHB_GATE) !=
			    !!(nandc->gate_snapshot &
			       TA1618_NANDC_AP_AHB_GATE) &&
		    !ret)
			ret = -EIO;
	}
	/*
	 * AON gate bits at 0x000 and 0x0b0 are shared.  They are intentionally
	 * never cleared here, including on probe failure.
	 */
	nandc->lifecycle_restore_error = ret;
	if (!ret)
		nandc->lifecycle_restored = true;
	return ret;
}

static void ta1618_nandc_release_buffers(struct ta1618_nandc *nandc)
{
	if (nandc->page_buffer) {
		dma_free_coherent(nandc->dev, TA1618_NANDC_PAGE_BYTES,
				  nandc->page_buffer, nandc->page_dma);
		nandc->page_buffer = NULL;
	}
	kvfree(nandc->staging_buffer);
	nandc->staging_buffer = NULL;
}

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ta1618_nandc *nandc = dev_get_drvdata(dev);

	return sysfs_emit(
		buf,
		"state=%s read_only=1\n"
		"arm_boot_mode=0x%08x id_raw=0x%08x\n"
		"last_error=%d total_commands=%u\n"
		"feature_a0=0x%08x feature_b0=0x%08x feature_c0=0x%08x\n"
		"int_raw=0x%08x spi_status=0x%08x axim_status=0x%08x\n"
		"read_error=%d status_polls=%u page_status=0x%08x\n"
		"last_page=%u internal_ecc_enabled=0\n"
		"raw_registered=%u io_faulted=%u runtime_sessions=%u runtime_pages=%llu\n"
		"lifecycle_restored=%u lifecycle_restore_error=%d\n"
		"shared_aon_pin_gates_retained=1\n",
		nandc->failed ? "failed" : "probe-pass", nandc->arm_boot_mode,
		nandc->id_raw, nandc->last_error, nandc->completed_operations,
		nandc->feature_raw[0], nandc->feature_raw[1],
		nandc->feature_raw[2], nandc->last_int_raw,
		nandc->last_spi_status, nandc->last_axim_status,
		nandc->page_read_error, nandc->status_poll_count,
		nandc->page_status, nandc->last_page, nandc->raw_registered,
		nandc->io_faulted, nandc->runtime_sessions_completed,
		nandc->runtime_pages_completed, nandc->lifecycle_restored,
		nandc->lifecycle_restore_error);
}
static DEVICE_ATTR_RO(audit);

static int ta1618_nandc_raw_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ta1618_nandc *nandc =
		container_of(misc, struct ta1618_nandc, raw_misc);

	if (file->f_mode & FMODE_WRITE)
		return -EROFS;
	if (READ_ONCE(nandc->shutting_down))
		return -ENODEV;
	if (READ_ONCE(nandc->io_faulted))
		return -EIO;
	file->private_data = nandc;
	return 0;
}

static loff_t ta1618_nandc_raw_llseek(struct file *file, loff_t offset,
				      int whence)
{
	return fixed_size_llseek(file, offset, whence, TA1618_NANDC_RAW_BYTES);
}

static ssize_t ta1618_nandc_raw_read_iter(struct kiocb *iocb,
					  struct iov_iter *to)
{
	struct ta1618_nandc *nandc = iocb->ki_filp->private_data;
	loff_t position = iocb->ki_pos;
	loff_t read_position = position;
	size_t requested;
	size_t staged = 0;
	size_t copied;
	u32 pages = 0;
	bool hardware_error = false;
	bool session_active = false;
	int cleanup_ret;
	int ret = 0;

	if (!iov_iter_count(to))
		return 0;
	if (position < 0)
		return -EINVAL;
	if (position >= TA1618_NANDC_RAW_BYTES)
		return 0;
	requested = min_t(size_t, iov_iter_count(to), TA1618_NANDC_BATCH_BYTES);
	if (requested > TA1618_NANDC_RAW_BYTES - position)
		requested = TA1618_NANDC_RAW_BYTES - position;
	if (mutex_lock_interruptible(&nandc->io_mutex))
		return -ERESTARTSYS;
	if (nandc->shutting_down) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (nandc->io_faulted) {
		ret = -EIO;
		goto out_unlock;
	}
	if (signal_pending(current)) {
		ret = -ERESTARTSYS;
		goto out_unlock;
	}

	ret = ta1618_nandc_activate(nandc);
	session_active = true;
	if (ret) {
		hardware_error = true;
		goto out_cleanup;
	}
	while (staged < requested) {
		u32 page_offset;
		u32 page = div_u64_rem((u64)read_position,
				       TA1618_NANDC_PHYSICAL_PAGE_BYTES,
				       &page_offset);
		size_t chunk =
			min_t(size_t, requested - staged,
			      TA1618_NANDC_PHYSICAL_PAGE_BYTES - page_offset);

		if (READ_ONCE(nandc->shutting_down)) {
			ret = -ENODEV;
			break;
		}
		if (signal_pending(current)) {
			if (!staged)
				ret = -ERESTARTSYS;
			break;
		}
		ret = ta1618_nandc_read_raw_slice(
			nandc, page, page_offset, chunk,
			nandc->staging_buffer + staged);
		if (ret) {
			nandc->page_read_error = ret;
			ret = ta1618_nandc_fail(nandc, ret, "NAND raw read");
			hardware_error = true;
			break;
		}
		staged += chunk;
		read_position += chunk;
		pages++;
	}

out_cleanup:
	cleanup_ret = session_active ? ta1618_nandc_restore_lifecycle(nandc) :
				       0;
	if (cleanup_ret) {
		nandc->io_faulted = true;
		ret = cleanup_ret;
	}
	if (hardware_error)
		nandc->io_faulted = true;
	if (session_active && !hardware_error && !cleanup_ret && pages) {
		nandc->runtime_sessions_completed++;
		nandc->runtime_pages_completed += pages;
	}
	if (ret)
		goto out_unlock;
	copied = copy_to_iter(nandc->staging_buffer, staged, to);
	if (!copied) {
		ret = -EFAULT;
		goto out_unlock;
	}
	position += copied;
	iocb->ki_pos = position;
	ret = copied;
out_unlock:
	mutex_unlock(&nandc->io_mutex);
	return ret;
}

static const struct file_operations ta1618_nandc_raw_fops = {
	.owner = THIS_MODULE,
	.open = ta1618_nandc_raw_open,
	.read_iter = ta1618_nandc_raw_read_iter,
	.llseek = ta1618_nandc_raw_llseek,
};

static int ta1618_nandc_probe(struct platform_device *pdev)
{
	struct ta1618_nandc *nandc;
	int restore_ret;
	int ret;

	nandc = devm_kzalloc(&pdev->dev, sizeof(*nandc), GFP_KERNEL);
	if (!nandc)
		return -ENOMEM;
	nandc->dev = &pdev->dev;
	mutex_init(&nandc->io_mutex);
	platform_set_drvdata(pdev, nandc);

	ret = ta1618_nandc_map_resources(nandc, pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid NANDC resources\n");
	ret = ta1618_nandc_parse_resources(nandc, pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "invalid NANDC DT data\n");
	ret = ta1618_nandc_check_arm_boot_mode(nandc);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unexpected ARM boot mode sentinel\n");
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "NANDC requires 32-bit coherent DMA\n");
	nandc->page_buffer = dma_alloc_coherent(&pdev->dev,
						TA1618_NANDC_PAGE_BYTES,
						&nandc->page_dma, GFP_KERNEL);
	if (!nandc->page_buffer)
		return -ENOMEM;
	if (upper_32_bits(nandc->page_dma) ||
	    !IS_ALIGNED(nandc->page_dma, 64U)) {
		ret = -ERANGE;
		goto out_release_page;
	}
	nandc->staging_buffer = kvzalloc(TA1618_NANDC_BATCH_BYTES, GFP_KERNEL);
	if (!nandc->staging_buffer) {
		ret = -ENOMEM;
		goto out_release_page;
	}
	ret = ta1618_nandc_activate(nandc);
	if (ret)
		goto out_restore;
	ret = ta1618_nandc_restore_lifecycle(nandc);
	if (ret) {
		nandc->failed = true;
		nandc->last_error = ret;
		dev_err(&pdev->dev,
			"NANDC baseline restore failed after identification: %d; "
			"retaining coherent buffer\n",
			ret);
		return ret;
	}
	ret = device_create_file(&pdev->dev, &dev_attr_audit);
	if (ret) {
		nandc->failed = true;
		nandc->last_error = ret;
		dev_err_probe(&pdev->dev, ret,
			      "could not create NANDC audit file\n");
		goto out_release_page;
	}
	nandc->audit_file_created = true;
	nandc->raw_misc.minor = MISC_DYNAMIC_MINOR;
	nandc->raw_misc.name = "ta1618-nand-raw";
	nandc->raw_misc.fops = &ta1618_nandc_raw_fops;
	nandc->raw_misc.parent = &pdev->dev;
	nandc->raw_misc.mode = 0444;
	ret = misc_register(&nandc->raw_misc);
	if (ret) {
		device_remove_file(&pdev->dev, &dev_attr_audit);
		nandc->audit_file_created = false;
		nandc->failed = true;
		nandc->last_error = ret;
		dev_err_probe(&pdev->dev, ret,
			      "could not register NAND raw device\n");
		goto out_release_page;
	}
	nandc->raw_registered = true;
	dev_notice(&pdev->dev, "read-only physical NAND stream ready\n");
	return 0;

out_restore:
	restore_ret = ta1618_nandc_restore_lifecycle(nandc);
	if (restore_ret) {
		dev_err(&pdev->dev,
			"NANDC baseline restore failed: %d; retaining coherent buffer\n",
			restore_ret);
		if (!ret)
			ret = restore_ret;
		return ret;
	}
	ta1618_nandc_release_buffers(nandc);
	return ret;

out_release_page:
	ta1618_nandc_release_buffers(nandc);
	return ret;
}

static void ta1618_nandc_remove(struct platform_device *pdev)
{
	struct ta1618_nandc *nandc = platform_get_drvdata(pdev);
	int ret;

	WRITE_ONCE(nandc->shutting_down, true);
	if (nandc->raw_registered) {
		misc_deregister(&nandc->raw_misc);
		nandc->raw_registered = false;
	}
	if (nandc->audit_file_created)
		device_remove_file(&pdev->dev, &dev_attr_audit);
	mutex_lock(&nandc->io_mutex);
	ret = ta1618_nandc_restore_lifecycle(nandc);
	if (ret) {
		mutex_unlock(&nandc->io_mutex);
		dev_err(&pdev->dev,
			"NANDC baseline restore failed: %d; retaining coherent buffer\n",
			ret);
		return;
	}
	ta1618_nandc_release_buffers(nandc);
	mutex_unlock(&nandc->io_mutex);
}

static void ta1618_nandc_shutdown(struct platform_device *pdev)
{
	ta1618_nandc_remove(pdev);
}

static const struct of_device_id ta1618_nandc_of_match[] = {
	{ .compatible = "fplinux,ta1618-nandc-ro" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_nandc_of_match);

static struct platform_driver ta1618_nandc_driver = {
	.probe = ta1618_nandc_probe,
	.remove = ta1618_nandc_remove,
	.shutdown = ta1618_nandc_shutdown,
	.driver = {
		.name = "ta1618-nandc-ro",
		.of_match_table = ta1618_nandc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ta1618_nandc_driver);

MODULE_DESCRIPTION("Nokia TA-1618 fixed-command physical NAND reader");
MODULE_LICENSE("GPL");
