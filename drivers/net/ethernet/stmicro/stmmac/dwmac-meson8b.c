// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amlogic Meson8b, Meson8m2 and GXBB DWMAC glue layer
 *
 * Copyright (C) 2016 Martin Blumenstingl <martin.blumenstingl@googlemail.com>
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"

#define PRG_ETH0			0x0

#define PRG_ETH0_RGMII_MODE		BIT(0)

#define PRG_ETH0_EXT_PHY_MODE_MASK	GENMASK(2, 0)
#define PRG_ETH0_EXT_RGMII_MODE		1
#define PRG_ETH0_EXT_RMII_MODE		4

/* mux to choose between fclk_div2 (bit unset) and mpll2 (bit set) */
#define PRG_ETH0_CLK_M250_SEL_MASK	GENMASK(4, 4)

/* TX clock delay in ns = "8ns / 4 * tx_dly_val" (where 8ns are exactly one
 * cycle of the 125MHz RGMII TX clock):
 * 0ns = 0x0, 2ns = 0x1, 4ns = 0x2, 6ns = 0x3
 */
#define PRG_ETH0_TXDLY_MASK		GENMASK(6, 5)

/* divider for the result of m250_sel */
#define PRG_ETH0_CLK_M250_DIV_SHIFT	7
#define PRG_ETH0_CLK_M250_DIV_WIDTH	3

#define PRG_ETH0_RGMII_TX_CLK_EN	10

#define PRG_ETH0_INVERTED_RMII_CLK	BIT(11)
#define PRG_ETH0_TX_AND_PHY_REF_CLK	BIT(12)

/* Bypass (= 0, the signal from the GPIO input directly connects to the
 * internal sampling) or enable (= 1) the internal logic for RXEN and RXD[3:0]
 * timing tuning.
 */
#define PRG_ETH0_ADJ_ENABLE		BIT(13)
/* Controls whether the RXEN and RXD[3:0] signals should be aligned with the
 * input RX rising/falling edge and sent to the Ethernet internals. This sets
 * the automatically delay and skew automatically (internally).
 */
#define PRG_ETH0_ADJ_SETUP		BIT(14)
/* An internal counter based on the "timing-adjustment" clock. The counter is
 * cleared on both, the falling and rising edge of the RX_CLK. This selects the
 * delay (= the counter value) when to start sampling RXEN and RXD[3:0].
 */
#define PRG_ETH0_ADJ_DELAY		GENMASK(19, 15)
/* Adjusts the skew between each bit of RXEN and RXD[3:0]. If a signal has a
 * large input delay, the bit for that signal (RXEN = bit 0, RXD[3] = bit 1,
 * ...) can be configured to be 1 to compensate for a delay of about 1ns.
 */
#define PRG_ETH0_ADJ_SKEW		GENMASK(24, 20)

#define PRG_ETH0_START_CALIBRATION	BIT(25)

    /* 0: falling edge, 1: rising edge */
#define PRG_ETH0_TEST_EDGE		BIT(26)

/* Select one signal from {RXDV, RXD[3:0]} to calibrate */
#define PRG_ETH0_SIGNAL_TO_CALIBRATE	GENMASK(29, 27)

#define PRG_ETH1			0x4

/* Signal switch position in 1ns resolution */
#define PRG_ETH1_SIGNAL_SWITCH_POSITION	GENMASK(4, 0)

/* RXC (RX clock) length in 1ns resolution */
#define PRG_ETH1_RX_CLK_LENGTH		GENMASK(9, 5)

#define PRG_ETH1_CALI_WAITING_FOR_EVENT	BIT(10)

#define PRG_ETH1_SIGNAL_UNDER_TEST	GENMASK(13, 11)

/* 0: falling edge, 1: rising edge */
#define PRG_ETH1_RESULT_EDGE		BIT(14)

#define PRG_ETH1_RESULT_IS_VALID	BIT(15)


/* Defined for adding a delay to the input RX_CLK for better timing.
 * Each step is 200ps. These bits are used with external RGMII PHYs
 * because RGMII RX only has the small window. cfg_rxclk_dly can
 * adjust the window between RX_CLK and RX_DATA and improve the stability
 * of "rx data valid". only valid on G12A and later?
 */
#define PRG_ETH1_CFG_RXCLK_DLY		GENMASK(19, 16)

struct meson8b_dwmac;

