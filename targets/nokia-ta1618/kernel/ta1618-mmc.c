// SPDX-License-Identifier: GPL-2.0-only
/*
 * Single-block 4-bit 13 MHz Linux MMC host for the UMS9117 SDIO0 instance in
 * Nokia TA-1618, driving the removable microSD slot.
 *
 * This is deliberately not an SDHCI driver.  UMS9117 has a related command,
 * response, interrupt and ADMA layout, but +0x28 is a 32-bit custom host
 * control register and there is no SDHCI POWER_CONTROL byte.  Platform gate,
 * reset, selector, pin and ADI/PMIC sequencing is fixed to the hardware-proven
 * TA-1618 SDIO0 recipe.
 *
 * Identification starts in the proven 1-bit selector-1/divider-0x2100 state at
 * 393939 Hz.  A 13 MHz request is deferred physically while the bus is 1-bit,
 * so Linux MMC core performs CMD55 and ACMD6 at the proven low clock.  Only a
 * clean RCA-scoped CMD55 APP_CMD response and clean no-data ACMD6 argument 2
 * permit one atomic custom transition to 4-bit HOST_CONTROL1 0x12 and the
 * selector-1/divider-0x0100 13 MHz clock.  No high-speed, UHS, 1.8 V, tuning or
 * CMD23 capability exists.  Data I/O remains one ADMA2 segment and one 512-byte
 * CMD17 or one 512-byte CMD24.  CMD18, CMD25, erase, discard and lock are
 * rejected before controller MMIO.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define TA1618_REG_SIZE 4U
#define TA1618_ADI_SIZE 0x0400U
#define TA1618_ANALOG_SIZE 0x1000U

#define TA1618_GATE_MASK BIT(7)
#define TA1618_AP_RESET_MASK BIT(11)
#define TA1618_SELECTOR_MASK GENMASK(2, 0)
#define TA1618_SELECTOR_26MHZ 0x00000001U

#define TA1618_PIN_COUNT 12U
#define TA1618_PIN_FUNC_MASK GENMASK(5, 4)
#define TA1618_DATA_PAD_TARGET 0x00182084U
#define TA1618_CMD_PAD_TARGET TA1618_DATA_PAD_TARGET
#define TA1618_D0_PAD_TARGET TA1618_DATA_PAD_TARGET
#define TA1618_D1_PAD_TARGET TA1618_DATA_PAD_TARGET
#define TA1618_D2_PAD_TARGET TA1618_DATA_PAD_TARGET
#define TA1618_D3_PAD_TARGET TA1618_DATA_PAD_TARGET
#define TA1618_CLK_PAD_TARGET 0x00302001U
#define TA1618_IOS_TRACE_DEPTH 16U

/* Shared with the keypad through the hardware ADI_USER_LOCK semaphore. */
#define TA1618_ADI_VERSION 0x0000U
#define TA1618_ADI_MST_CTL 0x0004U
#define TA1618_ADI_INT_RAW 0x0014U
#define TA1618_ADI_RD_CMD 0x0028U
#define TA1618_ADI_RD_DATA 0x002cU
#define TA1618_ADI_FIFO_STS 0x0030U
#define TA1618_ADI_USER_LOCK 0x0224U
#define TA1618_ADI_EXPECTED_VERSION 0x00000400U
#define TA1618_ADI_EXPECTED_MST_CTL 0x00000000U
#define TA1618_ADI_RD_BUSY BIT(31)
#define TA1618_ADI_RD_RETURNED_ADDRESS GENMASK(30, 16)
#define TA1618_ADI_FIFO_FULL BIT(11)
#define TA1618_ADI_FIFO_EMPTY BIT(10)
#define TA1618_ADI_ARM_FIFO_OVERFLOW_RAW BIT(3)
#define TA1618_ADI_USER_LOCK_RELEASE 0x5348554cU
#define TA1618_ADI_POLL_BUDGET_US 3000U

#define TA1618_PD_MASK BIT(0)
#define TA1618_VDDSDCORE_PD_OFFSET 0x0d00U
#define TA1618_VDDSDCORE_VOLT_OFFSET 0x0d04U
#define TA1618_VDDSDIO_PD_OFFSET 0x0d0cU
#define TA1618_VDDSDIO_VOLT_OFFSET 0x0d10U
#define TA1618_VDDSDCORE_VOLT_EXPECTED 0x0050U
#define TA1618_VDDSDIO_VOLT_EXPECTED 0x006fU
#define TA1618_RAIL_DELAY_MS 300U

#define UMS9117_HOST_RESET BIT(24)
#define UMS9117_CLOCK_DIVIDER_MASK 0x0000ffc0U
#define UMS9117_CLOCK_TIMEOUT_MASK GENMASK(19, 16)
#define UMS9117_CLOCK_TIMEOUT_SHIFT 16U
#define UMS9117_CLOCK_TIMEOUT_FPDOOM_VALUE 8U
#define UMS9117_CLOCK_TIMEOUT_FPDOOM_ENCODED \
	(UMS9117_CLOCK_TIMEOUT_FPDOOM_VALUE << UMS9117_CLOCK_TIMEOUT_SHIFT)
#define UMS9117_CLOCK_PROG_MODE 0x00000020U
#define UMS9117_CLOCK_PLL_EN 0x00000008U
#define UMS9117_CLOCK_CARD_EN 0x00000004U
#define UMS9117_CLOCK_INT_STABLE 0x00000002U
#define UMS9117_CLOCK_INT_EN 0x00000001U
#define UMS9117_CLOCK_VOLATILE_MASK \
	(UMS9117_HOST_RESET | UMS9117_CLOCK_INT_STABLE)
#define UMS9117_DIVIDER_ENCODED 0x00002100U
#define UMS9117_CLOCK_HZ 393939U
#define UMS9117_IDENT_REQUEST_MAX_HZ 400000U
#define UMS9117_13MHZ_DIVIDER_ENCODED 0x00000100U
#define UMS9117_13MHZ_OPERATIONAL_CLOCK_EXPECTED 0x08080107U
#define UMS9117_MAX_CLOCK_HZ 13000000U
#define UMS9117_HOST_VERSION_MASK 0xfffffffeU
#define UMS9117_HOST_VERSION_EXPECTED 0x00040000U

#define UMS9117_CMD_INHIBIT BIT(0)
#define UMS9117_DAT_INHIBIT BIT(1)
#define UMS9117_DAT0_LEVEL BIT(20)
#define UMS9117_DAT_LINE_ACTIVE BIT(2)
#define UMS9117_WRITE_ACTIVE BIT(8)
#define UMS9117_READ_ACTIVE BIT(9)
#define UMS9117_DATA_ACTIVE_MASK                                               \
	(UMS9117_CMD_INHIBIT | UMS9117_DAT_INHIBIT | UMS9117_DAT_LINE_ACTIVE | \
	 UMS9117_WRITE_ACTIVE | UMS9117_READ_ACTIVE)

#define UMS9117_INT_RESPONSE 0x00000001U
#define UMS9117_INT_TRANSFER 0x00000002U
#define UMS9117_INT_ERROR 0x00008000U
#define UMS9117_INT_TIMEOUT 0x00010000U
#define UMS9117_INT_CRC 0x00020000U
#define UMS9117_INT_END_BIT 0x00040000U
#define UMS9117_INT_INDEX 0x00080000U
#define UMS9117_INT_DATA_TIMEOUT 0x00100000U
#define UMS9117_INT_DATA_CRC 0x00200000U
#define UMS9117_INT_DATA_END_BIT 0x00400000U
/*
 * A failed automatic CMD12 is reported only here. Without this bit a stop
 * command that the controller issues on its own could fail unnoticed, so the
 * bit is owned and acknowledged even while multi-block transfers are refused.
 */
#define UMS9117_INT_AUTO_CMD12_ERROR 0x01000000U
/*
 * Host version 4 enabled, CMD23 not enabled, and every automatic command
 * error bit clear. This is what the board leaves behind after a reset.
 */
#define UMS9117_HOST_CTRL2_EXPECTED 0x10000000U
/* Automatic command error bits: not executed, timeout, CRC, end bit, index. */
#define UMS9117_AUTO_CMD_ERROR_MASK 0x0000009fU
#define UMS9117_INT_ADMA_ERROR 0x02000000U
#define UMS9117_INT_RESPONSE_ERROR 0x08000000U
#define UMS9117_INT_AXI_RESPONSE_ERROR 0x10000000U
#define UMS9117_STATUS_ENABLE_MASK 0x1b7f0003U
#define UMS9117_OWNED_STATUS_MASK UMS9117_STATUS_ENABLE_MASK
#define UMS9117_DETAIL_ERROR_MASK                                              \
	(UMS9117_INT_TIMEOUT | UMS9117_INT_CRC | UMS9117_INT_END_BIT |         \
	 UMS9117_INT_INDEX | UMS9117_INT_DATA_TIMEOUT | UMS9117_INT_DATA_CRC | \
	 UMS9117_INT_DATA_END_BIT | UMS9117_INT_AUTO_CMD12_ERROR |             \
	 UMS9117_INT_ADMA_ERROR | UMS9117_INT_RESPONSE_ERROR |                 \
	 UMS9117_INT_AXI_RESPONSE_ERROR)
#define UMS9117_SIGNAL_COMMAND \
	(UMS9117_INT_RESPONSE | UMS9117_DETAIL_ERROR_MASK)
#define UMS9117_SIGNAL_DATA (UMS9117_INT_TRANSFER | UMS9117_DETAIL_ERROR_MASK)

#define UMS9117_RESP_NONE 0x00U
#define UMS9117_RESP_LONG 0x01U
#define UMS9117_RESP_SHORT 0x02U
#define UMS9117_RESP_SHORT_BUSY 0x03U
#define UMS9117_SUB_CMD_FLAG 0x04U
#define UMS9117_CMD_CRC 0x08U
#define UMS9117_CMD_INDEX 0x10U
#define UMS9117_CMD_DATA 0x20U

#define UMS9117_COMMAND_OFFSET 2U

#define UMS9117_HOST_CTRL1_DMA_SEL_MASK GENMASK(4, 3)
#define UMS9117_HOST_CTRL1_WIDTH_MASK (BIT(5) | BIT(1))
#define UMS9117_HOST_CTRL1_OWNED_MASK \
	(UMS9117_HOST_CTRL1_DMA_SEL_MASK | UMS9117_HOST_CTRL1_WIDTH_MASK)
#define UMS9117_HOST_CTRL1_ADMA2 0x00000010U
#define UMS9117_HOST_CTRL1_1BIT_ADMA2 0x00000010U
#define UMS9117_HOST_CTRL1_4BIT_ADMA2 0x00000012U
#define UMS9117_TRANSFER_READ_ADMA2 0x0013U
#define UMS9117_TRANSFER_WRITE_ADMA2 0x0003U
/*
 * Multi-block read: DMA, block count enable, automatic CMD12, read direction
 * and multiple blocks. The controller issues the stop command itself, which is
 * why the automatic CMD12 error bit has to be owned.
 */
#define UMS9117_TRANSFER_CMD18_AUTO_CMD12_ADMA2 0x0037U
/*
 * Every descriptor but the last carries valid data and continues the chain;
 * the last one additionally ends the transfer.
 */
#define UMS9117_ADMA2_ATTR_TRANSFER 0x0021U
#define UMS9117_ADMA2_ATTR_TRANSFER_END 0x0023U
#define UMS9117_ADMA2_DESC_SIZE 8U
#define UMS9117_ADMA2_DESC_COUNT 32U
#define UMS9117_ADMA2_TABLE_SIZE \
	(UMS9117_ADMA2_DESC_COUNT * UMS9117_ADMA2_DESC_SIZE)
/* 128 KiB: the full descriptor table at one page per segment. */
#define UMS9117_MAX_REQUEST_BYTES 131072U

#define UMS9117_RESET_POLLS 64U
#define UMS9117_CLOCK_DEADLINE_MS 10U
#define UMS9117_INHIBIT_POLLS 100U
#define UMS9117_READ_QUIESCE_DEADLINE_MS 100U
#define UMS9117_WRITE_QUIESCE_DEADLINE_MS 5000U
#define UMS9117_POLL_DELAY_US 10U
#define UMS9117_REQUEST_TIMEOUT_MS 1000U
#define UMS9117_WRITE_REQUEST_TIMEOUT_MS 5000U
#define UMS9117_INITIAL_CLOCKS_DELAY_MS 10U

#define TA1618_SDIO0_SPI 57U
#define TA1618_SDIO0_INTID (TA1618_SDIO0_SPI + 32U)

struct ums9117_adma2_desc {
	__le16 attr;
	__le16 length;
	__le32 address;
} __packed;

static_assert(sizeof(struct ums9117_adma2_desc) == UMS9117_ADMA2_DESC_SIZE);
static_assert(UMS9117_TRANSFER_READ_ADMA2 == 0x0013U);
static_assert(UMS9117_TRANSFER_WRITE_ADMA2 == 0x0003U);
static_assert(((((u32)MMC_WRITE_BLOCK << 8) | UMS9117_RESP_SHORT |
		UMS9117_CMD_CRC | UMS9117_CMD_INDEX | UMS9117_CMD_DATA)
		       << 16 |
	       UMS9117_TRANSFER_WRITE_ADMA2) == 0x183a0003U);
static_assert(((((u32)MMC_READ_MULTIPLE_BLOCK << 8) | UMS9117_RESP_SHORT |
		UMS9117_CMD_CRC | UMS9117_CMD_INDEX | UMS9117_CMD_DATA)
		       << 16 |
	       UMS9117_TRANSFER_CMD18_AUTO_CMD12_ADMA2) == 0x123a0037U);
static_assert(UMS9117_ADMA2_ATTR_TRANSFER == 0x0021U);
static_assert(UMS9117_ADMA2_ATTR_TRANSFER_END == 0x0023U);
static_assert(UMS9117_ADMA2_TABLE_SIZE == 256U);
static_assert(UMS9117_HOST_CTRL1_WIDTH_MASK == 0x00000022U);
static_assert(UMS9117_HOST_CTRL1_4BIT_ADMA2 == 0x00000012U);
static_assert(UMS9117_13MHZ_DIVIDER_ENCODED == 0x00000100U);
static_assert(UMS9117_CLOCK_TIMEOUT_FPDOOM_ENCODED == 0x00080000U);
static_assert((0x08000000U | UMS9117_CLOCK_TIMEOUT_FPDOOM_ENCODED |
	       UMS9117_13MHZ_DIVIDER_ENCODED | UMS9117_CLOCK_CARD_EN |
	       UMS9117_CLOCK_INT_STABLE | UMS9117_CLOCK_INT_EN) ==
	      UMS9117_13MHZ_OPERATIONAL_CLOCK_EXPECTED);
static_assert(UMS9117_MAX_CLOCK_HZ == 13000000U);
static_assert((MMC_VDD_29_30 | MMC_VDD_30_31) == 0x00060000U);
static_assert(TA1618_SDIO0_INTID == 89U);
static_assert(TA1618_ADI_USER_LOCK + sizeof(u32) <= TA1618_ADI_SIZE);

