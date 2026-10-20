// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MEM4(address) (*(volatile uint32_t *)(uintptr_t)(address))
#define ERR_EXIT(...) abort()

struct analog_write {
	uint32_t address;
	uint32_t value;
};

static struct analog_write analog_writes[8];
static size_t analog_count;

uint32_t sys_timer_ms(void)
{
	return 0;
}

uint32_t adi_read(uint32_t address)
{
	(void)address;
	return 0;
}

void adi_write(uint32_t address, uint32_t value)
{
	assert(analog_count < 8);
	assert(MEM4(0x402e1000) == 0x100000);
	assert(MEM4(0x402e10b0) == 0x1000);
	assert(MEM4(0x402a0024) == 0x3b00);
	assert(MEM4(0x402a0018) == 0xf1234567);
	analog_writes[analog_count++] = (struct analog_write){ address, value };
}

typedef struct {
	uint32_t id;
	uint32_t id_mask;
	uint16_t width;
	uint16_t height;
	uint16_t memory_access;
	uint8_t timing_ns[6];
	struct {
		uint32_t hz;
	} spi;
	const uint8_t *commands;
} lcd_config_t;

#define LCM_CMD(command, length) (0x80 | (length)), (command)
#define LCM_DELAY(ms) (0x40 | (((ms) >> 8) & 0x1f)), ((ms) & 0xff)
#define LCM_END 0

#ifdef FPLINUX_TEST_SPI_SEND
static void spi_send(unsigned index, unsigned data)
{
	(void)index;
	(void)data;
}
#endif

#include "lcd_config.h"

static void *map_registers(uintptr_t address)
{
	void *mapping = mmap((void *)address, 0x1000, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			     -1, 0);

	assert(mapping == (void *)address);
	memset(mapping, 0xa5, 0x1000);
	return mapping;
}

int main(void)
{
	void *pins = map_registers(0x402a0000);
	void *gates = map_registers(0x402e1000);
	size_t index;

#ifdef FPLINUX_LCD_SEND
	(void)&FPLINUX_LCD_SEND;
#endif
#ifdef FPLINUX_LCD_BACKLIGHT_START
	(void)&FPLINUX_LCD_BACKLIGHT_START;
#endif
	MEM4(0x402a0018) = 0xf1234567;
	FPLINUX_BOARD_INIT();
	assert(MEM4(0x402a0018) == 0xe0000000);
	assert(MEM4(0x402a00b0) == 0xa5a5a5a5);
	assert(MEM4(0x402a0488) == 0xa5a5a5a5);
	assert(MEM4(0x402a04f0) == 0xa5a5a5a5);
	assert(MEM4(0x402a0520) == 0xa5a5a5a5);
	assert(MEM4(0x402a0554) == 0xa5a5a5a5);
	printf("foundation %x %x %x %x\n", MEM4(0x402a00dc), MEM4(0x402a04dc),
	       MEM4(0x402a0510), MEM4(0x402a0558));
	printf("nand-data6 %x\n", MEM4(0x402a0578));
	printf("radio %x %x %x\n", MEM4(0x402a0428), MEM4(0x402a0220),
	       MEM4(0x402a0620));
	printf("camera %x %x %x %x\n", MEM4(0x402a056c), MEM4(0x402a0570),
	       MEM4(0x402a05e8), MEM4(0x402a05f4));
	fputs("analog", stdout);
	for (index = 0; index < analog_count; index++)
		printf(" %x:%x", analog_writes[index].address,
		       analog_writes[index].value);
	putchar('\n');
	FPLINUX_LCD_PREPARE();
	printf("panel %x %x %x %x\n", MEM4(0x402a00b0), MEM4(0x402a04b0),
	       MEM4(0x402a00c8), MEM4(0x402a01f8));
	assert(munmap(gates, 0x1000) == 0);
	assert(munmap(pins, 0x1000) == 0);
	return 0;
}
