// SPDX-License-Identifier: GPL-2.0-only
#include <efuse-kernel.h>

static const char *compatible;
static struct nvmem_config nvmem;
static u32 registers[0x2000 / sizeof(u32)];
static bool locked;
static bool clocked;
static int lock_error;
static int clock_error;
static int fail_block;
static unsigned int reads;
static unsigned int clears;
static unsigned int writes;
static unsigned int barriers;
static unsigned int sleeps;
static void *allocation;

void mutex_init(struct mutex *mutex)
{
	mutex->held = false;
}
void mutex_lock(struct mutex *mutex)
{
	assert(!mutex->held);
	mutex->held = true;
}
void mutex_unlock(struct mutex *mutex)
{
	assert(mutex->held);
	mutex->held = false;
}
int hwspin_lock_timeout_raw(struct hwspinlock *lock, unsigned int timeout)
{
	assert(!locked);
	if (lock_error)
		return lock_error;
	locked = true;
	return 0;
}
void hwspin_unlock_raw(struct hwspinlock *lock)
{
	assert(locked && !clocked);
	locked = false;
}
int clk_prepare_enable(struct clk *clk)
{
	assert(locked && !clocked);
	if (clock_error)
		return clock_error;
	clocked = true;
	return 0;
}
void clk_disable_unprepare(struct clk *clk)
{
	assert(locked && clocked);
	assert(!(registers[0x20 / 4] & (BIT(0) | BIT(2))));
	clocked = false;
}

u32 readl(const void *address)
{
	size_t offset = (const u8 *)address - (const u8 *)registers;

	assert(locked && clocked && offset < sizeof(registers));
	if (offset >= 0x1000) {
		unsigned int block = (offset - 0x1000) / 4;

		assert(registers[0x20 / 4] & BIT(0));
		assert(!!(registers[0x20 / 4] & BIT(2)) ==
		       !!strcmp(compatible, "sprd,ums312-efuse"));
		reads++;
		if ((int)block == fail_block)
			registers[0x24 / 4] = BIT(0);
	}
	if (offset == 0x24 && strcmp(compatible, "sprd,ums9117-efuse"))
		assert(!(registers[0x20 / 4] & (BIT(0) | BIT(2))));
	return registers[offset / 4];
}
void writel(u32 value, void *address)
{
	size_t offset = (u8 *)address - (u8 *)registers;

	assert(locked && clocked && offset < sizeof(registers));
	assert(offset == 0x20 || offset == 0x28);
	writes++;
	if (offset == 0x28) {
		assert(strcmp(compatible, "sprd,ums9117-efuse"));
		assert(value == 0x3fff);
		registers[0x24 / 4] &= ~value;
		clears++;
	} else {
		registers[offset / 4] = value;
	}
}
void usleep_range(unsigned long low, unsigned long high)
{
	assert(locked && clocked);
	if (!strcmp(compatible, "sprd,ums9117-efuse"))
		assert(low == 2000 && high == 2500);
	else
		assert(low == 1000 && high == 1200);
	sleeps++;
}
void wmb(void)
{
	assert(locked && clocked);
	barriers++;
}
const void *of_device_get_match_data(struct device *dev)
{
	const struct of_device_id *match =
		test_platform_driver->driver.of_match_table;

	for (; match->compatible; match++)
		if (!strcmp(match->compatible, compatible))
			return match->data;
	abort();
}
void *devm_kzalloc(struct device *dev, size_t size, int flags)
{
	assert(!allocation);
	allocation = calloc(1, size);
	assert(allocation);
	return allocation;
}
void *devm_platform_ioremap_resource(struct platform_device *pdev, int index)
{
	assert(index == 0);
	return registers;
}
int of_hwspin_lock_get_id(struct device_node *node, int index)
{
	assert(index == 0);
	return 8;
}
struct hwspinlock *devm_hwspin_lock_request_specific(struct device *dev, int id)
{
	static struct hwspinlock lock;

