// SPDX-License-Identifier: GPL-2.0-only
/*
 * Framebuffer and panel driver for Nokia 3210 4G TA-1618.
 *
 * The driver resets and initializes the ST7789P3 panel, SPI1 and LCDC, then
 * registers the reserved RGB565 buffer as fb0. LCDC_DONE completes each frame.
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define FB_WIDTH 240u
#define FB_HEIGHT 320u
#define FB_STRIDE (FB_WIDTH * 2u)
#define FB_SIZE (FB_STRIDE * FB_HEIGHT)

#define LCDC_CTRL 0x000
#define LCDC_DISP_SIZE 0x004
#define LCDC_LCM_START 0x008
#define LCDC_LCM_SIZE 0x00c
#define LCDC_BG_COLOR 0x010
#define LCDC_IMG_CTRL 0x020
#define LCDC_IMG_Y_BASE 0x024
#define LCDC_IMG_SIZE_XY 0x02c
#define LCDC_IMG_PITCH 0x030
#define LCDC_IMG_DISP_XY 0x034
#define LCDC_CAP_CTRL 0x0e0
#define LCDC_CAP_BASE 0x0e4
#define LCDC_IRQ_EN 0x110
#define LCDC_IRQ_CLR 0x114
#define LCDC_IRQ_STATUS 0x118
#define LCDC_IRQ_RAW 0x11c
#define LCDC_DONE BIT(0)
#define LCDC_RUN BIT(3)
#define LCDC_FMARK_OFF BIT(1)
#define LCDC_FMARK_POL BIT(2)
#define LCDC_RGB_MODE (7u << 5)

#define LCM_CTRL 0x000
#define LCM_CS0_MODE 0x010
#define LCM_CS0_TIMING 0x014

#define SPI_TXD 0x000
#define SPI_CLKD 0x004
#define SPI_CTL0 0x008
#define SPI_CTL1 0x00c
#define SPI_CTL2 0x010
#define SPI_CTL4 0x018
#define SPI_CTL5 0x01c
#define SPI_INT_EN 0x020
#define SPI_INT_CLR 0x024
#define SPI_INT_RAW 0x028
#define SPI_STS2 0x034
#define SPI_CTL7 0x04c
#define SPI_CTL8 0x054
#define SPI_CTL9 0x058
#define SPI_CTL12 0x064
#define SPI_TX_END BIT(8)
#define SPI_MODE (7u << 3)
#define SPI_MODE_3WIRE_9BIT (1u << 3)
#define SPI_TX_HOLD BIT(7)
#define SPI_CS0 BIT(8)
#define SPI_RGB565 BIT(14)
#define SPI_LANE2 BIT(15)
#define SPI_LANE2_PIN BIT(13)

#define SPI_DIVIDER 0u
#define PANEL_INIT_DIVIDER 3u
#define PIXEL_BITS 17u

#define ADI_VERSION 0x000
#define ADI_MST_CTL 0x004
#define ADI_INT_RAW 0x014
#define ADI_INT_CLR 0x01c
#define ADI_RD_CMD 0x028
#define ADI_RD_DATA 0x02c
#define ADI_FIFO_STS 0x030
#define ADI_USER_LOCK 0x224
#define ADI_EXPECTED_VERSION 0x00000400u
#define ADI_EXPECTED_MST_CTL 0x00000000u
#define ADI_RD_BUSY BIT(31)
#define ADI_RD_RETURNED_ADDRESS GENMASK(30, 16)
#define ADI_FIFO_EMPTY BIT(10)
#define ADI_FIFO_FULL BIT(11)
#define ADI_ARM_FIFO_OVERFLOW BIT(3)
#define ADI_USER_LOCK_RELEASE 0x5348554cu
#define ADI_POLL_BUDGET_US 3000u

#define BLTC_CTRL 0x180
#define BLTC_CURRENT0 0x1b8
#define BLTC_CURRENT1 0x1bc
#define BLTC_CURRENT2 0x1c0
#define BLTC_CURRENT3 0x1c4
#define BLTC_WLED_PRESCALER 0x1c8
#define BLTC_WLED_DUTY 0x1cc
#define BLTC_PD_CTRL 0x1d8
#define BLTC_CURRENT_MASK GENMASK(5, 0)
#define BLTC_SW_PD BIT(0)
#define BLTC_HW_PD BIT(1)
#define BLTC_ACTIVE_CTRL 0xccccu
#define BLTC_CURRENT_COUNT 4u
#define BLTC_COLD_LEVEL 0x000au
#define WLED_DISABLE_ATTEMPTS 3u
#define WLED_DISABLE_RETRY_US 5000u

#define ANA_MODULE_EN0 0xc08
#define ANA_RTC_CLK_EN0 0xc10
#define ANA_LDO_PD_CTRL 0xdec
#define ANA_WLED_MODULE_EN BIT(9)
#define ANA_WLED_RTC_CLK_EN BIT(7)
#define ANA_WLED_LDO_PD (BIT(2) | BIT(0))

#define AON_SPI1_GATE_SET 0x1134
#define AON_PANEL_RESET_SET 0x160c
#define AON_PANEL_RESET_CLEAR 0x260c
#define SPI1_GATE BIT(9)
#define SPI1_RESET BIT(6)
#define AP_AHB_LCDC_GATE BIT(11)
#define AP_AHB_LCM_GATE BIT(12)
#define AP_AHB_LCDC_RESET BIT(1)

#define DISPLAY_PIN_COUNT 7u

#define PANEL_SLEEP_IN 0x10
#define PANEL_SLEEP_OUT 0x11
#define PANEL_INVERSION_ON 0x21
#define PANEL_DISPLAY_OFF 0x28
#define PANEL_DISPLAY_ON 0x29
#define PANEL_COLUMN_ADDRESS 0x2a
#define PANEL_PAGE_ADDRESS 0x2b
#define PANEL_WRITE_RAM 0x2c
#define PANEL_TE_ON 0x35
#define PANEL_MEMORY_ACCESS 0x36
#define PANEL_PIXEL_FORMAT 0x3a
#define PANEL_FRAME_RATE 0xc6

/* Stretches the line period so the scan cannot overtake the write. */
#define PANEL_LINE_PERIOD 0x18

#define TRANSFER_TIMEOUT_US 250000
#define DCS_TRANSFER_TIMEOUT_US 1000
#define FRAME_TIMEOUT_MS 250u
#define PANEL_RESET_PHASE_MS 10u
#define PANEL_RESET_RELEASE_MS 120u
#define PANEL_SLEEP_IN_MS 5u
#define PANEL_SLEEP_OUT_MS 120u

enum ta1618_panel_state {
	TA1618_PANEL_COLD_INIT,
	TA1618_PANEL_ACTIVE,
	TA1618_PANEL_BLANKING,
	TA1618_PANEL_BLANKED,
	TA1618_PANEL_WAKING,
	TA1618_PANEL_ERROR,
};

enum ta1618_frame_role {
	TA1618_FRAME_NORMAL,
	TA1618_FRAME_COLD,
	TA1618_FRAME_WAKE,
};

enum ta1618_dcs_display_state {
	TA1618_DCS_OFF_TX_COMPLETE,
	TA1618_DCS_ON_TX_COMPLETE,
	TA1618_DCS_UNKNOWN,
};

enum ta1618_dcs_sleep_state {
	TA1618_DCS_SLEEP_IN_TX_COMPLETE,
	TA1618_DCS_SLEEP_OUT_TX_COMPLETE,
	TA1618_DCS_SLEEP_UNKNOWN,
};

enum ta1618_transition_stage {
	TA1618_TRANSITION_NONE,
	TA1618_TRANSITION_QUIESCE,
	TA1618_TRANSITION_WLED_OFF,
	TA1618_TRANSITION_DCS_OFF,
	TA1618_TRANSITION_SLEEP_IN,
	TA1618_TRANSITION_SLEEP_OUT,
	TA1618_TRANSITION_DCS_ON,
	TA1618_TRANSITION_COLD_INIT,
	TA1618_TRANSITION_NORMAL_WRITE_RAM,
	TA1618_TRANSITION_COLD_WRITE_RAM,
	TA1618_TRANSITION_WAKE_WRITE_RAM,
	TA1618_TRANSITION_FRAME_TIMEOUT,
	TA1618_TRANSITION_IRQ,
	TA1618_TRANSITION_WLED_RESTORE,
};

struct ta1618_wled_snapshot {
	u16 ctrl;
	u16 level[BLTC_CURRENT_COUNT];
	u16 prescaler;
	u16 duty;
	u16 pd_ctrl;
};

struct ta1618_fb_stats {
	u64 frames_started;
	u64 frames_done_irq;
	u64 frame_timeouts;
	u64 irq_spurious;
	u64 irq_missed;
	u64 blank_count;
	u64 blank_completed;
	u64 wake_count;
	u64 dcs_errors;
	u64 dcs_timeouts;
	u64 wled_errors;
	u64 fail_dark_failures;
	u32 last_error_irq_status;
	u32 last_error_irq_raw;
};

struct ta1618_fb {
	struct fb_info *info;
	void __iomem *screen;
	void __iomem *transfer;
	void *snapshot;
	void __iomem *lcdc;
	void __iomem *spi;
	void __iomem *lcm;
	void __iomem *spi_clock_selector;
	void __iomem *spi_reset_set;
	void __iomem *spi_reset_clear;
	void __iomem *ap_ahb_gate_set;
	void __iomem *ap_ahb_reset_set;
	void __iomem *ap_ahb_reset_clear;
	void __iomem *pinmux;
	void __iomem *pinconf;
	void __iomem *adi;
	void __iomem *analog;
	struct regmap *aon_apb;
	phys_addr_t screen_phys;
	phys_addr_t transfer_phys;
	phys_addr_t spi_phys;
	struct work_struct refresh_work;
	struct work_struct wake_work;
	struct delayed_work timeout_work;
	struct completion frame_done;
	struct mutex transition_lock;
	struct mutex panel_lock;
	spinlock_t lock;
	struct ta1618_fb_stats stats;
	unsigned long frame_deadline;
	u64 damage_seq;
	u64 submitted_seq;
	u64 frame_generation;
	u64 active_generation;
	u64 timeout_generation;
	u64 done_generation;
	u64 wake_generation;
	u64 cold_generation;
	struct ta1618_wled_snapshot wled_snapshot;
	u32 pseudo_palette[16];
	unsigned int shown;
	int irq;
	int last_error_errno;
	enum ta1618_panel_state panel_state;
	enum ta1618_dcs_display_state dcs_display_state;
	enum ta1618_dcs_sleep_state dcs_sleep_state;
	enum ta1618_transition_stage last_error_stage;
	u8 last_dcs_command;
	bool stopping;
	bool in_flight;
	bool adi_poisoned;
	bool spi_faulted;
	bool wled_known;
	bool wled_on;
	bool audit_file_created;
};

static DEFINE_MUTEX(ta1618_lifetime_lock);
static struct fb_info *ta1618_retired_info;

static const char *
ta1618_transition_stage_name(enum ta1618_transition_stage stage);

struct ta1618_panel_command {
	u8 command;
	u8 length;
	u16 delay_ms;
	u8 data[14];
};

static const u32 ta1618_display_pinconf[DISPLAY_PIN_COUNT] = {
	0x00182001, 0x00082046, 0x00082001, 0x00102004,
	0x00082004, 0x00102004, 0x00082004,
};

