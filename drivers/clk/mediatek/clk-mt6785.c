// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 THToufique. All rights reserved.

/*
 * MediaTek MT6785 clock driver - Linux 6.12 port, part 1.
 *
 * Ported from the 4.14.356-openela tree
 * (drivers/clk/mediatek/clk-mt6785.c).
 *
 * This part covers the gate-only domains that do not depend on DVFSP.
 * The PLL/DVFSP side (mediatek,apmixed plls[], mt6785-dvfsp) is NOT here:
 * 6.12 has no dvfsp equivalent, and apmixed is its parent provider.
 *
 * Porting notes:
 *  - MTK's local mtk_clk_gate_ops_dummy / _dummy_all / _setclr_inv_dummy /
 *    _setclr_dump are defined here; upstream only provides the four plain ops.
 *    A "dummy" op disables nothing -- it is how the 4.14 tree keeps clocks on
 *    during bringup. Setting CONFIG_MT6785_CCF_BRINGUP=n selects the real
 *    gates, matching the 4.14 "#else" branch.
 *  - MTK's mtk_clk_gate_ops is equivalent to upstream mtk_clk_gate_ops_no_setclr
 *    (clear = enable, set = disable); the banks below have
 *    set_ofs == clr_ofs == sta_ofs, which is what that op expects.
 *  - The 4.14 driver registers via CLK_OF_DECLARE_DRIVER and of_clk_add_provider.
 *    6.12 requires a platform_driver probe, of_clk_add_hw_provider(), a
 *    struct clk_hw_onecell_data, and mtk_clk_register_gates() gains a
 *    struct device * first argument.
 *  - The 4.14 *_DUMMY gate ops (mtk_clk_gate_ops_dummy and friends) are NOT
 *    ported. They are selected only under MT_CCF_BRINGUP, where disabling is a
 *    deliberate no-op so clocks stay up during bringup. Production uses the real
 *    gates, so this driver maps them to the upstream ops.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include "clk-mt6785.h"

// Lock is required by the mux/PLL domains (topckgen, infracfg_ao); those are
// not ported yet, so nothing needs it in this part.

/*
 * Local gate ops missing from the common clock framework.
 * These mirror the 4.14 behaviour exactly: a "dummy" disable is a no-op.
 */
static const struct mtk_gate_regs audio0_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x0,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs audio1_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x4,
	.sta_ofs = 0x4,
};

#define GATE_AUDIO0(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &audio0_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_no_setclr)

#define GATE_AUDIO1(_id, _name, _parent, _shift)			\
	GATE_MTK(_id, _name, _parent, &audio1_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_no_setclr)

#define GATE_AUDIO0_DUMMY(_id, _name, _parent, _shift)		\
	GATE_MTK(_id, _name, _parent, &audio0_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_no_setclr)

