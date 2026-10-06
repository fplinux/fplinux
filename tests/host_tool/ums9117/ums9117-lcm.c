// SPDX-License-Identifier: GPL-2.0-only
/*
 * Host component: real LCM object and display layouts, fake kernel and MMIO.
 * Register facts are independent literals: CTRL +0, STATUS +8 bit 1 marks
 * writebufferBUSY, CS0 mode +0x10. Command/data writes occupy that buffer.
 * Initialization uses a fake timing property and named MMIO resources.
 * This checks timing encoding and transport admission, not panel or bus timing.
 */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <linux/iopoll.h>
#include <linux/of.h>
#include "ums9117-drm-internal.h"

struct device_node {
	const u32 *timings;
	size_t timing_count;
};

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
static struct resource data_resource = { .start = 0x60000000 };

#define CHECK(condition, message)                                       \
	do {                                                            \
		if (!(condition)) {                                     \
			fprintf(stderr, "%s: %s\n", __func__, message); \
			failures++;                                     \
		}                                                       \
	} while (0)

int of_property_read_u32_array(const struct device_node *node, const char *name,
			       u32 *values, size_t count)
{
	if (!node || strcmp(name, "sprd,dbi-timing-ns") ||
	    count != node->timing_count)
		return -EINVAL;
	memcpy(values, node->timings, count * sizeof(*values));
	return 0;
}

void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name)
{
	(void)pdev;
	if (!strcmp(name, "lcm"))
		return peripheral.registers;
	if (!strcmp(name, "lcm-command"))
		return &peripheral.command;
	if (!strcmp(name, "lcm-data"))
		return &peripheral.data;
	return (void *)(intptr_t)-EINVAL;
}

struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name)
{
	(void)pdev;
	if (type == IORESOURCE_MEM && !strcmp(name, "lcm-data"))
		return &data_resource;
	return NULL;
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	(void)dev;
	(void)format;
	return error;
}

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

static struct ums9117_drm reset_peripheral(void)
{
	memset(&peripheral, 0, sizeof(peripheral));
	peripheral.registers[0] = 0x11110000;
	return (struct ums9117_drm){
		.lcm = peripheral.registers,
		.lcm_command = &peripheral.command,
		.lcm_data = &peripheral.data,
	};
}

static void inoi244_timing_tuple_encodes_firmware_value(void)
{
	const u32 timings[] = { 5, 150, 150, 30, 80, 120 };
	struct device_node node = {
		.timings = timings,
		.timing_count = ARRAY_SIZE(timings),
	};
	struct platform_device pdev = { .dev.of_node = &node };
	struct ums9117_drm udrm = reset_peripheral();
	int result = ums9117_drm_lcm_init_transport(&udrm, &pdev);

	CHECK(result == 0, "valid timing tuple must initialize transport");
	/* Independent packed word from the phone's stock firmware tables. */
	CHECK(udrm.lcm_timing == 0x031d0bc1,
	      "INOI 244 timing must match its firmware value");
	CHECK(udrm.stream_phys == 0x60000000,
	      "pixel stream must use the data resource address");
}

static void missing_timing_tuple_rejects_initialization(void)
{
	struct device_node node = {};
	struct platform_device pdev = { .dev.of_node = &node };
	struct ums9117_drm udrm = reset_peripheral();
	int result = ums9117_drm_lcm_init_transport(&udrm, &pdev);

	CHECK(result == -EINVAL, "missing timing tuple must be rejected");
	CHECK(peripheral.writes == 0,
	      "invalid timing must leave MMIO untouched");
}

static void busy_buffer_rejects_command_and_frame(void)
{
	struct ums9117_drm ufb = reset_peripheral();
	const u8 parameter = 0x55;
	int result;

	peripheral.registers[2] = 2;
	peripheral.busy_until = UINT64_MAX;
	result = ums9117_drm_lcm_dcs(&ufb, 0x3a, &parameter, 1);
	CHECK(result == -ETIMEDOUT, "busy DCS must time out");
	CHECK(peripheral.writes == 0, "busy DCS must leave MMIO untouched");
	CHECK(peripheral.now > 0, "busy DCS must poll before timing out");

	ufb = reset_peripheral();
	peripheral.registers[2] = 2;
	peripheral.busy_until = UINT64_MAX;
	result = ums9117_drm_lcm_begin_frame(&ufb);
	CHECK(result == -ETIMEDOUT, "busy frame must time out");
	CHECK(peripheral.writes == 0, "busy frame must leave MMIO untouched");
}

