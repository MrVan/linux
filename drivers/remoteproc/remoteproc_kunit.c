// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for remoteproc core parsing and address translation.
 *
 * Copyright (C) 2026
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/elf.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "remoteproc_internal.h"

#define RPROC_KUNIT_FW_SIZE		0x400
#define RPROC_KUNIT_SHOFF		(sizeof(struct elf32_hdr) + \
					 sizeof(struct elf32_phdr))
#define RPROC_KUNIT_SHSTR_OFF		0x200
#define RPROC_KUNIT_RSC_OFF		0x300

struct rproc_kunit_priv {
	void *va;
	u64 da;
	size_t len;
};

static void rproc_kunit_release(void *data)
{
	struct rproc *rproc = data;

	kfree(rproc->cached_table);
	kfree(rproc->clean_table);
	rproc_free(rproc);
}

static struct rproc *rproc_kunit_alloc(struct kunit *test,
				      const struct rproc_ops *ops)
{
	struct device *dev = kunit_device_register(test, "rproc-kunit-dev");
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	rproc = rproc_alloc(dev, "rproc-kunit", ops, "rproc-kunit-fw",
			    sizeof(struct rproc_kunit_priv));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, rproc);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action(test, rproc_kunit_release, rproc));

	return rproc;
}

static void rproc_kunit_del_action(void *data)
{
	rproc_del(data);
}

static void rproc_kunit_unregister_pdev(void *data)
{
	platform_device_unregister(data);
}

static void rproc_kunit_add(struct kunit *test, struct rproc *rproc)
{
	KUNIT_ASSERT_EQ(test, 0, rproc_add(rproc));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action(test, rproc_kunit_del_action, rproc));
}

static void *rproc_kunit_fake_da_to_va(struct rproc *rproc, u64 da,
				       size_t len, bool *is_iomem)
{
	struct rproc_kunit_priv *priv = rproc->priv;
	u64 offset;

	if (da < priv->da)
		return NULL;

	offset = da - priv->da;
	if (offset > priv->len || len > priv->len - offset)
		return NULL;

	if (is_iomem)
		*is_iomem = false;

	return priv->va + offset;
}

static int rproc_kunit_start(struct rproc *rproc)
{
	return 0;
}

static int rproc_kunit_stop(struct rproc *rproc)
{
	return 0;
}

static const struct rproc_ops rproc_kunit_ops = {
	.start = rproc_kunit_start,
	.stop = rproc_kunit_stop,
};

static const struct rproc_ops rproc_kunit_ops_da = {
	.start = rproc_kunit_start,
	.stop = rproc_kunit_stop,
	.da_to_va = rproc_kunit_fake_da_to_va,
};

static void rproc_kunit_elf32_init(void *buf, size_t size, u64 phoff,
				   u64 shoff, u16 phnum, u16 shnum,
				   u16 shstrndx)
{
	struct elf32_hdr *ehdr = buf;

	memset(buf, 0, size);
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS32;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_type = ET_EXEC;
	ehdr->e_machine = EM_ARM;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_phoff = phoff;
	ehdr->e_shoff = shoff;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phentsize = sizeof(struct elf32_phdr);
	ehdr->e_phnum = phnum;
	ehdr->e_shentsize = sizeof(struct elf32_shdr);
	ehdr->e_shnum = shnum;
	ehdr->e_shstrndx = shstrndx;
}

static void rproc_kunit_elf64_init(void *buf, size_t size, u64 phoff,
				   u16 phnum)
{
	struct elf64_hdr *ehdr = buf;

	memset(buf, 0, size);
	memcpy(ehdr->e_ident, ELFMAG, SELFMAG);
	ehdr->e_ident[EI_CLASS] = ELFCLASS64;
	ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
	ehdr->e_ident[EI_VERSION] = EV_CURRENT;
	ehdr->e_type = ET_EXEC;
	ehdr->e_machine = EM_NONE;
	ehdr->e_version = EV_CURRENT;
	ehdr->e_phoff = phoff;
	ehdr->e_ehsize = sizeof(*ehdr);
	ehdr->e_phentsize = sizeof(struct elf64_phdr);
	ehdr->e_phnum = phnum;
}

