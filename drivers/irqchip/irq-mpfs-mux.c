// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) GPIO IRQ MUX
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 */

#define pr_fmt(fmt) "mpfs-irq-mux: " fmt

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define MPFS_MUX_CONFIG_OFFSET		0x54
#define MPFS_MUX_NUM_IRQS		41
#define MPFS_MUX_NUM_DIRECT_IRQS	38
#define MPFS_MUX_NUM_NON_DIRECT_IRQS	3
#define MPFS_MAX_IRQS_PER_GPIO		32
#define MPFS_NUM_IRQS_GPIO0		14
#define MPFS_NUM_IRQS_GPIO1		24
#define MPFS_NUM_IRQS_GPIO2		32
#define MPFS_NUM_IRQS_GPIO02_SHIFT	0
#define MPFS_NUM_IRQS_GPIO1_SHIFT	14

/*
 * There are 3 GPIO controllers on this SoC, of which:
 * - GPIO controller 0 has 14 GPIOs
 * - GPIO controller 1 has 24 GPIOs
 * - GPIO controller 2 has 32 GPIOs
 *
 * All GPIOs are capable of generating interrupts, for a total of 70.
 * There are only 41 IRQs available however, so a configurable mux is used to
 * ensure all GPIOs can be used for interrupt generation.
 * 38 of the 41 interrupts are in what the documentation calls "direct mode",
 * as they provide an exclusive connection from a GPIO to the PLIC.
 * The 3 remaining interrupts are used to mux the interrupts which do not have
 * a exclusive connection, one for each GPIO controller.
 * A register is used to set this configuration of this mux, depending on how
 * the "MSS Configurator" (FPGA configuration tool) has set things up.
 * This is done by the platform's firmware, so access from Linux is read-only.
 *
 * Documentation also refers to GPIO controller 0 & 1 as "pad" GPIO controllers
 * and GPIO controller 2 as the "fabric" GPIO controller. Despite that wording,
 * all 3 are "hard" peripherals.
 *
 * The mux has a single register, where bits 0 to 13 mux between GPIO controller
 * 1's 14 GPIOs and GPIO controller 2's first 14 GPIOs. The remaining bits mux
 * between the first 18 GPIOs of controller 1 and the last 18 GPIOS of
 * controller 2. If a bit in the mux's control register is set, the
 * corresponding interrupt line for GPIO controller 0 or 1 will be put in
 * "non-direct" mode. If cleared, the "fabric" controller's will.
 *
 * Register layout:
 *    GPIO 1 interrupt line 17 | mux bit 31 | GPIO 2 interrupt line 31
 *    ...                      | ...        | ...
 *    ...                      | ...        | ...
 *    GPIO 1 interrupt line  0 | mux bit 14 | GPIO 2 interrupt line 14
 *    GPIO 0 interrupt line 13 | mux bit 13 | GPIO 2 interrupt line 13
 *    ...                      | ...        | ...
 *    ...                      | ...        | ...
 *    GPIO 0 interrupt line  0 | mux bit  0 | GPIO 2 interrupt line  0
 *
 * That leaves 6 exclusive, or "direct", interrupts remaining. These are
 * used by GPIO controller 1's lines 18 to 23.
 */

struct mpfs_irq_mux_bank_config {
	u32 mask;
	u8 shift;
};

static const struct mpfs_irq_mux_bank_config mpfs_irq_mux_bank_configs[3] = {
	{GENMASK(MPFS_NUM_IRQS_GPIO0 - 1, 0), MPFS_NUM_IRQS_GPIO02_SHIFT},
	{GENMASK(MPFS_NUM_IRQS_GPIO1 - 1, 0), MPFS_NUM_IRQS_GPIO1_SHIFT},
	{GENMASK(MPFS_NUM_IRQS_GPIO2 - 1, 0), MPFS_NUM_IRQS_GPIO02_SHIFT},
};

struct mpfs_irq_mux_irqchip {
	struct irq_domain *domain;
	int bank;
	int irq;
	u8 offset;
};

struct mpfs_irq_mux {
	struct regmap *regmap;
	u32 mux_config;
	struct mpfs_irq_mux_irqchip nondirect_irqchips[MPFS_MUX_NUM_NON_DIRECT_IRQS];
	int parent_irqs[MPFS_MUX_NUM_DIRECT_IRQS];
};

