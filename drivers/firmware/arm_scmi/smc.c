// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Message SMC/HVC
 * Transport driver
 *
 * Copyright 2020 NXP
 */

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include "common.h"

/**
 * struct scmi_smc - Structure representing a SCMI smc transport
 *
 * @cinfo: SCMI channel info
 * @shmem: Transmit/Receive shared memory area
 * @func_id: smc/hvc call function id
 * @prot_id: The protocol id
 */

struct scmi_smc {
	struct scmi_chan_info *cinfo;
	struct scmi_shared_mem __iomem *shmem;
	u32 func_id;
	int prot_id;
};

typedef unsigned long (scmi_smc_fn)(unsigned long, unsigned long,
				    unsigned long, unsigned long,
				    unsigned long, unsigned long,
				    unsigned long, unsigned long);
static scmi_smc_fn *invoke_scmi_smc_fn;

#define client_to_scmi_smc(c) container_of(c, struct scmi_smc, cl)

static bool smc_chan_available(struct device *dev, int idx)
{
	return true;
}

static unsigned long
__invoke_scmi_fn_hvc(unsigned long function_id, unsigned long arg0,
		     unsigned long arg1, unsigned long arg2,
		     unsigned long arg3, unsigned long arg4,
		     unsigned long arg5, unsigned long arg6)
{
	struct arm_smccc_res res;

	arm_smccc_hvc(function_id, arg0, arg1, arg2, arg3, arg4, arg5,
		      arg6, &res);

	return res.a0;
}

static unsigned long
__invoke_scmi_fn_smc(unsigned long function_id, unsigned long arg0,
		     unsigned long arg1, unsigned long arg2,
		     unsigned long arg3, unsigned long arg4,
		     unsigned long arg5, unsigned long arg6)
{
	struct arm_smccc_res res;

	arm_smccc_smc(function_id, arg0, arg1, arg2, arg3, arg4, arg5,
		      arg6, &res);

	return res.a0;
}

static int scmi_smc_conduit_method(struct device_node *np)
{
	const char *method;

	if (invoke_scmi_smc_fn)
		return 0;

	if (of_property_read_string(np, "method", &method))
		return -ENXIO;

	if (!strcmp("hvc", method)) {
		invoke_scmi_smc_fn = __invoke_scmi_fn_hvc;
	} else if (!strcmp("smc", method)) {
		invoke_scmi_smc_fn = __invoke_scmi_fn_smc;
	} else {
		pr_warn("invalid \"method\" property: %s\n", method);
		return -EINVAL;
	}

	return 0;
}

static int smc_chan_setup(struct scmi_chan_info *cinfo, struct device *dev,
			  int prot_id, bool tx)
{
	struct device *cdev = cinfo->dev;
	struct scmi_smc *scmi_info;
	struct device_node *np;
	resource_size_t size;
	struct resource res;
	u32 func_id;
	int ret;

	if (!tx)
		return -ENODEV;

	scmi_info = devm_kzalloc(dev, sizeof(*scmi_info), GFP_KERNEL);
	if (!scmi_info)
		return -ENOMEM;

	np = of_parse_phandle(cdev->of_node, "shmem", 0);
	if (!np)
		np = of_parse_phandle(dev->of_node, "shmem", 0);
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret) {
		dev_err(cdev, "failed to get SCMI Tx shared memory\n");
		return ret;
	}

	size = resource_size(&res);
	scmi_info->shmem = devm_ioremap(dev, res.start, size);
	if (!scmi_info->shmem) {
		dev_err(dev, "failed to ioremap SCMI Tx shared memory\n");
		return -EADDRNOTAVAIL;
	}

	ret = of_property_read_u32(dev->of_node, "smc-id", &func_id);
	if (ret < 0)
		return ret;

	np = of_find_node_by_path("/psci");
	if (!np) {
		dev_err(dev, "Not able to find /psci node\n");
		return -ENODEV;
	}

	ret = scmi_smc_conduit_method(np);
	if (ret)
		return ret;

	of_node_put(np);

	scmi_info->func_id = func_id;
	scmi_info->cinfo = cinfo;
	scmi_info->prot_id = prot_id;
	cinfo->transport_info = scmi_info;

	return 0;
}

static int smc_chan_free(int id, void *p, void *data)
{
	struct scmi_chan_info *cinfo = p;
	struct scmi_smc *scmi_info = cinfo->transport_info;

	cinfo->transport_info = NULL;
	scmi_info->cinfo = NULL;

	scmi_free_channel(cinfo, data, id);

	return 0;
}

static int smc_send_message(struct scmi_chan_info *cinfo,
			    struct scmi_xfer *xfer)
{
	struct scmi_smc *scmi_info = cinfo->transport_info;
	int ret;

	shmem_tx_prepare(scmi_info->shmem, xfer);

	ret = invoke_scmi_smc_fn(scmi_info->func_id, scmi_info->prot_id,
				 0, 0, 0, 0, 0, 0);
	if (ret > 0)
		ret = 0;

	scmi_rx_callback(scmi_info->cinfo, shmem_read_header(scmi_info->shmem));

	return ret;
}

static void smc_mark_txdone(struct scmi_chan_info *cinfo, int ret)
{
}

static void smc_fetch_response(struct scmi_chan_info *cinfo,
			       struct scmi_xfer *xfer)
{
	struct scmi_smc *scmi_info = cinfo->transport_info;

	shmem_fetch_response(scmi_info->shmem, xfer);
}

static bool
smc_poll_done(struct scmi_chan_info *cinfo, struct scmi_xfer *xfer)
{
	struct scmi_smc *scmi_info = cinfo->transport_info;

	return shmem_poll_done(scmi_info->shmem, xfer);
}

static struct scmi_transport_ops scmi_smc_ops = {
	.chan_available = smc_chan_available,
	.chan_setup = smc_chan_setup,
	.chan_free = smc_chan_free,
	.send_message = smc_send_message,
	.mark_txdone = smc_mark_txdone,
	.fetch_response = smc_fetch_response,
	.poll_done = smc_poll_done,
};

const struct scmi_desc scmi_smc_desc = {
	.ops = &scmi_smc_ops,
	.max_rx_timeout_ms = 30,
	.max_msg = 1,
	.max_msg_size = 128,
};