static void rproc_kunit_build_rsc_elf(void *buf, size_t size)
{
	static const char shstr[] = "\0.shstrtab\0.resource_table\0";
	struct resource_table *table;
	struct elf32_shdr *shdr;
	struct elf32_phdr *phdr;
	struct elf32_hdr *ehdr = buf;

	rproc_kunit_elf32_init(buf, size, sizeof(*ehdr), RPROC_KUNIT_SHOFF,
			       1, 3, 1);

	phdr = buf + sizeof(*ehdr);
	phdr->p_type = PT_LOAD;
	phdr->p_offset = RPROC_KUNIT_RSC_OFF;
	phdr->p_vaddr = 0x1000;
	phdr->p_paddr = 0x1000;
	phdr->p_filesz = sizeof(*table);
	phdr->p_memsz = sizeof(*table);
	phdr->p_align = 4;

	memcpy(buf + RPROC_KUNIT_SHSTR_OFF, shstr, sizeof(shstr));

	shdr = buf + RPROC_KUNIT_SHOFF;
	shdr[1].sh_name = 1;
	shdr[1].sh_type = SHT_STRTAB;
	shdr[1].sh_offset = RPROC_KUNIT_SHSTR_OFF;
	shdr[1].sh_size = sizeof(shstr) - 1;
	shdr[1].sh_addralign = 1;

	shdr[2].sh_name = 11;
	shdr[2].sh_type = SHT_PROGBITS;
	shdr[2].sh_offset = RPROC_KUNIT_RSC_OFF;
	shdr[2].sh_size = sizeof(*table);
	shdr[2].sh_addr = 0x1000;

	table = buf + RPROC_KUNIT_RSC_OFF;
	table->ver = 1;
	table->num = 0;
}

static void rproc_kunit_elf_sanity_valid(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);

	rproc_kunit_elf32_init(elf, fw.size, sizeof(struct elf32_hdr),
			       RPROC_KUNIT_SHOFF, 1, 1, 0);
	KUNIT_EXPECT_EQ(test, 0, rproc_elf_sanity_check(rproc, &fw));
}

static void rproc_kunit_elf_sanity_bad_sections(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);

	rproc_kunit_elf32_init(elf, fw.size, sizeof(struct elf32_hdr),
			       RPROC_KUNIT_SHOFF, 1, 100, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_sanity_check(rproc, &fw));

	rproc_kunit_elf32_init(elf, fw.size, sizeof(struct elf32_hdr),
			       RPROC_KUNIT_SHOFF, 1, 1, 100);
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_sanity_check(rproc, &fw));
}

static void rproc_kunit_elf_sanity_bad_program_headers(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);

	rproc_kunit_elf32_init(elf, fw.size, sizeof(struct elf32_hdr),
			       RPROC_KUNIT_SHOFF, 100, 1, 0);
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_sanity_check(rproc, &fw));
}

static void rproc_kunit_elf_load_segments_valid(struct kunit *test)
{
	u8 zero[4] = {};
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct rproc_kunit_priv *priv;
	struct elf32_phdr *phdr;
	struct rproc *rproc;
	u8 *mem;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops_da);
	priv = rproc->priv;
	mem = kunit_kzalloc(test, 8, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mem);

	rproc_kunit_elf32_init(elf, fw.size, sizeof(struct elf32_hdr),
			       0, 1, 0, 0);
	phdr = (void *)(elf + sizeof(struct elf32_hdr));
	phdr->p_type = PT_LOAD;
	phdr->p_offset = 0x80;
	phdr->p_vaddr = 0x1000;
	phdr->p_paddr = 0x1000;
	phdr->p_filesz = 4;
	phdr->p_memsz = 8;
	phdr->p_align = 4;
	memcpy(elf + 0x80, "test", 4);

	priv->va = mem;
	priv->da = 0x1000;
	priv->len = 8;

	KUNIT_EXPECT_EQ(test, 0, rproc_elf_load_segments(rproc, &fw));
	KUNIT_EXPECT_MEMEQ(test, mem, "test", 4);
	KUNIT_EXPECT_MEMEQ(test, mem + 4, zero, 4);
}

static void rproc_kunit_elf_load_segments_offset_overflow(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct elf64_phdr *phdr;
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);

	rproc_kunit_elf64_init(elf, fw.size, sizeof(struct elf64_hdr), 1);
	phdr = (void *)(elf + sizeof(struct elf64_hdr));
	phdr->p_type = PT_LOAD;
	phdr->p_offset = U64_MAX;
	phdr->p_vaddr = 0x1000;
	phdr->p_paddr = 0x1000;
	phdr->p_filesz = 1;
	phdr->p_memsz = 1;
	phdr->p_align = 4;

	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_load_segments(rproc, &fw));
}