static const struct ta1618_panel_command ta1618_panel_init[] = {
	{ 0xb2, 5, 0, { 0x0c, 0x0c, 0x00, 0x33, 0x33 } },
	{ PANEL_TE_ON, 1, 0, { 0x00 } },
	{ PANEL_PIXEL_FORMAT, 1, 0, { 0x05 } },
	{ 0xb7, 1, 0, { 0x56 } },
	{ 0xbb, 1, 0, { 0x0c } },
	{ 0xc0, 1, 0, { 0x2c } },
	{ 0xc2, 1, 0, { 0x01 } },
	{ 0xc3, 1, 0, { 0x0f } },
	{ PANEL_FRAME_RATE, 1, 0, { 0x0f } },
	{ 0xd0, 1, 10, { 0xa7 } },
	{ 0xd0, 2, 0, { 0xa4, 0xa1 } },
	{ 0xd6, 1, 0, { 0xa1 } },
	{ 0xe0,
	  14,
	  0,
	  { 0xf0, 0x01, 0x08, 0x04, 0x05, 0x14, 0x33, 0x44, 0x49, 0x36, 0x11,
	    0x14, 0x2e, 0x36 } },
	{ 0xe1,
	  14,
	  0,
	  { 0xf0, 0x0c, 0x10, 0x0e, 0x0c, 0x08, 0x32, 0x43, 0x49, 0x28, 0x12,
	    0x12, 0x2c, 0x33 } },
	{ PANEL_INVERSION_ON, 0, 0, {} },
};

static void spi_channel_length(void __iomem *spi, unsigned int bits)
{
	u32 v = readl(spi + SPI_CTL0);

	v &= ~(0x1f << 2);
	v |= (bits & 0x1f) << 2;
	writel(v, spi + SPI_CTL0);
}

static void spi_tx_length(void __iomem *spi, unsigned int words)
{
	u32 ctl8 = readl(spi + SPI_CTL8) & ~0x3ff;
	u32 ctl9 = readl(spi + SPI_CTL9) & ~0xffff;

	ctl8 |= words >> 16;
	ctl9 |= words & 0xffff;
	writel(ctl8, spi + SPI_CTL8);
	writel(ctl9, spi + SPI_CTL9);
}

static int spi_timeout_remaining_us(ktime_t deadline,
				    unsigned int *remaining_us)
{
	s64 remaining = ktime_us_delta(deadline, ktime_get());

	if (remaining <= 0)
		return -ETIMEDOUT;
	*remaining_us = (unsigned int)remaining;
	return 0;
}

/* Sleeping here would cost a whole 10 ms tick for a wait of microseconds. */
static int spi_wait_idle_until(void __iomem *spi, ktime_t deadline)
{
	unsigned int remaining_us;
	u32 v;
	int ret;

	ret = spi_timeout_remaining_us(deadline, &remaining_us);
	if (ret)
		return ret;
	ret = readl_poll_timeout_atomic(spi + SPI_STS2, v, v & BIT(7), 1,
					remaining_us);
	if (ret)
		return ret;
	ret = spi_timeout_remaining_us(deadline, &remaining_us);
	if (ret)
		return ret;
	return readl_poll_timeout_atomic(spi + SPI_STS2, v, !(v & BIT(8)), 1,
					 remaining_us);
}

static void spi_restore_pixel_mode(void __iomem *spi)
{
	writel(readl(spi + SPI_CTL8) | BIT(15), spi + SPI_CTL8);
	writel(readl(spi + SPI_CTL7) | SPI_RGB565, spi + SPI_CTL7);
	spi_channel_length(spi, PIXEL_BITS);
}

static int spi_send_command_timeout_mode(void __iomem *spi, u8 command,
					 unsigned int timeout_us,
					 bool restore_pixel)
{
	ktime_t deadline = ktime_add_safe(ktime_get(), us_to_ktime(timeout_us));
	unsigned int remaining_us;
	u32 v;
	int ret;

	ret = spi_wait_idle_until(spi, deadline);
	if (ret)
		return ret;
	writel(SPI_TX_END, spi + SPI_INT_CLR);
	readl(spi + SPI_INT_RAW);

	writel(readl(spi + SPI_CTL7) & ~BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, 8);
	writel(readl(spi + SPI_CTL8) & ~BIT(15), spi + SPI_CTL8);
	spi_tx_length(spi, 1);
	writel(readl(spi + SPI_CTL12) | BIT(1), spi + SPI_CTL12);
	writel(command, spi + SPI_TXD);

	ret = spi_timeout_remaining_us(deadline, &remaining_us);
	if (ret)
		goto restore;
	ret = readl_poll_timeout_atomic(spi + SPI_INT_RAW, v, v & SPI_TX_END, 1,
					remaining_us);
	if (ret)
		goto restore;
	writel(SPI_TX_END, spi + SPI_INT_CLR);
	ret = spi_wait_idle_until(spi, deadline);

restore:
	writel(readl(spi + SPI_CTL8) | BIT(15), spi + SPI_CTL8);
	if (restore_pixel)
		spi_restore_pixel_mode(spi);
	return ret;
}

static int spi_send_command_timeout(void __iomem *spi, u8 command,
				    unsigned int timeout_us)
{
	return spi_send_command_timeout_mode(spi, command, timeout_us, true);
}

static int spi_send_command(void __iomem *spi, u8 command)
{
	return spi_send_command_timeout(spi, command, TRANSFER_TIMEOUT_US);
}

/* The command byte carries a clear data bit; a parameter carries a set one. */
static int spi_send_param_mode(void __iomem *spi, u8 value, bool restore_pixel)
{
	ktime_t deadline =
		ktime_add_safe(ktime_get(), us_to_ktime(TRANSFER_TIMEOUT_US));
	unsigned int remaining_us;
	u32 v;
	int ret;

	ret = spi_wait_idle_until(spi, deadline);
	if (ret)
		return ret;
	writel(readl(spi + SPI_CTL7) & ~BIT(14), spi + SPI_CTL7);
	spi_channel_length(spi, 8);
	writel(readl(spi + SPI_CTL8) | BIT(15), spi + SPI_CTL8);
	spi_tx_length(spi, 1);
	writel(readl(spi + SPI_CTL12) | BIT(1), spi + SPI_CTL12);
	writel(value, spi + SPI_TXD);
	ret = spi_timeout_remaining_us(deadline, &remaining_us);
	if (ret)
		goto restore;
	ret = readl_poll_timeout_atomic(spi + SPI_INT_RAW, v, v & BIT(8), 1,
					remaining_us);
	if (ret)
		goto restore;
	writel(BIT(8), spi + SPI_INT_CLR);
	ret = spi_wait_idle_until(spi, deadline);

restore:
	if (restore_pixel)
		spi_restore_pixel_mode(spi);
	return ret;
}

static int spi_send_param(void __iomem *spi, u8 value)
{
	return spi_send_param_mode(spi, value, true);
}

static void ta1618_mark_spi_fault(struct ta1618_fb *tfb);

static int ta1618_send_panel_data(struct ta1618_fb *tfb, u8 command,
				  const u8 *data, size_t length)
{
	size_t i;
	int ret;

	ret = spi_send_command(tfb->spi, command);
	for (i = 0; !ret && i < length; i++)
		ret = spi_send_param(tfb->spi, data[i]);
	if (ret)
		ta1618_mark_spi_fault(tfb);
	return ret;
}

static int ta1618_send_cold_panel_data(struct ta1618_fb *tfb, u8 command,
				       const u8 *data, size_t length)
{
	size_t i;
	int ret;

	ret = spi_send_command_timeout_mode(tfb->spi, command,
					    TRANSFER_TIMEOUT_US, false);
	for (i = 0; !ret && i < length; i++)
		ret = spi_send_param_mode(tfb->spi, data[i], false);
	if (ret)
		ta1618_mark_spi_fault(tfb);
	return ret;
}

