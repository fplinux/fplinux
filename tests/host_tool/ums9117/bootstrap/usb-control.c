// SPDX-License-Identifier: GPL-2.0-only
/* Controlled register peer for the bootstrap USB entry points. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbio.h"

#define USB_FIFO 0x20200020

static uint8_t fifo[128];
static size_t fifo_accesses;
static uint8_t endpoint_control;
static uint8_t endpoint_index;
static uint8_t bus_event;
static uint16_t interrupt_status;
static uint32_t dma_status;

uint8_t *usb_test_mem1(uint32_t address)
{
	switch (address) {
	case USB_FIFO:
		if (fifo_accesses == sizeof(fifo)) {
			fprintf(stderr, "too many FIFO accesses\n");
			exit(1);
		}
		return &fifo[fifo_accesses++];
	case 0x20200012:
		return &endpoint_control;
	case 0x2020000e:
		return &endpoint_index;
	case 0x2020000a:
		return &bus_event;
	default:
		fprintf(stderr, "unexpected byte register: %08x\n", address);
		exit(1);
	}
}

uint16_t *usb_test_mem2(uint32_t address)
{
	if (address == 0x20200002)
		return &interrupt_status;
	fprintf(stderr, "unexpected halfword register: %08x\n", address);
	exit(1);
}

uint32_t *usb_test_mem4(uint32_t address)
{
	if (address == 0x20201e88)
		return &dma_status;
	fprintf(stderr, "unexpected word register: %08x\n", address);
	exit(1);
}

void clean_dcache_range(void *start, void *end)
{
	(void)start;
	(void)end;
}

void invalidate_dcache_range(void *start, void *end)
{
	(void)start;
	(void)end;
}

int main(int argc, char **argv)
{
	unsigned descriptor, length;
	size_t i;
	int result;

	if (argc == 2 && !strcmp(argv[1], "empty")) {
		uint8_t data[4] = { 0xaa, 0xbb, 0xcc, 0xdd };

		result = usb_read(data, sizeof(data), USB_NOWAIT);
		printf("%d %02x%02x%02x%02x\n", result, data[0], data[1],
		       data[2], data[3]);
		return 0;
	}
	if (argc != 3)
		return 2;
	descriptor = (unsigned)strtoul(argv[1], NULL, 10);
	length = (unsigned)strtoul(argv[2], NULL, 10);
	if (descriptor > 255 || length > 255)
		return 2;

	/* Each FIFO lvalue represents one hardware access: eight input bytes,
	 * followed by the bytes sent to the host.
	 */
	fifo[0] = 0x80;
	fifo[1] = 0x06;
	fifo[3] = descriptor;
	fifo[6] = length;
	endpoint_control = 1;
	interrupt_status = 1;
	result = usb_getchar(USB_NOWAIT);
	printf("%d %02x %zu ", result, endpoint_control, fifo_accesses);
	for (i = 8; i < fifo_accesses; i++)
		printf("%02x", fifo[i]);
	putchar('\n');
	return 0;
}