static void rproc_kunit_elf_rsc_table_valid(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	rproc_kunit_build_rsc_elf(elf, fw.size);

	KUNIT_EXPECT_EQ(test, 0, rproc_elf_load_rsc_table(rproc, &fw));
	KUNIT_EXPECT_NOT_NULL(test, rproc->table_ptr);
	KUNIT_EXPECT_EQ(test, sizeof(struct resource_table), rproc->table_sz);
	KUNIT_EXPECT_EQ(test, 1U, rproc->table_ptr->ver);
}

static void rproc_kunit_elf_rsc_table_bad_string_table(struct kunit *test)
{
	u8 *elf = kunit_kzalloc(test, RPROC_KUNIT_FW_SIZE, GFP_KERNEL);
	struct firmware fw = {
		.data = elf,
		.size = RPROC_KUNIT_FW_SIZE,
	};
	struct elf32_shdr *shdr;
	struct rproc *rproc;

	KUNIT_ASSERT_NOT_NULL(test, elf);
	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	rproc_kunit_build_rsc_elf(elf, fw.size);

	shdr = (void *)(elf + RPROC_KUNIT_SHOFF);
	shdr[1].sh_offset = fw.size + 1;
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_load_rsc_table(rproc, &fw));

	rproc_kunit_build_rsc_elf(elf, fw.size);
	shdr = (void *)(elf + RPROC_KUNIT_SHOFF);
	shdr[1].sh_size = 12;
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_load_rsc_table(rproc, &fw));

	rproc_kunit_build_rsc_elf(elf, fw.size);
	shdr = (void *)(elf + RPROC_KUNIT_SHOFF);
	shdr[2].sh_name = 0x100;
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_elf_load_rsc_table(rproc, &fw));
}

struct rproc_kunit_rsc_ctx {
	int count;
	u32 type;
	int offset;
	int avail;
};

static int rproc_kunit_rsc_cb(u32 type, void *rsc, int offset, int avail,
			      void *data)
{
	struct rproc_kunit_rsc_ctx *ctx = data;

	ctx->count++;
	ctx->type = type;
	ctx->offset = offset;
	ctx->avail = avail;

	return 0;
}

static void rproc_kunit_rsc_table_iterator(struct kunit *test)
{
	struct rproc_kunit_rsc_ctx ctx = {};
	struct rproc *rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	struct resource_table *table;
	struct fw_rsc_hdr *hdr;
	u8 *buf;

	buf = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	table = (void *)buf;
	table->ver = 1;
	table->num = 1;
	table->offset[0] = 20;
	hdr = (void *)buf + 20;
	hdr->type = RSC_CARVEOUT;

	KUNIT_EXPECT_EQ(test, 0,
			rsc_table_for_each_entry(table, 24, &rproc->dev,
						 rproc_kunit_rsc_cb, &ctx));
	KUNIT_EXPECT_EQ(test, 1, ctx.count);
	KUNIT_EXPECT_EQ(test, RSC_CARVEOUT, ctx.type);
	KUNIT_EXPECT_EQ(test, 24, ctx.offset);
	KUNIT_EXPECT_EQ(test, 0, ctx.avail);

	memset(&ctx, 0, sizeof(ctx));
	table->num = 2;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			rsc_table_for_each_entry(table, 20, &rproc->dev,
						 rproc_kunit_rsc_cb, &ctx));
	KUNIT_EXPECT_EQ(test, 0, ctx.count);

	table->num = 1;
	KUNIT_EXPECT_EQ(test, -EINVAL,
			rsc_table_for_each_entry(table, 22, &rproc->dev,
						 rproc_kunit_rsc_cb, &ctx));
	KUNIT_EXPECT_EQ(test, 0, ctx.count);

	KUNIT_EXPECT_EQ(test, -EINVAL,
			rsc_table_for_each_entry(table, 8, &rproc->dev,
						 rproc_kunit_rsc_cb, &ctx));
}

