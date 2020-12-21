// SPDX-License-Identifier: GPL-2.0
//
// Ingenic VPU hardware spinlock driver
//
// Copyright (C) 2020, Paul Cercueil <paul@crapouillou.net>

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/hwspinlock.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "hwspinlock_internal.h"

#define REG_AUX_SPINLK		0x0
#define REG_AUX_SPIN1		0x4
#define REG_AUX_SPIN2		0x8

#define AUX_SPIN1_LOCKED	BIT(0)
#define AUX_SPIN2_LOCKED	BIT(1)

struct ingenic_lock {
	void __iomem *base;
	struct clk *clk;
	struct hwspinlock_device bank;
};

static int ingenic_hwspinlock_trylock(struct hwspinlock *lock)
{
	struct ingenic_lock *priv = lock->priv;
	int err;
	u32 val;

	err = clk_enable(priv->clk);
	if (err)
		return err;

	readl(priv->base + REG_AUX_SPIN1);

	val = readl(priv->base + REG_AUX_SPINLK);

	return val == AUX_SPIN1_LOCKED;
}

static void ingenic_hwspinlock_unlock(struct hwspinlock *lock)
{
	struct ingenic_lock *priv = lock->priv;

	writel(0, priv->base + REG_AUX_SPINLK);
	clk_disable(priv->clk);
}

static const struct hwspinlock_ops ingenic_hwspinlock_ops = {
	.trylock	= ingenic_hwspinlock_trylock,
	.unlock		= ingenic_hwspinlock_unlock,
};

static void ingenic_hwspinlock_clk_unprepare(void *d)
{
	clk_unprepare(d);
}

static int ingenic_hwspinlock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ingenic_lock *priv;
	int err;

	priv = devm_kzalloc(dev, struct_size(priv, bank.lock, 1), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = devm_clk_get(dev->parent, "aux");
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk), "Failed to get clock\n");

	err = clk_prepare(priv->clk);
	if (err)
		return err;

	err = devm_add_action_or_reset(dev, ingenic_hwspinlock_clk_unprepare,
				       priv->clk);
	if (err)
		return err;

	priv->bank.lock->priv = priv;

	/* Init registers */
	writel(0, priv->base + REG_AUX_SPINLK);
	writel(AUX_SPIN1_LOCKED, priv->base + REG_AUX_SPIN1);
	writel(AUX_SPIN2_LOCKED, priv->base + REG_AUX_SPIN2);

	return devm_hwspin_lock_register(dev, &priv->bank,
					 &ingenic_hwspinlock_ops, 0, 1);
}

static const struct of_device_id ingenic_hwspinlock_of_match[] = {
	{ .compatible = "ingenic,jz4760-vpu-hwspinlock", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ingenic_hwspinlock_of_match);

static struct platform_driver ingenic_hwspinlock_driver = {
	.probe = ingenic_hwspinlock_probe,
	.driver = {
		.name = "ingenic-hwspinlock",
		.of_match_table = ingenic_hwspinlock_of_match,
	},
};
module_platform_driver(ingenic_hwspinlock_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Ingenic VPU hardware spinlock driver");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
