/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USB_CONTROL_COMPAT_COMMON_H
#define USB_CONTROL_COMPAT_COMMON_H

#include <stdint.h>
#include <string.h>

uint8_t *usb_test_mem1(uint32_t address);
uint16_t *usb_test_mem2(uint32_t address);
uint32_t *usb_test_mem4(uint32_t address);

#define MEM1(address) (*usb_test_mem1(address))
#define MEM2(address) (*usb_test_mem2(address))
#define MEM4(address) (*usb_test_mem4(address))
#define ALIGN(bytes) __attribute__((aligned(bytes)))

#endif