static void ta1618_mark_spi_fault(struct ta1618_fb *tfb)
{
	unsigned long flags;

	spin_lock_irqsave(&tfb->lock, flags);
	tfb->spi_faulted = true;
	tfb->dcs_display_state = TA1618_DCS_UNKNOWN;
	tfb->dcs_sleep_state = TA1618_DCS_SLEEP_UNKNOWN;
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static void ta1618_record_error(struct ta1618_fb *tfb,
				enum ta1618_transition_stage stage, int error)
{
	unsigned long flags;

	if (!error)
		return;
	spin_lock_irqsave(&tfb->lock, flags);
	tfb->last_error_stage = stage;
	tfb->last_error_errno = error;
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static int ta1618_send_dcs_display(struct ta1618_fb *tfb, u8 command)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->spi_faulted) {
		spin_unlock_irqrestore(&tfb->lock, flags);
		return -EIO;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

	ret = spi_send_command_timeout(tfb->spi, command,
				       DCS_TRANSFER_TIMEOUT_US);

	spin_lock_irqsave(&tfb->lock, flags);
	if (ret) {
		tfb->last_dcs_command = command;
		tfb->spi_faulted = true;
		tfb->dcs_display_state = TA1618_DCS_UNKNOWN;
		tfb->dcs_sleep_state = TA1618_DCS_SLEEP_UNKNOWN;
		tfb->stats.dcs_errors++;
		if (ret == -ETIMEDOUT)
			tfb->stats.dcs_timeouts++;
	} else if (command == PANEL_DISPLAY_OFF) {
		tfb->dcs_display_state = TA1618_DCS_OFF_TX_COMPLETE;
	} else {
		tfb->dcs_display_state = TA1618_DCS_ON_TX_COMPLETE;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	return ret;
}

static int ta1618_send_dcs_sleep(struct ta1618_fb *tfb, u8 command)
{
	unsigned long flags;
	bool sleep_in = command == PANEL_SLEEP_IN;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->spi_faulted) {
		spin_unlock_irqrestore(&tfb->lock, flags);
		return -EIO;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

	ret = spi_send_command_timeout(tfb->spi, command,
				       DCS_TRANSFER_TIMEOUT_US);

	spin_lock_irqsave(&tfb->lock, flags);
	if (ret) {
		tfb->last_dcs_command = command;
		tfb->spi_faulted = true;
		tfb->dcs_sleep_state = TA1618_DCS_SLEEP_UNKNOWN;
		tfb->stats.dcs_errors++;
		if (ret == -ETIMEDOUT)
			tfb->stats.dcs_timeouts++;
	} else if (sleep_in) {
		tfb->dcs_sleep_state = TA1618_DCS_SLEEP_IN_TX_COMPLETE;
	} else {
		tfb->dcs_sleep_state = TA1618_DCS_SLEEP_OUT_TX_COMPLETE;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	return ret;
}

static int ta1618_send_cold_display_on(struct ta1618_fb *tfb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->spi_faulted) {
		spin_unlock_irqrestore(&tfb->lock, flags);
		return -EIO;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

	ret = ta1618_send_cold_panel_data(tfb, PANEL_DISPLAY_ON, NULL, 0);

	spin_lock_irqsave(&tfb->lock, flags);
	if (ret) {
		tfb->last_dcs_command = PANEL_DISPLAY_ON;
		tfb->spi_faulted = true;
		tfb->dcs_display_state = TA1618_DCS_UNKNOWN;
		tfb->stats.dcs_errors++;
		if (ret == -ETIMEDOUT)
			tfb->stats.dcs_timeouts++;
	} else {
		tfb->dcs_display_state = TA1618_DCS_ON_TX_COMPLETE;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	return ret;
}

static const u32 ta1618_bltc_current_reg[BLTC_CURRENT_COUNT] = {
	BLTC_CURRENT0,
	BLTC_CURRENT1,
	BLTC_CURRENT2,
	BLTC_CURRENT3,
};

static int ta1618_adi_validate(struct ta1618_fb *tfb)
{
	if (readl(tfb->adi + ADI_VERSION) != ADI_EXPECTED_VERSION ||
	    readl(tfb->adi + ADI_MST_CTL) != ADI_EXPECTED_MST_CTL)
		return -EPROTONOSUPPORT;
	return 0;
}

static int ta1618_adi_clear_overflow_locked(struct ta1618_fb *tfb)
{
	writel(ADI_ARM_FIFO_OVERFLOW, tfb->adi + ADI_INT_CLR);
	return readl(tfb->adi + ADI_INT_RAW) & ADI_ARM_FIFO_OVERFLOW ?
		       -EIO :
		       -EOVERFLOW;
}

static int ta1618_adi_wait_empty_locked(struct ta1618_fb *tfb)
{
	unsigned int waited;
	bool overflow = false;
	u32 raw;
	u32 status;

	for (waited = 0; waited < ADI_POLL_BUDGET_US; waited++) {
		raw = readl(tfb->adi + ADI_INT_RAW);
		status = readl(tfb->adi + ADI_FIFO_STS);
		overflow |= !!(raw & ADI_ARM_FIFO_OVERFLOW);
		if (status & ADI_FIFO_EMPTY)
			return overflow ?
				       ta1618_adi_clear_overflow_locked(tfb) :
				       0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int ta1618_adi_wait_quiescent_locked(struct ta1618_fb *tfb)
{
	unsigned int waited;
	bool overflow = false;
	u32 data;
	u32 raw;
	u32 status;

	for (waited = 0; waited < ADI_POLL_BUDGET_US; waited++) {
		raw = readl(tfb->adi + ADI_INT_RAW);
		status = readl(tfb->adi + ADI_FIFO_STS);
		data = readl(tfb->adi + ADI_RD_DATA);
		overflow |= !!(raw & ADI_ARM_FIFO_OVERFLOW);
		if ((status & ADI_FIFO_EMPTY) && !(data & ADI_RD_BUSY))
			return overflow ?
				       ta1618_adi_clear_overflow_locked(tfb) :
				       0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void ta1618_adi_poison(struct ta1618_fb *tfb)
{
	unsigned long flags;

	spin_lock_irqsave(&tfb->lock, flags);
	tfb->adi_poisoned = true;
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static int ta1618_adi_lock(struct ta1618_fb *tfb)
{
	unsigned int waited;
	int ret;

	if (READ_ONCE(tfb->adi_poisoned))
		return -EIO;
	for (waited = 0; waited < ADI_POLL_BUDGET_US; waited++) {
		if (!readl(tfb->adi + ADI_USER_LOCK)) {
			ret = ta1618_adi_wait_quiescent_locked(tfb);
			if (!ret)
				return 0;
			if (ret == -EOVERFLOW) {
				writel(ADI_USER_LOCK_RELEASE,
				       tfb->adi + ADI_USER_LOCK);
				return ret;
			}
			/* Fence an unfinished transaction from every ADI client. */
			ta1618_adi_poison(tfb);
			return ret;
		}
		udelay(1);
	}
	return -EBUSY;
}

static int ta1618_adi_unlock(struct ta1618_fb *tfb)
{
	int ret;

	ret = ta1618_adi_wait_quiescent_locked(tfb);
	if (!ret || ret == -EOVERFLOW) {
		writel(ADI_USER_LOCK_RELEASE, tfb->adi + ADI_USER_LOCK);
		return ret;
	}
	/* Releasing a busy controller would let another client overlap it. */
	ta1618_adi_poison(tfb);
	return ret;
}

static int ta1618_adi_read_locked(struct ta1618_fb *tfb, u32 offset, u16 *value)
{
	unsigned int waited;
	u32 data;
	u32 returned;
	int ret;

	if (!value || !IS_ALIGNED(offset, sizeof(u32)) ||
	    offset > 0x1000u - sizeof(u32))
		return -EINVAL;
	ret = ta1618_adi_validate(tfb);
	if (ret)
		return ret;

	writel(offset, tfb->adi + ADI_RD_CMD);
	for (waited = 0; waited < ADI_POLL_BUDGET_US; waited++) {
		data = readl(tfb->adi + ADI_RD_DATA);
		if (!(data & ADI_RD_BUSY))
			break;
		udelay(1);
	}
	if (waited == ADI_POLL_BUDGET_US)
		return -ETIMEDOUT;
	returned = FIELD_GET(ADI_RD_RETURNED_ADDRESS, data);
	if (returned != offset >> 2)
		return -EIO;
	*value = data & 0xffffu;
	return 0;
}

static int ta1618_adi_write_locked(struct ta1618_fb *tfb, u32 offset, u16 value)
{
	u16 readback;
	int ret;

	if (!IS_ALIGNED(offset, sizeof(u32)) || offset > 0x1000u - sizeof(u32))
		return -EINVAL;
	ret = ta1618_adi_wait_empty_locked(tfb);
	if (ret)
		return ret;
	if (readl(tfb->adi + ADI_FIFO_STS) & ADI_FIFO_FULL)
		return -EBUSY;
	ret = ta1618_adi_validate(tfb);
	if (ret)
		return ret;
	writel(value, tfb->analog + offset);
	ret = ta1618_adi_wait_empty_locked(tfb);
	if (ret)
		return ret;
	ret = ta1618_adi_read_locked(tfb, offset, &readback);
	if (ret)
		return ret;
	return readback == value ? 0 : -EIO;
}

static int ta1618_adi_update_bits_locked(struct ta1618_fb *tfb, u32 offset,
					 u16 mask, u16 value)
{
	u16 old_value;
	int ret;

	ret = ta1618_adi_read_locked(tfb, offset, &old_value);
	if (ret)
		return ret;
	return ta1618_adi_write_locked(tfb, offset,
				       (old_value & ~mask) | (value & mask));
}

static void ta1618_set_wled_state(struct ta1618_fb *tfb, bool known, bool on);

static int ta1618_prepare_wled(struct ta1618_fb *tfb)
{
	struct ta1618_wled_snapshot *snapshot = &tfb->wled_snapshot;
	unsigned int i;
	u16 register_value;
	int ret;
	int unlock_ret;

	ret = ta1618_adi_lock(tfb);
	if (ret)
		goto out_state;
	ret = ta1618_adi_update_bits_locked(
		tfb, ANA_MODULE_EN0, ANA_WLED_MODULE_EN, ANA_WLED_MODULE_EN);
	if (!ret)
		ret = ta1618_adi_update_bits_locked(tfb, ANA_RTC_CLK_EN0,
						    ANA_WLED_RTC_CLK_EN,
						    ANA_WLED_RTC_CLK_EN);
	if (!ret)
		ret = ta1618_adi_update_bits_locked(tfb, ANA_LDO_PD_CTRL,
						    ANA_WLED_LDO_PD, 0);
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_CTRL, 0);
	for (i = 0; !ret && i < BLTC_CURRENT_COUNT; i++) {
		ret = ta1618_adi_read_locked(tfb, ta1618_bltc_current_reg[i],
					     &register_value);
		if (!ret) {
			snapshot->level[i] =
				(register_value & ~BLTC_CURRENT_MASK) |
				BLTC_COLD_LEVEL;
			ret = ta1618_adi_write_locked(
				tfb, ta1618_bltc_current_reg[i],
				register_value & ~BLTC_CURRENT_MASK);
		}
	}
	if (!ret)
		ret = ta1618_adi_read_locked(tfb, BLTC_WLED_PRESCALER,
					     &register_value);
	if (!ret) {
		snapshot->prescaler = register_value & ~0xffu;
		ret = ta1618_adi_write_locked(tfb, BLTC_WLED_PRESCALER,
					      snapshot->prescaler);
	}
	if (!ret) {
		snapshot->duty = 0;
		ret = ta1618_adi_write_locked(tfb, BLTC_WLED_DUTY,
					      snapshot->duty);
	}
	if (!ret)
		ret = ta1618_adi_read_locked(tfb, BLTC_PD_CTRL,
					     &register_value);
	if (!ret) {
		snapshot->pd_ctrl = register_value & ~BLTC_SW_PD;
		ret = ta1618_adi_write_locked(tfb, BLTC_PD_CTRL,
					      register_value | BLTC_SW_PD);
	}
	snapshot->ctrl = BLTC_ACTIVE_CTRL;
	unlock_ret = ta1618_adi_unlock(tfb);
	if (!ret)
		ret = unlock_ret;

out_state:
	ta1618_set_wled_state(tfb, !ret, false);
	return ret;
}

static void ta1618_set_wled_state(struct ta1618_fb *tfb, bool known, bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&tfb->lock, flags);
	tfb->wled_known = known;
	tfb->wled_on = known && on;
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static int ta1618_wled_disable(struct ta1618_fb *tfb)
{
	u16 pd_ctrl;
	int ret;
	int unlock_ret;

	ret = ta1618_adi_lock(tfb);
	if (ret)
		goto out_state;
	ret = ta1618_adi_write_locked(tfb, BLTC_CTRL, 0);
	if (!ret)
		ret = ta1618_adi_read_locked(tfb, BLTC_PD_CTRL, &pd_ctrl);
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_PD_CTRL,
					      pd_ctrl | BLTC_SW_PD);
	unlock_ret = ta1618_adi_unlock(tfb);
	if (!ret)
		ret = unlock_ret;

out_state:
	ta1618_set_wled_state(tfb, !ret, false);
	return ret;
}

static int ta1618_wled_disable_bounded(struct ta1618_fb *tfb)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < WLED_DISABLE_ATTEMPTS; attempt++) {
		ret = ta1618_wled_disable(tfb);
		if (ret != -EBUSY)
			return ret;
		if (attempt + 1 < WLED_DISABLE_ATTEMPTS)
			usleep_range(WLED_DISABLE_RETRY_US,
				     WLED_DISABLE_RETRY_US + 1000u);
	}
	return ret;
}

static int ta1618_wled_restore(struct ta1618_fb *tfb)
{
	const struct ta1618_wled_snapshot *snapshot = &tfb->wled_snapshot;
	unsigned int i;
	u16 pd_ctrl;
	int ret;
	int unlock_ret;

	ret = ta1618_adi_lock(tfb);
	if (ret)
		goto out_state;
	for (i = 0; !ret && i < BLTC_CURRENT_COUNT; i++)
		ret = ta1618_adi_write_locked(tfb, ta1618_bltc_current_reg[i],
					      snapshot->level[i] &
						      ~BLTC_CURRENT_MASK);
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_WLED_PRESCALER,
					      snapshot->prescaler);
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_WLED_DUTY,
					      snapshot->duty);
	if (!ret)
		ret = ta1618_adi_read_locked(tfb, BLTC_PD_CTRL, &pd_ctrl);
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_PD_CTRL,
					      (pd_ctrl & ~BLTC_SW_PD) |
						      (snapshot->pd_ctrl &
						       BLTC_SW_PD));
	if (!ret)
		ret = ta1618_adi_write_locked(tfb, BLTC_CTRL, snapshot->ctrl);
	for (i = 0; !ret && i < BLTC_CURRENT_COUNT; i++)
		ret = ta1618_adi_write_locked(tfb, ta1618_bltc_current_reg[i],
					      snapshot->level[i]);
	unlock_ret = ta1618_adi_unlock(tfb);
	if (!ret)
		ret = unlock_ret;

out_state:
	ta1618_set_wled_state(tfb, !ret, true);
	return ret;
}

static void ta1618_note_wled_error(struct ta1618_fb *tfb, int error)
{
	unsigned long flags;

	if (!error)
		return;
	spin_lock_irqsave(&tfb->lock, flags);
	tfb->stats.wled_errors++;
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static int ta1618_fail_dark_locked(struct ta1618_fb *tfb)
{
	unsigned long flags;
	bool dcs_off;
	bool spi_faulted;
	bool wled_off;
	int dcs_ret = 0;
	int wled_ret = 0;

	spin_lock_irqsave(&tfb->lock, flags);
	wled_off = tfb->wled_known && !tfb->wled_on;
	dcs_off = tfb->dcs_display_state == TA1618_DCS_OFF_TX_COMPLETE;
	spi_faulted = tfb->spi_faulted;
	spin_unlock_irqrestore(&tfb->lock, flags);

	if (!wled_off) {
		wled_ret = ta1618_wled_disable_bounded(tfb);
		ta1618_note_wled_error(tfb, wled_ret);
	}
	if (!dcs_off) {
		if (spi_faulted)
			dcs_ret = -EIO;
		else
			dcs_ret =
				ta1618_send_dcs_display(tfb, PANEL_DISPLAY_OFF);
	}

	if (wled_ret && dcs_ret) {
		spin_lock_irqsave(&tfb->lock, flags);
		tfb->stats.fail_dark_failures++;
		spin_unlock_irqrestore(&tfb->lock, flags);
		return wled_ret;
	}
	return 0;
}

static void ta1618_configure_display_pins(struct ta1618_fb *tfb)
{
	unsigned int i;

	for (i = 0; i < DISPLAY_PIN_COUNT; i++) {
		writel(0, tfb->pinmux + i * sizeof(u32));
		writel(ta1618_display_pinconf[i],
		       tfb->pinconf + i * sizeof(u32));
	}
	readl(tfb->pinconf + (DISPLAY_PIN_COUNT - 1) * sizeof(u32));
}

static int ta1618_enable_display_blocks(struct ta1618_fb *tfb)
{
	int ret;

	ret = regmap_write(tfb->aon_apb, AON_SPI1_GATE_SET, SPI1_GATE);
	if (ret)
		return ret;
	writel(3, tfb->spi_clock_selector);
	writel(AP_AHB_LCDC_GATE | AP_AHB_LCM_GATE, tfb->ap_ahb_gate_set);
	usleep_range(1000, 2000);

	writel(0, tfb->lcm + LCM_CTRL);
	writel(1, tfb->lcm + LCM_CS0_MODE);
	writel(0x00a50100, tfb->lcm + LCM_CS0_TIMING);
	readl(tfb->lcm + LCM_CS0_TIMING);
	return 0;
}

static void ta1618_configure_spi_controller(struct ta1618_fb *tfb)
{
	u32 value;

	writel(SPI1_RESET, tfb->spi_reset_set);
	usleep_range(1000, 2000);
	writel(SPI1_RESET, tfb->spi_reset_clear);
	usleep_range(1000, 2000);

	writel(0, tfb->spi + SPI_INT_EN);
	writel(0xf00 | 2 | (8 << 2), tfb->spi + SPI_CTL0);
	value = readl(tfb->spi + SPI_CTL1);
	writel((value & ~0x3000) | 0x3000, tfb->spi + SPI_CTL1);
	writel((readl(tfb->spi + SPI_CTL2) & ~0x1f) | 7, tfb->spi + SPI_CTL2);
	writel(0x8000, tfb->spi + SPI_CTL4);
	writel(0, tfb->spi + SPI_CTL5);
	writel(PANEL_INIT_DIVIDER, tfb->spi + SPI_CLKD);
	writel(SPI_MODE_3WIRE_9BIT, tfb->spi + SPI_CTL7);
	writel(0, tfb->spi + SPI_CTL8);
	writel(SPI_TX_END, tfb->spi + SPI_INT_CLR);
}

static int ta1618_reset_panel(struct ta1618_fb *tfb)
{
	int ret;

	ret = regmap_write(tfb->aon_apb, AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	usleep_range(PANEL_RESET_PHASE_MS * 1000,
		     PANEL_RESET_PHASE_MS * 1000 + 1000);
	ret = regmap_write(tfb->aon_apb, AON_PANEL_RESET_CLEAR, BIT(0));
	if (ret)
		return ret;
	usleep_range(PANEL_RESET_PHASE_MS * 1000,
		     PANEL_RESET_PHASE_MS * 1000 + 1000);
	ret = regmap_write(tfb->aon_apb, AON_PANEL_RESET_SET, BIT(0));
	if (ret)
		return ret;
	usleep_range(PANEL_RESET_PHASE_MS * 1000,
		     PANEL_RESET_PHASE_MS * 1000 + 1000);

	msleep(PANEL_RESET_RELEASE_MS);
	return 0;
}

static void ta1618_reset_lcdc(struct ta1618_fb *tfb)
{
	writel(AP_AHB_LCDC_RESET, tfb->ap_ahb_reset_set);
	usleep_range(10000, 11000);
	writel(AP_AHB_LCDC_RESET, tfb->ap_ahb_reset_clear);
	readl(tfb->lcdc + LCDC_IRQ_RAW);
}

static int ta1618_send_cold_panel_init(struct ta1618_fb *tfb)
{
	static const u8 memory_access[] = { 0x00 };
	static const u8 column_address[] = { 0x00, 0x00, 0x00, 0xef };
	static const u8 page_address[] = { 0x00, 0x00, 0x01, 0x3f };
	static const u8 runtime_frame_rate[] = { PANEL_LINE_PERIOD };
	unsigned int i;
	int ret;

	ret = ta1618_send_cold_panel_data(tfb, PANEL_SLEEP_OUT, NULL, 0);
	if (ret)
		return ret;
	msleep(PANEL_SLEEP_OUT_MS);

	for (i = 0; i < ARRAY_SIZE(ta1618_panel_init); i++) {
		ret = ta1618_send_cold_panel_data(tfb,
						  ta1618_panel_init[i].command,
						  ta1618_panel_init[i].data,
						  ta1618_panel_init[i].length);
		if (ret)
			return ret;
		if (ta1618_panel_init[i].delay_ms)
			usleep_range(ta1618_panel_init[i].delay_ms * 1000,
				     ta1618_panel_init[i].delay_ms * 1000 +
					     1000);
	}

	ret = ta1618_send_cold_display_on(tfb);
	if (ret)
		return ret;
	ret = ta1618_send_cold_panel_data(tfb, PANEL_FRAME_RATE,
					  runtime_frame_rate,
					  ARRAY_SIZE(runtime_frame_rate));
	if (ret)
		return ret;
	ret = ta1618_send_cold_panel_data(tfb, PANEL_MEMORY_ACCESS,
					  memory_access,
					  ARRAY_SIZE(memory_access));
	if (ret)
		return ret;
	ret = ta1618_send_cold_panel_data(tfb, PANEL_COLUMN_ADDRESS,
					  column_address,
					  ARRAY_SIZE(column_address));
	if (ret)
		return ret;
	return ta1618_send_cold_panel_data(tfb, PANEL_PAGE_ADDRESS,
					   page_address,
					   ARRAY_SIZE(page_address));
}

static bool ta1618_panel_refresh_enabled(enum ta1618_panel_state state)
{
	return state == TA1618_PANEL_ACTIVE;
}

static bool ta1618_can_refresh(struct ta1618_fb *tfb)
{
	return ta1618_panel_refresh_enabled(tfb->panel_state) &&
	       !tfb->stopping && !tfb->in_flight;
}

static bool ta1618_can_start_frame(struct ta1618_fb *tfb,
				   enum ta1618_frame_role role)
{
	if (tfb->stopping || tfb->in_flight ||
	    tfb->panel_state == TA1618_PANEL_ERROR)
		return false;
	if (role == TA1618_FRAME_COLD)
		return tfb->panel_state == TA1618_PANEL_COLD_INIT;
	if (role == TA1618_FRAME_WAKE)
		return tfb->panel_state == TA1618_PANEL_WAKING;
	return ta1618_panel_refresh_enabled(tfb->panel_state);
}

static bool ta1618_damage_pending(struct ta1618_fb *tfb)
{
	return tfb->damage_seq != tfb->submitted_seq;
}

static void ta1618_force_refresh_locked(struct ta1618_fb *tfb)
{
	if (!ta1618_damage_pending(tfb))
		tfb->damage_seq++;
	if (ta1618_can_refresh(tfb))
		schedule_work(&tfb->refresh_work);
}

static void ta1618_record_damage_locked(struct ta1618_fb *tfb)
{
	tfb->damage_seq++;
	if (ta1618_can_refresh(tfb))
		schedule_work(&tfb->refresh_work);
}

static void ta1618_mark_damage(struct ta1618_fb *tfb)
{
	unsigned long flags;

	spin_lock_irqsave(&tfb->lock, flags);
	ta1618_record_damage_locked(tfb);
	spin_unlock_irqrestore(&tfb->lock, flags);
}

static void ta1618_stop_lcdc(struct ta1618_fb *tfb)
{
	writel(readl(tfb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
	       tfb->lcdc + LCDC_IRQ_EN);
	writel(readl(tfb->lcdc + LCDC_CTRL) & ~(BIT(0) | LCDC_RUN),
	       tfb->lcdc + LCDC_CTRL);
	writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
	readl(tfb->lcdc + LCDC_IRQ_RAW);
}

static void ta1618_enter_error(struct ta1618_fb *tfb,
			       enum ta1618_transition_stage stage, int error,
			       bool fail_dark)
{
	unsigned long flags;
	bool complete_frame;
	int dark_ret;

	ta1618_record_error(tfb, stage, error);
	spin_lock_irqsave(&tfb->lock, flags);
	complete_frame = tfb->in_flight;
	tfb->in_flight = false;
	tfb->panel_state = TA1618_PANEL_ERROR;
	spin_unlock_irqrestore(&tfb->lock, flags);

	ta1618_stop_lcdc(tfb);
	if (fail_dark) {
		dark_ret = ta1618_fail_dark_locked(tfb);
		if (dark_ret)
			dev_err(tfb->info->device,
				"could not fail display dark after error: %d\n",
				dark_ret);
	}
	if (complete_frame)
		complete(&tfb->frame_done);
	dev_err_ratelimited(tfb->info->device,
			    "display entered error state at %s: %d\n",
			    ta1618_transition_stage_name(stage), error);
}

static int ta1618_start_frame(struct ta1618_fb *tfb,
			      enum ta1618_frame_role role)
{
	unsigned long flags;
	unsigned int shown;
	u64 frame_seq;
	u32 v;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	if (!ta1618_can_start_frame(tfb, role) || !ta1618_damage_pending(tfb)) {
		spin_unlock_irqrestore(&tfb->lock, flags);
		return -ESHUTDOWN;
	}
	frame_seq = tfb->damage_seq;
	shown = tfb->shown;
	spin_unlock_irqrestore(&tfb->lock, flags);

	ret = spi_send_command_timeout(tfb->spi, PANEL_WRITE_RAM,
				       role == TA1618_FRAME_NORMAL ?
					       TRANSFER_TIMEOUT_US :
					       DCS_TRANSFER_TIMEOUT_US);
	if (ret) {
		ta1618_mark_spi_fault(tfb);
		return ret;
	}

	memcpy_fromio(tfb->snapshot, tfb->screen + shown * FB_STRIDE, FB_SIZE);
	memcpy_toio(tfb->transfer, tfb->snapshot, FB_SIZE);
	/* Drain write combining before LCDC fetches from DRAM. */
	wmb();

	spi_tx_length(tfb->spi, FB_WIDTH * FB_HEIGHT);
	writel(readl(tfb->spi + SPI_CTL12) | BIT(1), tfb->spi + SPI_CTL12);

	writel(readl(tfb->lcdc + LCDC_CTRL) | BIT(0), tfb->lcdc + LCDC_CTRL);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_DISP_SIZE);
	writel(0, tfb->lcdc + LCDC_LCM_START);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_LCM_SIZE);
	writel(0, tfb->lcdc + LCDC_BG_COLOR);

	v = readl(tfb->lcdc + LCDC_IMG_CTRL);
	v &= ~BIT(1);
	v = (v & ~(0xf << 4)) | (5 << 4);
	v = (v & ~(3 << 8)) | (2 << 8);
	v |= BIT(0);
	writel(v, tfb->lcdc + LCDC_IMG_CTRL);
	writel((u32)(tfb->transfer_phys >> 2), tfb->lcdc + LCDC_IMG_Y_BASE);
	writel(FB_WIDTH | (FB_HEIGHT << 16), tfb->lcdc + LCDC_IMG_SIZE_XY);
	writel(FB_WIDTH, tfb->lcdc + LCDC_IMG_PITCH);
	writel(0, tfb->lcdc + LCDC_IMG_DISP_XY);

	v = readl(tfb->lcdc + LCDC_CAP_CTRL);
	v &= ~(3 << 6);
	v |= 0x20;
	writel(v, tfb->lcdc + LCDC_CAP_CTRL);
	writel((u32)((tfb->spi_phys + SPI_TXD) >> 2),
	       tfb->lcdc + LCDC_CAP_BASE);

	/* Latch the layer and capture setup before arming the transfer. */
	wmb();
	reinit_completion(&tfb->frame_done);
	writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
	writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_EN);

	v = readl(tfb->lcdc + LCDC_CTRL);
	v &= ~LCDC_RGB_MODE;
	v &= ~LCDC_FMARK_OFF;
	v |= LCDC_FMARK_POL;

	spin_lock_irqsave(&tfb->lock, flags);
	if (!ta1618_can_start_frame(tfb, role)) {
		spin_unlock_irqrestore(&tfb->lock, flags);
		writel(readl(tfb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
		       tfb->lcdc + LCDC_IRQ_EN);
		writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
		return -ESHUTDOWN;
	}
	tfb->submitted_seq = frame_seq;
	tfb->frame_generation++;
	tfb->active_generation = tfb->frame_generation;
	if (role == TA1618_FRAME_COLD)
		tfb->cold_generation = tfb->active_generation;
	else if (role == TA1618_FRAME_WAKE)
		tfb->wake_generation = tfb->active_generation;
	tfb->timeout_generation = tfb->active_generation;
	tfb->frame_deadline = jiffies + msecs_to_jiffies(FRAME_TIMEOUT_MS);
	tfb->in_flight = true;
	tfb->stats.frames_started++;
	writel(v | LCDC_RUN, tfb->lcdc + LCDC_CTRL);
	spin_unlock_irqrestore(&tfb->lock, flags);
	return 0;
}

static void ta1618_refresh_work(struct work_struct *work)
{
	struct ta1618_fb *tfb =
		container_of(work, struct ta1618_fb, refresh_work);
	unsigned long flags;
	bool refresh;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	refresh = ta1618_can_refresh(tfb) && ta1618_damage_pending(tfb);
	spin_unlock_irqrestore(&tfb->lock, flags);
	if (!refresh)
		return;

	cancel_delayed_work_sync(&tfb->timeout_work);
	mutex_lock(&tfb->panel_lock);
	spin_lock_irqsave(&tfb->lock, flags);
	refresh = ta1618_can_refresh(tfb) && ta1618_damage_pending(tfb);
	spin_unlock_irqrestore(&tfb->lock, flags);
	if (!refresh) {
		mutex_unlock(&tfb->panel_lock);
		return;
	}

	ret = ta1618_start_frame(tfb, TA1618_FRAME_NORMAL);
	if (ret && ret != -ESHUTDOWN)
		ta1618_enter_error(tfb, TA1618_TRANSITION_NORMAL_WRITE_RAM, ret,
				   true);
	mutex_unlock(&tfb->panel_lock);
	if (!ret)
		schedule_delayed_work(&tfb->timeout_work,
				      msecs_to_jiffies(FRAME_TIMEOUT_MS));
}

static void ta1618_wake_work(struct work_struct *work)
{
	struct ta1618_fb *tfb = container_of(work, struct ta1618_fb, wake_work);
	unsigned long flags;
	bool ready;
	int ret;

	mutex_lock(&tfb->transition_lock);
	mutex_lock(&tfb->panel_lock);
	spin_lock_irqsave(&tfb->lock, flags);
	ready = !tfb->stopping && tfb->panel_state == TA1618_PANEL_WAKING &&
		!tfb->in_flight &&
		tfb->done_generation == tfb->wake_generation &&
		!tfb->spi_faulted &&
		tfb->dcs_display_state == TA1618_DCS_ON_TX_COMPLETE &&
		tfb->dcs_sleep_state == TA1618_DCS_SLEEP_OUT_TX_COMPLETE;
	spin_unlock_irqrestore(&tfb->lock, flags);
	if (!ready)
		goto out;

	ret = ta1618_wled_restore(tfb);
	if (ret) {
		ta1618_note_wled_error(tfb, ret);
		ta1618_enter_error(tfb, TA1618_TRANSITION_WLED_RESTORE, ret,
				   true);
		goto out;
	}

	spin_lock_irqsave(&tfb->lock, flags);
	if (!tfb->stopping && tfb->panel_state == TA1618_PANEL_WAKING &&
	    tfb->done_generation == tfb->wake_generation) {
		tfb->panel_state = TA1618_PANEL_ACTIVE;
		tfb->stats.wake_count++;
		if (ta1618_damage_pending(tfb))
			schedule_work(&tfb->refresh_work);
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

out:
	mutex_unlock(&tfb->panel_lock);
	mutex_unlock(&tfb->transition_lock);
}

static irqreturn_t ta1618_lcdc_irq(int irq, void *data)
{
	struct ta1618_fb *tfb = data;
	unsigned long flags;
	u32 status = readl(tfb->lcdc + LCDC_IRQ_STATUS);
	bool cold_done;
	bool wake_done;

	(void)irq;
	spin_lock_irqsave(&tfb->lock, flags);
	if (!(status & LCDC_DONE)) {
		tfb->stats.irq_spurious++;
		tfb->stats.last_error_irq_status = status;
		tfb->stats.last_error_irq_raw = readl(tfb->lcdc + LCDC_IRQ_RAW);
		tfb->last_error_stage = TA1618_TRANSITION_IRQ;
		tfb->last_error_errno = -EIO;
		spin_unlock_irqrestore(&tfb->lock, flags);
		return IRQ_NONE;
	}

	if (!tfb->in_flight) {
		tfb->stats.irq_spurious++;
		tfb->stats.last_error_irq_status = status;
		tfb->stats.last_error_irq_raw = readl(tfb->lcdc + LCDC_IRQ_RAW);
		tfb->last_error_stage = TA1618_TRANSITION_IRQ;
		tfb->last_error_errno = -EIO;
		writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
		readl(tfb->lcdc + LCDC_IRQ_RAW);
	} else {
		writel(LCDC_DONE, tfb->lcdc + LCDC_IRQ_CLR);
		readl(tfb->lcdc + LCDC_IRQ_RAW);
		tfb->in_flight = false;
		tfb->done_generation = tfb->active_generation;
		tfb->stats.frames_done_irq++;
		cold_done = !tfb->stopping &&
			    tfb->panel_state == TA1618_PANEL_COLD_INIT &&
			    tfb->done_generation == tfb->cold_generation;
		wake_done = !tfb->stopping &&
			    tfb->panel_state == TA1618_PANEL_WAKING &&
			    tfb->done_generation == tfb->wake_generation;
		cancel_delayed_work(&tfb->timeout_work);
		complete(&tfb->frame_done);
		if (cold_done) {
			writel(readl(tfb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
			       tfb->lcdc + LCDC_IRQ_EN);
		} else if (wake_done) {
			writel(readl(tfb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
			       tfb->lcdc + LCDC_IRQ_EN);
			schedule_work(&tfb->wake_work);
		} else if (ta1618_can_refresh(tfb) &&
			   ta1618_damage_pending(tfb)) {
			schedule_work(&tfb->refresh_work);
		} else {
			writel(readl(tfb->lcdc + LCDC_IRQ_EN) & ~LCDC_DONE,
			       tfb->lcdc + LCDC_IRQ_EN);
		}
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	return IRQ_HANDLED;
}

static void ta1618_timeout_work(struct work_struct *work)
{
	struct ta1618_fb *tfb = container_of(to_delayed_work(work),
					     struct ta1618_fb, timeout_work);
	unsigned long flags;
	u32 status;
	u32 raw;
	bool timed_out = false;
	int ret;

	mutex_lock(&tfb->panel_lock);
	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->in_flight && !tfb->stopping &&
	    tfb->panel_state != TA1618_PANEL_ERROR &&
	    tfb->timeout_generation == tfb->active_generation) {
		status = readl(tfb->lcdc + LCDC_IRQ_STATUS);
		raw = readl(tfb->lcdc + LCDC_IRQ_RAW);
		tfb->stats.last_error_irq_status = status;
		tfb->stats.last_error_irq_raw = raw;
		tfb->stats.frame_timeouts++;
		if (raw & LCDC_DONE)
			tfb->stats.irq_missed++;
		tfb->in_flight = false;
		tfb->panel_state = TA1618_PANEL_ERROR;
		timed_out = true;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

	if (timed_out) {
		ta1618_stop_lcdc(tfb);
		ta1618_record_error(tfb, TA1618_TRANSITION_FRAME_TIMEOUT,
				    -ETIMEDOUT);
		ret = ta1618_fail_dark_locked(tfb);
		if (ret)
			dev_err(tfb->info->device,
				"could not fail display dark after LCDC timeout: %d\n",
				ret);
	}
	mutex_unlock(&tfb->panel_lock);
	if (timed_out) {
		complete(&tfb->frame_done);
		dev_err(tfb->info->device, "LCDC_DONE timed out\n");
	}
}

static int ta1618_quiesce_pipeline(struct ta1618_fb *tfb)
{
	unsigned long flags;
	unsigned long completed;
	unsigned long remaining;
	u32 status;
	u32 raw;
	bool forced_timeout = false;
	bool in_flight;
	int ret;

	cancel_work_sync(&tfb->refresh_work);
	cancel_delayed_work_sync(&tfb->timeout_work);

	spin_lock_irqsave(&tfb->lock, flags);
	in_flight = tfb->in_flight;
	remaining = in_flight && time_before(jiffies, tfb->frame_deadline) ?
			    tfb->frame_deadline - jiffies :
			    0;
	spin_unlock_irqrestore(&tfb->lock, flags);
	if (in_flight) {
		completed = wait_for_completion_timeout(&tfb->frame_done,
							remaining);
		if (!completed) {
			mutex_lock(&tfb->panel_lock);
			status = readl(tfb->lcdc + LCDC_IRQ_STATUS);
			raw = readl(tfb->lcdc + LCDC_IRQ_RAW);
			spin_lock_irqsave(&tfb->lock, flags);
			if (tfb->in_flight) {
				tfb->stats.last_error_irq_status = status;
				tfb->stats.last_error_irq_raw = raw;
				tfb->stats.frame_timeouts++;
				if (raw & LCDC_DONE)
					tfb->stats.irq_missed++;
				tfb->in_flight = false;
				tfb->panel_state = TA1618_PANEL_ERROR;
				forced_timeout = true;
			}
			spin_unlock_irqrestore(&tfb->lock, flags);
			ta1618_stop_lcdc(tfb);
			if (forced_timeout) {
				ta1618_record_error(tfb,
						    TA1618_TRANSITION_QUIESCE,
						    -ETIMEDOUT);
				ret = ta1618_fail_dark_locked(tfb);
				if (ret)
					dev_err(tfb->info->device,
						"could not fail display dark while stopping: %d\n",
						ret);
			}
			mutex_unlock(&tfb->panel_lock);
			if (forced_timeout) {
				complete(&tfb->frame_done);
				dev_err(tfb->info->device,
					"LCDC_DONE timed out while stopping pipeline\n");
			}
			synchronize_irq(tfb->irq);
			return forced_timeout ? -ETIMEDOUT : 0;
		}
	}

	mutex_lock(&tfb->panel_lock);
	ta1618_stop_lcdc(tfb);
	mutex_unlock(&tfb->panel_lock);
	synchronize_irq(tfb->irq);
	return 0;
}

static int ta1618_wait_cold_frame(struct ta1618_fb *tfb)
{
	unsigned long flags;
	unsigned long completed;
	u32 status;
	u32 raw;
	int ret;

	completed = wait_for_completion_timeout(
		&tfb->frame_done, msecs_to_jiffies(FRAME_TIMEOUT_MS));

	spin_lock_irqsave(&tfb->lock, flags);
	if (!tfb->in_flight && tfb->done_generation == tfb->cold_generation) {
		ret = 0;
	} else if (tfb->panel_state != TA1618_PANEL_COLD_INIT) {
		ret = -EIO;
	} else {
		status = readl(tfb->lcdc + LCDC_IRQ_STATUS);
		raw = readl(tfb->lcdc + LCDC_IRQ_RAW);
		tfb->stats.last_error_irq_status = status;
		tfb->stats.last_error_irq_raw = raw;
		tfb->stats.frame_timeouts++;
		if (raw & LCDC_DONE)
			tfb->stats.irq_missed++;
		tfb->in_flight = false;
		tfb->panel_state = TA1618_PANEL_ERROR;
		ret = completed ? -EIO : -ETIMEDOUT;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);

	if (ret) {
		ta1618_stop_lcdc(tfb);
		synchronize_irq(tfb->irq);
	}
	return ret;
}

static int ta1618_cold_init(struct ta1618_fb *tfb)
{
	unsigned long flags;
	int dark_ret;
	int ret;

	memset_io(tfb->screen, 0, 2 * FB_SIZE);
	memset_io(tfb->transfer, 0, FB_SIZE);
	memset(tfb->snapshot, 0, FB_SIZE);
	/* Publish the known frame before resetting and starting LCDC. */
	wmb();

	spin_lock_irqsave(&tfb->lock, flags);
	tfb->panel_state = TA1618_PANEL_COLD_INIT;
	tfb->dcs_display_state = TA1618_DCS_UNKNOWN;
	tfb->dcs_sleep_state = TA1618_DCS_SLEEP_UNKNOWN;
	tfb->damage_seq++;
	spin_unlock_irqrestore(&tfb->lock, flags);

	mutex_lock(&tfb->panel_lock);
	ta1618_configure_spi_controller(tfb);
	ta1618_stop_lcdc(tfb);
	ret = ta1618_prepare_wled(tfb);
	if (ret)
		goto fail_locked;
	ret = ta1618_reset_panel(tfb);
	if (ret)
		goto fail_locked;
	writel(readl(tfb->spi + SPI_CTL0) & ~SPI_CS0, tfb->spi + SPI_CTL0);
	ret = ta1618_send_cold_panel_init(tfb);
	if (ret)
		goto fail_locked;
	writel(SPI_DIVIDER, tfb->spi + SPI_CLKD);
	writel(readl(tfb->spi + SPI_CTL7) | SPI_TX_HOLD, tfb->spi + SPI_CTL7);
	spi_restore_pixel_mode(tfb->spi);
	ta1618_reset_lcdc(tfb);
	ret = ta1618_start_frame(tfb, TA1618_FRAME_COLD);
	if (ret)
		goto fail_locked;
	mutex_unlock(&tfb->panel_lock);

	ret = ta1618_wait_cold_frame(tfb);
	ta1618_record_error(tfb, TA1618_TRANSITION_COLD_WRITE_RAM, ret);
	if (ret) {
		mutex_lock(&tfb->panel_lock);
		dark_ret = ta1618_fail_dark_locked(tfb);
		mutex_unlock(&tfb->panel_lock);
		if (dark_ret)
			dev_err(tfb->info->device,
				"could not fail display dark after cold frame: %d\n",
				dark_ret);
		return ret;
	}

	mutex_lock(&tfb->panel_lock);
	ret = ta1618_wled_restore(tfb);
	if (ret) {
		ta1618_note_wled_error(tfb, ret);
		ta1618_enter_error(tfb, TA1618_TRANSITION_WLED_RESTORE, ret,
				   true);
		mutex_unlock(&tfb->panel_lock);
		return ret;
	}
	mutex_unlock(&tfb->panel_lock);

	spin_lock_irqsave(&tfb->lock, flags);
	tfb->panel_state = TA1618_PANEL_ACTIVE;
	tfb->dcs_sleep_state = TA1618_DCS_SLEEP_OUT_TX_COMPLETE;
	spin_unlock_irqrestore(&tfb->lock, flags);
	return 0;

fail_locked:
	ta1618_enter_error(tfb, TA1618_TRANSITION_COLD_INIT, ret, true);
	mutex_unlock(&tfb->panel_lock);
	return ret;
}

static int ta1618_setcolreg(unsigned int regno, unsigned int red,
			    unsigned int green, unsigned int blue,
			    unsigned int transp, struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;

	if (regno >= ARRAY_SIZE(tfb->pseudo_palette))
		return -EINVAL;
	tfb->pseudo_palette[regno] = ((red >> 11) << 11) |
				     ((green >> 10) << 5) | (blue >> 11);
	return 0;
}

static void ta1618_damage_range(struct fb_info *info, off_t offset,
				size_t length)
{
	(void)offset;
	(void)length;
	ta1618_mark_damage(info->par);
}

static void ta1618_damage_area(struct fb_info *info, u32 x, u32 y, u32 width,
			       u32 height)
{
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	ta1618_mark_damage(info->par);
}

/* Twice the height is optional: a program unaware of it still runs. */
static int ta1618_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (var->xres != FB_WIDTH || var->yres != FB_HEIGHT ||
	    var->bits_per_pixel != 16)
		return -EINVAL;
	if (var->xres_virtual != FB_WIDTH)
		return -EINVAL;
	if (var->yres_virtual != FB_HEIGHT &&
	    var->yres_virtual != 2 * FB_HEIGHT)
		return -EINVAL;
	if (var->yoffset != 0 && var->yoffset != FB_HEIGHT)
		return -EINVAL;
	if (var->yoffset + FB_HEIGHT > var->yres_virtual)
		return -EINVAL;
	return 0;
}

static int ta1618_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;
	unsigned long flags;

	if (var->yoffset != 0 && var->yoffset != FB_HEIGHT)
		return -EINVAL;
	spin_lock_irqsave(&tfb->lock, flags);
	tfb->shown = var->yoffset;
	ta1618_record_damage_locked(tfb);
	spin_unlock_irqrestore(&tfb->lock, flags);
	return 0;
}

static int ta1618_unblank_locked(struct ta1618_fb *tfb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->stopping) {
		ret = -ENODEV;
		goto out_unlock;
	}
	switch (tfb->panel_state) {
	case TA1618_PANEL_ACTIVE:
	case TA1618_PANEL_WAKING:
		ret = 0;
		goto out_unlock;
	case TA1618_PANEL_COLD_INIT:
	case TA1618_PANEL_BLANKING:
		ret = -EBUSY;
		goto out_unlock;
	case TA1618_PANEL_ERROR:
		ret = -EIO;
		goto out_unlock;
	case TA1618_PANEL_BLANKED:
		break;
	}
	if (!tfb->wled_known || tfb->wled_on || tfb->spi_faulted ||
	    tfb->dcs_display_state != TA1618_DCS_OFF_TX_COMPLETE ||
	    tfb->dcs_sleep_state != TA1618_DCS_SLEEP_IN_TX_COMPLETE) {
		ret = -EIO;
		goto out_unlock;
	}
	tfb->panel_state = TA1618_PANEL_WAKING;
	if (!ta1618_damage_pending(tfb))
		tfb->damage_seq++;
	spin_unlock_irqrestore(&tfb->lock, flags);

	mutex_lock(&tfb->panel_lock);
	ret = ta1618_send_dcs_sleep(tfb, PANEL_SLEEP_OUT);
	if (ret) {
		ta1618_enter_error(tfb, TA1618_TRANSITION_SLEEP_OUT, ret, true);
		goto out_panel;
	}
	msleep(PANEL_SLEEP_OUT_MS);

	ret = ta1618_send_dcs_display(tfb, PANEL_DISPLAY_ON);
	if (ret) {
		ta1618_enter_error(tfb, TA1618_TRANSITION_DCS_ON, ret, true);
		goto out_panel;
	}

	ret = ta1618_start_frame(tfb, TA1618_FRAME_WAKE);
	if (ret && ret != -ESHUTDOWN)
		ta1618_enter_error(tfb, TA1618_TRANSITION_WAKE_WRITE_RAM, ret,
				   true);

out_panel:
	mutex_unlock(&tfb->panel_lock);
	if (!ret)
		schedule_delayed_work(&tfb->timeout_work,
				      msecs_to_jiffies(FRAME_TIMEOUT_MS));
	return ret == -ESHUTDOWN ? -ENODEV : ret;

out_unlock:
	spin_unlock_irqrestore(&tfb->lock, flags);
	return ret;
}

static int ta1618_blank(int blank, struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;
	unsigned long flags;
	bool turn_off = false;
	int dcs_ret;
	int ret = 0;
	int sleep_ret = 0;
	int wled_ret;

	mutex_lock(&tfb->transition_lock);
	if (blank == FB_BLANK_UNBLANK) {
		ret = ta1618_unblank_locked(tfb);
		goto out;
	}

	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->stopping) {
		ret = -ENODEV;
	} else if (tfb->panel_state == TA1618_PANEL_ERROR) {
		ret = -EIO;
	} else if (tfb->panel_state != TA1618_PANEL_BLANKED &&
		   tfb->panel_state != TA1618_PANEL_BLANKING) {
		tfb->panel_state = TA1618_PANEL_BLANKING;
		tfb->stats.blank_count++;
		turn_off = true;
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	if (!turn_off)
		goto out;

	ret = ta1618_quiesce_pipeline(tfb);
	if (ret) {
		ta1618_record_error(tfb, TA1618_TRANSITION_QUIESCE, ret);
		goto out;
	}

	mutex_lock(&tfb->panel_lock);
	wled_ret = ta1618_wled_disable_bounded(tfb);
	ta1618_note_wled_error(tfb, wled_ret);
	dcs_ret = ta1618_send_dcs_display(tfb, PANEL_DISPLAY_OFF);
	if (!dcs_ret) {
		sleep_ret = ta1618_send_dcs_sleep(tfb, PANEL_SLEEP_IN);
		if (!sleep_ret)
			msleep(PANEL_SLEEP_IN_MS);
	}
	ret = wled_ret ? wled_ret : dcs_ret ? dcs_ret : sleep_ret;
	if (ret) {
		if (wled_ret && dcs_ret) {
			spin_lock_irqsave(&tfb->lock, flags);
			tfb->stats.fail_dark_failures++;
			spin_unlock_irqrestore(&tfb->lock, flags);
		}
		ta1618_enter_error(tfb,
				   wled_ret ? TA1618_TRANSITION_WLED_OFF :
				   dcs_ret  ? TA1618_TRANSITION_DCS_OFF :
					      TA1618_TRANSITION_SLEEP_IN,
				   ret, false);
	} else {
		spin_lock_irqsave(&tfb->lock, flags);
		if (tfb->panel_state == TA1618_PANEL_BLANKING) {
			tfb->panel_state = TA1618_PANEL_BLANKED;
			tfb->stats.blank_completed++;
		} else {
			ret = -EIO;
		}
		spin_unlock_irqrestore(&tfb->lock, flags);
		ta1618_record_error(tfb, TA1618_TRANSITION_SLEEP_IN, ret);
	}
	mutex_unlock(&tfb->panel_lock);

out:
	mutex_unlock(&tfb->transition_lock);
	return ret;
}

FB_GEN_DEFAULT_DEFERRED_IOMEM_OPS(ta1618, ta1618_damage_range,
				  ta1618_damage_area)

static int ta1618_fb_open(struct fb_info *info, int user)
{
	struct ta1618_fb *tfb = info->par;
	unsigned long flags;
	int ret;

	(void)user;
	spin_lock_irqsave(&tfb->lock, flags);
	ret = tfb->stopping ? -ENODEV : 0;
	spin_unlock_irqrestore(&tfb->lock, flags);
	return ret;
}

static void ta1618_fb_destroy(struct fb_info *info)
{
	struct ta1618_fb *tfb = info->par;

	fb_dealloc_cmap(&info->cmap);
	kvfree(tfb->snapshot);
	iounmap(tfb->screen);

	mutex_lock(&ta1618_lifetime_lock);
	if (ta1618_retired_info == info)
		ta1618_retired_info = NULL;
	mutex_unlock(&ta1618_lifetime_lock);

	framebuffer_release(info);
}

static void ta1618_retire_framebuffer(struct fb_info *info)
{
	mutex_lock(&ta1618_lifetime_lock);
	ta1618_retired_info = info;
	mutex_unlock(&ta1618_lifetime_lock);

	unregister_framebuffer(info);
}

static const struct fb_ops ta1618_fb_ops = {
	.owner = THIS_MODULE,
	__FB_DEFAULT_DEFERRED_OPS_RDWR(ta1618),
	__FB_DEFAULT_DEFERRED_OPS_DRAW(ta1618),
	__FB_DEFAULT_IOMEM_OPS_MMAP,
	.fb_open = ta1618_fb_open,
	.fb_setcolreg = ta1618_setcolreg,
	.fb_check_var = ta1618_check_var,
	.fb_pan_display = ta1618_pan_display,
	.fb_blank = ta1618_blank,
	.fb_destroy = ta1618_fb_destroy,
};

static const char *ta1618_panel_state_name(enum ta1618_panel_state state)
{
	switch (state) {
	case TA1618_PANEL_COLD_INIT:
		return "COLD_INIT";
	case TA1618_PANEL_ACTIVE:
		return "ACTIVE";
	case TA1618_PANEL_BLANKING:
		return "BLANKING";
	case TA1618_PANEL_BLANKED:
		return "BLANKED";
	case TA1618_PANEL_WAKING:
		return "WAKING";
	case TA1618_PANEL_ERROR:
		return "ERROR";
	}
	return "ERROR";
}

static const char *
ta1618_dcs_display_state_name(enum ta1618_dcs_display_state state)
{
	switch (state) {
	case TA1618_DCS_OFF_TX_COMPLETE:
		return "off-tx-complete";
	case TA1618_DCS_ON_TX_COMPLETE:
		return "on-tx-complete";
	case TA1618_DCS_UNKNOWN:
		return "unknown";
	}
	return "unknown";
}

static const char *
ta1618_dcs_sleep_state_name(enum ta1618_dcs_sleep_state state)
{
	switch (state) {
	case TA1618_DCS_SLEEP_IN_TX_COMPLETE:
		return "in-tx-complete";
	case TA1618_DCS_SLEEP_OUT_TX_COMPLETE:
		return "out-tx-complete";
	case TA1618_DCS_SLEEP_UNKNOWN:
		return "unknown";
	}
	return "unknown";
}

static const char *
ta1618_transition_stage_name(enum ta1618_transition_stage stage)
{
	switch (stage) {
	case TA1618_TRANSITION_NONE:
		return "none";
	case TA1618_TRANSITION_QUIESCE:
		return "quiesce";
	case TA1618_TRANSITION_WLED_OFF:
		return "wled-off";
	case TA1618_TRANSITION_DCS_OFF:
		return "dcs-off";
	case TA1618_TRANSITION_SLEEP_IN:
		return "sleep-in";
	case TA1618_TRANSITION_SLEEP_OUT:
		return "sleep-out";
	case TA1618_TRANSITION_DCS_ON:
		return "dcs-on";
	case TA1618_TRANSITION_COLD_INIT:
		return "cold-init";
	case TA1618_TRANSITION_NORMAL_WRITE_RAM:
		return "normal-write-ram";
	case TA1618_TRANSITION_COLD_WRITE_RAM:
		return "cold-write-ram";
	case TA1618_TRANSITION_WAKE_WRITE_RAM:
		return "wake-write-ram";
	case TA1618_TRANSITION_FRAME_TIMEOUT:
		return "frame-timeout";
	case TA1618_TRANSITION_IRQ:
		return "irq";
	case TA1618_TRANSITION_WLED_RESTORE:
		return "wled-restore";
	}
	return "unknown";
}

static ssize_t audit_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct ta1618_fb *tfb = dev_get_drvdata(dev);
	struct ta1618_fb_stats stats;
	enum ta1618_panel_state panel_state;
	enum ta1618_dcs_display_state dcs_display_state;
	enum ta1618_dcs_sleep_state dcs_sleep_state;
	enum ta1618_transition_stage last_error_stage;
	unsigned long flags;
	unsigned int shown_yoffset;
	int last_error_errno;
	u8 last_dcs_command;
	bool in_flight;
	bool damage_pending;
	bool adi_poisoned;
	bool spi_faulted;
	bool wled_known;
	bool wled_on;
	ssize_t len = 0;

	(void)attr;
	spin_lock_irqsave(&tfb->lock, flags);
	stats = tfb->stats;
	panel_state = tfb->panel_state;
	dcs_display_state = tfb->dcs_display_state;
	dcs_sleep_state = tfb->dcs_sleep_state;
	last_error_stage = tfb->last_error_stage;
	shown_yoffset = tfb->shown;
	last_error_errno = tfb->last_error_errno;
	last_dcs_command = tfb->last_dcs_command;
	in_flight = tfb->in_flight;
	damage_pending = ta1618_damage_pending(tfb);
	adi_poisoned = tfb->adi_poisoned;
	spi_faulted = tfb->spi_faulted;
	wled_known = tfb->wled_known;
	wled_on = tfb->wled_on;
	spin_unlock_irqrestore(&tfb->lock, flags);

	len += sysfs_emit_at(buf, len,
			     "init_mode=cold-reset\n"
			     "completion_mode=irq-only\n"
			     "timeout_mode=finite-to-error\n"
			     "damage_mode=full-frame-coalesced\n"
			     "pan_mode=fb-pan-notify\n"
			     "lifecycle_mode=wled+dcs-display+sleep\n"
			     "panel_state=%s\n"
			     "dcs_display_state=%s\n"
			     "dcs_sleep_state=%s\n"
			     "in_flight=%u\n"
			     "damage_pending=%u\n"
			     "shown_yoffset=%u\n"
			     "wled_state=%s\n"
			     "adi_poisoned=%u\n"
			     "spi_faulted=%u\n",
			     ta1618_panel_state_name(panel_state),
			     ta1618_dcs_display_state_name(dcs_display_state),
			     ta1618_dcs_sleep_state_name(dcs_sleep_state),
			     in_flight ? 1U : 0U, damage_pending ? 1U : 0U,
			     shown_yoffset,
			     !wled_known ? "unknown" :
			     wled_on	 ? "on" :
					   "off",
			     adi_poisoned ? 1U : 0U, spi_faulted ? 1U : 0U);
	len += sysfs_emit_at(buf, len,
			     "frames_started=%llu\n"
			     "frames_done_irq=%llu\n"
			     "frame_timeouts=%llu\n"
			     "irq_spurious=%llu\n"
			     "irq_missed=%llu\n"
			     "blank_count=%llu\n"
			     "blank_completed=%llu\n"
			     "wake_count=%llu\n"
			     "dcs_errors=%llu\n"
			     "dcs_timeouts=%llu\n"
			     "wled_errors=%llu\n"
			     "fail_dark_failures=%llu\n",
			     stats.frames_started, stats.frames_done_irq,
			     stats.frame_timeouts, stats.irq_spurious,
			     stats.irq_missed, stats.blank_count,
			     stats.blank_completed, stats.wake_count,
			     stats.dcs_errors, stats.dcs_timeouts,
			     stats.wled_errors, stats.fail_dark_failures);
	len += sysfs_emit_at(buf, len,
			     "last_error_stage=%s\n"
			     "last_error_errno=%d\n"
			     "last_error_dcs_command=0x%02x\n"
			     "last_error_irq_status=0x%08x\n"
			     "last_error_irq_raw=0x%08x\n",
			     ta1618_transition_stage_name(last_error_stage),
			     last_error_errno, last_dcs_command,
			     stats.last_error_irq_status,
			     stats.last_error_irq_raw);
	return len;
}
static DEVICE_ATTR_RO(audit);

static void __iomem *ta1618_ioremap_shared(struct platform_device *pdev,
					   const char *name)
{
	struct resource *resource;
	void __iomem *base;

	resource = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!resource || resource_size(resource) < sizeof(u32))
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, resource->start,
			    resource_size(resource));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

static int ta1618_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *analogres;
	struct resource *adires;
	struct resource *fbres;
	struct resource *spires;
	struct fb_info *info;
	struct ta1618_fb *tfb;
	unsigned long flags;
	bool registered = false;
	bool display_control_ready = false;
	int cleanup_ret;
	int quiesce_ret;
	int ret;

	mutex_lock(&ta1618_lifetime_lock);
	ret = ta1618_retired_info ? -EBUSY : 0;
	mutex_unlock(&ta1618_lifetime_lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "previous framebuffer is still in use\n");

	info = framebuffer_alloc(sizeof(*tfb), dev);
	if (!info)
		return -ENOMEM;
	tfb = info->par;
	tfb->info = info;
	spin_lock_init(&tfb->lock);
	mutex_init(&tfb->transition_lock);
	mutex_init(&tfb->panel_lock);
	init_completion(&tfb->frame_done);
	INIT_WORK(&tfb->refresh_work, ta1618_refresh_work);
	INIT_WORK(&tfb->wake_work, ta1618_wake_work);
	INIT_DELAYED_WORK(&tfb->timeout_work, ta1618_timeout_work);
	tfb->panel_state = TA1618_PANEL_COLD_INIT;
	tfb->dcs_display_state = TA1618_DCS_UNKNOWN;
	tfb->dcs_sleep_state = TA1618_DCS_SLEEP_UNKNOWN;
	tfb->snapshot = kvmalloc(FB_SIZE, GFP_KERNEL);
	if (!tfb->snapshot) {
		ret = -ENOMEM;
		goto release;
	}

	fbres = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					     "framebuffer");
	if (!fbres) {
		ret = -ENODEV;
		goto release;
	}
	if (resource_size(fbres) < 3 * FB_SIZE) {
		ret = -EINVAL;
		goto release;
	}
	tfb->screen_phys = fbres->start;
	tfb->screen = ioremap_wc(fbres->start, resource_size(fbres));
	if (!tfb->screen) {
		ret = -ENOMEM;
		goto release;
	}
	tfb->transfer_phys = tfb->screen_phys + 2 * FB_SIZE;
	tfb->transfer = tfb->screen + 2 * FB_SIZE;
	tfb->lcdc = devm_platform_ioremap_resource_byname(pdev, "lcdc");
	if (IS_ERR(tfb->lcdc)) {
		ret = PTR_ERR(tfb->lcdc);
		goto release;
	}
	spires = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spi");
	tfb->spi = devm_ioremap_resource(dev, spires);
	if (IS_ERR(tfb->spi)) {
		ret = PTR_ERR(tfb->spi);
		goto release;
	}
	tfb->spi_phys = spires->start;
	tfb->lcm = devm_platform_ioremap_resource_byname(pdev, "lcm");
	if (IS_ERR(tfb->lcm)) {
		ret = PTR_ERR(tfb->lcm);
		goto release;
	}
	tfb->spi_clock_selector = devm_platform_ioremap_resource_byname(
		pdev, "spi-clock-selector");
	if (IS_ERR(tfb->spi_clock_selector)) {
		ret = PTR_ERR(tfb->spi_clock_selector);
		goto release;
	}
	tfb->spi_reset_set =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-set");
	if (IS_ERR(tfb->spi_reset_set)) {
		ret = PTR_ERR(tfb->spi_reset_set);
		goto release;
	}
	tfb->spi_reset_clear =
		devm_platform_ioremap_resource_byname(pdev, "spi-reset-clear");
	if (IS_ERR(tfb->spi_reset_clear)) {
		ret = PTR_ERR(tfb->spi_reset_clear);
		goto release;
	}
	tfb->pinmux = devm_platform_ioremap_resource_byname(pdev, "pinmux");
	if (IS_ERR(tfb->pinmux)) {
		ret = PTR_ERR(tfb->pinmux);
		goto release;
	}
	tfb->pinconf = devm_platform_ioremap_resource_byname(pdev, "pinconf");
	if (IS_ERR(tfb->pinconf)) {
		ret = PTR_ERR(tfb->pinconf);
		goto release;
	}
	tfb->ap_ahb_gate_set = ta1618_ioremap_shared(pdev, "ap-ahb-gate-set");
	if (IS_ERR(tfb->ap_ahb_gate_set)) {
		ret = PTR_ERR(tfb->ap_ahb_gate_set);
		goto release;
	}
	tfb->ap_ahb_reset_set = ta1618_ioremap_shared(pdev, "ap-ahb-reset-set");
	if (IS_ERR(tfb->ap_ahb_reset_set)) {
		ret = PTR_ERR(tfb->ap_ahb_reset_set);
		goto release;
	}
	tfb->ap_ahb_reset_clear =
		ta1618_ioremap_shared(pdev, "ap-ahb-reset-clear");
	if (IS_ERR(tfb->ap_ahb_reset_clear)) {
		ret = PTR_ERR(tfb->ap_ahb_reset_clear);
		goto release;
	}
	tfb->aon_apb =
		syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(tfb->aon_apb)) {
		ret = dev_err_probe(dev, PTR_ERR(tfb->aon_apb),
				    "could not resolve AON APB syscon\n");
		goto release;
	}
	adires = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					      "adi-controller");
	analogres = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						 "analog-slave");
	if (!adires || !analogres ||
	    resource_size(adires) < ADI_USER_LOCK + sizeof(u32) ||
	    resource_size(analogres) < ANA_LDO_PD_CTRL + sizeof(u32)) {
		ret = -EINVAL;
		goto release;
	}
	tfb->adi = devm_ioremap(dev, adires->start, resource_size(adires));
	tfb->analog =
		devm_ioremap(dev, analogres->start, resource_size(analogres));
	if (!tfb->adi || !tfb->analog) {
		ret = -ENOMEM;
		goto release;
	}
	tfb->irq = platform_get_irq_optional(pdev, 0);
	if (tfb->irq < 0) {
		ret = dev_err_probe(dev, tfb->irq,
				    "LCDC_DONE IRQ is required\n");
		goto release;
	}
	ta1618_configure_display_pins(tfb);
	ret = ta1618_enable_display_blocks(tfb);
	if (ret)
		goto release;
	ta1618_stop_lcdc(tfb);

	strscpy(info->fix.id, "ta1618-rgb565", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.ypanstep = FB_HEIGHT;
	info->fix.smem_start = tfb->screen_phys;
	info->fix.smem_len = 2 * FB_SIZE;
	info->fix.line_length = FB_STRIDE;

	info->var.xres = FB_WIDTH;
	info->var.yres = FB_HEIGHT;
	info->var.xres_virtual = FB_WIDTH;
	info->var.yres_virtual = FB_HEIGHT;
	info->var.bits_per_pixel = 16;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	info->fbops = &ta1618_fb_ops;
	info->screen_base = tfb->screen;
	info->screen_size = 2 * FB_SIZE;
	info->pseudo_palette = tfb->pseudo_palette;

	ret = fb_alloc_cmap(&info->cmap, 16, 0);
	if (ret)
		goto release;
	ret = devm_request_irq(dev, tfb->irq, ta1618_lcdc_irq, 0, dev_name(dev),
			       tfb);
	if (ret)
		goto cmap;
	platform_set_drvdata(pdev, tfb);
	display_control_ready = true;

	ret = ta1618_cold_init(tfb);
	if (ret) {
		dev_err(dev, "cold display initialization failed at %s: %d\n",
			ta1618_transition_stage_name(
				READ_ONCE(tfb->last_error_stage)),
			ret);
		goto stop;
	}

	ret = register_framebuffer(info);
	registered = refcount_read(&info->count) != 0;
	if (ret)
		goto stop;

	console_lock();
	lock_fb_info(info);
	spin_lock_irqsave(&tfb->lock, flags);
	if (tfb->panel_state == TA1618_PANEL_ERROR) {
		ret = -EIO;
	} else {
		ret = 0;
		if (ta1618_panel_refresh_enabled(tfb->panel_state)) {
			info->blank = FB_BLANK_UNBLANK;
			ta1618_force_refresh_locked(tfb);
		}
	}
	spin_unlock_irqrestore(&tfb->lock, flags);
	unlock_fb_info(info);
	console_unlock();
	if (ret)
		goto stop;

	ret = device_create_file(dev, &dev_attr_audit);
	if (ret)
		goto stop;
	tfb->audit_file_created = true;

	dev_info(dev, "TA-1618 RGB565 framebuffer registered as fb%d\n",
		 info->node);
	return 0;

stop:
	if (tfb->audit_file_created) {
		device_remove_file(dev, &dev_attr_audit);
		tfb->audit_file_created = false;
	}
	mutex_lock(&tfb->transition_lock);
	spin_lock_irq(&tfb->lock);
	tfb->stopping = true;
	spin_unlock_irq(&tfb->lock);
	mutex_unlock(&tfb->transition_lock);
	if (registered)
		console_lock();
	cancel_work_sync(&tfb->wake_work);
	mutex_lock(&tfb->transition_lock);
	quiesce_ret = ta1618_quiesce_pipeline(tfb);
	cleanup_ret = 0;
	if (!quiesce_ret && display_control_ready) {
		mutex_lock(&tfb->panel_lock);
		cleanup_ret = ta1618_fail_dark_locked(tfb);
		mutex_unlock(&tfb->panel_lock);
	}
	mutex_unlock(&tfb->transition_lock);
	if (cleanup_ret)
		dev_err(dev,
			"could not fail display dark after probe failure: %d\n",
			cleanup_ret);
	display_control_ready = false;
	if (registered)
		console_unlock();
	devm_free_irq(dev, tfb->irq, tfb);
	platform_set_drvdata(pdev, NULL);
	if (registered) {
		ta1618_retire_framebuffer(info);
		return ret;
	}

cmap:
	fb_dealloc_cmap(&info->cmap);
release:
	if (display_control_ready) {
		mutex_lock(&tfb->panel_lock);
		cleanup_ret = ta1618_fail_dark_locked(tfb);
		mutex_unlock(&tfb->panel_lock);
		if (cleanup_ret)
			dev_err(dev,
				"could not fail display dark after probe failure: %d\n",
				cleanup_ret);
	}
	if (tfb->screen)
		iounmap(tfb->screen);
	kvfree(tfb->snapshot);
	framebuffer_release(info);
	return ret;
}