	assert(id == 8);
	return &lock;
}
struct clk *devm_clk_get(struct device *dev, const char *name)
{
	static struct clk clk;

	assert(!strcmp(name, "enable"));
	return &clk;
}
struct nvmem_device *devm_nvmem_register(struct device *dev,
					 const struct nvmem_config *config)
{
	static struct nvmem_device device;

	nvmem = *config;
	return &device;
}

static void reset_reads(void)
{
	assert(!locked && !clocked);
	lock_error = 0;
	clock_error = 0;
	fail_block = -1;
	reads = 0;
	clears = 0;
	writes = 0;
	barriers = 0;
	sleeps = 0;
	memset(registers, 0, sizeof(registers));
	registers[0x14 / 4] = 0x0800;
	registers[0x54 / 4] = 0x02;
}

static void probe(const char *match)
{
	struct platform_device pdev = {};

	free(allocation);
	allocation = NULL;
	compatible = match;
	reset_reads();
	assert(test_platform_driver->probe(&pdev) == 0);
	assert(nvmem.stride == 1 && nvmem.word_size == 1);
}

static void read_sc9820e_blocks_and_unaligned_bytes(void)
{
	const u8 expected[] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
				0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10 };
	u8 buffer[18];

	probe("sprd,sc9820e-efuse");
	assert(nvmem.size == 16 && nvmem.read_only && !nvmem.reg_write);
	assert(!nvmem.root_only);
	registers[(0x1000 + 41 * 4) / 4] = 0x44332211;
	registers[(0x1000 + 42 * 4) / 4] = 0x88776655;
	registers[(0x1000 + 43 * 4) / 4] = 0xccbbaa99;
	registers[(0x1000 + 44 * 4) / 4] = 0x10ffeedd;
	registers[0x24 / 4] = BIT(1);
	memset(buffer, 0xa5, sizeof(buffer));
	assert(nvmem.reg_read(nvmem.priv, 0, buffer + 1, 16) == 0);
	assert(buffer[0] == 0xa5 && buffer[17] == 0xa5);
	assert(!memcmp(buffer + 1, expected, sizeof(expected)));
	assert(reads == 4 && clears == 4);
	reads = clears = 0;
	memset(buffer, 0xa5, sizeof(buffer));
	assert(nvmem.reg_read(nvmem.priv, 3, buffer + 1, 11) == 0);
	assert(!memcmp(buffer + 1, expected + 3, 11));
	assert(buffer[0] == 0xa5 && buffer[12] == 0xa5);
	assert(reads == 4 && clears == 4 && !locked && !clocked);
}

static void distinguish_zero_and_read_errors(void)
{
	u8 buffer[16];

	reset_reads();
	memset(buffer, 0xa5, sizeof(buffer));
	assert(nvmem.reg_read(nvmem.priv, 12, buffer, 4) == 0);
	assert(buffer[0] == 0 && buffer[3] == 0 && reads == 1);
	reset_reads();
	fail_block = 42;
	memset(buffer, 0xa5, sizeof(buffer));
	assert(nvmem.reg_read(nvmem.priv, 0, buffer, 16) == -EBUSY);
	assert(reads == 2 && clears == 3 && !locked && !clocked);
	assert(buffer[4] == 0xa5 && buffer[15] == 0xa5);
	reset_reads();
	lock_error = -ETIMEDOUT;
	assert(nvmem.reg_read(nvmem.priv, 0, buffer, 4) == -ETIMEDOUT);
	assert(!reads && !clears && !locked && !clocked);
	reset_reads();
	clock_error = -EIO;
	assert(nvmem.reg_read(nvmem.priv, 0, buffer, 4) == -EIO);
	assert(!reads && !clears && !locked && !clocked);
	reset_reads();
	assert(nvmem.reg_read(nvmem.priv, 0, buffer, 4) == 0);
}

