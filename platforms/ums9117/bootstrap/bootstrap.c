// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bootstrap.h"
#include "syscode.h"

#define UMS9117_AON_TIMER_BASE_PHYS 0x40050000U
#define UMS9117_AP_SYSCNT_PHYS 0x4023000cU
#define UMS9117_AON_APB_EB0_PHYS 0x402e0000U
#define UMS9117_AON_APB_RTC_EB_PHYS 0x402e0010U
#define UMS9117_AON_APB_CLK_EB0_PHYS 0x402e0134U
#define UMS9117_MUSB_BASE_PHYS 0x20200000U

#define BIT(n) (1U << (n))
#define UMS9117_TIMER_EB0_BITS (BIT(11) | BIT(10))
#define UMS9117_TIMER_RTC_BITS (BIT(4) | BIT(3))
#define UMS9117_TIMER_CLK_BITS BIT(11)

#define UMS9117_TIMER_LOAD_LO 0x00U
#define UMS9117_TIMER_LOAD_HI 0x04U
#define UMS9117_TIMER_VALUE_LO 0x08U
#define UMS9117_TIMER_CTL 0x10U
#define UMS9117_TIMER_INT 0x14U
#define UMS9117_TIMER_SHDW_LO 0x18U
#define UMS9117_TIMER_CTL_PERIOD BIT(0)
#define UMS9117_TIMER_CTL_ENABLE BIT(1)
#define UMS9117_TIMER_CTL_64BIT BIT(16)
#define UMS9117_TIMER_INT_EN BIT(0)
#define UMS9117_TIMER_INT_RAW BIT(1)
#define UMS9117_TIMER_INT_MASK BIT(2)
#define UMS9117_TIMER_INT_CLR BIT(3)

#define UMS9117_MUSB_POWER 0x001U
#define UMS9117_MUSB_DMA_CHANNEL(n) (0x1c00U + ((n) - 1U) * 0x20U)
#define UMS9117_MUSB_DMA_PAUSE 0x00U
#define UMS9117_MUSB_DMA_CFG 0x04U
#define UMS9117_MUSB_DMA_INTR 0x08U
#define UMS9117_MUSB_DMA_LLIST_PTR 0x14U
#define UMS9117_MUSB_DMA_CHN_EN BIT(0)
#define UMS9117_MUSB_DMA_CLEAR_INT_EN BIT(5)
#define UMS9117_MUSB_DMA_CHN_CLR BIT(15)
#define UMS9117_MUSB_DMA_CLR_STATUS BIT(21)
#define UMS9117_MUSB_DMA_INTR_CLEAR \
	(BIT(24) | BIT(25) | BIT(26) | BIT(27) | BIT(28))
#define UMS9117_MUSB_POWER_SOFTCONN BIT(6)
#define UMS9117_MUSB_RXCSR_DMA_BITS (BIT(15) | BIT(13) | BIT(11))

extern const unsigned char linux_zimage_start[];
extern const unsigned char linux_zimage_end[];
extern const unsigned char linux_dtb_start[];
extern const unsigned char linux_dtb_end[];

static uint32_t reg_read(uint32_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}

static uint16_t reg_read16(uint32_t address)
{
	return *(volatile uint16_t *)(uintptr_t)address;
}

static uint8_t reg_read8(uint32_t address)
{
	return *(volatile uint8_t *)(uintptr_t)address;
}

static void reg_write(uint32_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
}

static void reg_write16(uint32_t address, uint16_t value)
{
	*(volatile uint16_t *)(uintptr_t)address = value;
}

static void reg_write8(uint32_t address, uint8_t value)
{
	*(volatile uint8_t *)(uintptr_t)address = value;
}

static void reg_or(uint32_t address, uint32_t bits)
{
	reg_write(address, reg_read(address) | bits);
}

void lcd_appinit(void)
{
	struct sys_display *display = &sys_data.display;

	display->w2 = display->w1;
	display->h2 = display->h1;
}

