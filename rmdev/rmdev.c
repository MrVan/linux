// SPDX-License-Identifier: GPL-2.0
/*
 * rmdev - unregister a platform device to exercise the IOMMU
 * release path (BUS_NOTIFY_REMOVED_DEVICE -> iommu_release_device()).
 *
 * Driver unbind does NOT reach iommu_release_device(); the device has
 * to be deleted. Example:
 *
 *   insmod rmdev.ko target=42850000.mmc
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static char *target = "";
module_param(target, charp, 0444);
MODULE_PARM_DESC(target, "Platform device name to unregister, e.g. 42850000.mmc");

static int __init rmdev_init(void)
{
	struct device *dev;
	int ret;

	if (!target || !*target) {
		pr_err("rmdev: target= is required\n");
		return -EINVAL;
	}

	dev = bus_find_device_by_name(&platform_bus_type, NULL, target);
	if (!dev) {
		pr_err("rmdev: no platform device named %s\n", target);
		return -ENODEV;
	}

	if (!dev->of_node || !of_node_check_flag(dev->of_node, OF_POPULATED)) {
		pr_err("rmdev: %s is not an OF-populated platform device\n",
		       target);
		put_device(dev);
		return -EINVAL;
	}

	pr_info("rmdev: unregistering %s (driver %s)\n", dev_name(dev),
		dev->driver ? dev->driver->name : "none");
	ret = of_platform_device_destroy(dev, NULL);
	put_device(dev);

	if (ret)
		pr_err("rmdev: failed to unregister %s: %d\n", target, ret);
	else
		pr_info("rmdev: %s is gone\n", target);

	return ret;
}

static void __exit rmdev_exit(void)
{
}

module_init(rmdev_init);
module_exit(rmdev_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unregister a platform device to test IOMMU release paths");