enum ta1618_resource_index {
	RES_GATE_STATE,
	RES_GATE_SET,
	RES_GATE_CLEAR,
	RES_AP_RESET_STATE,
	RES_AP_RESET_SET,
	RES_AP_RESET_CLEAR,
	RES_CLOCK_SELECTOR,
	RES_BLOCK_COUNT,
	RES_BLOCK_SIZE,
	RES_PRESENT_STATE,
	RES_HOST_CONTROL1,
	RES_CLOCK_RESET,
	RES_ARGUMENT,
	RES_TRANSFER_COMMAND,
	RES_RESPONSE0,
	RES_RESPONSE1,
	RES_RESPONSE2,
	RES_RESPONSE3,
	RES_INTERRUPT_STATUS,
	RES_INTERRUPT_STATUS_ENABLE,
	RES_INTERRUPT_SIGNAL_ENABLE,
	RES_ADMA_ERROR,
	RES_ADMA_ADDRESS_LOW,
	RES_ADMA_ADDRESS_HIGH,
	RES_HOST_VERSION,
	RES_PWR_PAD_CTL,
	RES_CMD_MUX,
	RES_CMD_PAD,
	RES_D0_MUX,
	RES_D0_PAD,
	RES_CLK_MUX,
	RES_CLK_PAD,
	RES_D3_MUX,
	RES_D3_PAD,
	RES_D2_MUX,
	RES_D2_PAD,
	RES_D1_MUX,
	RES_D1_PAD,
	RES_ADI,
	RES_ANALOG,
	RES_HOST_CONTROL2,
	RES_COUNT,
};

struct ta1618_resource_definition {
	const char *name;
	resource_size_t address;
	resource_size_t size;
};

static const struct ta1618_resource_definition ta1618_resources[RES_COUNT] = {
	{ "gate-state", 0x20e00000U, TA1618_REG_SIZE },
	{ "gate-set", 0x20e01000U, TA1618_REG_SIZE },
	{ "gate-clear", 0x20e02000U, TA1618_REG_SIZE },
	{ "ap-reset-state", 0x20e00004U, TA1618_REG_SIZE },
	{ "ap-reset-set", 0x20e01004U, TA1618_REG_SIZE },
	{ "ap-reset-clear", 0x20e02004U, TA1618_REG_SIZE },
	{ "ap-clock-selector", 0x2150006cU, TA1618_REG_SIZE },
	{ "block-count", 0x20300000U, TA1618_REG_SIZE },
	{ "block-size", 0x20300004U, TA1618_REG_SIZE },
	{ "present-state", 0x20300024U, TA1618_REG_SIZE },
	{ "host-control1", 0x20300028U, TA1618_REG_SIZE },
	{ "clock-timeout-reset", 0x2030002cU, TA1618_REG_SIZE },
	{ "argument", 0x20300008U, TA1618_REG_SIZE },
	{ "transfer-command", 0x2030000cU, TA1618_REG_SIZE },
	{ "response0", 0x20300010U, TA1618_REG_SIZE },
	{ "response1", 0x20300014U, TA1618_REG_SIZE },
	{ "response2", 0x20300018U, TA1618_REG_SIZE },
	{ "response3", 0x2030001cU, TA1618_REG_SIZE },
	{ "interrupt-status", 0x20300030U, TA1618_REG_SIZE },
	{ "interrupt-status-enable", 0x20300034U, TA1618_REG_SIZE },
	{ "interrupt-signal-enable", 0x20300038U, TA1618_REG_SIZE },
	{ "adma-error", 0x20300054U, TA1618_REG_SIZE },
	{ "adma2-address-low", 0x20300058U, TA1618_REG_SIZE },
	{ "adma2-address-high", 0x2030005cU, TA1618_REG_SIZE },
	{ "host-version-slot-int", 0x203000fcU, TA1618_REG_SIZE },
	{ "pwr-pad-ctl", 0x402a0000U, TA1618_REG_SIZE },
	{ "cmd-mux", 0x402a0128U, TA1618_REG_SIZE },
	{ "cmd-pad", 0x402a0528U, TA1618_REG_SIZE },
	{ "d0-mux", 0x402a012cU, TA1618_REG_SIZE },
	{ "d0-pad", 0x402a052cU, TA1618_REG_SIZE },
	{ "clk-mux", 0x402a0134U, TA1618_REG_SIZE },
	{ "clk-pad", 0x402a0534U, TA1618_REG_SIZE },
	{ "d3-mux", 0x402a0120U, TA1618_REG_SIZE },
	{ "d3-pad", 0x402a0520U, TA1618_REG_SIZE },
	{ "d2-mux", 0x402a0124U, TA1618_REG_SIZE },
	{ "d2-pad", 0x402a0524U, TA1618_REG_SIZE },
	{ "d1-mux", 0x402a0130U, TA1618_REG_SIZE },
	{ "d1-pad", 0x402a0530U, TA1618_REG_SIZE },
	{ "adi-controller", 0x40600000U, TA1618_ADI_SIZE },
	{ "analog-slave", 0x40608000U, TA1618_ANALOG_SIZE },
	{ "host-control2", 0x2030003cU, TA1618_REG_SIZE },
};

struct ta1618_pin_definition {
	enum ta1618_resource_index resource;
	u32 target;
	bool mux;
};

static const struct ta1618_pin_definition ta1618_pins[TA1618_PIN_COUNT] = {
	{ RES_CMD_MUX, 0x00000000U, true },
	{ RES_CMD_PAD, TA1618_CMD_PAD_TARGET, false },
	{ RES_D0_MUX, 0x00000000U, true },
	{ RES_D0_PAD, TA1618_D0_PAD_TARGET, false },
	{ RES_CLK_MUX, 0x00000000U, true },
	{ RES_CLK_PAD, TA1618_CLK_PAD_TARGET, false },
	{ RES_D3_MUX, 0x00000000U, true },
	{ RES_D3_PAD, TA1618_D3_PAD_TARGET, false },
	{ RES_D2_MUX, 0x00000000U, true },
	{ RES_D2_PAD, TA1618_D2_PAD_TARGET, false },
	{ RES_D1_MUX, 0x00000000U, true },
	{ RES_D1_PAD, TA1618_D1_PAD_TARGET, false },
};

struct ta1618_analog_definition {
	u32 offset;
	u32 command_word;
	bool power_down;
};

enum ta1618_analog_index {
	ANA_CORE_PD,
	ANA_CORE_VOLT,
	ANA_IO_PD,
	ANA_IO_VOLT,
	ANA_COUNT,
};

static const struct ta1618_analog_definition ta1618_analog[ANA_COUNT] = {
	{ TA1618_VDDSDCORE_PD_OFFSET, 0x0340U, true },
	{ TA1618_VDDSDCORE_VOLT_OFFSET, 0x0341U, false },
	{ TA1618_VDDSDIO_PD_OFFSET, 0x0343U, true },
	{ TA1618_VDDSDIO_VOLT_OFFSET, 0x0344U, false },
};

struct ta1618_ios_trace_entry {
	u32 sequence;
	u32 clock;
	u32 power_mode;
	u32 bus_width;
	u32 timing;
	int result;
};

struct ta1618_mmc_audit {
	struct ta1618_ios_trace_entry ios[TA1618_IOS_TRACE_DEPTH];
	u32 ios_calls;
	u32 ios_trace_count;
	u32 cmd55_attempt_count;
	u32 cmd55_clean_count;
	u32 cmd55_app_cmd_count;
	u32 cmd55_last_arg;
	u32 cmd55_last_response;
	u32 acmd6_attempt_count;
	u32 acmd6_clean_count;
	u32 acmd6_failure_count;
	u32 cmd17_count;
	u32 cmd24_count;
	u32 cmd18_count;
	u32 cmd25_count;
	u32 max_data_blocks;
	u32 max_descriptors;
	u32 irq_count;
	u32 irq_error_count;
	u32 irq_crc_count;
	u32 irq_end_bit_count;
	u32 irq_timeout_count;
	u32 irq_command_timeout_count;
	u32 irq_data_timeout_count;
	u32 irq_auto_cmd12_count;
	u32 host_control2_after_reset;
	u32 irq_adma_error_count;
	u32 watchdog_timeout_count;
	u32 last_irq_raw_status;
	u32 last_irq_owned_status;
	u32 last_irq_w1c_readback;
	u32 last_opcode;
	u32 last_argument;
	u32 last_lba;
	u32 last_cmd17_ordinal;
	bool last_command_valid;
	bool last_lba_valid;
	u32 host_ctrl1_width_before;
	u32 host_ctrl1_width_after;
	u32 selector_after_activate;
	u32 clock_reset_after_activate;
	u32 selector_width_before;
	u32 selector_width_after;
	u32 clock_width_before;
	u32 clock_width_after;
	u32 timeout_field_before;
	u32 timeout_field_candidate;
	u32 timeout_field_readback;
	u32 timeout_register_before;
	u32 timeout_register_candidate;
	u32 timeout_register_readback;
	u32 requested_clock_hz;
	u32 deferred_clock_hz;
	u32 applied_clock_hz;
	u32 deferred_clock_count;
	u32 applied_clock_count;
};

struct ta1618_sd_mmc {
	struct device *dev;
	struct mmc_host *mmc;
	void __iomem *regs[RES_COUNT];
	struct resource *resources[RES_COUNT];
	int irq;
	bool irq_requested;

	spinlock_t lock;
	struct mutex state_mutex;
	struct mmc_request *active_mrq;
	struct mmc_request *finish_mrq;
	struct delayed_work timeout_work;
	struct work_struct finish_work;
	u32 finish_status;
	u32 finish_auto_cmd;
	u32 finish_response[4];
	bool finish_ack_failed;
	bool finish_needs_quiesce;
	bool active_width_acmd6;
	bool finish_width_acmd6;

	struct ums9117_adma2_desc *descriptors;
	dma_addr_t descriptors_dma;
	bool data_mapped;
	enum dma_data_direction dma_direction;
	unsigned int mapped_nents;
	unsigned int mapped_sg_len;
	size_t data_length;

	u32 gate_snapshot;
	u32 reset_snapshot;
	u32 selector_snapshot;
	u32 controller_snapshot;
	u32 host_control1_snapshot;
	u32 status_enable_snapshot;
	u32 signal_enable_snapshot;
	u32 pin_snapshot[TA1618_PIN_COUNT];
	u16 analog_snapshot[ANA_COUNT];
	bool snapshots_valid;
	bool controller_snapshot_valid;
	bool platform_active;
	bool rails_on;
	bool card_clock_on;
	bool write_status_pending;
	bool app_cmd_armed;
	u32 app_cmd_arg;
	bool width_acmd6_clean;
	bool physical_width4;
	bool clock_13mhz_deferred;
	bool clock_13mhz_applied;
	u32 actual_clock_hz;
	bool width_switch_fatal;
	bool terminal_cleanup_hold;
	bool fatal_error;
	bool stopping;
	bool adi_locked;
	bool audit_file_created;
	struct ta1618_mmc_audit audit;
};

static inline u32 ta1618_readl(struct ta1618_sd_mmc *host,
			       enum ta1618_resource_index resource)
{
	return readl(host->regs[resource]);
}

static inline void ta1618_writel(struct ta1618_sd_mmc *host,
				 enum ta1618_resource_index resource, u32 value)
{
	writel(value, host->regs[resource]);
}

static inline u32 ta1618_readl_at(struct ta1618_sd_mmc *host,
				  enum ta1618_resource_index resource,
				  u32 offset)
{
	return readl(host->regs[resource] + offset);
}

static inline void ta1618_writel_at(struct ta1618_sd_mmc *host,
				    enum ta1618_resource_index resource,
				    u32 offset, u32 value)
{
	writel(value, host->regs[resource] + offset);
}

static void ums9117_record_ios(struct ta1618_sd_mmc *host,
			       const struct mmc_ios *ios, int result)
{
	struct ta1618_ios_trace_entry *entry;
	unsigned long flags;
	u32 sequence;

	spin_lock_irqsave(&host->lock, flags);
	sequence = ++host->audit.ios_calls;
	entry = &host->audit.ios[(sequence - 1U) % TA1618_IOS_TRACE_DEPTH];
	entry->sequence = sequence;
	entry->clock = ios->clock;
	entry->power_mode = ios->power_mode;
	entry->bus_width = ios->bus_width;
	entry->timing = ios->timing;
	entry->result = result;
	if (host->audit.ios_trace_count < TA1618_IOS_TRACE_DEPTH)
		host->audit.ios_trace_count++;
	spin_unlock_irqrestore(&host->lock, flags);
}

static void ums9117_audit_command_locked(struct ta1618_sd_mmc *host,
					 const struct mmc_command *cmd,
					 bool width_acmd6)
{
	host->audit.last_command_valid = true;
	host->audit.last_opcode = cmd->opcode;
	host->audit.last_argument = cmd->arg;
	host->audit.last_lba_valid = false;
	host->audit.last_lba = 0;
	if (cmd->data) {
		host->audit.max_data_blocks =
			max(host->audit.max_data_blocks, cmd->data->blocks);
		host->audit.max_descriptors =
			max(host->audit.max_descriptors, host->mapped_nents);
	}
	switch (cmd->opcode) {
	case MMC_APP_CMD:
		host->audit.cmd55_attempt_count++;
		break;
	case MMC_READ_SINGLE_BLOCK:
		host->audit.cmd17_count++;
		host->audit.last_cmd17_ordinal = host->audit.cmd17_count;
		if (host->mmc->card) {
			host->audit.last_lba =
				mmc_card_is_blockaddr(host->mmc->card) ?
					cmd->arg :
					cmd->arg >> 9;
			host->audit.last_lba_valid = true;
		}
		break;
	case MMC_WRITE_BLOCK:
		host->audit.cmd24_count++;
		break;
	case MMC_READ_MULTIPLE_BLOCK:
		host->audit.cmd18_count++;
		break;
	case MMC_WRITE_MULTIPLE_BLOCK:
		host->audit.cmd25_count++;
		break;
	default:
		break;
	}
	if (width_acmd6)
		host->audit.acmd6_attempt_count++;
}

static void ums9117_audit_irq_locked(struct ta1618_sd_mmc *host, u32 status,
				     u32 owned, u32 w1c_readback)
{
	host->audit.irq_count++;
	host->audit.last_irq_raw_status = status;
	host->audit.last_irq_owned_status = owned;
	host->audit.last_irq_w1c_readback = w1c_readback;
	if (status & UMS9117_INT_ERROR)
		host->audit.irq_error_count++;
	if (status & (UMS9117_INT_CRC | UMS9117_INT_DATA_CRC))
		host->audit.irq_crc_count++;
	if (status & (UMS9117_INT_END_BIT | UMS9117_INT_DATA_END_BIT))
		host->audit.irq_end_bit_count++;
	if (status & (UMS9117_INT_TIMEOUT | UMS9117_INT_DATA_TIMEOUT))
		host->audit.irq_timeout_count++;
	if (status & UMS9117_INT_TIMEOUT)
		host->audit.irq_command_timeout_count++;
	if (status & UMS9117_INT_DATA_TIMEOUT)
		host->audit.irq_data_timeout_count++;
	if (status & UMS9117_INT_AUTO_CMD12_ERROR)
		host->audit.irq_auto_cmd12_count++;
	if (status & UMS9117_INT_ADMA_ERROR)
		host->audit.irq_adma_error_count++;
}