struct meson8b_dwmac_data {
	int (*set_phy_mode)(struct meson8b_dwmac *dwmac);
	bool has_prg_eth1_rgmii_rx_delay;
};

struct meson8b_dwmac {
	struct device			*dev;
	void __iomem			*regs;

	const struct meson8b_dwmac_data	*data;
	phy_interface_t			phy_mode;
	struct clk			*rgmii_tx_clk;
	u32				tx_delay_ns;
	u32				rx_delay_ps;
	struct clk			*timing_adj_clk;
};

struct meson8b_dwmac_clk_configs {
	struct clk_mux		m250_mux;
	struct clk_divider	m250_div;
	struct clk_fixed_factor	fixed_div2;
	struct clk_gate		rgmii_tx_en;
};

static void meson8b_dwmac_mask_bits(struct meson8b_dwmac *dwmac, u32 reg,
				    u32 mask, u32 value)
{
	u32 data;

	data = readl(dwmac->regs + reg);
	data &= ~mask;
	data |= (value & mask);

	writel(data, dwmac->regs + reg);
}

static struct clk *meson8b_dwmac_register_clk(struct meson8b_dwmac *dwmac,
					      const char *name_suffix,
					      const struct clk_parent_data *parents,
					      int num_parents,
					      const struct clk_ops *ops,
					      struct clk_hw *hw)
{
	struct clk_init_data init = { };
	char clk_name[32];

	snprintf(clk_name, sizeof(clk_name), "%s#%s", dev_name(dwmac->dev),
		 name_suffix);

	init.name = clk_name;
	init.ops = ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_data = parents;
	init.num_parents = num_parents;

	hw->init = &init;

	return devm_clk_register(dwmac->dev, hw);
}