static void rproc_kunit_da_to_va(struct kunit *test)
{
	struct rproc_mem_entry entry = {};
	struct rproc *rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	u8 *mem = kunit_kzalloc(test, 0x100, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, mem);
	entry.va = mem;
	entry.da = 0x1000;
	entry.len = 0x100;
	list_add_tail(&entry.node, &rproc->carveouts);

	KUNIT_EXPECT_PTR_EQ(test, mem,
			    rproc_da_to_va(rproc, 0x1000, 0x100, NULL));
	KUNIT_EXPECT_PTR_EQ(test, mem + 0xff,
			    rproc_da_to_va(rproc, 0x10ff, 1, NULL));
	KUNIT_EXPECT_NULL(test, rproc_da_to_va(rproc, 0x1100, 1, NULL));
	KUNIT_EXPECT_NULL(test, rproc_da_to_va(rproc, 0x1000, 0x101, NULL));
	KUNIT_EXPECT_NULL(test,
			  rproc_da_to_va(rproc, 0x1000 + (1ULL << 32), 1, NULL));

	list_del(&entry.node);
}

static void rproc_kunit_parse_vring(struct kunit *test)
{
	struct rproc *rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	struct rproc_vdev rvdev = {};
	struct fw_rsc_vdev *vdev;

	vdev = kunit_kzalloc(test, sizeof(*vdev) + sizeof(vdev->vring[0]),
			     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);
	rvdev.rproc = rproc;

	vdev->vring[0].num = 4;
	vdev->vring[0].align = 4;
	KUNIT_EXPECT_EQ(test, 0, rproc_parse_vring(&rvdev, vdev, 0));
	KUNIT_EXPECT_EQ(test, 4U, rvdev.vring[0].num);

	vdev->vring[0].num = 3;
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_parse_vring(&rvdev, vdev, 0));

	vdev->vring[0].num = 4;
	vdev->vring[0].align = 3;
	KUNIT_EXPECT_EQ(test, -EINVAL, rproc_parse_vring(&rvdev, vdev, 0));
}

static void rproc_kunit_vdev(struct kunit *test, u8 num_vrings)
{
	struct rproc_vdev_data data = {};
	struct fw_rsc_vdev *vdev;
	struct platform_device *pdev;
	struct rproc *rproc;
	size_t size;

	rproc = rproc_kunit_alloc(test, &rproc_kunit_ops);
	size = sizeof(*vdev) + num_vrings * sizeof(vdev->vring[0]);
	vdev = kunit_kzalloc(test, size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, vdev);

	vdev->num_of_vrings = num_vrings;
	if (num_vrings) {
		vdev->vring[0].da = FW_RSC_ADDR_ANY;
		vdev->vring[0].num = 4;
		vdev->vring[0].align = 4;
	}

	rproc->auto_boot = false;
	rproc->table_ptr = (void *)vdev;
	rproc->table_sz = size;
	rproc_kunit_add(test, rproc);

	data.id = 0;
	data.index = 0;
	data.rsc_offset = 0;
	data.rsc = vdev;
	pdev = platform_device_register_data(&rproc->dev, "rproc-virtio",
					     PLATFORM_DEVID_AUTO, &data,
					     sizeof(data));
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pdev);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action(test, rproc_kunit_unregister_pdev, pdev));
}

static void rproc_kunit_vdev_zero_vrings(struct kunit *test)
{
	rproc_kunit_vdev(test, 0);
}

static void rproc_kunit_vdev_one_vring(struct kunit *test)
{
	rproc_kunit_vdev(test, 1);
}

static struct kunit_case rproc_kunit_cases[] = {
	KUNIT_CASE(rproc_kunit_elf_sanity_valid),
	KUNIT_CASE(rproc_kunit_elf_sanity_bad_sections),
	KUNIT_CASE(rproc_kunit_elf_sanity_bad_program_headers),
	KUNIT_CASE(rproc_kunit_elf_load_segments_valid),
	KUNIT_CASE(rproc_kunit_elf_load_segments_offset_overflow),
	KUNIT_CASE(rproc_kunit_elf_rsc_table_valid),
	KUNIT_CASE(rproc_kunit_elf_rsc_table_bad_string_table),
	KUNIT_CASE(rproc_kunit_rsc_table_iterator),
	KUNIT_CASE(rproc_kunit_da_to_va),
	KUNIT_CASE(rproc_kunit_parse_vring),
	KUNIT_CASE(rproc_kunit_vdev_zero_vrings),
	KUNIT_CASE(rproc_kunit_vdev_one_vring),
	{}
};

static struct kunit_suite rproc_kunit_suite = {
	.name = "remoteproc-core",
	.test_cases = rproc_kunit_cases,
};

kunit_test_suite(rproc_kunit_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for remoteproc core parsing");
