// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host component: real LCM object and fb/ADI layouts, fake kernel and MMIO.
 * Register facts are independent literals: CTRL +0, STATUS +8 bit 1 marks
 * writebufferBUSY, CS0 mode +0x10. Command/data writes occupy that buffer.
 * This checks transport admission and packing, not panel or bus timing.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <linux/iopoll.h>
#include "ums9117-fb-internal.h"

static struct {
	u32 registers[6];
	u16 command;
	u16 data;
	u64 now;
	u64 busy_until;
	u64 port_busy_us;
	unsigned int writes;
	unsigned int unsafe_writes;
} peripheral;
static unsigned int failures;

#define CHECK(condition, message)                                       \
	do {                                                            \
		if (!(condition)) {                                     \
			fprintf(stderr, "%s: %s\n", __func__, message); \
			failures++;                                     \
		}                                                       \
	} while (0)

u64 lcm_fake_time_us(void)
{
	return peripheral.now;
}

void lcm_fake_delay_us(unsigned long delay)
{
	peripheral.now += delay;
	if (peripheral.now >= peripheral.busy_until)
		peripheral.registers[2] = 0;
}

u32 readl(const void *address)
{
	return *(const u32 *)address;
}

static void record_write(void)
{
	peripheral.writes++;
	if (peripheral.registers[2] & 2)
		peripheral.unsafe_writes++;
}

void writel(u32 value, void *address)
{
	record_write();
	*(u32 *)address = value;
}

void writew(u16 value, void *address)
{
	record_write();
	*(u16 *)address = value;
	if (peripheral.port_busy_us) {
		peripheral.registers[2] = 2;
		peripheral.busy_until =
			peripheral.port_busy_us == UINT64_MAX ?
				UINT64_MAX :
				peripheral.now + peripheral.port_busy_us;
	}
}

static struct ums9117_fb reset_peripheral(void)
{
	memset(&peripheral, 0, sizeof(peripheral));
	peripheral.registers[0] = 0x11110000;
	return (struct ums9117_fb){
		.lcm = peripheral.registers,
		.lcm_command = &peripheral.command,
		.lcm_data = &peripheral.data,
	};
}

static void busy_buffer_rejects_command_and_frame(void)
{
	struct ums9117_fb ufb = reset_peripheral();
	const u8 parameter = 0x55;
	int result;

	peripheral.registers[2] = 2;
	peripheral.busy_until = UINT64_MAX;
	result = ums9117_fb_lcm_dcs(&ufb, 0x3a, &parameter, 1);
	CHECK(result == -ETIMEDOUT, "busy DCS must time out");
	CHECK(peripheral.writes == 0, "busy DCS must leave MMIO untouched");
	CHECK(peripheral.now > 0, "busy DCS must poll before timing out");

	ufb = reset_peripheral();
	peripheral.registers[2] = 2;
	peripheral.busy_until = UINT64_MAX;
	result = ums9117_fb_lcm_begin_frame(&ufb);
	CHECK(result == -ETIMEDOUT, "busy frame must time out");
	CHECK(peripheral.writes == 0, "busy frame must leave MMIO untouched");
}

static void completing_buffer_accepts_command_parameter(void)
{
	struct ums9117_fb ufb = reset_peripheral();
	const u8 parameter = 0x55;
	int result;

	/* Every port write occupies the buffer until virtual time advances. */
	peripheral.port_busy_us = 7;
	result = ums9117_fb_lcm_dcs(&ufb, 0x3a, &parameter, 1);
	CHECK(result == 0, "completed DCS must succeed");
	CHECK(peripheral.command == 0x3a,
	      "DCS command must reach command port");
	CHECK(peripheral.data == 0x55, "DCS parameter must reach data port");
	CHECK(peripheral.registers[4] == 1, "DCS must use command packing");
	CHECK(peripheral.registers[2] == 0,
	      "DCS must finish with buffer drained");
	CHECK(peripheral.unsafe_writes == 0,
	      "DCS must not write into busy buffer");
}

static void stalled_ramwr_keeps_command_packing(void)
{
	struct ums9117_fb ufb = reset_peripheral();
	int result;

	peripheral.port_busy_us = UINT64_MAX;
	result = ums9117_fb_lcm_begin_frame(&ufb);
	CHECK(result == -ETIMEDOUT, "stalled RAMWR must time out");
	CHECK(peripheral.command == 0x2c, "frame must issue RAMWR");
	CHECK(peripheral.registers[4] == 1,
	      "pending RAMWR must keep command packing");
	CHECK(peripheral.unsafe_writes == 0,
	      "pending RAMWR must prevent mode writes");
}

static void completed_ramwr_accepts_pixel_packing(void)
{
	struct ums9117_fb ufb = reset_peripheral();
	int result;

	peripheral.port_busy_us = 7;
	result = ums9117_fb_lcm_begin_frame(&ufb);
	CHECK(result == 0, "completed RAMWR must admit frame");
	CHECK(peripheral.command == 0x2c, "frame must issue RAMWR");
	CHECK(peripheral.registers[4] == 0x28,
	      "frame must select pixel packing");
	CHECK(peripheral.unsafe_writes == 0,
	      "pixel packing must wait for RAMWR");
}

int main(void)
{
	busy_buffer_rejects_command_and_frame();
	completing_buffer_accepts_command_parameter();
	stalled_ramwr_keeps_command_packing();
	completed_ramwr_accepts_pixel_packing();
	return failures ? 1 : 0;
}