static int meson8b_init_rgmii_tx_clk(struct meson8b_dwmac *dwmac)
{
	struct clk *clk;
	struct device *dev = dwmac->dev;
	static const struct clk_parent_data mux_parents[] = {
		{ .fw_name = "clkin0", },
		{ .index = -1, },
	};
	static const struct clk_div_table div_table[] = {
		{ .div = 2, .val = 2, },
		{ .div = 3, .val = 3, },
		{ .div = 4, .val = 4, },
		{ .div = 5, .val = 5, },
		{ .div = 6, .val = 6, },
		{ .div = 7, .val = 7, },
		{ /* end of array */ }
	};
	struct meson8b_dwmac_clk_configs *clk_configs;
	struct clk_parent_data parent_data = { };

	clk_configs = devm_kzalloc(dev, sizeof(*clk_configs), GFP_KERNEL);
	if (!clk_configs)
		return -ENOMEM;

	clk_configs->m250_mux.reg = dwmac->regs + PRG_ETH0;
	clk_configs->m250_mux.shift = __ffs(PRG_ETH0_CLK_M250_SEL_MASK);
	clk_configs->m250_mux.mask = PRG_ETH0_CLK_M250_SEL_MASK >>
				     clk_configs->m250_mux.shift;
	clk = meson8b_dwmac_register_clk(dwmac, "m250_sel", mux_parents,
					 ARRAY_SIZE(mux_parents), &clk_mux_ops,
					 &clk_configs->m250_mux.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_data.hw = &clk_configs->m250_mux.hw;
	clk_configs->m250_div.reg = dwmac->regs + PRG_ETH0;
	clk_configs->m250_div.shift = PRG_ETH0_CLK_M250_DIV_SHIFT;
	clk_configs->m250_div.width = PRG_ETH0_CLK_M250_DIV_WIDTH;
	clk_configs->m250_div.table = div_table;
	clk_configs->m250_div.flags = CLK_DIVIDER_ALLOW_ZERO |
				      CLK_DIVIDER_ROUND_CLOSEST;
	clk = meson8b_dwmac_register_clk(dwmac, "m250_div", &parent_data, 1,
					 &clk_divider_ops,
					 &clk_configs->m250_div.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_data.hw = &clk_configs->m250_div.hw;
	clk_configs->fixed_div2.mult = 1;
	clk_configs->fixed_div2.div = 2;
	clk = meson8b_dwmac_register_clk(dwmac, "fixed_div2", &parent_data, 1,
					 &clk_fixed_factor_ops,
					 &clk_configs->fixed_div2.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	parent_data.hw = &clk_configs->fixed_div2.hw;
	clk_configs->rgmii_tx_en.reg = dwmac->regs + PRG_ETH0;
	clk_configs->rgmii_tx_en.bit_idx = PRG_ETH0_RGMII_TX_CLK_EN;
	clk = meson8b_dwmac_register_clk(dwmac, "rgmii_tx_en", &parent_data, 1,
					 &clk_gate_ops,
					 &clk_configs->rgmii_tx_en.hw);
	if (WARN_ON(IS_ERR(clk)))
		return PTR_ERR(clk);

	dwmac->rgmii_tx_clk = clk;

	return 0;
}

static int meson8b_set_phy_mode(struct meson8b_dwmac *dwmac)
{
	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* enable RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_RGMII_MODE,
					PRG_ETH0_RGMII_MODE);
		break;
	case PHY_INTERFACE_MODE_RMII:
		/* disable RGMII mode -> enables RMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_RGMII_MODE, 0);
		break;
	default:
		dev_err(dwmac->dev, "fail to set phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	return 0;
}

static int meson_axg_set_phy_mode(struct meson8b_dwmac *dwmac)
{
	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		/* enable RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_EXT_PHY_MODE_MASK,
					PRG_ETH0_EXT_RGMII_MODE);
		break;
	case PHY_INTERFACE_MODE_RMII:
		/* disable RGMII mode -> enables RMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_EXT_PHY_MODE_MASK,
					PRG_ETH0_EXT_RMII_MODE);
		break;
	default:
		dev_err(dwmac->dev, "fail to set phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	return 0;
}

static void meson8b_clk_disable_unprepare(void *data)
{
	clk_disable_unprepare(data);
}

static int meson8b_devm_clk_prepare_enable(struct meson8b_dwmac *dwmac,
					   struct clk *clk)
{
	int ret;

	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dwmac->dev,
					meson8b_clk_disable_unprepare, clk);
}

static int meson8b_init_rgmii_delays(struct meson8b_dwmac *dwmac)
{
	u32 tx_dly_config, rx_adj_config, cfg_rxclk_dly, delay_config;
	int ret;

	rx_adj_config = 0;
	cfg_rxclk_dly = 0;
	tx_dly_config = FIELD_PREP(PRG_ETH0_TXDLY_MASK,
				   dwmac->tx_delay_ns >> 1);

	if (dwmac->data->has_prg_eth1_rgmii_rx_delay)
		cfg_rxclk_dly = FIELD_PREP(PRG_ETH1_CFG_RXCLK_DLY,
					   dwmac->rx_delay_ps / 200);
	else if (dwmac->rx_delay_ps == 2000)
		rx_adj_config = PRG_ETH0_ADJ_ENABLE | PRG_ETH0_ADJ_SETUP;

	switch (dwmac->phy_mode) {
	case PHY_INTERFACE_MODE_RGMII:
		delay_config = tx_dly_config | rx_adj_config;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		delay_config = tx_dly_config;
		cfg_rxclk_dly = 0;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		dev_info(dwmac->dev, "DEVMFC: %s Selected RGMII-TXID phy-mode, so not setting TX clock skew on mac side\n", __func__);
		delay_config = rx_adj_config;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RMII:
		delay_config = 0;
		cfg_rxclk_dly = 0;
		break;
	default:
		dev_err(dwmac->dev, "unsupported phy-mode %s\n",
			phy_modes(dwmac->phy_mode));
		return -EINVAL;
	}

	if (delay_config & PRG_ETH0_ADJ_ENABLE) {
		dev_info(dwmac->dev, "DEVMFC: %s Old fashioned RX delay selected, configuring timing adjustment clock\n", __func__);

		if (!dwmac->timing_adj_clk) {
			dev_err(dwmac->dev,
				"The timing-adjustment clock is mandatory for the RX delay re-timing\n");
			return -EINVAL;
		}

		/* The timing adjustment logic is driven by a separate clock */
		ret = meson8b_devm_clk_prepare_enable(dwmac,
						      dwmac->timing_adj_clk);
		if (ret) {
			dev_err(dwmac->dev,
				"Failed to enable the timing-adjustment clock\n");
			return ret;
		}
	}

	if(delay_config & PRG_ETH0_TXDLY_MASK)
		dev_info(dwmac->dev, "DEVMFC: %s, enabling mac side TX clock delay: %d\n", __func__, dwmac->tx_delay_ns);

	meson8b_dwmac_mask_bits(dwmac, PRG_ETH0, PRG_ETH0_TXDLY_MASK |
				PRG_ETH0_ADJ_ENABLE | PRG_ETH0_ADJ_SETUP |
				PRG_ETH0_ADJ_DELAY | PRG_ETH0_ADJ_SKEW,
				delay_config);

	meson8b_dwmac_mask_bits(dwmac, PRG_ETH1, PRG_ETH1_CFG_RXCLK_DLY,
				cfg_rxclk_dly);

	return 0;
}

static int meson8b_init_prg_eth(struct meson8b_dwmac *dwmac)
{
	int ret;

	if (phy_interface_mode_is_rgmii(dwmac->phy_mode)) {
		/* only relevant for RMII mode -> disable in RGMII mode */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_INVERTED_RMII_CLK, 0);

		/* Configure the 125MHz RGMII TX clock, the IP block changes
		 * the output automatically (= without us having to configure
		 * a register) based on the line-speed (125MHz for Gbit speeds,
		 * 25MHz for 100Mbit/s and 2.5MHz for 10Mbit/s).
		 */
		ret = clk_set_rate(dwmac->rgmii_tx_clk, 125 * 1000 * 1000);
		if (ret) {
			dev_err(dwmac->dev,
				"failed to set RGMII TX clock\n");
			return ret;
		}

		ret = meson8b_devm_clk_prepare_enable(dwmac,
						      dwmac->rgmii_tx_clk);
		if (ret) {
			dev_err(dwmac->dev,
				"failed to enable the RGMII TX clock\n");
			return ret;
		}
	} else {
		/* invert internal clk_rmii_i to generate 25/2.5 tx_rx_clk */
		meson8b_dwmac_mask_bits(dwmac, PRG_ETH0,
					PRG_ETH0_INVERTED_RMII_CLK,
					PRG_ETH0_INVERTED_RMII_CLK);
	}

	/* enable TX_CLK and PHY_REF_CLK generator */
	meson8b_dwmac_mask_bits(dwmac, PRG_ETH0, PRG_ETH0_TX_AND_PHY_REF_CLK,
				PRG_ETH0_TX_AND_PHY_REF_CLK);

	return 0;
}