static bool ta1618_dma_address_valid(dma_addr_t address, size_t length)
{
	u64 start = (u64)address;

	return start && IS_ALIGNED(start, sizeof(u32)) && start <= U32_MAX &&
	       length && length - 1 <= U32_MAX - start;
}

static int ta1618_adi_validate(struct ta1618_sd_mmc *host)
{
	if (ta1618_readl_at(host, RES_ADI, TA1618_ADI_VERSION) !=
		    TA1618_ADI_EXPECTED_VERSION ||
	    ta1618_readl_at(host, RES_ADI, TA1618_ADI_MST_CTL) !=
		    TA1618_ADI_EXPECTED_MST_CTL)
		return -EPROTONOSUPPORT;
	return 0;
}

static int ta1618_adi_wait_empty_locked(struct ta1618_sd_mmc *host)
{
	unsigned int waited;
	u32 raw;
	u32 status;

	if (!host->adi_locked)
		return -EPERM;
	for (waited = 0; waited < TA1618_ADI_POLL_BUDGET_US; waited++) {
		raw = ta1618_readl_at(host, RES_ADI, TA1618_ADI_INT_RAW);
		status = ta1618_readl_at(host, RES_ADI, TA1618_ADI_FIFO_STS);
		if (raw & TA1618_ADI_ARM_FIFO_OVERFLOW_RAW)
			return -EOVERFLOW;
		if (status & TA1618_ADI_FIFO_EMPTY)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int ta1618_adi_lock(struct ta1618_sd_mmc *host)
{
	unsigned int waited;

	if (host->adi_locked)
		return -EDEADLK;
	for (waited = 0; waited < TA1618_ADI_POLL_BUDGET_US; waited++) {
		/* USER_LOCK is an acquire-on-read semaphore; zero grants it. */
		if (!ta1618_readl_at(host, RES_ADI, TA1618_ADI_USER_LOCK)) {
			host->adi_locked = true;
			return 0;
		}
		udelay(1);
	}
	return -EBUSY;
}

static int ta1618_adi_unlock(struct ta1618_sd_mmc *host)
{
	int ret;

	if (!host->adi_locked)
		return 0;
	ret = ta1618_adi_wait_empty_locked(host);
	ta1618_writel_at(host, RES_ADI, TA1618_ADI_USER_LOCK,
			 TA1618_ADI_USER_LOCK_RELEASE);
	host->adi_locked = false;
	return ret;
}

static int ta1618_adi_read_locked(struct ta1618_sd_mmc *host,
				  enum ta1618_analog_index index, u16 *value)
{
	const struct ta1618_analog_definition *definition;
	unsigned int waited;
	u32 data;
	u32 returned;
	int ret;

	if (!host->adi_locked || index >= ANA_COUNT || !value)
		return -EINVAL;
	definition = &ta1618_analog[index];
	if (!IS_ALIGNED(definition->offset, sizeof(u32)) ||
	    definition->offset > TA1618_ANALOG_SIZE - sizeof(u32) ||
	    definition->command_word != definition->offset >> 2)
		return -ERANGE;
	ret = ta1618_adi_validate(host);
	if (ret)
		return ret;

	ta1618_writel_at(host, RES_ADI, TA1618_ADI_RD_CMD, definition->offset);
	for (waited = 0; waited < TA1618_ADI_POLL_BUDGET_US; waited++) {
		data = ta1618_readl_at(host, RES_ADI, TA1618_ADI_RD_DATA);
		if (!(data & TA1618_ADI_RD_BUSY))
			break;
		udelay(1);
	}
	if (waited == TA1618_ADI_POLL_BUDGET_US)
		return -ETIMEDOUT;
	returned = FIELD_GET(TA1618_ADI_RD_RETURNED_ADDRESS, data);
	if (returned != definition->command_word)
		return -EIO;
	*value = data & 0xffffU;
	return 0;
}

static int ta1618_adi_write_pd_locked(struct ta1618_sd_mmc *host,
				      enum ta1618_analog_index index, u16 value)
{
	u16 readback;
	int ret;

	if (index >= ANA_COUNT || !ta1618_analog[index].power_down)
		return -EPERM;
	ret = ta1618_adi_wait_empty_locked(host);
	if (ret)
		return ret;
	if (ta1618_readl_at(host, RES_ADI, TA1618_ADI_FIFO_STS) &
	    TA1618_ADI_FIFO_FULL)
		return -EBUSY;
	ret = ta1618_adi_validate(host);
	if (ret)
		return ret;
	ta1618_writel_at(host, RES_ANALOG, ta1618_analog[index].offset, value);
	ret = ta1618_adi_wait_empty_locked(host);
	if (ret)
		return ret;
	ret = ta1618_adi_read_locked(host, index, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}

static int ta1618_snapshot_platform(struct ta1618_sd_mmc *host)
{
	unsigned int pass;
	unsigned int i;
	u32 first;
	u16 analog;
	int ret;
	int unlock_ret;

	host->gate_snapshot = ta1618_readl(host, RES_GATE_STATE);
	if (host->gate_snapshot & TA1618_GATE_MASK)
		return -EBUSY;
	host->reset_snapshot = ta1618_readl(host, RES_AP_RESET_STATE);
	if (host->reset_snapshot & TA1618_AP_RESET_MASK)
		return -EBUSY;
	host->selector_snapshot = ta1618_readl(host, RES_CLOCK_SELECTOR);
	(void)ta1618_readl(host, RES_PWR_PAD_CTL);

	for (i = 0; i < TA1618_PIN_COUNT; i++) {
		first = ta1618_readl(host, ta1618_pins[i].resource);
		host->pin_snapshot[i] = first;
		if (ta1618_readl(host, ta1618_pins[i].resource) != first)
			return -EAGAIN;
		if (ta1618_pins[i].mux && (first & ~TA1618_PIN_FUNC_MASK))
			return -ERANGE;
	}

	ret = ta1618_adi_validate(host);
	if (ret)
		return ret;
	ret = ta1618_adi_lock(host);
	if (ret)
		return ret;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < ANA_COUNT; i++) {
			ret = ta1618_adi_read_locked(host, i, &analog);
			if (ret)
				goto out_unlock;
			if (!pass)
				host->analog_snapshot[i] = analog;
			else if (host->analog_snapshot[i] != analog) {
				ret = -EAGAIN;
				goto out_unlock;
			}
		}
	}
	if (host->analog_snapshot[ANA_CORE_VOLT] !=
		    TA1618_VDDSDCORE_VOLT_EXPECTED ||
	    host->analog_snapshot[ANA_IO_VOLT] != TA1618_VDDSDIO_VOLT_EXPECTED)
		ret = -ERANGE;

out_unlock:
	unlock_ret = ta1618_adi_unlock(host);
	if (!ret && unlock_ret)
		ret = unlock_ret;
	if (!ret)
		host->snapshots_valid = true;
	return ret;
}

static int ta1618_set_rails(struct ta1618_sd_mmc *host, bool enable)
{
	static const enum ta1618_analog_index enable_order[] = {
		ANA_CORE_PD,
		ANA_IO_PD,
	};
	static const enum ta1618_analog_index disable_order[] = {
		ANA_CORE_PD,
		ANA_IO_PD,
	};
	const enum ta1618_analog_index *order;
	unsigned int i;
	u16 current_pd;
	u16 target;
	int ret;
	int unlock_ret;
	bool changed = false;

	order = enable ? enable_order : disable_order;
	ret = ta1618_adi_lock(host);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(enable_order); i++) {
		ret = ta1618_adi_read_locked(host, order[i], &current_pd);
		if (ret)
			goto out_unlock;
		target = enable ? current_pd & ~TA1618_PD_MASK :
				  current_pd | TA1618_PD_MASK;
		if (target == current_pd)
			continue;
		ret = ta1618_adi_write_pd_locked(host, order[i], target);
		if (ret)
			goto out_unlock;
		changed = true;
	}

out_unlock:
	unlock_ret = ta1618_adi_unlock(host);
	if (!ret && unlock_ret)
		ret = unlock_ret;
	if (!ret) {
		host->rails_on = enable;
		if (changed)
			msleep(TA1618_RAIL_DELAY_MS);
	}
	return ret;
}

