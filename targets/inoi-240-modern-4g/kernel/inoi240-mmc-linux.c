// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "inoi240-sdio-board.h"
#include "ums9117-mmc.h"

static int inoi240_mmc_probe(struct platform_device *pdev)
{
	return ums9117_mmc_probe(pdev, &inoi240_sdio_board);
}

static const struct of_device_id inoi240_mmc_of_match[] = {
	{ .compatible = "fplinux,inoi240-mmc" },
	{}
};
MODULE_DEVICE_TABLE(of, inoi240_mmc_of_match);

static struct platform_driver inoi240_mmc_driver = {
	.probe = inoi240_mmc_probe,
	.remove = ums9117_mmc_remove,
	.shutdown = ums9117_mmc_shutdown,
	.driver = {
		.name = "inoi240-mmc",
		.of_match_table = inoi240_mmc_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(inoi240_mmc_driver);

MODULE_DESCRIPTION("UMS9117 SDIO0 microSD host for INOI 240 Modern 4G");
MODULE_LICENSE("GPL");
