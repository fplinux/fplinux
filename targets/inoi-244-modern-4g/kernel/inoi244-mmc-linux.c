// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "inoi244-sdio-board.h"
#include "ums9117-mmc.h"

static int inoi244_mmc_probe(struct platform_device *pdev)
{
	return ums9117_mmc_probe(pdev, &inoi244_sdio_board);
}

static const struct of_device_id inoi244_mmc_of_match[] = {
	{ .compatible = "fplinux,inoi244-mmc" },
	{}
};
MODULE_DEVICE_TABLE(of, inoi244_mmc_of_match);

static struct platform_driver inoi244_mmc_driver = {
	.probe = inoi244_mmc_probe,
	.remove = ums9117_mmc_remove,
	.shutdown = ums9117_mmc_shutdown,
	.driver = {
		.name = "inoi244-mmc",
		.of_match_table = inoi244_mmc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(inoi244_mmc_driver);

MODULE_DESCRIPTION("UMS9117 SDIO0 microSD host for INOI 244 Modern 4G");
MODULE_LICENSE("GPL");