static void preserve_ums312_window_and_write_registration(void)
{
	u8 buffer[8];
	const u8 expected[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };

	probe("sprd,ums312-efuse");
	assert(nvmem.size == 96 && !nvmem.read_only && nvmem.reg_write);
	registers[(0x1000 + 72 * 4) / 4] = 0x67452301;
	registers[(0x1000 + 73 * 4) / 4] = 0xefcdab89;
	assert(nvmem.reg_read(nvmem.priv, 0, buffer, 8) == 0);
	assert(!memcmp(buffer, expected, sizeof(expected)));
	assert(reads == 2 && !clears && !locked && !clocked);
}

static void preserve_ums9117_guarded_multiword_reads(void)
{
	const u8 expected[] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
	u8 buffer[8];

	probe("sprd,ums9117-efuse");
	assert(nvmem.size == 288 && nvmem.read_only && nvmem.root_only);
	assert(!nvmem.reg_write);
	registers[(0x1000 + 25 * 4) / 4] = 0x67452301;
	registers[(0x1000 + 26 * 4) / 4] = 0xefcdab89;
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 8) == 0);
	assert(!memcmp(buffer, expected, sizeof(expected)));
	assert(reads == 2 && clears == 0 && writes == 4);
	assert(barriers == 4 && sleeps == 4);
	assert(registers[0x20 / 4] == 0 && registers[0x54 / 4] == 2);
	assert(registers[0x2c / 4] == 0 && registers[0x24 / 4] == 0);
	assert(!locked && !clocked);
}

static void reject_ums9117_inherited_or_new_errors(void)
{
	u8 buffer[12];

	probe("sprd,ums9117-efuse");
	registers[0x14 / 4] = 0x0801;
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 4) == -ENODEV);
	assert(reads == 0 && writes == 0 && !locked && !clocked);
	reset_reads();
	registers[0x20 / 4] = BIT(4);
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 4) == -EBUSY);
	assert(reads == 0 && writes == 0 && registers[0x20 / 4] == BIT(4));
	reset_reads();
	registers[0x2c / 4] = 0x8810;
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 4) == -EBUSY);
	assert(reads == 0 && writes == 0 && registers[0x2c / 4] == 0x8810);
	reset_reads();
	registers[0x54 / 4] = 0x06;
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 4) == -EBUSY);
	assert(reads == 0 && writes == 0 && registers[0x54 / 4] == 0x06);
	reset_reads();
	registers[0x24 / 4] = 0x02;
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 4) == -EUCLEAN);
	assert(reads == 0 && writes == 0 && registers[0x24 / 4] == 0x02);
	reset_reads();
	registers[(0x1000 + 25 * 4) / 4] = 0x67452301;
	fail_block = 26;
	memset(buffer, 0xa5, sizeof(buffer));
	assert(nvmem.reg_read(nvmem.priv, 100, buffer, 12) == -EUCLEAN);
	assert(!memcmp(buffer, "\x01\x23\x45\x67", 4));
	assert(buffer[4] == 0xa5 && buffer[11] == 0xa5);
	assert(reads == 2 && clears == 0 && registers[0x24 / 4] == 1);
	assert(registers[0x20 / 4] == 0 && registers[0x54 / 4] == 2);
	assert(!locked && !clocked);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "qin-words"))
		read_sc9820e_blocks_and_unaligned_bytes();
	else if (!strcmp(argv[1], "qin-errors")) {
		probe("sprd,sc9820e-efuse");
		distinguish_zero_and_read_errors();
	} else if (!strcmp(argv[1], "ums312"))
		preserve_ums312_window_and_write_registration();
	else if (!strcmp(argv[1], "ums9117-words"))
		preserve_ums9117_guarded_multiword_reads();
	else if (!strcmp(argv[1], "ums9117-errors"))
		reject_ums9117_inherited_or_new_errors();
	else
		return 2;
	free(allocation);
	return 0;
}
