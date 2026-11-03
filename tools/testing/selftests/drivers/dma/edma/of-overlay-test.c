// SPDX-License-Identifier: GPL-2.0
/*
 * Test helper for applying a DT overlay from a firmware file.
 *
 * Mainline does not provide /sys/kernel/config/device-tree; use this module
 * together with the edma overlay tests instead.
 */

#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>

static char *dtbo;
module_param(dtbo, charp, 0444);
MODULE_PARM_DESC(dtbo, "firmware name of the overlay to apply");

static int ovcs_id = -1;

static int __init of_overlay_test_init(void)
{
	const struct firmware *fw;
	int ret;

	if (!dtbo || !*dtbo)
		return -EINVAL;

	ret = request_firmware_direct(&fw, dtbo, NULL);
	if (ret)
		return ret;

	if (fw->size > U32_MAX) {
		release_firmware(fw);
		return -EINVAL;
	}

	ret = of_overlay_fdt_apply(fw->data, fw->size, &ovcs_id, NULL);
	release_firmware(fw);
	if (ret)
		return ret;

	pr_info("applied overlay %s, changeset %d\n", dtbo, ovcs_id);
	return 0;
}

static void __exit of_overlay_test_exit(void)
{
	if (ovcs_id >= 0)
		of_overlay_remove(&ovcs_id);
}

module_init(of_overlay_test_init);
module_exit(of_overlay_test_exit);
MODULE_DESCRIPTION("Apply a DT overlay from firmware for edma tests");
MODULE_LICENSE("GPL");