void keytrn_init(void)
{
	uint8_t map[64];
	int index;

	(void)sys_getkeymap(map);
	for (index = 0; index < 64; ++index) {
		sys_data.keytrn[0][index] = map[index];
		sys_data.keytrn[1][index] = map[index] | 0x8000;
	}
}

void ums9117_bootstrap_enable_timer_gates(
	struct ums9117_bootstrap_timer_gates *snapshot)
{
	if (snapshot != NULL) {
		snapshot->eb0_before = reg_read(UMS9117_AON_APB_EB0_PHYS);
		snapshot->rtc_before = reg_read(UMS9117_AON_APB_RTC_EB_PHYS);
		snapshot->clk_before = reg_read(UMS9117_AON_APB_CLK_EB0_PHYS);
	}
	reg_or(UMS9117_AON_APB_EB0_PHYS, UMS9117_TIMER_EB0_BITS);
	reg_or(UMS9117_AON_APB_RTC_EB_PHYS, UMS9117_TIMER_RTC_BITS);
	reg_or(UMS9117_AON_APB_CLK_EB0_PHYS, UMS9117_TIMER_CLK_BITS);
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
	if (snapshot != NULL) {
		snapshot->eb0_after = reg_read(UMS9117_AON_APB_EB0_PHYS);
		snapshot->rtc_after = reg_read(UMS9117_AON_APB_RTC_EB_PHYS);
		snapshot->clk_after = reg_read(UMS9117_AON_APB_CLK_EB0_PHYS);
	}
}

int ums9117_bootstrap_probe_timer(struct ums9117_bootstrap_timer_result *result)
{
	if (result == NULL)
		return 0;

	result->ctl_before =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL,
		  result->ctl_before & ~UMS9117_TIMER_CTL_ENABLE);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT) |
			  UMS9117_TIMER_INT_CLR);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_LOAD_HI, 0);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_LOAD_LO, 256);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  UMS9117_TIMER_INT_EN);
	result->ctl = (result->ctl_before &
		       ~(UMS9117_TIMER_CTL_PERIOD | UMS9117_TIMER_CTL_64BIT)) |
		      UMS9117_TIMER_CTL_ENABLE;

	result->syscnt_before = reg_read(UMS9117_AP_SYSCNT_PHYS);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL, result->ctl);
	result->polls = 0;
	do {
		result->int_status = reg_read(UMS9117_AON_TIMER_BASE_PHYS +
					      UMS9117_TIMER_INT);
		result->syscnt_after = reg_read(UMS9117_AP_SYSCNT_PHYS);
	} while (!(result->int_status &
		   (UMS9117_TIMER_INT_RAW | UMS9117_TIMER_INT_MASK)) &&
		 (uint32_t)(result->syscnt_after - result->syscnt_before) <
			 50 &&
		 ++result->polls < 5000000UL);

	result->value =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_VALUE_LO);
	result->shadow =
		reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_SHDW_LO);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL,
		  reg_read(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_CTL) &
			  ~UMS9117_TIMER_CTL_ENABLE);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT,
		  result->int_status | UMS9117_TIMER_INT_CLR);
	reg_write(UMS9117_AON_TIMER_BASE_PHYS + UMS9117_TIMER_INT, 0);
	__asm__ volatile("dsb sy" : : : "memory");

	return result->syscnt_after != result->syscnt_before &&
	       (result->int_status &
		(UMS9117_TIMER_INT_RAW | UMS9117_TIMER_INT_MASK));
}

