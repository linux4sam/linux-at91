// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2021 Bootlin

#include <linux/arm-smccc.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>

#define REGMAP_SMC_READ		0
#define REGMAP_SMC_WRITE	1

struct regmap_smccc_ctx {
	u32 regmap_smc_id;
};

static int regmap_smccc_reg_write(void *context, unsigned int reg,
				  unsigned int val)
{
	struct regmap_smccc_ctx *ctx = context;
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(ctx->regmap_smc_id, REGMAP_SMC_WRITE, reg, val,
			     &res);

	if (res.a0)
		return -EACCES;

	return 0;
}

static int regmap_smccc_reg_read(void *context, unsigned int reg,
				 unsigned int *val)
{
	struct regmap_smccc_ctx *ctx = context;
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(ctx->regmap_smc_id, REGMAP_SMC_READ, reg, &res);

	if (res.a0)
		return -EACCES;

	*val = res.a1;

	return 0;
}

static struct regmap_bus regmap_smccc = {
	.reg_write = regmap_smccc_reg_write,
	.reg_read = regmap_smccc_reg_read,
};

static int regmap_smccc_bits_is_supported(int val_bits)
{
	switch (val_bits) {
	case 8:
	case 16:
	case 32:
		return 0;
	case 64:
	/*
	 * SMCs are using registers to pass information so if architecture is
	 * not using 64 bits registers, we won't be able to pass information
	 * transparently.
	 */
#if !defined(CONFIG_64BIT)
		return -EINVAL;
#else
		return 0;
#endif
	default:
		return -EINVAL;
	}
}

static struct regmap_smccc_ctx *smccc_regmap_init_ctx(
					const struct regmap_config *config,
					u32 regmap_smc_id)
{
	int ret;
	struct regmap_smccc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ret = regmap_smccc_bits_is_supported(config->val_bits);
	if (ret)
		return ERR_PTR(ret);

	ctx->regmap_smc_id = regmap_smc_id;

	return ctx;
}

struct regmap *__regmap_init_smccc(struct device *dev, u32 regmap_smc_id,
				   const struct regmap_config *config,
				   struct lock_class_key *lock_key,
				   const char *lock_name)
{
	struct regmap_smccc_ctx *ctx;

	ctx = smccc_regmap_init_ctx(config, regmap_smc_id);
	if (IS_ERR(ctx))
		return ERR_CAST(ctx);

	return __regmap_init(dev, &regmap_smccc, ctx, config, lock_key,
			     lock_name);
}
EXPORT_SYMBOL_GPL(__regmap_init_smccc);

struct regmap *__devm_regmap_init_smccc(struct device *dev, u32 regmap_smc_id,
					const struct regmap_config *config,
					struct lock_class_key *lock_key,
					const char *lock_name)
{
	struct regmap_smccc_ctx *ctx;

	ctx = smccc_regmap_init_ctx(config, regmap_smc_id);
	if (IS_ERR(ctx))
		return ERR_CAST(ctx);

	return __devm_regmap_init(dev, &regmap_smccc, ctx, config, lock_key,
				  lock_name);
}
EXPORT_SYMBOL_GPL(__devm_regmap_init_smccc);

MODULE_AUTHOR("Clément Léger <clement.leger@bootlin.com>");
MODULE_DESCRIPTION("Regmap SMCCC Module");
MODULE_LICENSE("GPL v2");
