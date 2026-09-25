// SPDX-License-Identifier: GPL-2.0
/*
 * NVMEM layout for the factory-programmed EUI-48 identifier stored in the
 * Microchip/SST vendor-specific SFDP parameter table (e.g. SST26VF064BEUI).
 *
 * The whole SFDP is exposed as a read-only NVMEM device by the SPI NOR core.
 * This layout locates the Microchip vendor parameter table at runtime and
 * registers the EUI-48 address as an NVMEM cell, so that a network driver can
 * consume it as a MAC address. No offset is hardcoded in the device tree.
 *
 * Copyright (C) 2026 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Manikandan Muralidharan <manikandan.m@microchip.com>
 */

#include <linux/etherdevice.h>
#include <linux/minmax.h>
#include <linux/nvmem-consumer.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/unaligned.h>
#include <uapi/linux/if_ether.h>

/* SFDP header and parameter header, as laid out on the flash. */
struct sfdp_header {
	u8 signature[4];
	u8 minor;
	u8 major;
	u8 nph;
	u8 unused;
};

struct sfdp_parameter_header {
	u8 id_lsb;
	u8 minor;
	u8 major;
	u8 length;
	u8 parameter_table_pointer[3];
	u8 id_msb;
};

#define SFDP_SIGNATURE			0x50444653U

#define SFDP_PARAM_HEADER_ID(h)		(((h)->id_msb << 8) | (h)->id_lsb)
#define SFDP_PARAM_HEADER_PTP(h)	get_unaligned_le24((h)->parameter_table_pointer)

/* Microchip (vendor) parameter table identifier: id_msb << 8 | id_lsb. */
#define SFDP_MCHP_VENDOR_ID		0x01bf

#define SFDP_MCHP_EUI48_MARKER_OFFSET	0x60
#define SFDP_MCHP_EUI48_MARKER		0x30
#define SFDP_MCHP_EUI48_OFFSET		0x61

static int sfdp_eui_read_post_process(void *priv, const char *id, int index,
				      unsigned int offset, void *buf,
				      size_t bytes)
{
	u8 *data = buf;
	int i;

	/* SFDP stores the address least-significant octet first; reverse it. */
	for (i = 0; i < bytes / 2; i++)
		swap(data[i], data[bytes - 1 - i]);

	if (bytes == ETH_ALEN && !is_valid_ether_addr(buf))
		return -EINVAL;

	return 0;
}

static int sfdp_eui_find_vendor_table(struct nvmem_device *nvmem, u32 *ptp)
{
	struct sfdp_parameter_header ph;
	struct sfdp_header hdr;
	int nph, i, ret;

	ret = nvmem_device_read(nvmem, 0, sizeof(hdr), &hdr);
	if (ret < 0)
		return ret;

	if (get_unaligned_le32(hdr.signature) != SFDP_SIGNATURE)
		return -EINVAL;

	/* The number of parameter headers (NPH) field is zero-based. */
	nph = hdr.nph;

	for (i = 0; i <= nph; i++) {
		ret = nvmem_device_read(nvmem, sizeof(hdr) + i * sizeof(ph),
					sizeof(ph), &ph);
		if (ret < 0)
			return ret;

		if (SFDP_PARAM_HEADER_ID(&ph) != SFDP_MCHP_VENDOR_ID)
			continue;

		*ptp = SFDP_PARAM_HEADER_PTP(&ph);
		return 0;
	}

	return -ENOENT;
}

static int sfdp_eui_add_cells(struct nvmem_layout *layout)
{
	struct nvmem_device *nvmem = layout->nvmem;
	struct device *dev = &layout->dev;
	struct nvmem_cell_info info = { };
	struct device_node *layout_np;
	u32 base = 0;
	u8 marker;
	int ret;

	ret = sfdp_eui_find_vendor_table(nvmem, &base);
	if (ret == -ENOENT) {
		dev_dbg(dev, "no Microchip SFDP vendor table found\n");
		return 0;
	}
	if (ret)
		return ret;

	/* The EUI-48 is present only if its marker byte is programmed. */
	ret = nvmem_device_read(nvmem, base + SFDP_MCHP_EUI48_MARKER_OFFSET,
				1, &marker);
	if (ret < 0)
		return ret;
	if (marker != SFDP_MCHP_EUI48_MARKER) {
		dev_dbg(dev, "EUI-48 not programmed (marker 0x%02x)\n", marker);
		return 0;
	}

	layout_np = of_nvmem_layout_get_container(nvmem);
	if (!layout_np)
		return -ENOENT;

	info.name = "mac-address";
	info.offset = base + SFDP_MCHP_EUI48_OFFSET;
	info.bytes = ETH_ALEN;
	info.np = of_get_child_by_name(layout_np, "mac-address");
	info.read_post_process = sfdp_eui_read_post_process;

	ret = nvmem_add_one_cell(nvmem, &info);
	if (ret)
		of_node_put(info.np);
	else
		dev_dbg(dev, "exposed EUI-48 at SFDP offset 0x%x\n", info.offset);

	of_node_put(layout_np);

	return ret;
}

static int sfdp_eui_probe(struct nvmem_layout *layout)
{
	layout->add_cells = sfdp_eui_add_cells;

	return nvmem_layout_register(layout);
}

static void sfdp_eui_remove(struct nvmem_layout *layout)
{
	nvmem_layout_unregister(layout);
}

static const struct of_device_id sfdp_eui_of_match_table[] = {
	{ .compatible = "microchip,sst26vf-sfdp-eui" },
	{}
};
MODULE_DEVICE_TABLE(of, sfdp_eui_of_match_table);

static struct nvmem_layout_driver sfdp_eui_layout = {
	.driver = {
		.name = "microchip-sst26vf-sfdp-eui-layout",
		.of_match_table = sfdp_eui_of_match_table,
	},
	.probe = sfdp_eui_probe,
	.remove = sfdp_eui_remove,
};
module_nvmem_layout_driver(sfdp_eui_layout);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Manikandan Muralidharan <manikandan.m@microchip.com>");
MODULE_DESCRIPTION("NVMEM layout for the EUI-48 in the Microchip/SST SFDP vendor table");
