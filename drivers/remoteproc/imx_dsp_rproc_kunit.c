// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for i.MX DSP remoteproc platform-device probe/remove.
 */

#include <kunit/of.h>
#include <kunit/test.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

static void imx_dsp_rproc_kunit_probe(struct kunit *test)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct rproc *rproc;

	KUNIT_ASSERT_EQ(test, 0,
			of_overlay_apply_kunit(test, imx_dsp_rproc_kunit));
	wait_for_device_probe();

	np = of_find_node_by_path("/dsp@10001000");
	KUNIT_ASSERT_NOT_NULL(test, np);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	KUNIT_ASSERT_NOT_NULL(test, pdev);

	rproc = dev_get_drvdata(&pdev->dev);
	KUNIT_EXPECT_NOT_NULL(test, rproc);
	if (rproc)
		KUNIT_EXPECT_STREQ(test, "imx-dsp-rproc", rproc->name);

	platform_device_put(pdev);
}

static struct kunit_case imx_dsp_rproc_kunit_cases[] = {
	KUNIT_CASE(imx_dsp_rproc_kunit_probe),
	{}
};

static struct kunit_suite imx_dsp_rproc_kunit_suite = {
	.name = "imx-dsp-rproc",
	.test_cases = imx_dsp_rproc_kunit_cases,
};

kunit_test_suite(imx_dsp_rproc_kunit_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for i.MX DSP remoteproc");