static int ums9117_reset_controller(struct ta1618_sd_mmc *host)
{
	unsigned int i;
	u32 before;
	u32 value;

	before = ta1618_readl(host, RES_CLOCK_RESET);
	if (before & UMS9117_HOST_RESET)
		return -EBUSY;
	ta1618_writel(host, RES_CLOCK_RESET, before | UMS9117_HOST_RESET);
	for (i = 0; i < UMS9117_RESET_POLLS; i++) {
		value = ta1618_readl(host, RES_CLOCK_RESET);
		if ((value & ~UMS9117_HOST_RESET) !=
		    (before & ~UMS9117_HOST_RESET))
			return -EIO;
		if (!(value & UMS9117_HOST_RESET))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int ums9117_set_card_clock(struct ta1618_sd_mmc *host, bool enable)
{
	unsigned long flags;
	ktime_t deadline;
	u32 target;
	u32 value;

	value = ta1618_readl(host, RES_CLOCK_RESET);
	if (!enable) {
		target = value & ~UMS9117_CLOCK_CARD_EN;
		ta1618_writel(host, RES_CLOCK_RESET, target);
		value = ta1618_readl(host, RES_CLOCK_RESET);
		if (value != target)
			return -EIO;
		host->card_clock_on = false;
		host->actual_clock_hz = 0;
		return 0;
	}

	target = value;
	target &= ~(UMS9117_CLOCK_DIVIDER_MASK | UMS9117_CLOCK_PROG_MODE |
		    UMS9117_CLOCK_PLL_EN | UMS9117_CLOCK_CARD_EN |
		    UMS9117_CLOCK_INT_STABLE | UMS9117_CLOCK_INT_EN);
	target |= UMS9117_DIVIDER_ENCODED | UMS9117_CLOCK_INT_EN;
	ta1618_writel(host, RES_CLOCK_RESET, target);
	deadline = ktime_add_ms(ktime_get(), UMS9117_CLOCK_DEADLINE_MS);
	for (;;) {
		value = ta1618_readl(host, RES_CLOCK_RESET);
		if (!(value & UMS9117_CLOCK_INT_EN) ||
		    (value & ~UMS9117_CLOCK_INT_STABLE) !=
			    (target & ~UMS9117_CLOCK_INT_STABLE))
			return -EIO;
		if (value & UMS9117_CLOCK_INT_STABLE)
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return -ETIMEDOUT;
		udelay(UMS9117_POLL_DELAY_US);
	}
	target = value | UMS9117_CLOCK_CARD_EN;
	ta1618_writel(host, RES_CLOCK_RESET, target);
	if (ta1618_readl(host, RES_CLOCK_RESET) != target)
		return -EIO;
	host->card_clock_on = true;
	host->actual_clock_hz = UMS9117_CLOCK_HZ;
	spin_lock_irqsave(&host->lock, flags);
	host->audit.selector_after_activate =
		ta1618_readl(host, RES_CLOCK_SELECTOR);
	host->audit.clock_reset_after_activate = target;
	host->audit.applied_clock_hz = UMS9117_CLOCK_HZ;
	host->audit.applied_clock_count++;
	spin_unlock_irqrestore(&host->lock, flags);
	msleep(UMS9117_INITIAL_CLOCKS_DELAY_MS);
	return 0;
}

static int ta1618_configure_pins(struct ta1618_sd_mmc *host)
{
	unsigned int i;

	for (i = 0; i < TA1618_PIN_COUNT; i++) {
		if (ta1618_readl(host, ta1618_pins[i].resource) !=
		    host->pin_snapshot[i])
			return -EAGAIN;
		ta1618_writel(host, ta1618_pins[i].resource,
			      ta1618_pins[i].target);
		if (ta1618_readl(host, ta1618_pins[i].resource) !=
		    ta1618_pins[i].target)
			return -EIO;
	}
	return 0;
}

static void ums9117_mask_and_ack(struct ta1618_sd_mmc *host)
{
	u32 status;
	u32 owned;

	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	status = ta1618_readl(host, RES_INTERRUPT_STATUS);
	owned = status & UMS9117_OWNED_STATUS_MASK;
	if (owned)
		ta1618_writel(host, RES_INTERRUPT_STATUS, owned);
}

static int ta1618_activate_platform(struct ta1618_sd_mmc *host)
{
	unsigned long flags;
	u32 readback;
	u32 value;
	u32 expected;
	int ret;

	if (host->platform_active)
		return ta1618_set_rails(host, true);
	if (!host->snapshots_valid)
		return -EINVAL;

	ta1618_writel(host, RES_GATE_SET, TA1618_GATE_MASK);
	host->platform_active = true;
	if (ta1618_readl(host, RES_GATE_STATE) !=
	    (host->gate_snapshot | TA1618_GATE_MASK))
		return -EIO;

	ta1618_writel(host, RES_AP_RESET_SET, TA1618_AP_RESET_MASK);
	expected = host->reset_snapshot | TA1618_AP_RESET_MASK;
	if (ta1618_readl(host, RES_AP_RESET_STATE) != expected)
		return -EIO;
	ta1618_writel(host, RES_AP_RESET_CLEAR, TA1618_AP_RESET_MASK);
	if (ta1618_readl(host, RES_AP_RESET_STATE) != host->reset_snapshot)
		return -EIO;

	value = host->selector_snapshot & ~TA1618_SELECTOR_MASK;
	ta1618_writel(host, RES_CLOCK_SELECTOR, value);
	if (ta1618_readl(host, RES_CLOCK_SELECTOR) != value)
		return -EIO;
	value |= TA1618_SELECTOR_26MHZ;
	ta1618_writel(host, RES_CLOCK_SELECTOR, value);
	if (ta1618_readl(host, RES_CLOCK_SELECTOR) != value)
		return -EIO;

	host->controller_snapshot = ta1618_readl(host, RES_CLOCK_RESET);
	host->host_control1_snapshot = ta1618_readl(host, RES_HOST_CONTROL1);
	host->status_enable_snapshot =
		ta1618_readl(host, RES_INTERRUPT_STATUS_ENABLE);
	host->signal_enable_snapshot =
		ta1618_readl(host, RES_INTERRUPT_SIGNAL_ENABLE);
	host->controller_snapshot_valid = true;
	if ((ta1618_readl(host, RES_HOST_VERSION) &
	     UMS9117_HOST_VERSION_MASK) != UMS9117_HOST_VERSION_EXPECTED ||
	    host->host_control1_snapshot ||
	    (host->controller_snapshot &
	     (UMS9117_HOST_RESET | UMS9117_CLOCK_CARD_EN)))
		return -EPROTONOSUPPORT;

	ums9117_mask_and_ack(host);
	ret = ums9117_reset_controller(host);
	if (ret)
		return ret;
	/*
	 * Read only after the reset, so the value describes the state the
	 * driver actually starts from rather than whatever the boot loader
	 * left behind. Nothing here is programmed: an automatic CMD12 needs
	 * host version 4 mode, which this driver inherits and never sets, so
	 * an unexpected value is recorded and warned about rather than
	 * treated as a probe failure.
	 */
	host->audit.host_control2_after_reset =
		ta1618_readl(host, RES_HOST_CONTROL2);
	if (host->audit.host_control2_after_reset !=
	    UMS9117_HOST_CTRL2_EXPECTED)
		dev_warn(host->dev,
			 "unexpected host control 2 after reset: 0x%08x\n",
			 host->audit.host_control2_after_reset);
	ret = ta1618_configure_pins(host);
	if (ret)
		return ret;
	ret = ta1618_set_rails(host, false);
	if (ret)
		return ret;
	ret = ta1618_set_rails(host, true);
	if (ret)
		return ret;

	value = ta1618_readl(host, RES_HOST_CONTROL1);
	value &= ~UMS9117_HOST_CTRL1_OWNED_MASK;
	value |= UMS9117_HOST_CTRL1_1BIT_ADMA2;
	ta1618_writel(host, RES_HOST_CONTROL1, value);
	readback = ta1618_readl(host, RES_HOST_CONTROL1);
	if (readback != value || (readback & UMS9117_HOST_CTRL1_OWNED_MASK) !=
					 UMS9117_HOST_CTRL1_1BIT_ADMA2)
		return -EIO;
	ta1618_writel(host, RES_INTERRUPT_STATUS_ENABLE,
		      UMS9117_STATUS_ENABLE_MASK);
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	host->physical_width4 = false;
	host->width_acmd6_clean = false;
	host->clock_13mhz_deferred = false;
	host->clock_13mhz_applied = false;
	host->actual_clock_hz = 0;
	host->terminal_cleanup_hold = false;
	host->app_cmd_armed = false;
	host->app_cmd_arg = 0;
	spin_lock_irqsave(&host->lock, flags);
	host->audit.host_ctrl1_width_before = readback;
	host->audit.host_ctrl1_width_after = readback;
	host->audit.selector_after_activate =
		ta1618_readl(host, RES_CLOCK_SELECTOR);
	host->audit.clock_reset_after_activate =
		ta1618_readl(host, RES_CLOCK_RESET);
	spin_unlock_irqrestore(&host->lock, flags);
	host->platform_active = true;
	return 0;
}

static int ta1618_restore_rails(struct ta1618_sd_mmc *host)
{
	static const enum ta1618_analog_index order[] = {
		ANA_IO_PD,
		ANA_CORE_PD,
	};
	unsigned int i;
	u16 current_pd;
	u16 target;
	int ret;
	int first_error = 0;
	int unlock_ret;

	ret = ta1618_adi_lock(host);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(order); i++) {
		ret = ta1618_adi_read_locked(host, order[i], &current_pd);
		if (ret) {
			if (!first_error)
				first_error = ret;
			continue;
		}
		target = (current_pd & ~TA1618_PD_MASK) |
			 (host->analog_snapshot[order[i]] & TA1618_PD_MASK);
		if (target == current_pd)
			continue;
		ret = ta1618_adi_write_pd_locked(host, order[i], target);
		if (ret && !first_error)
			first_error = ret;
	}
	unlock_ret = ta1618_adi_unlock(host);
	if (!first_error && unlock_ret)
		first_error = unlock_ret;
	return first_error;
}

static void ta1618_restore_platform(struct ta1618_sd_mmc *host)
{
	unsigned int i;
	u32 value;
	int ret;

	if (!host->platform_active)
		return;
	if (host->controller_snapshot_valid) {
		ums9117_mask_and_ack(host);
		ta1618_writel(host, RES_INTERRUPT_STATUS_ENABLE,
			      host->status_enable_snapshot);
		ta1618_writel(host, RES_HOST_CONTROL1,
			      host->host_control1_snapshot);
		ta1618_writel(host, RES_CLOCK_RESET,
			      host->controller_snapshot & ~UMS9117_HOST_RESET);
	}

	ret = ta1618_restore_rails(host);
	if (ret)
		dev_err(host->dev, "failed to restore SD rails: %d\n", ret);
	for (i = 0; i < TA1618_PIN_COUNT; i++)
		ta1618_writel(host, ta1618_pins[i].resource,
			      host->pin_snapshot[i]);
	value = ta1618_readl(host, RES_CLOCK_SELECTOR);
	value = (value & ~TA1618_SELECTOR_MASK) |
		(host->selector_snapshot & TA1618_SELECTOR_MASK);
	ta1618_writel(host, RES_CLOCK_SELECTOR, value);
	if (ta1618_readl(host, RES_AP_RESET_STATE) != host->reset_snapshot)
		ta1618_writel(host, RES_AP_RESET_CLEAR, TA1618_AP_RESET_MASK);
	ta1618_writel(host, RES_GATE_CLEAR, TA1618_GATE_MASK);
	if (ta1618_readl(host, RES_GATE_STATE) != host->gate_snapshot)
		dev_err(host->dev, "failed to restore SDIO0 gate state\n");
	host->platform_active = false;
	host->rails_on = false;
	host->card_clock_on = false;
	host->actual_clock_hz = 0;
}

static bool ums9117_destructive_command(const struct mmc_command *cmd)
{
	switch (cmd->opcode) {
	case MMC_WRITE_DAT_UNTIL_STOP:
	case MMC_SET_BLOCK_COUNT:
	case MMC_WRITE_MULTIPLE_BLOCK:
	case MMC_PROGRAM_CID:
	case MMC_PROGRAM_CSD:
	case MMC_SET_WRITE_PROT:
	case MMC_CLR_WRITE_PROT:
	case SD_ERASE_WR_BLK_START:
	case SD_ERASE_WR_BLK_END:
	case MMC_ERASE_GROUP_START:
	case MMC_ERASE_GROUP_END:
	case MMC_ERASE:
	case MMC_LOCK_UNLOCK:
		return true;
	case SD_SWITCH:
		return !!(cmd->arg & BIT(31));
	default:
		return false;
	}
}

/*
 * The core builds exactly one shape for a multi-block read: CMD18 carrying the
 * data, a bare CMD12 attached as the stop command, and no set-block-count
 * because this host never claims to support one. Every field is checked rather
 * than assumed, so an unexpected shape is refused before any register is
 * touched instead of being transferred wrongly.
 */
static bool ums9117_is_multi_read(const struct mmc_request *mrq,
				  const struct mmc_host *mmc)
{
	const struct mmc_command *cmd = mrq->cmd;
	const struct mmc_command *stop = mrq->stop;
	const struct mmc_data *data = cmd->data;

	return cmd->opcode == MMC_READ_MULTIPLE_BLOCK && data &&
	       data->flags == MMC_DATA_READ && data->blksz == 512 &&
	       data->blocks >= 2 && data->blocks <= mmc->max_blk_count &&
	       data->sg && data->sg_len >= 1 && data->sg_len <= mmc->max_segs &&
	       !mrq->sbc && stop && stop->opcode == MMC_STOP_TRANSMISSION &&
	       !stop->arg && !stop->data && mmc_resp_type(stop) == MMC_RSP_R1 &&
	       !mrq->cap_cmd_during_tfr;
}

static int ums9117_response_flags(const struct mmc_command *cmd, u16 *flags)
{
	u32 response = mmc_resp_type(cmd);

	if (!response)
		*flags = UMS9117_RESP_NONE;
	else if (response == MMC_RSP_R2)
		*flags = UMS9117_RESP_LONG | UMS9117_CMD_CRC;
	else if (response == MMC_RSP_R1B)
		*flags = UMS9117_RESP_SHORT_BUSY | UMS9117_CMD_CRC |
			 UMS9117_CMD_INDEX;
	else if (response == MMC_RSP_R3)
		*flags = UMS9117_RESP_SHORT;
	else if (response == MMC_RSP_R1 || response == MMC_RSP_R6 ||
		 response == MMC_RSP_R7)
		*flags = UMS9117_RESP_SHORT | UMS9117_CMD_CRC |
			 UMS9117_CMD_INDEX;
	else
		return -EOPNOTSUPP;
	return 0;
}

static void ums9117_set_request_error(struct mmc_request *mrq, int error)
{
	mrq->cmd->error = error;
	if (mrq->cmd->data) {
		mrq->cmd->data->error = error;
		mrq->cmd->data->bytes_xfered = 0;
	}
}

static void ums9117_unmap_data(struct ta1618_sd_mmc *host)
{
	struct mmc_data *data;

	if (!host->data_mapped)
		return;
	data = host->finish_mrq ? host->finish_mrq->cmd->data : NULL;
	if (!data && host->active_mrq)
		data = host->active_mrq->cmd->data;
	if (data)
		dma_unmap_sg(host->dev, data->sg, host->mapped_sg_len,
			     host->dma_direction);
	host->data_mapped = false;
	host->mapped_nents = 0;
	host->mapped_sg_len = 0;
	host->data_length = 0;
}

static int ums9117_prepare_data(struct ta1618_sd_mmc *host,
				struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	struct scatterlist *sg;
	enum dma_data_direction direction;
	dma_addr_t address;
	size_t total_expected;
	size_t length;
	size_t total;
	int mapped;
	int i;

	if (data->flags == MMC_DATA_WRITE) {
		if (cmd->opcode != MMC_WRITE_BLOCK || data->blocks != 1 ||
		    data->blksz != 512 || data->sg_len != 1)
			return -EOPNOTSUPP;
		direction = DMA_TO_DEVICE;
	} else if (cmd->opcode == MMC_READ_MULTIPLE_BLOCK) {
		/*
		 * Every field of a multi-block read was already checked before
		 * the request was admitted, so repeating the single-block
		 * shape guard here would only reject valid work.
		 */
		direction = DMA_FROM_DEVICE;
	} else {
		if (data->flags != MMC_DATA_READ || data->blocks != 1 ||
		    !data->blksz || data->blksz > 512 || data->sg_len != 1 ||
		    (cmd->opcode == MMC_READ_SINGLE_BLOCK &&
		     data->blksz != 512))
			return -EOPNOTSUPP;
		direction = DMA_FROM_DEVICE;
	}
	total_expected = (size_t)data->blocks * data->blksz;
	mapped = dma_map_sg(host->dev, data->sg, data->sg_len, direction);
	if (mapped <= 0)
		return mapped < 0 ? mapped : -EIO;
	if (mapped > (int)UMS9117_ADMA2_DESC_COUNT) {
		dma_unmap_sg(host->dev, data->sg, data->sg_len, direction);
		return -EOPNOTSUPP;
	}

	memset(host->descriptors, 0, UMS9117_ADMA2_TABLE_SIZE);
	total = 0;
	for_each_sg(data->sg, sg, mapped, i) {
		address = sg_dma_address(sg);
		length = sg_dma_len(sg);
		if (!length || length > host->mmc->max_seg_size ||
		    !ta1618_dma_address_valid(address, length)) {
			dma_unmap_sg(host->dev, data->sg, data->sg_len,
				     direction);
			return -ERANGE;
		}
		host->descriptors[i].attr = cpu_to_le16(
			i == mapped - 1 ? UMS9117_ADMA2_ATTR_TRANSFER_END :
					  UMS9117_ADMA2_ATTR_TRANSFER);
		host->descriptors[i].length = cpu_to_le16(length);
		host->descriptors[i].address = cpu_to_le32((u32)address);
		total += length;
	}
	/*
	 * The controller stops after block count times block size, so a
	 * scatterlist that does not add up to exactly that would silently
	 * transfer the wrong bytes rather than fail.
	 */
	if (total != total_expected) {
		dma_unmap_sg(host->dev, data->sg, data->sg_len, direction);
		return -EIO;
	}

	host->data_mapped = true;
	host->dma_direction = direction;
	host->mapped_nents = mapped;
	host->mapped_sg_len = data->sg_len;
	host->data_length = total;
	return 0;
}

static int ums9117_wait_inhibit(struct ta1618_sd_mmc *host, bool data)
{
	unsigned int i;
	u32 mask = UMS9117_CMD_INHIBIT;

	if (data)
		mask |= UMS9117_DAT_INHIBIT;
	for (i = 0; i < UMS9117_INHIBIT_POLLS; i++) {
		if (!(ta1618_readl(host, RES_PRESENT_STATE) & mask))
			return 0;
		udelay(UMS9117_POLL_DELAY_US);
	}
	return -EBUSY;
}

static void ums9117_width_fail_closed(struct ta1618_sd_mmc *host,
				      const char *reason, int error)
{
	unsigned long flags;
	u32 clock_before;
	u32 clock_after;
	u32 signal_after;
	bool cleanup_confirmed;

	/* Do not reset, re-clock, alter the divider, roll back ACMD6, or cycle rails. */
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	signal_after = ta1618_readl(host, RES_INTERRUPT_SIGNAL_ENABLE);
	synchronize_irq(host->irq);
	clock_before = ta1618_readl(host, RES_CLOCK_RESET);
	clock_after = clock_before & ~UMS9117_CLOCK_CARD_EN;
	if (clock_after != clock_before)
		ta1618_writel(host, RES_CLOCK_RESET, clock_after);
	clock_after = ta1618_readl(host, RES_CLOCK_RESET);
	cleanup_confirmed = !signal_after &&
			    !(clock_after & UMS9117_CLOCK_CARD_EN);

	spin_lock_irqsave(&host->lock, flags);
	host->fatal_error = true;
	host->width_switch_fatal = true;
	host->terminal_cleanup_hold |= !cleanup_confirmed;
	host->app_cmd_armed = false;
	host->card_clock_on = !!(clock_after & UMS9117_CLOCK_CARD_EN);
	if (!host->card_clock_on)
		host->actual_clock_hz = 0;
	spin_unlock_irqrestore(&host->lock, flags);
	dev_crit(
		host->dev,
		"4-bit/13MHz fail-closed: %s error=%d signal=0x%08x clock_before=0x%08x clock_after=0x%08x cleanup_confirmed=%u; IRQ masked, card clock off when confirmed, no reset/rail cycle/ACMD6 rollback/reclock; physical power-cycle required\n",
		reason, error, signal_after, clock_before, clock_after,
		cleanup_confirmed ? 1U : 0U);
}

static void ums9117_reject_request(struct ta1618_sd_mmc *host,
				   struct mmc_request *mrq, int error,
				   const char *reason)
{
	/*
	 * Every caller runs before the first controller write, so neither the
	 * bus nor the card has moved and there is nothing to fail closed on.
	 * The core legitimately issues commands this host refuses, and each
	 * one used to cost the whole slot.
	 */
	dev_warn_ratelimited(
		host->dev,
		"request rejected before MMIO: %s; opcode=%u error=%d\n",
		reason, mrq->cmd->opcode, error);
	ums9117_set_request_error(mrq, error);
	mmc_request_done(host->mmc, mrq);
}

static int ums9117_transition_width4_13mhz(struct ta1618_sd_mmc *host)
{
	unsigned long flags;
	ktime_t deadline;
	u32 clock_after;
	u32 clock_before;
	u32 clock_candidate;
	u32 control_after;
	u32 control_before;
	u32 control_candidate;
	u32 present;
	u32 selector_after;
	u32 selector_before;
	int ret;

	if (!READ_ONCE(host->width_acmd6_clean) ||
	    READ_ONCE(host->physical_width4) ||
	    !READ_ONCE(host->clock_13mhz_deferred))
		return -EPERM;
	ret = ums9117_wait_inhibit(host, true);
	if (ret)
		return ret;

	spin_lock_irqsave(&host->lock, flags);
	if (host->active_mrq || host->finish_mrq) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	if (ta1618_readl(host, RES_INTERRUPT_SIGNAL_ENABLE)) {
		ret = -EIO;
		goto out_unlock;
	}
	present = ta1618_readl(host, RES_PRESENT_STATE);
	if (present & (UMS9117_CMD_INHIBIT | UMS9117_DAT_INHIBIT)) {
		ret = -EBUSY;
		goto out_unlock;
	}

	selector_before = ta1618_readl(host, RES_CLOCK_SELECTOR);
	clock_before = ta1618_readl(host, RES_CLOCK_RESET);
	control_before = ta1618_readl(host, RES_HOST_CONTROL1);
	selector_after = selector_before;
	clock_after = clock_before;
	control_after = control_before;
	host->audit.timeout_field_before =
		FIELD_GET(UMS9117_CLOCK_TIMEOUT_MASK, clock_before);
	host->audit.timeout_register_before = clock_before;
	host->audit.timeout_field_candidate = 0;
	host->audit.timeout_register_candidate = 0;
	host->audit.timeout_field_readback = 0;
	host->audit.timeout_register_readback = 0;
	if ((selector_before & TA1618_SELECTOR_MASK) != TA1618_SELECTOR_26MHZ ||
	    (clock_before & UMS9117_CLOCK_DIVIDER_MASK) !=
		    UMS9117_DIVIDER_ENCODED ||
	    (clock_before & UMS9117_CLOCK_TIMEOUT_MASK) ||
	    !(clock_before & UMS9117_CLOCK_INT_EN) ||
	    !(clock_before & UMS9117_CLOCK_INT_STABLE) ||
	    !(clock_before & UMS9117_CLOCK_CARD_EN) ||
	    (clock_before & (UMS9117_CLOCK_PROG_MODE | UMS9117_CLOCK_PLL_EN |
			     UMS9117_HOST_RESET)) ||
	    (control_before != UMS9117_HOST_CTRL1_1BIT_ADMA2) ||
	    host->actual_clock_hz != UMS9117_CLOCK_HZ) {
		ret = -EPROTONOSUPPORT;
		goto cache_and_unlock;
	}

	/* Gate the card clock before changing any physical width or divider bit. */
	clock_candidate = clock_before & ~UMS9117_CLOCK_CARD_EN;
	ta1618_writel(host, RES_CLOCK_RESET, clock_candidate);
	clock_after = ta1618_readl(host, RES_CLOCK_RESET);
	if (clock_after != clock_candidate) {
		ret = -EIO;
		goto cache_and_unlock;
	}

	/* Stop the internal clock and confirm both enable and stable are clear. */
	clock_candidate &= ~(UMS9117_CLOCK_INT_EN | UMS9117_CLOCK_INT_STABLE);
	ta1618_writel(host, RES_CLOCK_RESET, clock_candidate);
	deadline = ktime_add_ms(ktime_get(), UMS9117_CLOCK_DEADLINE_MS);
	for (;;) {
		clock_after = ta1618_readl(host, RES_CLOCK_RESET);
		if (!(clock_after &
		      (UMS9117_CLOCK_INT_EN | UMS9117_CLOCK_INT_STABLE)) &&
		    clock_after == clock_candidate)
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			ret = -ETIMEDOUT;
			goto cache_and_unlock;
		}
		udelay(UMS9117_POLL_DELAY_US);
	}

	control_candidate = (control_before & ~UMS9117_HOST_CTRL1_OWNED_MASK) |
			    UMS9117_HOST_CTRL1_4BIT_ADMA2;
	ta1618_writel(host, RES_HOST_CONTROL1, control_candidate);
	control_after = ta1618_readl(host, RES_HOST_CONTROL1);
	if (control_after != control_candidate ||
	    (control_after & ~UMS9117_HOST_CTRL1_OWNED_MASK) !=
		    (control_before & ~UMS9117_HOST_CTRL1_OWNED_MASK) ||
	    (control_after & UMS9117_HOST_CTRL1_OWNED_MASK) !=
		    UMS9117_HOST_CTRL1_4BIT_ADMA2) {
		ret = -EIO;
		goto cache_and_unlock;
	}

	clock_candidate = (clock_after & ~(UMS9117_CLOCK_DIVIDER_MASK |
					   UMS9117_CLOCK_TIMEOUT_MASK)) |
			  UMS9117_13MHZ_DIVIDER_ENCODED |
			  UMS9117_CLOCK_TIMEOUT_FPDOOM_ENCODED;
	host->audit.timeout_field_candidate =
		FIELD_GET(UMS9117_CLOCK_TIMEOUT_MASK, clock_candidate);
	host->audit.timeout_register_candidate = clock_candidate;
	ta1618_writel(host, RES_CLOCK_RESET, clock_candidate);
	clock_after = ta1618_readl(host, RES_CLOCK_RESET);
	host->audit.timeout_field_readback =
		FIELD_GET(UMS9117_CLOCK_TIMEOUT_MASK, clock_after);
	host->audit.timeout_register_readback = clock_after;
	if (clock_after != clock_candidate) {
		ret = -EIO;
		goto cache_and_unlock;
	}

	/* Start the custom internal clock and accept only the exact stable state. */
	clock_candidate |= UMS9117_CLOCK_INT_EN;
	ta1618_writel(host, RES_CLOCK_RESET, clock_candidate);
	deadline = ktime_add_ms(ktime_get(), UMS9117_CLOCK_DEADLINE_MS);
	for (;;) {
		clock_after = ta1618_readl(host, RES_CLOCK_RESET);
		if (!(clock_after & UMS9117_CLOCK_INT_EN) ||
		    (clock_after & ~UMS9117_CLOCK_INT_STABLE) !=
			    (clock_candidate & ~UMS9117_CLOCK_INT_STABLE)) {
			ret = -EIO;
			goto cache_and_unlock;
		}
		if (clock_after & UMS9117_CLOCK_INT_STABLE)
			break;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			ret = -ETIMEDOUT;
			goto cache_and_unlock;
		}
		udelay(UMS9117_POLL_DELAY_US);
	}

	clock_candidate = clock_after | UMS9117_CLOCK_CARD_EN;
	ta1618_writel(host, RES_CLOCK_RESET, clock_candidate);
	clock_after = ta1618_readl(host, RES_CLOCK_RESET);
	selector_after = ta1618_readl(host, RES_CLOCK_SELECTOR);
	control_after = ta1618_readl(host, RES_HOST_CONTROL1);
	ret = 0;
	if (clock_after != clock_candidate ||
	    clock_after != UMS9117_13MHZ_OPERATIONAL_CLOCK_EXPECTED ||
	    selector_after != selector_before ||
	    control_after != control_candidate ||
	    (clock_after & UMS9117_CLOCK_DIVIDER_MASK) !=
		    UMS9117_13MHZ_DIVIDER_ENCODED ||
	    FIELD_GET(UMS9117_CLOCK_TIMEOUT_MASK, clock_after) !=
		    UMS9117_CLOCK_TIMEOUT_FPDOOM_VALUE)
		ret = -EIO;