int cmdline_mc_val = -1;
module_param_named(mc_val, cmdline_mc_val, int, 0); // 0 is permission? dwmac-meson8b.mc_val=0x1621
MODULE_PARM_DESC(mc_val, "Kernel commandline mc_val. Is override value for the complete (u32|0xffff_ffff) ETH_REG0 register");

int cmdline_cali_val = -1;
module_param_named(cali_val, cmdline_cali_val, int, 0); // 0 is permission? dwmac-meson8b.cali_val = 0x40000
MODULE_PARM_DESC(cali_val, "Kernel commandline cali_val. Is override value for the complete (u32|0xffff_ffff) ETH_REG1 register");

static void debug_show_regs(struct meson8b_dwmac* dwmac, char* state_name)
{
	u32 tmp_val;
	
	tmp_val = readl(dwmac->regs + PRG_ETH0);
	dev_info(dwmac->dev, "DEVMFC:  %s eth reg0 value: 0x%08X\n", state_name, tmp_val);

	tmp_val = readl(dwmac->regs + PRG_ETH1);
	dev_info(dwmac->dev, "DEVMFC:  %s eth reg1 value: 0x%08X\n", state_name, tmp_val);
}

static int meson8b_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct meson8b_dwmac *dwmac;
	int ret;

	//pr_info("DEVMFC: %s\n", __func__);
	
	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	dwmac->data = (const struct meson8b_dwmac_data *)
		of_device_get_match_data(&pdev->dev);
	if (!dwmac->data)
		return -EINVAL;
	dwmac->regs = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(dwmac->regs))
		return PTR_ERR(dwmac->regs);

	dwmac->dev = &pdev->dev;
	ret = of_get_phy_mode(pdev->dev.of_node, &dwmac->phy_mode);
	if (ret) {
		dev_err(&pdev->dev, "missing phy-mode property\n");
		return ret;
	}

	/* use 2ns as fallback since this value was previously hardcoded */
	if (of_property_read_u32(pdev->dev.of_node, "amlogic,tx-delay-ns",
				 &dwmac->tx_delay_ns))
		dwmac->tx_delay_ns = 2;

	/* RX delay defaults to 0ps since this is what many boards use */
	if (of_property_read_u32(pdev->dev.of_node, "rx-internal-delay-ps",
				 &dwmac->rx_delay_ps)) {
		if (!of_property_read_u32(pdev->dev.of_node,
					  "amlogic,rx-delay-ns",
					  &dwmac->rx_delay_ps))
			/* convert ns to ps */
			dwmac->rx_delay_ps *= 1000;
	}

	if (dwmac->data->has_prg_eth1_rgmii_rx_delay) {
		if (dwmac->rx_delay_ps > 3000 || dwmac->rx_delay_ps % 200) {
			dev_err(dwmac->dev,
				"The RGMII RX delay range is 0..3000ps in 200ps steps");
			return -EINVAL;
		}
	} else {
		if (dwmac->rx_delay_ps != 0 && dwmac->rx_delay_ps != 2000) {
			dev_err(dwmac->dev,
				"The only allowed RGMII RX delays values are: 0ps, 2000ps");
			return -EINVAL;
		}
	}

	dwmac->timing_adj_clk = devm_clk_get_optional(dwmac->dev,
						      "timing-adjustment");
	if (IS_ERR(dwmac->timing_adj_clk))
		return PTR_ERR(dwmac->timing_adj_clk);

	ret = meson8b_init_rgmii_delays(dwmac);
	if (ret)
		return ret;

	ret = meson8b_init_rgmii_tx_clk(dwmac);
	if (ret)
		return ret;

	ret = dwmac->data->set_phy_mode(dwmac);
	if (ret)
		return ret;

	ret = meson8b_init_prg_eth(dwmac);
	if (ret)
		return ret;

	plat_dat->bsp_priv = dwmac;
	
	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		return ret;

	debug_show_regs(dwmac, "after stmmac_dvr_probe");
	
	/* DEVMMFC: debug */
	if (cmdline_mc_val > -1) {
		dev_info(dwmac->dev, "DEVMFC: set reg0 value to dwmac-meson8b.mc_val: 0x%08X\n", cmdline_mc_val);
		writel(cmdline_mc_val, dwmac->regs + PRG_ETH0);
	}
		
	if (cmdline_cali_val > -1) {
		dev_info(dwmac->dev, "DEVMFC: set reg1 value to dwmac-meson8b.cali_val: 0x%08X\n", cmdline_cali_val);
		writel(cmdline_cali_val, dwmac->regs + PRG_ETH1);
	}
	
	debug_show_regs(dwmac, "resulting");
	/* end debug */

	return 0;
	
}