static void ta1618_fb_remove(struct platform_device *pdev)
{
	struct ta1618_fb *tfb = platform_get_drvdata(pdev);
	struct fb_info *info = tfb->info;
	unsigned long flags;
	int cleanup_ret = 0;
	int quiesce_ret;

	mutex_lock(&tfb->transition_lock);
	spin_lock_irqsave(&tfb->lock, flags);
	tfb->stopping = true;
	spin_unlock_irqrestore(&tfb->lock, flags);
	mutex_unlock(&tfb->transition_lock);

	if (tfb->audit_file_created) {
		device_remove_file(&pdev->dev, &dev_attr_audit);
		tfb->audit_file_created = false;
	}
	console_lock();
	cancel_work_sync(&tfb->wake_work);
	mutex_lock(&tfb->transition_lock);
	quiesce_ret = ta1618_quiesce_pipeline(tfb);
	if (!quiesce_ret) {
		mutex_lock(&tfb->panel_lock);
		cleanup_ret = ta1618_fail_dark_locked(tfb);
		mutex_unlock(&tfb->panel_lock);
	}
	mutex_unlock(&tfb->transition_lock);
	console_unlock();
	if (cleanup_ret)
		dev_err(&pdev->dev,
			"could not fail display dark during remove: %d\n",
			cleanup_ret);
	devm_free_irq(&pdev->dev, tfb->irq, tfb);
	platform_set_drvdata(pdev, NULL);
	ta1618_retire_framebuffer(info);
}

static const struct of_device_id ta1618_fb_of_match[] = {
	{ .compatible = "fplinux,ta1618-fb" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_fb_of_match);

static struct platform_driver ta1618_fb_driver = {
	.probe = ta1618_fb_probe,
	.remove = ta1618_fb_remove,
	.driver = {
		.name = "ta1618-fb",
		.of_match_table = ta1618_fb_of_match,
	},
};
module_platform_driver(ta1618_fb_driver);

MODULE_DESCRIPTION("Nokia TA-1618 framebuffer and ST7789P3 panel driver");
MODULE_LICENSE("GPL");