cache_and_unlock:
	host->audit.host_ctrl1_width_before = control_before;
	host->audit.host_ctrl1_width_after = control_after;
	host->audit.selector_width_before = selector_before;
	host->audit.selector_width_after = selector_after;
	host->audit.clock_width_before = clock_before;
	host->audit.clock_width_after = clock_after;
	if (!ret) {
		host->physical_width4 = true;
		host->clock_13mhz_deferred = false;
		host->clock_13mhz_applied = true;
		host->card_clock_on = true;
		host->actual_clock_hz = UMS9117_MAX_CLOCK_HZ;
		host->audit.applied_clock_hz = UMS9117_MAX_CLOCK_HZ;
		host->audit.applied_clock_count++;
	}
out_unlock:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static int ums9117_status_error(u32 status)
{
	if (status & (UMS9117_INT_TIMEOUT | UMS9117_INT_DATA_TIMEOUT))
		return -ETIMEDOUT;
	if (status &
	    (UMS9117_INT_CRC | UMS9117_INT_END_BIT | UMS9117_INT_INDEX |
	     UMS9117_INT_DATA_CRC | UMS9117_INT_DATA_END_BIT))
		return -EILSEQ;
	if (status & (UMS9117_DETAIL_ERROR_MASK | UMS9117_INT_ERROR))
		return -EIO;
	return 0;
}

static int ums9117_response_error(const struct mmc_command *cmd, u32 response)
{
	u32 status;

	if (mmc_resp_type(cmd) != MMC_RSP_R1 &&
	    mmc_resp_type(cmd) != MMC_RSP_R1B)
		return 0;
	status = R1_STATUS(response);
	if (!status)
		return 0;
	if (status & (R1_WP_VIOLATION | R1_CARD_IS_LOCKED | R1_WP_ERASE_SKIP))
		return -EROFS;
	if (status & (R1_OUT_OF_RANGE | R1_ADDRESS_ERROR | R1_BLOCK_LEN_ERROR))
		return -EINVAL;
	if (status & (R1_COM_CRC_ERROR | R1_CARD_ECC_FAILED))
		return -EILSEQ;
	return -EIO;
}

static bool ums9117_is_cmd24_write(const struct mmc_command *cmd)
{
	return cmd && cmd->opcode == MMC_WRITE_BLOCK && cmd->data &&
	       cmd->data->flags == MMC_DATA_WRITE;
}

static bool ums9117_wait_quiescent(struct ta1618_sd_mmc *host,
				   unsigned int deadline_ms, u32 *last_present)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), deadline_ms);
	u32 present;

	for (;;) {
		present = ta1618_readl(host, RES_PRESENT_STATE);
		if (!(present & UMS9117_DATA_ACTIVE_MASK) &&
		    (present & UMS9117_DAT0_LEVEL)) {
			if (last_present)
				*last_present = present;
			return true;
		}
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			if (last_present)
				*last_present = present;
			return false;
		}
		usleep_range(UMS9117_POLL_DELAY_US,
			     UMS9117_POLL_DELAY_US + 20U);
	}
}