#define GATE_AUDIO1_DUMMY(_id, _name, _parent, _shift)		\
	GATE_MTK(_id, _name, _parent, &audio1_cg_regs, _shift,		\
		 &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate audio_clks[] __initconst = {
#if IS_ENABLED(CONFIG_COMMON_CLK_MT6785_CCF_BRINGUP)
	/* AUDIO0 */
	GATE_AUDIO0_DUMMY(AUDIO_AFE, "aud_afe", "audio_sel",
		2),
	GATE_AUDIO0_DUMMY(AUDIO_22M, "aud_22m", "aud_eng1_sel",
		8),
	GATE_AUDIO0(AUDIO_24M, "aud_24m", "aud_eng2_sel",
		9),
	GATE_AUDIO0_DUMMY(AUDIO_APLL2_TUNER, "aud_apll2_tuner", "aud_eng2_sel",
		18),
	GATE_AUDIO0_DUMMY(AUDIO_APLL_TUNER, "aud_apll_tuner", "aud_eng1_sel",
		19),
	GATE_AUDIO0_DUMMY(AUDIO_TDM, "aud_tdm", "aud_eng1_sel",
		20),
	GATE_AUDIO0_DUMMY(AUDIO_ADC, "aud_adc", "audio_sel",
		24),
	GATE_AUDIO0_DUMMY(AUDIO_DAC, "aud_dac", "audio_sel",
		25),
	GATE_AUDIO0_DUMMY(AUDIO_DAC_PREDIS, "aud_dac_predis", "audio_sel",
		26),
	GATE_AUDIO0_DUMMY(AUDIO_TML, "aud_tml", "audio_sel",
		27),
	GATE_AUDIO0_DUMMY(AUDIO_NLE, "aud_nle", "audio_sel",
		28),
	/* AUDIO1: hf_faudio_ck/hf_faud_engen1_ck/hf_faud_engen2_ck */
	GATE_AUDIO1_DUMMY(AUDIO_I2S1_BCLK_SW, "aud_i2s1_bclk", "audio_sel",
		4),
	GATE_AUDIO1_DUMMY(AUDIO_I2S2_BCLK_SW, "aud_i2s2_bclk", "audio_sel",
		5),
	GATE_AUDIO1_DUMMY(AUDIO_I2S3_BCLK_SW, "aud_i2s3_bclk", "audio_sel",
		6),
	GATE_AUDIO1_DUMMY(AUDIO_I2S4_BCLK_SW, "aud_i2s4_bclk", "audio_sel",
		7),
	GATE_AUDIO1_DUMMY(AUDIO_I2S5_BCLK_SW, "aud_i2s5_bclk", "audio_sel",
		8),

	GATE_AUDIO1_DUMMY(AUDIO_CONN_I2S_ASRC, "aud_conn_i2s", "audio_sel",
		12),
	GATE_AUDIO1_DUMMY(AUDIO_GENERAL1_ASRC, "aud_general1", "audio_sel",
		13),
	GATE_AUDIO1_DUMMY(AUDIO_GENERAL2_ASRC, "aud_general2", "audio_sel",
		14),
	GATE_AUDIO1_DUMMY(AUDIO_DAC_HIRES, "aud_dac_hires", "audio_h_sel",
		15),
	GATE_AUDIO1_DUMMY(AUDIO_ADC_HIRES, "aud_adc_hires", "audio_h_sel",
		16),
	GATE_AUDIO1_DUMMY(AUDIO_ADC_HIRES_TML, "aud_adc_hires_tml",
		"audio_h_sel",
		17),

	GATE_AUDIO1_DUMMY(AUDIO_PDN_ADDA6_ADC, "aud_pdn_adda6_adc",
		"audio_sel",
		20),
	GATE_AUDIO1_DUMMY(AUDIO_ADDA6_ADC_HIRES, "aud_adda6_adc_hires",
		"audio_h_sel",
		21),
	GATE_AUDIO1_DUMMY(AUDIO_3RD_DAC, "aud_3rd_dac", "audio_sel",
		28),
	GATE_AUDIO1_DUMMY(AUDIO_3RD_DAC_PREDIS, "aud_3rd_dac_predis",
		"audio_sel",
		29),
	GATE_AUDIO1_DUMMY(AUDIO_3RD_DAC_TML, "aud_3rd_dac_tml",
		"audio_sel",
		30),
	GATE_AUDIO1_DUMMY(AUDIO_3RD_DAC_HIRES, "aud_3rd_dac_hires",
		"audio_h_sel",
		31),
#else
	/* AUDIO0 */
	GATE_AUDIO0(AUDIO_AFE, "aud_afe", "audio_sel",
		2),
	GATE_AUDIO0(AUDIO_22M, "aud_22m", "aud_eng1_sel",
		8),
	GATE_AUDIO0(AUDIO_24M, "aud_24m", "aud_eng2_sel",
		9),
	GATE_AUDIO0(AUDIO_APLL2_TUNER, "aud_apll2_tuner", "aud_eng2_sel",
		18),
	GATE_AUDIO0(AUDIO_APLL_TUNER, "aud_apll_tuner", "aud_eng1_sel",
		19),
	GATE_AUDIO0(AUDIO_TDM, "aud_tdm", "aud_eng1_sel",
		20),
	GATE_AUDIO0(AUDIO_ADC, "aud_adc", "audio_sel",
		24),
	GATE_AUDIO0(AUDIO_DAC, "aud_dac", "audio_sel",
		25),
	GATE_AUDIO0(AUDIO_DAC_PREDIS, "aud_dac_predis", "audio_sel",
		26),
	GATE_AUDIO0(AUDIO_TML, "aud_tml", "audio_sel",
		27),
	GATE_AUDIO0(AUDIO_NLE, "aud_nle", "audio_sel",
		28),
	/* AUDIO1: hf_faudio_ck/hf_faud_engen1_ck/hf_faud_engen2_ck */
	GATE_AUDIO1(AUDIO_I2S1_BCLK_SW, "aud_i2s1_bclk", "audio_sel",
		4),
	GATE_AUDIO1(AUDIO_I2S2_BCLK_SW, "aud_i2s2_bclk", "audio_sel",
		5),
	GATE_AUDIO1(AUDIO_I2S3_BCLK_SW, "aud_i2s3_bclk", "audio_sel",
		6),
	GATE_AUDIO1(AUDIO_I2S4_BCLK_SW, "aud_i2s4_bclk", "audio_sel",
		7),
	GATE_AUDIO1(AUDIO_I2S5_BCLK_SW, "aud_i2s5_bclk", "audio_sel",
		8),

	GATE_AUDIO1(AUDIO_CONN_I2S_ASRC, "aud_conn_i2s", "audio_sel",
		12),
	GATE_AUDIO1(AUDIO_GENERAL1_ASRC, "aud_general1", "audio_sel",
		13),
	GATE_AUDIO1(AUDIO_GENERAL2_ASRC, "aud_general2", "audio_sel",
		14),
	GATE_AUDIO1(AUDIO_DAC_HIRES, "aud_dac_hires", "audio_h_sel",
		15),
	GATE_AUDIO1(AUDIO_ADC_HIRES, "aud_adc_hires", "audio_h_sel",
		16),
	GATE_AUDIO1(AUDIO_ADC_HIRES_TML, "aud_adc_hires_tml", "audio_h_sel",
		17),

	GATE_AUDIO1(AUDIO_PDN_ADDA6_ADC, "aud_pdn_adda6_adc", "audio_sel",
		20),
	GATE_AUDIO1(AUDIO_ADDA6_ADC_HIRES, "aud_adda6_adc_hires", "audio_h_sel",
		21),
	GATE_AUDIO1(AUDIO_3RD_DAC, "aud_3rd_dac", "audio_sel",
		28),
	GATE_AUDIO1(AUDIO_3RD_DAC_PREDIS, "aud_3rd_dac_predis", "audio_sel",
		29),
	GATE_AUDIO1(AUDIO_3RD_DAC_TML, "aud_3rd_dac_tml", "audio_sel",
		30),
	GATE_AUDIO1(AUDIO_3RD_DAC_HIRES, "aud_3rd_dac_hires", "audio_h_sel",
		31),
#endif
};

static int clk_mt6785_audio_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(AUDIO_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, audio_clks,
			       ARRAY_SIZE(audio_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}

static const struct of_device_id of_match_clk_mt6785[] = {
	{
		.compatible = "mediatek,audio",
		.data = clk_mt6785_audio_probe,
	},
	/* sentinel */
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6785);

static int clk_mt6785_probe(struct platform_device *pdev)
{
	int (*clk_probe)(struct platform_device *d);
	int r;

	clk_probe = of_device_get_match_data(&pdev->dev);
	if (!clk_probe)
		return -EINVAL;

	r = clk_probe(pdev);
	if (r)
		dev_err(&pdev->dev, "clock provider registration failed: %d\n",
			r);

	return r;
}

static struct platform_driver clk_mt6785_drv = {
	.probe = clk_mt6785_probe,
	.driver = {
		.name = "clk-mt6785",
		.of_match_table = of_match_clk_mt6785,
	},
};
module_platform_driver(clk_mt6785_drv);