/*
 * There is no "control" hardware in this mux, and as such there is no ability
 * to mask at this level. As the irq has been disconnected from the hierarchy,
 * there's no parent irqchip from which to use mask functions either.
 */
static void mpfs_irq_mux_irq_mask(struct irq_data *d) {}
static void mpfs_irq_mux_irq_unmask(struct irq_data *d) {}

static struct irq_chip mpfs_irq_mux_nondirect_irq_chip = {
	.name = "MPFS GPIO Interrupt Mux",
	.irq_mask = mpfs_irq_mux_irq_mask,
	.irq_unmask = mpfs_irq_mux_irq_unmask,
	.flags = IRQCHIP_IMMUTABLE,
};

static struct irq_chip mpfs_irq_mux_irq_chip = {
	.name = "MPFS GPIO Interrupt Mux",
	.irq_mask = irq_chip_mask_parent,
	.irq_unmask = irq_chip_unmask_parent,
	.irq_eoi = irq_chip_eoi_parent,
	.irq_set_type = irq_chip_set_type_parent,
	.irq_set_affinity = irq_chip_set_affinity_parent,
	.flags = IRQCHIP_IMMUTABLE,
};

/*
 * Returns an unsigned long, where a set bit indicates the corresponding
 * interrupt is in non-direct/muxed mode for that bank/GPIO controller.
 */
static inline unsigned long mpfs_irq_mux_get_muxed_irqs(struct mpfs_irq_mux *priv,
							unsigned int bank)
{
	unsigned long mux_config = priv->mux_config, muxed_irqs = -1;
	struct mpfs_irq_mux_bank_config bank_config = mpfs_irq_mux_bank_configs[bank];

	/*
	 * If a bit is set in the mux, GPIO the corresponding interrupt from
	 * controller 2 is direct and that controllers 0 or 1 is muxed.
	 * Invert the bits in the configuration register, so that set bits
	 * equate to non-direct mode, for GPIO controller 2.
	 */
	if (bank == 2u)
		mux_config = ~mux_config;

	muxed_irqs &= bank_config.mask;
	muxed_irqs &= mux_config >> bank_config.shift;

	return muxed_irqs;
}

static void mpfs_irq_mux_nondirect_handler(struct irq_desc *desc)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = irq_desc_get_handler_data(desc);
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						 nondirect_irqchips[irqchip_data->bank]);
	unsigned long muxed_irqs;
	int pos;

	chained_irq_enter(irq_desc_get_chip(desc), desc);

	muxed_irqs = mpfs_irq_mux_get_muxed_irqs(priv, irqchip_data->bank);

	for_each_set_bit(pos, &muxed_irqs, MPFS_MAX_IRQS_PER_GPIO)
		generic_handle_domain_irq(irqchip_data->domain, irqchip_data->offset + pos);

	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static bool mpfs_irq_mux_is_direct(struct mpfs_irq_mux *priv, struct irq_fwspec *fwspec)
{
	unsigned int bank, line;
	u32 muxed_irqs;

	bank = fwspec->param[0] / MPFS_MAX_IRQS_PER_GPIO;
	line = fwspec->param[0] % MPFS_MAX_IRQS_PER_GPIO;

	muxed_irqs = mpfs_irq_mux_get_muxed_irqs(priv, bank);

	if (BIT(line) & muxed_irqs)
		return false;

	return true;
}

static int mpfs_irq_mux_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
				  unsigned long *out_hwirq, unsigned int *out_type)
{
	if (!is_of_node(fwspec->fwnode))
		return -EINVAL;

	return irq_domain_translate_onecell(d, fwspec, out_hwirq, out_type);
}

static int mpfs_irq_mux_nondirect_alloc(struct irq_domain *d, unsigned int virq,
					struct irq_fwspec *fwspec, struct mpfs_irq_mux *priv)
{
	unsigned int bank = fwspec->param[0] / MPFS_MAX_IRQS_PER_GPIO;

	if (bank > 2)
		return -EINVAL;

	priv->nondirect_irqchips[bank].domain = d;

	irq_domain_set_hwirq_and_chip(d, virq, fwspec->param[0],
				      &mpfs_irq_mux_nondirect_irq_chip, priv);
	irq_set_chained_handler_and_data(virq, handle_untracked_irq,
					 &priv->nondirect_irqchips[bank]);

