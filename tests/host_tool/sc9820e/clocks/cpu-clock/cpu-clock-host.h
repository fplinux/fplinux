/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SC9820E_CPU_CLOCK_HOST_H
#define FPLINUX_SC9820E_CPU_CLOCK_HOST_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#define BIT(bit) (1UL << (bit))
#define GENMASK(high, low) \
	((UINT32_MAX >> (31 - (high))) & (UINT32_MAX << (low)))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define struct_size(pointer, member, count) \
	(sizeof(*(pointer)) + sizeof((pointer)->member[0]) * (count))
#define container_of(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))
#define DIV_ROUND_CLOSEST_ULL(value, divisor) \
	(((value) + (divisor) / 2) / (divisor))
#define DIV_ROUND_UP_ULL(value, divisor) (((value) + (divisor) - 1) / (divisor))
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0x200
#define CLK_GET_RATE_NOCACHE BIT(1)
#define CPUFREQ_CONST_LOOPS BIT(1)
#define CPUFREQ_TABLE_END (~1U)
#define CPUFREQ_DEFAULT_TRANSITION_LATENCY_NS 1000000
#define __iomem
#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define IOMEM_ERR_PTR(error) ERR_PTR(error)
#define PTR_ERR(pointer) ((int)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
/* The target clock API returns a 32-bit unsigned long. */
#define ULONG_MAX UINT32_MAX

struct regmap;
struct device_node {
	struct regmap *regmap;
	struct device_node *clock_provider;
	unsigned int clock_output;
	int clock_args_count;
	int clock_error;
};
struct device {
	struct device_node *of_node;
};
struct resource {
	unsigned long start;
	unsigned long end;
	const char *name;
};
struct platform_device {
	struct device dev;
	struct resource resources[2];
};
struct of_device_id {
	const char *compatible;
};
struct platform_driver {
	int (*probe)(struct platform_device *pdev);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};
struct clk_hw;
struct clk_rate_request {
	unsigned long rate;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long best_parent_rate;
	struct clk_hw *best_parent_hw;
};
struct clk_ops {
	unsigned long (*recalc_rate)(struct clk_hw *hw,
				     unsigned long parent_rate);
	int (*set_rate)(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate);
	int (*set_parent)(struct clk_hw *hw, u8 index);
	u8 (*get_parent)(struct clk_hw *hw);
	int (*determine_rate)(struct clk_hw *hw, struct clk_rate_request *req);
	int (*prepare)(struct clk_hw *hw);
	void (*unprepare)(struct clk_hw *hw);
	int (*enable)(struct clk_hw *hw);
	void (*disable)(struct clk_hw *hw);
};
struct clk_parent_data {
	const char *fw_name;
};
struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
	const struct clk_parent_data *parent_data;
	u8 num_parents;
	unsigned long flags;
};
struct clk_hw {
	const struct clk_init_data *init;
};
struct clk_hw_onecell_data {
	unsigned int num;
	struct clk_hw *hws[];
};
struct mutex {
	bool held;
};
struct cpumask {
	unsigned int bits;
};
struct cpufreq_frequency_table {
	unsigned int frequency;
	unsigned int driver_data;
};
struct cpufreq_policy {
	unsigned int cpu;
	struct cpumask *cpus;
	void *driver_data;
	struct cpufreq_frequency_table *freq_table;
	unsigned int cur;
	struct {
		unsigned int transition_latency;
	} cpuinfo;
	bool dvfs_possible_from_any_cpu;
};
struct cpufreq_policy_data;
struct cpufreq_driver {
	char name[16];
	unsigned int flags;
	void *driver_data;
	int (*init)(struct cpufreq_policy *policy);
	int (*verify)(struct cpufreq_policy_data *policy);
	int (*target_index)(struct cpufreq_policy *policy, unsigned int index);
	unsigned int (*get)(unsigned int cpu);
};
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < 2; (cpu)++)
struct of_phandle_args {
	struct device_node *np;
	int args_count;
	u32 args[16];
};
typedef struct clk_hw *(*host_clk_get)(struct of_phandle_args *args,
				       void *data);

static inline u64 div_u64(u64 dividend, u32 divisor)
{
	return dividend / divisor;
}

u32 readl(const void *address);
void writel(u32 value, void *address);
void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags);
struct regmap *syscon_regmap_lookup_by_phandle(struct device_node *node,
					       const char *name);
int regmap_read(struct regmap *regmap, unsigned int offset,
		unsigned int *value);
int regmap_write(struct regmap *regmap, unsigned int offset,
		 unsigned int value);
int regmap_update_bits(struct regmap *regmap, unsigned int offset,
		       unsigned int mask, unsigned int value);
struct clk_hw *clk_hw_get_parent(const struct clk_hw *hw);
unsigned long clk_hw_get_rate(const struct clk_hw *hw);
int devm_clk_hw_register(struct device *dev, struct clk_hw *hw);
int devm_of_clk_add_hw_provider(struct device *dev, host_clk_get get,
				void *data);
struct clk_hw *of_clk_hw_simple_get(struct of_phandle_args *args, void *data);
struct clk_hw *of_clk_hw_onecell_get(struct of_phandle_args *args, void *data);
struct resource *platform_get_resource(struct platform_device *pdev,
				       unsigned int type, unsigned int index);
struct resource *platform_get_resource_byname(struct platform_device *pdev,
					      unsigned int type,
					      const char *name);
unsigned long resource_size(const struct resource *resource);
void *devm_platform_ioremap_resource_byname(struct platform_device *pdev,
					    const char *name);
void *devm_platform_ioremap_resource(struct platform_device *pdev,
				     unsigned int index);
int dev_err_probe(struct device *dev, int error, const char *format, ...);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
			     void *data);
void mutex_init(struct mutex *lock);
void mutex_lock(struct mutex *lock);
void mutex_unlock(struct mutex *lock);
void cpumask_set_cpu(unsigned int cpu, struct cpumask *mask);
bool cpumask_test_cpu(unsigned int cpu, const struct cpumask *mask);
unsigned int cpumask_weight(const struct cpumask *mask);
void cpumask_copy(struct cpumask *destination, const struct cpumask *source);
struct device *get_cpu_device(unsigned int cpu);
struct device_node *of_parse_phandle(struct device_node *node, const char *name,
				     unsigned int index);
int of_parse_phandle_with_args(const struct device_node *node,
			       const char *list_name, const char *cells_name,
			       int index, struct of_phandle_args *out_args);
void of_node_put(struct device_node *node);
void *cpufreq_get_driver_data(void);
int cpufreq_register_driver(struct cpufreq_driver *driver);
void cpufreq_unregister_driver(struct cpufreq_driver *driver);
int cpufreq_generic_frequency_table_verify(struct cpufreq_policy_data *policy);

#define MODULE_DEVICE_TABLE(type, table)
#define MODULE_DESCRIPTION(description)
#define MODULE_LICENSE(license)
#define module_platform_driver(driver) \
	struct platform_driver *host_##driver = &(driver)

extern struct platform_driver *host_sc9820e_cpu_clock_driver;
extern struct platform_driver *host_sc9820e_hs_spi_clock_driver;

#endif
