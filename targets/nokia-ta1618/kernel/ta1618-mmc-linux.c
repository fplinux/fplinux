// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "ta1618-sdio-board.h"
#include "ums9117-mmc.h"

static int ta1618_mmc_probe(struct platform_device *pdev)
{
	return ums9117_mmc_probe(pdev, &ta1618_sdio_board);
}

static const struct of_device_id ta1618_mmc_of_match[] = {
	{ .compatible = "fplinux,ta1618-mmc" },
	{}
};
MODULE_DEVICE_TABLE(of, ta1618_mmc_of_match);

static struct platform_driver ta1618_mmc_driver = {
	.probe = ta1618_mmc_probe,
	.remove = ums9117_mmc_remove,
	.shutdown = ums9117_mmc_shutdown,
	.driver = {
		.name = "ta1618-mmc",
		.of_match_table = ta1618_mmc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(ta1618_mmc_driver);

MODULE_DESCRIPTION("UMS9117 SDIO0 microSD host for Nokia TA-1618");
MODULE_LICENSE("GPL");