	return irq_domain_disconnect_hierarchy(d->parent, virq);
}

static int mpfs_irq_mux_alloc(struct irq_domain *d, unsigned int virq,
			      unsigned int nr_irqs, void *arg)
{
	struct mpfs_irq_mux *priv = d->host_data;
	struct irq_fwspec *fwspec = arg;
	struct irq_fwspec parent_fwspec;
	unsigned int bank, line, irq;

	if (!mpfs_irq_mux_is_direct(priv, fwspec))
		return mpfs_irq_mux_nondirect_alloc(d, virq, fwspec, priv);

	bank = fwspec->param[0] / MPFS_MAX_IRQS_PER_GPIO;
	line = fwspec->param[0] % MPFS_MAX_IRQS_PER_GPIO;
	irq = line + mpfs_irq_mux_bank_configs[bank].shift;

	parent_fwspec.fwnode = d->parent->fwnode;
	parent_fwspec.param[0] = priv->parent_irqs[irq];
	parent_fwspec.param_count = 1;

	irq_domain_set_hwirq_and_chip(d, virq, fwspec->param[0], &mpfs_irq_mux_irq_chip, priv);

	return irq_domain_alloc_irqs_parent(d, virq, 1, &parent_fwspec);
}

static const struct irq_domain_ops mpfs_irq_mux_domain_ops = {
	.translate = mpfs_irq_mux_translate,
	.alloc = mpfs_irq_mux_alloc,
	.free = irq_domain_free_irqs_common,
};

static int mpfs_irq_mux_probe(struct platform_device *pdev)
{
	struct device_node *parent;
	struct device *dev = &pdev->dev;
	struct mpfs_irq_mux *priv;
	struct irq_domain *hier_domain, *parent_domain;
	int i, ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = device_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	ret = regmap_read(priv->regmap, MPFS_MUX_CONFIG_OFFSET, &priv->mux_config);
	if (ret)
		return ret;

	for (i = 0; i < MPFS_MUX_NUM_DIRECT_IRQS; i++) {
		struct of_phandle_args parent_irq;
		int ret;

		ret = of_irq_parse_one(to_of_node(dev->fwnode), i, &parent_irq);
		if (ret)
			return ret;

		/*
		 * The parent irqs are saved off for the first 38 interrupts
		 * from the devicetree entry so that they can be used in the
		 * domains alloc callback to allocate irqs from the parent irq
		 * chip directly.
		 */
		priv->parent_irqs[i] = parent_irq.args[0];
	}

	parent = of_irq_find_parent(to_of_node(dev->fwnode));
	parent_domain = irq_find_host(parent);
	hier_domain = irq_domain_add_hierarchy(parent_domain, 0, MPFS_MAX_IRQS_PER_GPIO * 3,
					       to_of_node(dev->fwnode), &mpfs_irq_mux_domain_ops,
					       priv);
	if (!hier_domain)
		return dev_err_probe(dev, -ENODEV, "failed to add hierarchical domain\n");

	/*
	 * The last 3 interrupts must be the non-direct/muxed ones, per
	 * the dt-binding.
	 */
	for (i = 0; i < MPFS_MUX_NUM_NON_DIRECT_IRQS; i++) {
		int irq_index = i + MPFS_MUX_NUM_DIRECT_IRQS;

		priv->nondirect_irqchips[i].bank = i;
		priv->nondirect_irqchips[i].irq = irq_of_parse_and_map(to_of_node(dev->fwnode),
								       irq_index);
		priv->nondirect_irqchips[i].offset = i * MPFS_MAX_IRQS_PER_GPIO;
		irq_set_chained_handler_and_data(priv->nondirect_irqchips[i].irq,
						 mpfs_irq_mux_nondirect_handler,
						 &priv->nondirect_irqchips[i]);
	}

	dev_info(dev, "mux configuration %x\n", priv->mux_config);

	return 0;
}

static const struct of_device_id mpfs_irq_mux_match[] = {
	{ .compatible = "microchip,mpfs-gpio-irq-mux" },
	{}
};

static struct platform_driver mpfs_irq_mux_driver = {
	.driver = {
		.name = "mpfs_irq_mux",
		.of_match_table	= mpfs_irq_mux_match,
	},
	.probe = mpfs_irq_mux_probe,
};
builtin_platform_driver(mpfs_irq_mux_driver);