static void ums9117_finish_work(struct work_struct *work)
{
	struct ta1618_sd_mmc *host =
		container_of(work, struct ta1618_sd_mmc, finish_work);
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	unsigned long flags;
	u32 status;
	u32 response[4];
	bool ack_failed;
	bool needs_quiesce;
	bool width_acmd6;
	bool write_request;
	bool quiescent = true;
	u32 auto_cmd;
	u32 present = 0;
	unsigned int quiesce_deadline_ms;
	int error;

	cancel_delayed_work_sync(&host->timeout_work);
	spin_lock_irqsave(&host->lock, flags);
	mrq = host->finish_mrq;
	status = host->finish_status;
	memcpy(response, host->finish_response, sizeof(response));
	auto_cmd = host->finish_auto_cmd;
	ack_failed = host->finish_ack_failed;
	needs_quiesce = host->finish_needs_quiesce;
	width_acmd6 = host->finish_width_acmd6;
	spin_unlock_irqrestore(&host->lock, flags);
	if (!mrq)
		return;
	cmd = mrq->cmd;
	write_request = ums9117_is_cmd24_write(cmd);
	quiesce_deadline_ms = write_request ?
				      UMS9117_WRITE_QUIESCE_DEADLINE_MS :
				      UMS9117_READ_QUIESCE_DEADLINE_MS;

	error = ums9117_status_error(status);
	if (!error && ack_failed)
		error = -EIO;
	if (!error && cmd->data &&
	    (status & (UMS9117_INT_RESPONSE | UMS9117_INT_TRANSFER)) !=
		    (UMS9117_INT_RESPONSE | UMS9117_INT_TRANSFER))
		error = -EIO;
	if (!error && !cmd->data && (cmd->flags & MMC_RSP_BUSY) &&
	    (status & (UMS9117_INT_RESPONSE | UMS9117_INT_TRANSFER)) !=
		    (UMS9117_INT_RESPONSE | UMS9117_INT_TRANSFER))
		error = -EIO;
	if (!error && !cmd->data && !(cmd->flags & MMC_RSP_BUSY) &&
	    (cmd->flags & MMC_RSP_PRESENT) && !(status & UMS9117_INT_RESPONSE))
		error = -EIO;
	if (!error)
		error = ums9117_response_error(cmd, response[0]);
	/*
	 * The controller issued the stop command on its own, so its outcome is
	 * reported only through the status bit and the automatic command error
	 * field. Both are consulted: a stop that failed leaves the card in the
	 * data state, and the core has to be told so it can recover.
	 */
	if (mrq->stop && ((status & UMS9117_INT_AUTO_CMD12_ERROR) ||
			  (auto_cmd & UMS9117_AUTO_CMD_ERROR_MASK))) {
		dev_err(host->dev,
			"automatic CMD12 failed: irq_status=0x%08x host_control2=0x%08x opcode=%u\n",
			status, auto_cmd, cmd->opcode);
		if (!error)
			error = -EIO;
	}
	memcpy(cmd->resp, response, sizeof(cmd->resp));
	if (needs_quiesce) {
		quiescent = ums9117_wait_quiescent(host, quiesce_deadline_ms,
						   &present);
		if (!quiescent) {
			/*
			 * The transfer interrupt already arrived, so the
			 * controller is done with the buffer even though the
			 * data lines have not settled. Report the error and
			 * let the request complete: the MMC core has no way
			 * to abandon a request, so returning here would hang
			 * the block layer instead of protecting anything.
			 */
			dev_err(host->dev,
				"transfer quiescence not confirmed: opcode=%u irq_status=0x%08x present_state=0x%08x deadline_ms=%u\n",
				cmd->opcode, status, present,
				quiesce_deadline_ms);
			if (!error)
				error = -ETIMEDOUT;
		}
	}

	if (cmd->opcode == MMC_APP_CMD) {
		spin_lock_irqsave(&host->lock, flags);
		host->audit.cmd55_last_arg = cmd->arg;
		host->audit.cmd55_last_response = response[0];
		if (!error)
			host->audit.cmd55_clean_count++;
		if (!error && (response[0] & R1_APP_CMD)) {
			host->audit.cmd55_app_cmd_count++;
			host->app_cmd_armed = true;
			host->app_cmd_arg = cmd->arg;
		} else {
			host->app_cmd_armed = false;
			host->app_cmd_arg = 0;
		}
		spin_unlock_irqrestore(&host->lock, flags);
	}
	if (width_acmd6) {
		if (error) {
			spin_lock_irqsave(&host->lock, flags);
			host->audit.acmd6_failure_count++;
			spin_unlock_irqrestore(&host->lock, flags);
			ums9117_width_fail_closed(
				host, "admitted ACMD6 did not complete cleanly",
				error);
		} else {
			spin_lock_irqsave(&host->lock, flags);
			host->width_acmd6_clean = true;
			host->audit.acmd6_clean_count++;
			spin_unlock_irqrestore(&host->lock, flags);
		}
	}

	if (host->data_mapped)
		ums9117_unmap_data(host);
	if (error && (ums9117_is_cmd24_write(cmd) ||
		      (READ_ONCE(host->write_status_pending) &&
		       cmd->opcode == MMC_SEND_STATUS))) {
		WRITE_ONCE(host->fatal_error, true);
		dev_err(host->dev,
			"write completion failed; blocking further controller I/O: %d\n",
			error);
	} else if (!error && ums9117_is_cmd24_write(cmd)) {
		WRITE_ONCE(host->write_status_pending, true);
	} else if (!error && READ_ONCE(host->write_status_pending) &&
		   cmd->opcode == MMC_SEND_STATUS &&
		   mmc_ready_for_data(response[0])) {
		WRITE_ONCE(host->write_status_pending, false);
	}
	if (error)
		ums9117_set_request_error(mrq, error);
	else if (cmd->data) {
		cmd->data->error = 0;
		cmd->data->bytes_xfered =
			(unsigned int)cmd->data->blksz * cmd->data->blocks;
	}
	/*
	 * The stop command was never issued by this driver, so its response is
	 * left zero. The core only inspects it when the error is set, and a
	 * zero response trips none of its checks.
	 */
	if (mrq->stop)
		mrq->stop->error = error;
	cmd->error = error;

	spin_lock_irqsave(&host->lock, flags);
	host->finish_width_acmd6 = false;
	host->finish_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static int ums9117_recover(struct ta1618_sd_mmc *host)
{
	int ret;
	u32 control;

	if (READ_ONCE(host->physical_width4) ||
	    READ_ONCE(host->width_acmd6_clean))
		return -EPERM;
	ums9117_mask_and_ack(host);
	ret = ums9117_reset_controller(host);
	if (ret)
		return ret;
	control = ta1618_readl(host, RES_HOST_CONTROL1);
	control &= ~UMS9117_HOST_CTRL1_OWNED_MASK;
	control |= UMS9117_HOST_CTRL1_1BIT_ADMA2;
	ta1618_writel(host, RES_HOST_CONTROL1, control);
	ta1618_writel(host, RES_INTERRUPT_STATUS_ENABLE,
		      UMS9117_STATUS_ENABLE_MASK);
	if (host->rails_on && host->card_clock_on)
		ret = ums9117_set_card_clock(host, true);
	return ret;
}

static void ums9117_timeout_work(struct work_struct *work)
{
	struct ta1618_sd_mmc *host = container_of(
		to_delayed_work(work), struct ta1618_sd_mmc, timeout_work);
	struct mmc_request *mrq;
	unsigned long flags;
	bool post_write_status_timed_out;
	bool width_acmd6_timed_out;
	bool write_timed_out;
	bool write_related;
	bool last_lba_valid = false;
	u32 last_opcode = 0;
	u32 last_argument = 0;
	u32 last_lba = 0;
	u32 last_cmd17_ordinal = 0;
	u32 watchdog_count = 0;
	u32 present = 0;
	int ret;

	spin_lock_irqsave(&host->lock, flags);
	mrq = host->active_mrq;
	width_acmd6_timed_out = mrq && host->active_width_acmd6;
	write_timed_out = mrq && ums9117_is_cmd24_write(mrq->cmd);
	post_write_status_timed_out = mrq &&
				      READ_ONCE(host->write_status_pending) &&
				      mrq->cmd->opcode == MMC_SEND_STATUS;
	write_related = write_timed_out || post_write_status_timed_out;
	if (mrq) {
		/* Win the IRQ race and retain this request until handling is safe. */
		host->audit.watchdog_timeout_count++;
		watchdog_count = host->audit.watchdog_timeout_count;
		last_opcode = host->audit.last_opcode;
		last_argument = host->audit.last_argument;
		last_lba = host->audit.last_lba;
		last_lba_valid = host->audit.last_lba_valid;
		last_cmd17_ordinal = host->audit.last_cmd17_ordinal;
		ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
		host->active_mrq = NULL;
		host->finish_mrq = mrq;
		host->finish_width_acmd6 = host->active_width_acmd6;
		host->active_width_acmd6 = false;
		if (write_related)
			WRITE_ONCE(host->fatal_error, true);
	}
	spin_unlock_irqrestore(&host->lock, flags);
	if (!mrq)
		return;
	dev_err(host->dev,
		"software request watchdog fired: count=%u opcode=%u arg=0x%08x lba_valid=%u lba=%u cmd17_ordinal=%u deadline_ms=%u\n",
		watchdog_count, last_opcode, last_argument,
		last_lba_valid ? 1U : 0U, last_lba, last_cmd17_ordinal,
		write_timed_out ? UMS9117_WRITE_REQUEST_TIMEOUT_MS :
				  UMS9117_REQUEST_TIMEOUT_MS);
	synchronize_irq(host->irq);

	if (width_acmd6_timed_out) {
		spin_lock_irqsave(&host->lock, flags);
		host->audit.acmd6_failure_count++;
		spin_unlock_irqrestore(&host->lock, flags);
		ums9117_width_fail_closed(host, "admitted ACMD6 timed out",
					  -ETIMEDOUT);
		ums9117_set_request_error(mrq, -ETIMEDOUT);
		spin_lock_irqsave(&host->lock, flags);
		host->finish_width_acmd6 = false;
		host->finish_mrq = NULL;
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(host->mmc, mrq);
		return;
	}

	if (write_related) {
		if (!ums9117_wait_quiescent(
			    host, UMS9117_WRITE_QUIESCE_DEADLINE_MS, &present))
			dev_err(host->dev,
				"write-related timeout without confirmed quiescence: present_state=0x%08x; completing with an error anyway\n",
				present);
		if (write_timed_out)
			dev_err(host->dev,
				"CMD25 request timed out after admission; quiescence confirmed, completing with error and blocking controller I/O until physical power-cycle\n");
		else
			dev_err(host->dev,
				"post-write CMD13 timed out; data lines quiescent, completing with error and blocking controller I/O until physical power-cycle\n");
		if (host->data_mapped)
			ums9117_unmap_data(host);
		ums9117_set_request_error(mrq, -ETIMEDOUT);
		spin_lock_irqsave(&host->lock, flags);
		host->finish_width_acmd6 = false;
		host->finish_mrq = NULL;
		spin_unlock_irqrestore(&host->lock, flags);
		mmc_request_done(host->mmc, mrq);
		return;
	}

	if (READ_ONCE(host->width_acmd6_clean)) {
		/*
		 * Only a bus that never settles justifies gating the card
		 * clock for good. If the lines are quiet the card and the
		 * controller are in a known state, so refusing further I/O
		 * until the core power-cycles the slot is enough.
		 */
		if (mrq->cmd->data &&
		    !ums9117_wait_quiescent(host,
					    UMS9117_WRITE_QUIESCE_DEADLINE_MS,
					    &present)) {
			ums9117_width_fail_closed(
				host, "timed-out transfer never quiesced",
				-ETIMEDOUT);
		} else {
			/*
			 * Quiet lines mean the card and the controller are in
			 * a known state. The core recovers a stalled transfer
			 * on its own with CMD13, then CMD12, and finally a
			 * power cycle of the slot, so one late transfer is no
			 * reason to refuse every later request as well.
			 */
			dev_err(host->dev,
				"request timed out after accepted ACMD6; data lines quiescent, completing with an error and leaving the slot usable\n");
		}
	} else {
		mutex_lock(&host->state_mutex);
		ret = ums9117_recover(host);
		if (ret) {
			WRITE_ONCE(host->fatal_error, true);
			dev_err(host->dev,
				"request timeout recovery failed: %d\n", ret);
		}
		mutex_unlock(&host->state_mutex);
	}

	if (host->data_mapped)
		ums9117_unmap_data(host);
	ums9117_set_request_error(mrq, -ETIMEDOUT);
	spin_lock_irqsave(&host->lock, flags);
	host->finish_width_acmd6 = false;
	host->finish_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);
	mmc_request_done(host->mmc, mrq);
}

static irqreturn_t ums9117_irq(int irq, void *data)
{
	struct ta1618_sd_mmc *host = data;
	struct mmc_request *mrq;
	struct mmc_command *cmd;
	unsigned long flags;
	u32 status;
	u32 owned;
	u32 readback;
	u32 auto_cmd;
	u32 raw[4];
	bool terminal;

	if (irq != host->irq)
		return IRQ_NONE;
	status = ta1618_readl(host, RES_INTERRUPT_STATUS);
	owned = status & UMS9117_OWNED_STATUS_MASK;
	if (!owned)
		return IRQ_NONE;

	/* SPI57 is level-high: mask the controller before ACKing owned W1C bits. */
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	/*
	 * The automatic command error bits are cleared together with the
	 * interrupt status bit that reports them, so this has to be sampled
	 * before the acknowledge below or it would always read back clean.
	 */
	auto_cmd = ta1618_readl(host, RES_HOST_CONTROL2);
	ta1618_writel(host, RES_INTERRUPT_STATUS, owned);
	readback = ta1618_readl(host, RES_INTERRUPT_STATUS);

	spin_lock_irqsave(&host->lock, flags);
	ums9117_audit_irq_locked(host, status, owned, readback);
	if (status & (UMS9117_INT_TIMEOUT | UMS9117_INT_DATA_TIMEOUT))
		dev_err(host->dev,
			"hardware timeout IRQ: command=%u data=%u raw=0x%08x owned=0x%08x w1c_readback=0x%08x opcode=%u arg=0x%08x lba_valid=%u lba=%u cmd17_ordinal=%u\n",
			(status & UMS9117_INT_TIMEOUT) ? 1U : 0U,
			(status & UMS9117_INT_DATA_TIMEOUT) ? 1U : 0U, status,
			owned, readback, host->audit.last_opcode,
			host->audit.last_argument,
			host->audit.last_lba_valid ? 1U : 0U,
			host->audit.last_lba, host->audit.last_cmd17_ordinal);
	mrq = host->active_mrq;
	if (!mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		return IRQ_HANDLED;
	}
	cmd = mrq->cmd;
	terminal = !!(status & (UMS9117_DETAIL_ERROR_MASK | UMS9117_INT_ERROR));
	if (cmd->data || (cmd->flags & MMC_RSP_BUSY))
		terminal |= !!(status & UMS9117_INT_TRANSFER);
	else
		terminal |= !!(status & UMS9117_INT_RESPONSE);
	if (!terminal) {
		ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE,
			      cmd->data || (cmd->flags & MMC_RSP_BUSY) ?
				      UMS9117_SIGNAL_DATA :
				      UMS9117_SIGNAL_COMMAND);
		spin_unlock_irqrestore(&host->lock, flags);
		return IRQ_HANDLED;
	}

	raw[0] = ta1618_readl(host, RES_RESPONSE0);
	raw[1] = ta1618_readl(host, RES_RESPONSE1);
	raw[2] = ta1618_readl(host, RES_RESPONSE2);
	raw[3] = ta1618_readl(host, RES_RESPONSE3);
	if (cmd->flags & MMC_RSP_136) {
		host->finish_response[0] = (raw[3] << 8) | (raw[2] >> 24);
		host->finish_response[1] = (raw[2] << 8) | (raw[1] >> 24);
		host->finish_response[2] = (raw[1] << 8) | (raw[0] >> 24);
		host->finish_response[3] = raw[0] << 8;
	} else {
		host->finish_response[0] = raw[0];
		host->finish_response[1] = 0;
		host->finish_response[2] = 0;
		host->finish_response[3] = 0;
	}
	host->finish_status = status;
	host->finish_auto_cmd = auto_cmd;
	host->finish_ack_failed = !!(readback & owned);
	host->finish_needs_quiesce = cmd->data || (cmd->flags & MMC_RSP_BUSY);
	host->finish_width_acmd6 = host->active_width_acmd6;
	host->active_width_acmd6 = false;
	host->finish_mrq = mrq;
	host->active_mrq = NULL;
	spin_unlock_irqrestore(&host->lock, flags);

	cancel_delayed_work(&host->timeout_work);
	schedule_work(&host->finish_work);
	return IRQ_HANDLED;
}