uint32_t ums9117_bootstrap_quiesce_usb_dma_channel(unsigned channel)
{
	uint32_t base =
		UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_DMA_CHANNEL(channel);
	uint32_t cfg;
	uint32_t intr;
	uint32_t pause;
	unsigned long polls;

	cfg = reg_read(base + UMS9117_MUSB_DMA_CFG);
	if (!(cfg & UMS9117_MUSB_DMA_CHN_EN)) {
		reg_write(base + UMS9117_MUSB_DMA_LLIST_PTR, 0);
		reg_write(base + UMS9117_MUSB_DMA_PAUSE, 0);
		__asm__ volatile("dsb sy" : : : "memory");
		return UMS9117_BOOTSTRAP_DMA_OK;
	}

	pause = reg_read(base + UMS9117_MUSB_DMA_PAUSE);
	intr = reg_read(base + UMS9117_MUSB_DMA_INTR);
	reg_write(base + UMS9117_MUSB_DMA_INTR,
		  intr | UMS9117_MUSB_DMA_CLEAR_INT_EN);
	reg_write(base + UMS9117_MUSB_DMA_PAUSE,
		  pause | UMS9117_MUSB_DMA_CHN_CLR);
	__asm__ volatile("dsb sy" : : : "memory");

	for (polls = 0; polls < 1000000UL; ++polls) {
		intr = reg_read(base + UMS9117_MUSB_DMA_INTR);
		if (intr & UMS9117_MUSB_DMA_CLR_STATUS)
			break;
	}

	reg_write(base + UMS9117_MUSB_DMA_INTR,
		  intr | UMS9117_MUSB_DMA_INTR_CLEAR);
	reg_write(base + UMS9117_MUSB_DMA_CFG,
		  reg_read(base + UMS9117_MUSB_DMA_CFG) &
			  ~UMS9117_MUSB_DMA_CHN_EN);
	reg_write(base + UMS9117_MUSB_DMA_LLIST_PTR, 0);
	reg_write(base + UMS9117_MUSB_DMA_PAUSE, 0);
	__asm__ volatile("dsb sy" : : : "memory");

	return (intr & UMS9117_MUSB_DMA_CLR_STATUS ?
			UMS9117_BOOTSTRAP_DMA_CLEAR_SEEN :
			0) |
	       (reg_read(base + UMS9117_MUSB_DMA_CFG) &
				UMS9117_MUSB_DMA_CHN_EN ?
			0 :
			UMS9117_BOOTSTRAP_DMA_DISABLED);
}

void ums9117_bootstrap_cleanup_usb_dma_and_disconnect(void)
{
	uint16_t csr;

	csr = reg_read16(UMS9117_MUSB_BASE_PHYS + 0x152U);
	csr &= ~(BIT(15) | BIT(12));
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x152U, csr);
	csr &= ~BIT(10);
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x152U, csr);
	csr = reg_read16(UMS9117_MUSB_BASE_PHYS + 0x166U);
	reg_write16(UMS9117_MUSB_BASE_PHYS + 0x166U,
		    csr & ~UMS9117_MUSB_RXCSR_DMA_BITS);
	reg_write8(UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_POWER,
		   reg_read8(UMS9117_MUSB_BASE_PHYS + UMS9117_MUSB_POWER) &
			   ~UMS9117_MUSB_POWER_SOFTCONN);
	__asm__ volatile("dsb sy\n\tisb" : : : "memory");
}

size_t ums9117_bootstrap_zimage_size(void)
{
	return (size_t)(linux_zimage_end - linux_zimage_start);
}

size_t ums9117_bootstrap_dtb_size(void)
{
	return (size_t)(linux_dtb_end - linux_dtb_start);
}

void ums9117_bootstrap_copy_zimage(uint32_t destination, size_t bytes)
{
	memmove((void *)(uintptr_t)destination, linux_zimage_start, bytes);
	clean_dcache_range((void *)(uintptr_t)destination,
			   (void *)(uintptr_t)(destination + bytes));
}

void ums9117_bootstrap_copy_dtb(uint32_t destination, size_t bytes)
{
	memcpy((void *)(uintptr_t)destination, linux_dtb_start, bytes);
	clean_dcache_range((void *)(uintptr_t)destination,
			   (void *)(uintptr_t)(destination + bytes));
}