static const struct meson8b_dwmac_data meson8b_dwmac_data = {
	.set_phy_mode = meson8b_set_phy_mode,
	.has_prg_eth1_rgmii_rx_delay = false,
};

static const struct meson8b_dwmac_data meson_axg_dwmac_data = {
	.set_phy_mode = meson_axg_set_phy_mode,
	.has_prg_eth1_rgmii_rx_delay = false,
};

static const struct meson8b_dwmac_data meson_g12a_dwmac_data = {
	.set_phy_mode = meson_axg_set_phy_mode,
	.has_prg_eth1_rgmii_rx_delay = true,
};

static const struct of_device_id meson8b_dwmac_match[] = {
	{
		.compatible = "amlogic,meson8b-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson8m2-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson-gxbb-dwmac",
		.data = &meson8b_dwmac_data,
	},
	{
		.compatible = "amlogic,meson-axg-dwmac",
		.data = &meson_axg_dwmac_data,
	},
	{
		.compatible = "amlogic,meson-g12a-dwmac",
		.data = &meson_g12a_dwmac_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, meson8b_dwmac_match);

static struct platform_driver meson8b_dwmac_driver = {
	.probe  = meson8b_dwmac_probe,
	.remove_new = stmmac_pltfr_remove,
	.driver = {
		.name           = "meson8b-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = meson8b_dwmac_match,
	},
};
module_platform_driver(meson8b_dwmac_driver);

MODULE_AUTHOR("Martin Blumenstingl <martin.blumenstingl@googlemail.com>");
MODULE_DESCRIPTION("Amlogic Meson8b, Meson8m2 and GXBB DWMAC glue layer");
MODULE_LICENSE("GPL v2");