static void ums9117_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ta1618_sd_mmc *host = mmc_priv(mmc);
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_data *data = cmd->data;
	unsigned long flags;
	u32 stale;
	u32 clock;
	u32 control;
	u32 selector;
	u32 signal;
	u16 command_flags;
	u16 command;
	u16 transfer = 0;
	u32 app_cmd_arg = 0;
	bool app_context = false;
	bool cmd24_write;
	bool multi_read;
	bool post_write_status;
	bool width_acmd6;
	int ret;

	if (READ_ONCE(host->width_switch_fatal)) {
		ums9117_set_request_error(mrq, -EIO);
		mmc_request_done(mmc, mrq);
		return;
	}
	spin_lock_irqsave(&host->lock, flags);
	if (cmd->opcode != MMC_APP_CMD) {
		app_context = host->app_cmd_armed;
		app_cmd_arg = host->app_cmd_arg;
		host->app_cmd_armed = false;
		host->app_cmd_arg = 0;
	}
	spin_unlock_irqrestore(&host->lock, flags);
	width_acmd6 = app_context && (app_cmd_arg & 0xffff0000U) &&
		      !(app_cmd_arg & 0x0000ffffU) &&
		      cmd->opcode == SD_APP_SET_BUS_WIDTH && !data &&
		      cmd->arg == SD_BUS_WIDTH_4;
	if ((cmd->opcode == SD_APP_SET_BUS_WIDTH && !data && !width_acmd6) ||
	    (width_acmd6 &&
	     (READ_ONCE(host->physical_width4) ||
	      READ_ONCE(host->width_acmd6_clean) ||
	      !READ_ONCE(host->clock_13mhz_deferred) ||
	      READ_ONCE(host->actual_clock_hz) != UMS9117_CLOCK_HZ ||
	      !READ_ONCE(host->card_clock_on)))) {
		ums9117_reject_request(
			host, mrq, -EPROTO,
			"application-command qualification rejected request");
		return;
	}
	post_write_status = READ_ONCE(host->write_status_pending) &&
			    cmd->opcode == MMC_SEND_STATUS && !data;
	if (READ_ONCE(host->write_status_pending) && !post_write_status) {
		WRITE_ONCE(host->fatal_error, true);
		ums9117_reject_request(
			host, mrq, -EIO,
			"post-CMD24 status sequencing rejected request");
		return;
	}
	cmd24_write = cmd->opcode == MMC_WRITE_BLOCK && data &&
		      data->flags == MMC_DATA_WRITE && data->blocks == 1 &&
		      data->blksz == 512 && data->sg_len == 1;
	multi_read = ums9117_is_multi_read(mrq, mmc);
	if ((cmd->opcode == MMC_READ_SINGLE_BLOCK ||
	     cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
	     cmd->opcode == MMC_WRITE_BLOCK) &&
	    (!READ_ONCE(host->physical_width4) ||
	     !READ_ONCE(host->clock_13mhz_applied) ||
	     READ_ONCE(host->actual_clock_hz) != UMS9117_MAX_CLOCK_HZ)) {
		ums9117_reject_request(
			host, mrq, -EPROTO,
			"block I/O requested before physical 4-bit 13 MHz state");
		return;
	}
	if (ums9117_destructive_command(cmd) ||
	    (cmd->opcode == MMC_WRITE_BLOCK && !cmd24_write) ||
	    (cmd->opcode == MMC_READ_MULTIPLE_BLOCK && !multi_read) ||
	    (data && (data->flags & MMC_DATA_WRITE) && !cmd24_write)) {
		ums9117_reject_request(
			host, mrq, -EOPNOTSUPP,
			"forbidden data or destructive opcode rejected before MMIO");
		return;
	}
	if (mrq->sbc || (mrq->stop && !multi_read) || cmd->opcode > 63U) {
		ums9117_reject_request(
			host, mrq, -EOPNOTSUPP,
			"SBC, STOP, or invalid opcode rejected before MMIO");
		return;
	}
	ret = ums9117_response_flags(cmd, &command_flags);
	if (ret) {
		ums9117_reject_request(
			host, mrq, ret,
			"unsupported response type rejected before MMIO");
		return;
	}
	if (data) {
		ret = ums9117_prepare_data(host, cmd);
		if (ret) {
			ums9117_reject_request(
				host, mrq, ret,
				"data mapping or shape rejected before MMIO");
			return;
		}
		if (multi_read)
			transfer = UMS9117_TRANSFER_CMD18_AUTO_CMD12_ADMA2;
		else
			transfer = data->flags == MMC_DATA_WRITE ?
					   UMS9117_TRANSFER_WRITE_ADMA2 :
					   UMS9117_TRANSFER_READ_ADMA2;
	}
	if (READ_ONCE(host->fatal_error) || READ_ONCE(host->stopping) ||
	    !READ_ONCE(host->rails_on) || !READ_ONCE(host->card_clock_on)) {
		ret = -EIO;
		goto out_error;
	}
	ret = ums9117_wait_inhibit(host, data || (cmd->flags & MMC_RSP_BUSY));
	if (ret)
		goto out_error;

	spin_lock_irqsave(&host->lock, flags);
	if (host->active_mrq || host->finish_mrq) {
		spin_unlock_irqrestore(&host->lock, flags);
		ret = -EBUSY;
		goto out_error;
	}
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	if (width_acmd6 && ta1618_readl(host, RES_INTERRUPT_SIGNAL_ENABLE)) {
		spin_unlock_irqrestore(&host->lock, flags);
		ret = -EIO;
		goto out_error;
	}
	if (width_acmd6 ||
	    (cmd->opcode == MMC_APP_CMD && !host->physical_width4)) {
		selector = ta1618_readl(host, RES_CLOCK_SELECTOR);
		clock = ta1618_readl(host, RES_CLOCK_RESET);
		control = ta1618_readl(host, RES_HOST_CONTROL1);
		if ((selector & TA1618_SELECTOR_MASK) !=
			    TA1618_SELECTOR_26MHZ ||
		    (clock & UMS9117_CLOCK_DIVIDER_MASK) !=
			    UMS9117_DIVIDER_ENCODED ||
		    !(clock & UMS9117_CLOCK_INT_EN) ||
		    !(clock & UMS9117_CLOCK_INT_STABLE) ||
		    !(clock & UMS9117_CLOCK_CARD_EN) ||
		    (clock & (UMS9117_CLOCK_PROG_MODE | UMS9117_CLOCK_PLL_EN |
			      UMS9117_HOST_RESET)) ||
		    control != UMS9117_HOST_CTRL1_1BIT_ADMA2 ||
		    host->actual_clock_hz != UMS9117_CLOCK_HZ) {
			spin_unlock_irqrestore(&host->lock, flags);
			ret = -EPROTO;
			goto out_error;
		}
	}
	if (cmd->opcode == MMC_READ_SINGLE_BLOCK ||
	    cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
	    cmd->opcode == MMC_WRITE_BLOCK) {
		selector = ta1618_readl(host, RES_CLOCK_SELECTOR);
		clock = ta1618_readl(host, RES_CLOCK_RESET);
		control = ta1618_readl(host, RES_HOST_CONTROL1);
		if ((selector & TA1618_SELECTOR_MASK) !=
			    TA1618_SELECTOR_26MHZ ||
		    (clock & UMS9117_CLOCK_DIVIDER_MASK) !=
			    UMS9117_13MHZ_DIVIDER_ENCODED ||
		    !(clock & UMS9117_CLOCK_INT_EN) ||
		    !(clock & UMS9117_CLOCK_INT_STABLE) ||
		    !(clock & UMS9117_CLOCK_CARD_EN) ||
		    (clock & (UMS9117_CLOCK_PROG_MODE | UMS9117_CLOCK_PLL_EN |
			      UMS9117_HOST_RESET)) ||
		    control != UMS9117_HOST_CTRL1_4BIT_ADMA2 ||
		    host->actual_clock_hz != UMS9117_MAX_CLOCK_HZ) {
			spin_unlock_irqrestore(&host->lock, flags);
			ret = -EPROTO;
			goto out_error;
		}
	}
	/*
	 * The block count register is only 32 bits wide in host version 4
	 * mode, and the controller only issues an automatic CMD12 there. The
	 * mode is inherited from the boot loader and never programmed here, so
	 * it is confirmed immediately before every multi-block transfer rather
	 * than trusted from probe time.
	 */
	if (multi_read && ta1618_readl(host, RES_HOST_CONTROL2) !=
				  UMS9117_HOST_CTRL2_EXPECTED) {
		spin_unlock_irqrestore(&host->lock, flags);
		ret = -EPROTO;
		goto out_error;
	}
	stale = ta1618_readl(host, RES_INTERRUPT_STATUS) &
		UMS9117_OWNED_STATUS_MASK;
	if (stale)
		ta1618_writel(host, RES_INTERRUPT_STATUS, stale);
	if (ta1618_readl(host, RES_INTERRUPT_STATUS) & stale) {
		spin_unlock_irqrestore(&host->lock, flags);
		ret = -EIO;
		goto out_error;
	}
	ta1618_writel(host, RES_INTERRUPT_STATUS_ENABLE,
		      UMS9117_STATUS_ENABLE_MASK);
	if (data) {
		control = ta1618_readl(host, RES_HOST_CONTROL1);
		control &= ~UMS9117_HOST_CTRL1_DMA_SEL_MASK;
		control |= UMS9117_HOST_CTRL1_ADMA2;
		ta1618_writel(host, RES_HOST_CONTROL1, control);
		ta1618_writel(host, RES_BLOCK_COUNT, data->blocks);
		ta1618_writel(host, RES_BLOCK_SIZE, data->blksz);
		dma_wmb();
		/* UMS9117 qualification requires high then low ADMA address writes. */
		ta1618_writel(host, RES_ADMA_ADDRESS_HIGH, 0);
		ta1618_writel(host, RES_ADMA_ADDRESS_LOW,
			      (u32)host->descriptors_dma);
		command_flags |= UMS9117_CMD_DATA;
		signal = UMS9117_SIGNAL_DATA;
	} else {
		command_flags |= UMS9117_SUB_CMD_FLAG;
		signal = cmd->flags & MMC_RSP_BUSY ? UMS9117_SIGNAL_DATA :
						     UMS9117_SIGNAL_COMMAND;
	}
	command = (cmd->opcode << 8) | command_flags;
	host->active_mrq = mrq;
	host->active_width_acmd6 = width_acmd6;
	ums9117_audit_command_locked(host, cmd, width_acmd6);
	ta1618_writel(host, RES_ARGUMENT, cmd->arg);
	schedule_delayed_work(
		&host->timeout_work,
		msecs_to_jiffies(cmd24_write ?
					 UMS9117_WRITE_REQUEST_TIMEOUT_MS :
					 UMS9117_REQUEST_TIMEOUT_MS));
	ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, signal);
	if (data)
		ta1618_writel(host, RES_TRANSFER_COMMAND,
			      ((u32)command << 16) | transfer);
	else
		writew(command, host->regs[RES_TRANSFER_COMMAND] +
					UMS9117_COMMAND_OFFSET);
	spin_unlock_irqrestore(&host->lock, flags);
	return;

out_error:
	if (host->data_mapped) {
		host->active_mrq = mrq;
		ums9117_unmap_data(host);
		host->active_mrq = NULL;
	}
	ums9117_set_request_error(mrq, ret);
	mmc_request_done(mmc, mrq);
}

static int ums9117_set_host_1bit_powered_off(struct ta1618_sd_mmc *host)
{
	unsigned long flags;
	u32 candidate;
	u32 control;
	int ret = 0;

	spin_lock_irqsave(&host->lock, flags);
	if (host->active_mrq || host->finish_mrq) {
		ret = -EBUSY;
		goto out_unlock;
	}
	control = ta1618_readl(host, RES_HOST_CONTROL1);
	candidate = (control & ~UMS9117_HOST_CTRL1_OWNED_MASK) |
		    UMS9117_HOST_CTRL1_1BIT_ADMA2;
	ta1618_writel(host, RES_HOST_CONTROL1, candidate);
	if (ta1618_readl(host, RES_HOST_CONTROL1) != candidate) {
		ret = -EIO;
		goto out_unlock;
	}
	host->physical_width4 = false;
	host->width_acmd6_clean = false;
	host->clock_13mhz_deferred = false;
	host->clock_13mhz_applied = false;
	host->actual_clock_hz = 0;
	host->app_cmd_armed = false;
	host->app_cmd_arg = 0;
out_unlock:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static void ums9117_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ta1618_sd_mmc *host = mmc_priv(mmc);
	unsigned long flags;
	int ret = 0;

	mutex_lock(&host->state_mutex);
	spin_lock_irqsave(&host->lock, flags);
	host->audit.requested_clock_hz = ios->clock;
	spin_unlock_irqrestore(&host->lock, flags);
	if (host->stopping)
		goto out_trace;
	if (READ_ONCE(host->width_switch_fatal) &&
	    ios->power_mode != MMC_POWER_OFF) {
		ret = -ESHUTDOWN;
		dev_err(host->dev,
			"set_ios refused after the 4-bit fail-closed; only a power cycle clears it\n");
		goto out_trace;
	}
	if ((ios->bus_width != MMC_BUS_WIDTH_1 &&
	     ios->bus_width != MMC_BUS_WIDTH_4) ||
	    ios->timing != MMC_TIMING_LEGACY ||
	    (ios->clock && ios->clock != UMS9117_MAX_CLOCK_HZ &&
	     (ios->clock < UMS9117_CLOCK_HZ ||
	      ios->clock > UMS9117_IDENT_REQUEST_MAX_HZ))) {
		ret = -EOPNOTSUPP;
		goto out_fail;
	}

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		if (host->platform_active) {
			ums9117_mask_and_ack(host);
			ret = ums9117_set_card_clock(host, false);
			if (!ret)
				ret = ta1618_set_rails(host, false);
			if (!ret)
				ret = ums9117_set_host_1bit_powered_off(host);
		}
		if (!ret) {
			/*
			 * The rails are down and the host is back in 1-bit
			 * ADMA2, so the card and the controller can no longer
			 * disagree about the bus width. That is the whole
			 * reason the latches existed; clearing them here is
			 * what lets the core recover a slot by power-cycling
			 * it instead of needing the battery pulled.
			 */
			spin_lock_irqsave(&host->lock, flags);
			host->fatal_error = false;
			host->width_switch_fatal = false;
			host->terminal_cleanup_hold = false;
			spin_unlock_irqrestore(&host->lock, flags);
		}
		break;
	case MMC_POWER_UP:
		if (ios->bus_width != MMC_BUS_WIDTH_1) {
			ret = -EPROTO;
			break;
		}
		ret = ta1618_activate_platform(host);
		break;
	case MMC_POWER_ON:
		if (!host->platform_active || !host->rails_on) {
			ret = -EIO;
			break;
		}
		if (ios->bus_width == MMC_BUS_WIDTH_1) {
			if (host->width_acmd6_clean || host->physical_width4 ||
			    host->clock_13mhz_applied) {
				ret = -EPROTO;
				break;
			}
			if (ios->clock && !host->card_clock_on)
				ret = ums9117_set_card_clock(host, true);
			else if (!ios->clock && host->card_clock_on)
				ret = ums9117_set_card_clock(host, false);
			if (ret)
				break;
			if (ios->clock &&
			    host->actual_clock_hz != UMS9117_CLOCK_HZ) {
				ret = -EPROTO;
				break;
			}
			if (ios->clock == UMS9117_MAX_CLOCK_HZ) {
				host->clock_13mhz_deferred = true;
				spin_lock_irqsave(&host->lock, flags);
				host->audit.deferred_clock_hz =
					UMS9117_MAX_CLOCK_HZ;
				host->audit.deferred_clock_count++;
				spin_unlock_irqrestore(&host->lock, flags);
			} else {
				host->clock_13mhz_deferred = false;
			}
		} else {
			if (!host->width_acmd6_clean || !host->card_clock_on ||
			    ios->clock != UMS9117_MAX_CLOCK_HZ) {
				ret = -EPROTO;
				break;
			}
			if (!host->physical_width4)
				ret = ums9117_transition_width4_13mhz(host);
			else if (!host->clock_13mhz_applied ||
				 host->actual_clock_hz != UMS9117_MAX_CLOCK_HZ)
				ret = -EPROTO;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret)
		goto out_trace;

out_fail:
	if (READ_ONCE(host->width_acmd6_clean)) {
		ums9117_width_fail_closed(
			host, "set_ios failed after accepted ACMD6", ret);
		goto out_trace;
	}
	host->fatal_error = true;
	dev_err(host->dev,
		"set_ios failed: power=%u clock=%u width=%u timing=%u actual_clock=%u error=%d\n",
		ios->power_mode, ios->clock, ios->bus_width, ios->timing,
		host->actual_clock_hz, ret);
	ta1618_restore_platform(host);
out_trace:
	ums9117_record_ios(host, ios, ret);
	mutex_unlock(&host->state_mutex);
}

static int ums9117_get_cd(struct mmc_host *mmc)
{
	/*
	 * This reports card presence, which the board can detect but this
	 * host never samples, so a card is assumed. Reporting absence to
	 * signal a driver problem would be worse than useless: the core
	 * answers it by powering the slot off and giving up, which is
	 * exactly the recovery that a failed slot needs. A card that is
	 * really gone still surfaces through the periodic status command.
	 */
	(void)mmc;
	return 1;
}

static int ums9117_get_ro(struct mmc_host *mmc)
{
	/*
	 * The slot has no write-protect input to read. A card that is
	 * physically locked still stays read-only, because the core also
	 * requires the block-write command class from the card itself.
	 */
	(void)mmc;
	return 0;
}