static void completing_buffer_accepts_command_parameter(void)
{
	struct ums9117_drm ufb = reset_peripheral();
	const u8 parameter = 0x55;
	int result;

	/* Every port write occupies the buffer until virtual time advances. */
	peripheral.port_busy_us = 7;
	result = ums9117_drm_lcm_dcs(&ufb, 0x3a, &parameter, 1);
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
	struct ums9117_drm ufb = reset_peripheral();
	int result;

	peripheral.port_busy_us = UINT64_MAX;
	result = ums9117_drm_lcm_begin_frame(&ufb);
	CHECK(result == -ETIMEDOUT, "stalled RAMWR must time out");
	CHECK(peripheral.command == 0x2c, "frame must issue RAMWR");
	CHECK(peripheral.registers[4] == 1,
	      "pending RAMWR must keep command packing");
	CHECK(peripheral.unsafe_writes == 0,
	      "pending RAMWR must prevent mode writes");
}

static void completed_ramwr_accepts_pixel_packing(void)
{
	struct ums9117_drm ufb = reset_peripheral();
	int result;

	peripheral.port_busy_us = 7;
	result = ums9117_drm_lcm_begin_frame(&ufb);
	CHECK(result == 0, "completed RAMWR must admit frame");
	CHECK(peripheral.command == 0x2c, "frame must issue RAMWR");
	CHECK(peripheral.registers[4] == 0x28,
	      "frame must select pixel packing");
	CHECK(peripheral.unsafe_writes == 0,
	      "pixel packing must wait for RAMWR");
}

static void rgb565_damage_alignment_preserves_aligned_rectangle(void)
{
	struct drm_rect damage = { .x1 = 16, .y1 = 81, .x2 = 50, .y2 = 113 };

	ums9117_drm_rgb565_align_damage(&damage);
	CHECK(damage.x1 == 16 && damage.y1 == 81 && damage.x2 == 50 &&
		      damage.y2 == 113,
	      "aligned damage must remain unchanged");
}

static void rgb565_damage_alignment_expands_odd_edges_outward(void)
{
	struct drm_rect damage = { .x1 = 17, .y1 = 81, .x2 = 49, .y2 = 113 };

	ums9117_drm_rgb565_align_damage(&damage);
	CHECK(damage.x1 == 16 && damage.y1 == 81 && damage.x2 == 50 &&
		      damage.y2 == 113,
	      "odd damage edges must expand outward and preserve rows");
}

static void rgb565_damage_alignment_keeps_240_pixel_corner_in_bounds(void)
{
	struct drm_rect damage = { .x1 = 239, .y1 = 319, .x2 = 240, .y2 = 320 };

	ums9117_drm_rgb565_align_damage(&damage);
	CHECK(damage.x1 == 238 && damage.y1 == 319 && damage.x2 == 240 &&
		      damage.y2 == 320,
	      "240-pixel right edge must stay in bounds and preserve the corner");
}

static void rgb565_damage_alignment_keeps_128_pixel_right_edge_in_bounds(void)
{
	struct drm_rect damage = { .x1 = 127, .y1 = 10, .x2 = 128, .y2 = 11 };

	ums9117_drm_rgb565_align_damage(&damage);
	CHECK(damage.x1 == 126 && damage.y1 == 10 && damage.x2 == 128 &&
		      damage.y2 == 11,
	      "128-pixel right edge must stay in bounds and preserve rows");
}

static void rgb565_damage_alignment_preserves_full_frame(void)
{
	struct drm_rect damage = { .x1 = 0, .y1 = 0, .x2 = 240, .y2 = 320 };

	ums9117_drm_rgb565_align_damage(&damage);
	CHECK(damage.x1 == 0 && damage.y1 == 0 && damage.x2 == 240 &&
		      damage.y2 == 320,
	      "full 240 by 320 damage must remain unchanged");
}

int main(void)
{
	inoi244_timing_tuple_encodes_firmware_value();
	missing_timing_tuple_rejects_initialization();
	busy_buffer_rejects_command_and_frame();
	completing_buffer_accepts_command_parameter();
	stalled_ramwr_keeps_command_packing();
	completed_ramwr_accepts_pixel_packing();
	rgb565_damage_alignment_preserves_aligned_rectangle();
	rgb565_damage_alignment_expands_odd_edges_outward();
	rgb565_damage_alignment_keeps_240_pixel_corner_in_bounds();
	rgb565_damage_alignment_keeps_128_pixel_right_edge_in_bounds();
	rgb565_damage_alignment_preserves_full_frame();
	return failures ? 1 : 0;
}