static int ums9117_multi_io_quirk(struct mmc_card *card, unsigned int direction,
				  int blk_size)
{
	/*
	 * The core takes this answer as the block count for the request, so
	 * returning one is what confines every transfer to a single block.
	 * Reads are released up to the host limit; writes still are not.
	 */
	if (direction == MMC_DATA_READ)
		return min_t(int, blk_size, card->host->max_blk_count);
	return 1;
}

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ta1618_sd_mmc *host = dev_get_drvdata(dev);
	struct ta1618_mmc_audit audit;
	unsigned long flags;
	bool active;
	bool app_cmd_armed;
	bool fatal;
	bool finish;
	bool clock_13mhz_applied;
	bool clock_13mhz_deferred;
	bool physical_width4;
	bool terminal_cleanup_hold;
	bool width_acmd6_clean;
	bool width_switch_fatal;
	u32 actual_clock_hz;
	unsigned int i;
	ssize_t len = 0;

	(void)attr;
	spin_lock_irqsave(&host->lock, flags);
	audit = host->audit;
	active = host->active_mrq != NULL;
	finish = host->finish_mrq != NULL;
	app_cmd_armed = host->app_cmd_armed;
	width_acmd6_clean = host->width_acmd6_clean;
	physical_width4 = host->physical_width4;
	clock_13mhz_deferred = host->clock_13mhz_deferred;
	clock_13mhz_applied = host->clock_13mhz_applied;
	actual_clock_hz = host->actual_clock_hz;
	fatal = host->fatal_error;
	width_switch_fatal = host->width_switch_fatal;
	terminal_cleanup_hold = host->terminal_cleanup_hold;
	spin_unlock_irqrestore(&host->lock, flags);

	len += sysfs_emit_at(
		buf, len,
		"schema=fplinux.ta1618-sd-perf-4bit-13mhz-audit/v3\n");
	len += sysfs_emit_at(
		buf, len,
		"profile=sd-perf-4bit-13mhz-lab identification_width=1 read_only=0 caps_4bit=1 caps_highspeed=0 caps_uhs=0 caps_cmd23=0 caps_8bit=0 voltage_switch=0 tuning=0\n");
	len += sysfs_emit_at(
		buf, len,
		"clock_ident=selector:0x%08x,divider:0x%08x,actual_hz:%u,clock_reset:0x%08x\n",
		audit.selector_after_activate, UMS9117_DIVIDER_ENCODED,
		UMS9117_CLOCK_HZ, audit.clock_reset_after_activate);
	len += sysfs_emit_at(
		buf, len,
		"clock_state=requested_hz:%u,deferred_hz:%u,deferred_count:%u,applied_hz:%u,applied_count:%u,actual_hz:%u,deferred:%u,applied_13mhz:%u,max_hz:%u,candidate_divider:0x%08x\n",
		audit.requested_clock_hz, audit.deferred_clock_hz,
		audit.deferred_clock_count, audit.applied_clock_hz,
		audit.applied_clock_count, actual_clock_hz,
		clock_13mhz_deferred ? 1U : 0U, clock_13mhz_applied ? 1U : 0U,
		UMS9117_MAX_CLOCK_HZ, UMS9117_13MHZ_DIVIDER_ENCODED);
	len += sysfs_emit_at(
		buf, len,
		"cmd55=attempts:%u,clean:%u,app_cmd:%u,last_arg:0x%08x,last_response:0x%08x,armed:%u\n",
		audit.cmd55_attempt_count, audit.cmd55_clean_count,
		audit.cmd55_app_cmd_count, audit.cmd55_last_arg,
		audit.cmd55_last_response, app_cmd_armed ? 1U : 0U);
	len += sysfs_emit_at(
		buf, len,
		"acmd6=attempts:%u,clean:%u,failures:%u,arg:0x%08x,host_generated:0\n",
		audit.acmd6_attempt_count, audit.acmd6_clean_count,
		audit.acmd6_failure_count, SD_BUS_WIDTH_4);
	len += sysfs_emit_at(
		buf, len,
		"width=acmd6_clean:%u,physical_4bit:%u,host_ctrl1_before:0x%08x,host_ctrl1_after:0x%08x,owned_mask:0x%08x,owned_1bit:0x%08x,owned_4bit:0x%08x\n",
		width_acmd6_clean ? 1U : 0U, physical_width4 ? 1U : 0U,
		audit.host_ctrl1_width_before, audit.host_ctrl1_width_after,
		(u32)UMS9117_HOST_CTRL1_OWNED_MASK,
		UMS9117_HOST_CTRL1_1BIT_ADMA2, UMS9117_HOST_CTRL1_4BIT_ADMA2);
	len += sysfs_emit_at(
		buf, len,
		"width_clock=selector_before:0x%08x,selector_after:0x%08x,clock_before:0x%08x,clock_after:0x%08x\n",
		audit.selector_width_before, audit.selector_width_after,
		audit.clock_width_before, audit.clock_width_after);
	len += sysfs_emit_at(
		buf, len,
		"timeout_program=mask:0x%08x,fpdoom_value:%u,before_field:%u,before_register:0x%08x,candidate_field:%u,candidate_register:0x%08x,readback_field:%u,readback_register:0x%08x,operational_expected:0x%08x,operational_readback:0x%08x\n",
		(u32)UMS9117_CLOCK_TIMEOUT_MASK,
		UMS9117_CLOCK_TIMEOUT_FPDOOM_VALUE, audit.timeout_field_before,
		audit.timeout_register_before, audit.timeout_field_candidate,
		audit.timeout_register_candidate, audit.timeout_field_readback,
		audit.timeout_register_readback,
		UMS9117_13MHZ_OPERATIONAL_CLOCK_EXPECTED,
		audit.clock_width_after);
	len += sysfs_emit_at(
		buf, len,
		"commands=cmd17:%u,cmd24:%u,cmd18:%u,cmd25:%u,max_blocks:%u\n",
		audit.cmd17_count, audit.cmd24_count, audit.cmd18_count,
		audit.cmd25_count, audit.max_data_blocks);
	len += sysfs_emit_at(buf, len,
			     "descriptors=table:%u,capacity:%u,max_used:%u\n",
			     (u32)UMS9117_ADMA2_TABLE_SIZE,
			     (u32)UMS9117_ADMA2_DESC_COUNT,
			     audit.max_descriptors);
	len += sysfs_emit_at(
		buf, len,
		"last_command=valid:%u,opcode:%u,arg:0x%08x,lba_valid:%u,lba:%u,cmd17_ordinal:%u\n",
		audit.last_command_valid ? 1U : 0U, audit.last_opcode,
		audit.last_argument, audit.last_lba_valid ? 1U : 0U,
		audit.last_lba, audit.last_cmd17_ordinal);
	len += sysfs_emit_at(
		buf, len,
		"irq=count:%u,error:%u,crc:%u,end_bit:%u,timeout:%u,adma:%u,auto_cmd12:%u\n",
		audit.irq_count, audit.irq_error_count, audit.irq_crc_count,
		audit.irq_end_bit_count, audit.irq_timeout_count,
		audit.irq_adma_error_count, audit.irq_auto_cmd12_count);
	len += sysfs_emit_at(
		buf, len, "host_control2=after_reset:0x%08x,expected:0x%08x\n",
		audit.host_control2_after_reset, UMS9117_HOST_CTRL2_EXPECTED);
	len += sysfs_emit_at(
		buf, len,
		"timeouts=command_irq:%u,command_mask:0x%08x,data_irq:%u,data_mask:0x%08x,software_watchdog:%u\n",
		audit.irq_command_timeout_count, UMS9117_INT_TIMEOUT,
		audit.irq_data_timeout_count, UMS9117_INT_DATA_TIMEOUT,
		audit.watchdog_timeout_count);
	len += sysfs_emit_at(
		buf, len,
		"irq_last=raw_status:0x%08x,owned_status:0x%08x,w1c_readback:0x%08x\n",
		audit.last_irq_raw_status, audit.last_irq_owned_status,
		audit.last_irq_w1c_readback);
	len += sysfs_emit_at(
		buf, len,
		"state=active:%u,finish:%u,fatal:%u,width_fail_closed:%u,terminal_cleanup_hold:%u\n",
		active ? 1U : 0U, finish ? 1U : 0U, fatal ? 1U : 0U,
		width_switch_fatal ? 1U : 0U, terminal_cleanup_hold ? 1U : 0U);
	len += sysfs_emit_at(buf, len, "ios_trace=count:%u,total:%u\n",
			     audit.ios_trace_count, audit.ios_calls);
	for (i = 0; i < TA1618_IOS_TRACE_DEPTH && len < PAGE_SIZE; i++) {
		const struct ta1618_ios_trace_entry *entry = &audit.ios[i];

		if (!entry->sequence)
			continue;
		len += sysfs_emit_at(
			buf, len,
			"ios[%u]=sequence:%u,power:%u,clock:%u,width:%u,timing:%u,result:%d\n",
			i, entry->sequence, entry->power_mode, entry->clock,
			entry->bus_width, entry->timing, entry->result);
	}
	return len;
}
static DEVICE_ATTR_RO(audit);

static const struct mmc_host_ops ums9117_mmc_ops = {
	.request = ums9117_request,
	.set_ios = ums9117_set_ios,
	.get_cd = ums9117_get_cd,
	.get_ro = ums9117_get_ro,
	.multi_io_quirk = ums9117_multi_io_quirk,
};

static int ta1618_validate_resource(struct platform_device *pdev,
				    unsigned int index,
				    struct resource **validated)
{
	const struct ta1618_resource_definition *definition =
		&ta1618_resources[index];
	struct resource *resource =
		platform_get_resource(pdev, IORESOURCE_MEM, index);

	if (!resource || resource_type(resource) != IORESOURCE_MEM ||
	    !resource->name || strcmp(resource->name, definition->name) ||
	    resource->start != definition->address ||
	    resource_size(resource) != definition->size ||
	    !IS_ALIGNED(resource->start, definition->size))
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"resource %u must be %s at %pa with exact size %pa\n",
			index, definition->name, &definition->address,
			&definition->size);
	*validated = resource;
	return 0;
}

static int ums9117_ta1618_mmc_probe(struct platform_device *pdev)
{
	struct resource *resources[RES_COUNT];
	struct ta1618_sd_mmc *host;
	struct mmc_host *mmc;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	unsigned int i;
	u32 trigger;
	int irq;
	int ret;

	for (i = 0; i < RES_COUNT; i++) {
		ret = ta1618_validate_resource(pdev, i, &resources[i]);
		if (ret)
			return ret;
	}
	if (platform_get_resource(pdev, IORESOURCE_MEM, RES_COUNT))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "exactly %u memory resources required\n",
				     RES_COUNT);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	irq_data = irq_get_irq_data(irq);
	if (!irq_data)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "SPI57 has no IRQ domain data\n");
	hwirq = irqd_to_hwirq(irq_data);
	trigger = irqd_get_trigger_type(irq_data);
	if (hwirq != TA1618_SDIO0_INTID || trigger != IRQ_TYPE_LEVEL_HIGH)
		return dev_err_probe(
			&pdev->dev, -EINVAL,
			"IRQ must be SDIO0 SPI%u/INTID%u level-high, got %lu type 0x%x\n",
			TA1618_SDIO0_SPI, TA1618_SDIO0_INTID,
			(unsigned long)hwirq, trigger);

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->dev = &pdev->dev;
	host->mmc = mmc;
	host->irq = irq;
	spin_lock_init(&host->lock);
	mutex_init(&host->state_mutex);
	INIT_DELAYED_WORK(&host->timeout_work, ums9117_timeout_work);
	INIT_WORK(&host->finish_work, ums9117_finish_work);
	for (i = 0; i < RES_COUNT; i++) {
		host->resources[i] = resources[i];
		host->regs[i] = devm_ioremap_resource(&pdev->dev, resources[i]);
		if (IS_ERR(host->regs[i])) {
			ret = PTR_ERR(host->regs[i]);
			goto out_free_host;
		}
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out_free_host;
	/*
	 * A descriptor carries a 16-bit length, so a segment must never be
	 * allowed to grow past one page and overflow it.
	 */
	dma_set_max_seg_size(&pdev->dev, PAGE_SIZE);
	host->descriptors =
		dma_alloc_coherent(&pdev->dev, UMS9117_ADMA2_TABLE_SIZE,
				   &host->descriptors_dma, GFP_KERNEL);
	if (!host->descriptors) {
		ret = -ENOMEM;
		goto out_free_host;
	}
	if (!ta1618_dma_address_valid(host->descriptors_dma,
				      UMS9117_ADMA2_TABLE_SIZE)) {
		ret = -ERANGE;
		goto out_free_descriptor;
	}

	ret = ta1618_snapshot_platform(host);
	if (ret)
		goto out_free_descriptor;
	ret = request_irq(irq, ums9117_irq, 0, dev_name(&pdev->dev), host);
	if (ret)
		goto out_free_descriptor;
	host->irq_requested = true;

	mmc->ops = &ums9117_mmc_ops;
	mmc->f_min = UMS9117_CLOCK_HZ;
	mmc->f_max = UMS9117_MAX_CLOCK_HZ;
	mmc->f_init = UMS9117_CLOCK_HZ;
	mmc->ocr_avail = MMC_VDD_29_30 | MMC_VDD_30_31;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	/*
	 * These four numbers are the real limit on request shape: the block
	 * layer derives its own from them, and the core refuses anything that
	 * exceeds them before the driver is ever called. The segment count
	 * must not outgrow the descriptor table.
	 */
	mmc->max_segs = UMS9117_ADMA2_DESC_COUNT;
	mmc->max_seg_size = PAGE_SIZE;
	mmc->max_req_size = UMS9117_MAX_REQUEST_BYTES;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = UMS9117_MAX_REQUEST_BYTES / 512;
	mmc->max_busy_timeout = UMS9117_REQUEST_TIMEOUT_MS;
	platform_set_drvdata(pdev, host);

	ret = mmc_add_host(mmc);
	if (ret)
		goto out_free_irq;
	ret = device_create_file(&pdev->dev, &dev_attr_audit);
	if (ret)
		goto out_remove_host;
	host->audit_file_created = true;
	dev_notice(
		&pdev->dev,
		"registered 4-bit 13MHz UMS9117 SDIO0 MMC host on SPI57; identification 1-bit at %u Hz, 13MHz deferred until clean CMD55/ACMD6 and width4, up to %u 32-bit ADMA2 segments and %u bytes per request, multi-block reads and writes with automatic CMD12\n",
		UMS9117_CLOCK_HZ, UMS9117_ADMA2_DESC_COUNT,
		UMS9117_MAX_REQUEST_BYTES);
	return 0;

out_remove_host:
	mmc_remove_host(mmc);
out_free_irq:
	free_irq(irq, host);
	host->irq_requested = false;
out_free_descriptor:
	dma_free_coherent(&pdev->dev, UMS9117_ADMA2_TABLE_SIZE,
			  host->descriptors, host->descriptors_dma);
out_free_host:
	mmc_free_host(mmc);
	return ret;
}

static void ums9117_ta1618_mmc_remove(struct platform_device *pdev)
{
	struct ta1618_sd_mmc *host = platform_get_drvdata(pdev);

	if (host->audit_file_created) {
		device_remove_file(&pdev->dev, &dev_attr_audit);
		host->audit_file_created = false;
	}
	mmc_remove_host(host->mmc);
	WRITE_ONCE(host->stopping, true);
	if (host->platform_active)
		ta1618_writel(host, RES_INTERRUPT_SIGNAL_ENABLE, 0);
	cancel_delayed_work_sync(&host->timeout_work);
	cancel_work_sync(&host->finish_work);
	if (host->irq_requested) {
		synchronize_irq(host->irq);
		free_irq(host->irq, host);
		host->irq_requested = false;
	}
	mutex_lock(&host->state_mutex);
	ta1618_restore_platform(host);
	mutex_unlock(&host->state_mutex);
	dma_free_coherent(host->dev, UMS9117_ADMA2_TABLE_SIZE,
			  host->descriptors, host->descriptors_dma);
	mmc_free_host(host->mmc);
}

static const struct of_device_id ta1618_mmc_of_match[] = {
	{ .compatible = "fplinux,ta1618-mmc" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_mmc_of_match);

static struct platform_driver ta1618_mmc_driver = {
	.probe = ums9117_ta1618_mmc_probe,
	.remove = ums9117_ta1618_mmc_remove,
	.driver = {
		.name = "ta1618-mmc",
		.of_match_table = ta1618_mmc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ta1618_mmc_driver);

MODULE_DESCRIPTION("UMS9117 SDIO0 microSD host for Nokia TA-1618");
MODULE_LICENSE("GPL");
