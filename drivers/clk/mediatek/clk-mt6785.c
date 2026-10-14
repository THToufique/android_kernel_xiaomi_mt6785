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
#include "clk-pll.h"
#include "clk-mux.h"

#include "clk-mt6785.h"

// Lock is required by the mux/PLL domains (topckgen, infracfg_ao); those are
// not ported yet, so nothing needs it in this part.

/*
 * Local gate ops missing from the common clock framework.
 * These mirror the 4.14 behaviour exactly: a "dummy" disable is a no-op.
 */
#define GATE_APMIXED(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &apmixed_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,		\
	}
static const struct mtk_gate_regs apmixed_cg_regs = {
	.set_ofs = 0x20,
	.clr_ofs = 0x20,
	.sta_ofs = 0x20,
};
static const struct mtk_gate apmixed_clks[] __initconst = {
	GATE_APMIXED(APMIXED_SSUSB26M, "apmixed_ssusb26m", "f_f26m_ck",
		4),
	GATE_APMIXED(APMIXED_APPLL26M, "apmixed_appll26m", "f_f26m_ck",
		5),
	GATE_APMIXED(APMIXED_MIPIC0_26M, "apmixed_mipic026m", "f_f26m_ck",
		6),
	GATE_APMIXED(APMIXED_MDPLLGP26M, "apmixed_mdpll26m", "f_f26m_ck",
		7),
	GATE_APMIXED(APMIXED_MMSYS_F26M, "apmixed_mmsys26m", "f_f26m_ck",
		8),
	GATE_APMIXED(APMIXED_UFS26M, "apmixed_ufs26m", "f_f26m_ck",
		9),
	GATE_APMIXED(APMIXED_MIPIC1_26M, "apmixed_mipic126m", "f_f26m_ck",
		11),
	GATE_APMIXED(APMIXED_MEMPLL26M, "apmixed_mempll26m", "f_f26m_ck",
		13),
	GATE_APMIXED(APMIXED_CLKSQ_LVPLL_26M, "apmixed_lvpll26m", "f_f26m_ck",
		14),
	GATE_APMIXED(APMIXED_MIPID0_26M, "apmixed_mipid026m", "f_f26m_ck",
		16),
	GATE_APMIXED(APMIXED_MIPID1_26M, "apmixed_mipid126m", "f_f26m_ck",
		17),
};

/*
 * topckgen (cksys) register offsets, from the 4.14 MT6785 clock driver.
 * Plain offsets from the DT reg base; the 4.14 variants that folded in
 * cksys_base + N are not carried over.
 */
#define CK_CFG_0	0x20
#define CK_CFG_0_SET	0x24
#define CK_CFG_0_CLR	0x28
#define CK_CFG_1	0x30
#define CK_CFG_1_SET	0x34
#define CK_CFG_1_CLR	0x38
#define CK_CFG_2	0x40
#define CK_CFG_2_SET	0x44
#define CK_CFG_2_CLR	0x48
#define CK_CFG_3	0x50
#define CK_CFG_3_SET	0x54
#define CK_CFG_3_CLR	0x58
#define CK_CFG_4	0x60
#define CK_CFG_4_SET	0x64
#define CK_CFG_4_CLR	0x68
#define CK_CFG_5	0x70
#define CK_CFG_5_SET	0x74
#define CK_CFG_5_CLR	0x78
#define CK_CFG_6	0x80
#define CK_CFG_6_SET	0x84
#define CK_CFG_6_CLR	0x88
#define CK_CFG_7	0x90
#define CK_CFG_7_SET	0x94
#define CK_CFG_7_CLR	0x98
#define CK_CFG_8	0xA0
#define CK_CFG_8_SET	0xA4
#define CK_CFG_8_CLR	0xA8
#define CK_CFG_9	0xB0
#define CK_CFG_9_SET	0xB4
#define CK_CFG_9_CLR	0xB8
#define CK_CFG_10	0xC0
#define CK_CFG_10_SET	0xC4
#define CK_CFG_10_CLR	0xC8
#define CK_CFG_11	0xD0
#define CK_CFG_11_SET	0xD4
#define CK_CFG_11_CLR	0xD8
#define CK_CFG_12	0xE0
#define CK_CFG_12_SET	0xE4
#define CK_CFG_12_CLR	0xE8
#define CK_CFG_13	0xF0
#define CK_CFG_13_SET	0xF4
#define CK_CFG_13_CLR	0xF8

static const struct mtk_fixed_clk fixed_clks[] __initconst = {
	FIXED_CLK(TOP_CLK26M, "f_f26m_ck", "clk26m", 26000000),
};
static const struct mtk_fixed_factor top_divs[] __initconst = {
	FACTOR(TOP_CLK13M, "clk13m", "clk26m", 1,
		2),
	FACTOR(TOP_F26M_CK_D2, "csw_f26m_ck_d2", "clk26m", 1,
		2),
	FACTOR(TOP_MAINPLL_CK, "mainpll_ck", "mainpll", 1,
		1),
	FACTOR(TOP_MAINPLL_D2, "mainpll_d2", "mainpll_ck", 1,
		2),
	FACTOR(TOP_MAINPLL_D2_D2, "mainpll_d2_d2", "mainpll_d2", 1,
		2),
	FACTOR(TOP_MAINPLL_D2_D4, "mainpll_d2_d4", "mainpll_d2", 1,
		4),
	FACTOR(TOP_MAINPLL_D2_D8, "mainpll_d2_d8", "mainpll_d2", 1,
		8),
	FACTOR(TOP_MAINPLL_D2_D16, "mainpll_d2_d16", "mainpll_d2", 1,
		16),

	FACTOR(TOP_MAINPLL_D3, "mainpll_d3", "mainpll", 1,
		3),
	FACTOR(TOP_MAINPLL_D3_D2, "mainpll_d3_d2", "mainpll_d3", 1,
		2),
	FACTOR(TOP_MAINPLL_D3_D4, "mainpll_d3_d4", "mainpll_d3", 1,
		4),
	FACTOR(TOP_MAINPLL_D3_D8, "mainpll_d3_d8", "mainpll_d3", 1,
		8),

	FACTOR(TOP_MAINPLL_D5, "mainpll_d5", "mainpll", 1,
		5),
	FACTOR(TOP_MAINPLL_D5_D2, "mainpll_d5_d2", "mainpll_d5", 1,
		2),
	FACTOR(TOP_MAINPLL_D5_D4, "mainpll_d5_d4", "mainpll_d5", 1,
		4),

	FACTOR(TOP_MAINPLL_D7, "mainpll_d7", "mainpll", 1,
		7),
	FACTOR(TOP_MAINPLL_D7_D2, "mainpll_d7_d2", "mainpll_d7", 1,
		2),
	FACTOR(TOP_MAINPLL_D7_D4, "mainpll_d7_d4", "mainpll_d7", 1,
		4),

	FACTOR(TOP_UNIVPLL_CK, "univpll", "univ2pll", 1,
		2),
	FACTOR(TOP_UNIVPLL_D2, "univpll_d2", "univpll", 1,
		2),
	FACTOR(TOP_UNIVPLL_D2_D2, "univpll_d2_d2", "univpll_d2", 1,
		2),
	FACTOR(TOP_UNIVPLL_D2_D4, "univpll_d2_d4", "univpll_d2", 1,
		4),
	FACTOR(TOP_UNIVPLL_D2_D8, "univpll_d2_d8", "univpll_d2", 1,
		8),


	FACTOR(TOP_UNIVPLL_D3, "univpll_d3", "univpll", 1,
		3),
	FACTOR(TOP_UNIVPLL_D3_D2, "univpll_d3_d2", "univpll_d3", 1,
		2),
	FACTOR(TOP_UNIVPLL_D3_D4, "univpll_d3_d4", "univpll_d3", 1,
		4),
	FACTOR(TOP_UNIVPLL_D3_D8, "univpll_d3_d8", "univpll_d3", 1,
		8),
	FACTOR(TOP_UNIVPLL_D3_D16, "univpll_d3_d16", "univpll_d3", 1,
		16),

	FACTOR(TOP_UNIVPLL_D5, "univpll_d5", "univpll", 1,
		5),
	FACTOR(TOP_UNIVPLL_D5_D2, "univpll_d5_d2", "univpll_d5", 1,
		2),
	FACTOR(TOP_UNIVPLL_D5_D4, "univpll_d5_d4", "univpll_d5", 1,
		4),
	FACTOR(TOP_UNIVPLL_D5_D8, "univpll_d5_d8", "univpll_d5", 1,
		8),

	FACTOR(TOP_UNIVPLL_D7, "univpll_d7", "univpll", 1,
		7),

	FACTOR(TOP_UNIVP_192M_CK, "univpll_192m_ck", "univ2pll", 1,
		13),
	FACTOR(TOP_UNIVP_192M_D2, "univpll_192m_d2", "univpll_192m_ck", 1,
		2),
	FACTOR(TOP_UNIVP_192M_D4, "univpll_192m_d4", "univpll_192m_ck", 1,
		4),
	FACTOR(TOP_UNIVP_192M_D8, "univpll_192m_d8", "univpll_192m_ck", 1,
		8),
	FACTOR(TOP_UNIVP_192M_D16, "univpll_192m_d16", "univpll_192m_ck", 1,
		16),
	FACTOR(TOP_UNIVP_192M_D32, "univpll_192m_d32", "univpll_192m_ck", 1,
		32),

	FACTOR(TOP_APLL1_CK, "apll1_ck", "apll1", 1,
		1),
	FACTOR(TOP_APLL1_D2, "apll1_d2", "apll1", 1,
		2),
	FACTOR(TOP_APLL1_D4, "apll1_d4", "apll1", 1,
		4),
	FACTOR(TOP_APLL1_D8, "apll1_d8", "apll1", 1,
		8),
	FACTOR(TOP_APLL2_CK, "apll2_ck", "apll2", 1,
		1),
	FACTOR(TOP_APLL2_D2, "apll2_d2", "apll2", 1,
		2),
	FACTOR(TOP_APLL2_D4, "apll2_d4", "apll2", 1,
		4),
	FACTOR(TOP_APLL2_D8, "apll2_d8", "apll2", 1,
		8),
	FACTOR(TOP_TVDPLL_CK, "tvdpll_ck", "tvdpll", 1,
		1),
	FACTOR(TOP_TVDPLL_D2, "tvdpll_d2", "tvdpll_ck", 1,
		2),
	FACTOR(TOP_TVDPLL_D4, "tvdpll_d4", "tvdpll", 1,
		4),
	FACTOR(TOP_TVDPLL_D8, "tvdpll_d8", "tvdpll", 1,
		8),
	FACTOR(TOP_TVDPLL_D16, "tvdpll_d16", "tvdpll", 1,
		16),

	FACTOR(TOP_MMPLL_CK, "mmpll_ck", "mmpll", 1,
		1),
	FACTOR(TOP_MMPLL_D4, "mmpll_d4", "mmpll", 1,
		4),
	FACTOR(TOP_MMPLL_D4_D2, "mmpll_d4_d2", "mmpll_d4", 1,
		2),
	FACTOR(TOP_MMPLL_D5, "mmpll_d5", "mmpll", 1,
		5),
	FACTOR(TOP_MMPLL_D5_D2, "mmpll_d5_d2", "mmpll_d5", 1,
		2),
	FACTOR(TOP_MMPLL_D6, "mmpll_d6", "mmpll", 1,
		6),
	FACTOR(TOP_MMPLL_D7, "mmpll_d7", "mmpll", 1,
		7),

	FACTOR(TOP_MFGPLL_CK, "mfgpll_ck", "mfgpll", 1,
		1),
	FACTOR(TOP_ADSPPLL_CK, "adsppll_ck", "adsppll", 1,
		1),
	FACTOR(TOP_ADSPPLL_D4, "adsppll_d4", "adsppll", 1,
		4),
	FACTOR(TOP_ADSPPLL_D5, "adsppll_d5", "adsppll", 1,
		5),
	FACTOR(TOP_ADSPPLL_D6, "adsppll_d6", "adsppll", 1,
		6),

	FACTOR(TOP_MSDCPLL_CK, "msdcpll_ck", "msdcpll", 1,
		1),
	FACTOR(TOP_MSDCPLL_D2, "msdcpll_d2", "msdcpll", 1,
		2),
	FACTOR(TOP_MSDCPLL_D4, "msdcpll_d4", "msdcpll", 1,
		4),

	FACTOR(TOP_AD_OSC_CK, "ad_osc_ck", "osc", 1,
		1),
	FACTOR(TOP_OSC_D2, "osc_d2", "osc", 1,
		2),
	FACTOR(TOP_OSC_D4, "osc_d4", "osc", 1,
		4),
	FACTOR(TOP_OSC_D8, "osc_d8", "osc", 1,
		8),
	FACTOR(TOP_OSC_D10, "osc_d10", "osc", 1,
		10),
	FACTOR(TOP_OSC_D16, "osc_d16", "osc", 1,
		16),
	FACTOR(TOP_AD_OSC2_CK, "ad_osc2_ck", "osc2", 1,
		1),
	FACTOR(TOP_OSC2_D3, "osc2_d3", "osc2", 1,
		3),

	FACTOR(TOP_TVDPLL_MAINPLL_D2_CK, "tvdpll_mainpll_d2_ck", "tvdpll", 1,
		1),

	FACTOR(TOP_FMEM_466M_CK, "fmem_466m_ck", "fmem", 1,
		1),
	FACTOR(TOP_APUPLL_CK, "apupll_ck", "apupll", 1,
		1),
};
static const char * const axi_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d4",
	"mainpll_d7",
	"osc_d4"
};

static const char * const mm_parents[] __initconst = {
	"clk26m",
	"adsppll_d5",
	"mmpll_d7",
	"mmpll_d5_d2",
	"mainpll_d2_d2",
	"tvdpll_mainpll_d2_ck"
};

static const char * const scp_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d2",
	"mainpll_d2_d4",
	"mainpll_d3",
	"univpll_d3",
	"mainpll_d5",
	"mainpll_d3_d2",
	"osc2_d3"
};

static const char * const img_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mmpll_d6",
	"adsppll_d5",
	"mainpll_d3",
	"mmpll_d5_d2",
	"tvdpll_mainpll_d2_ck",
	"mainpll_d2_d2"
};

static const char * const ipe_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mmpll_d6",
	"adsppll_d5",
	"mainpll_d3",
	"mmpll_d5_d2",
	"tvdpll_mainpll_d2_ck",
	"mainpll_d2_d2"
};

static const char * const dpe_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mmpll_d6",
	"adsppll_d5",
	"mainpll_d3",
	"mmpll_d5_d2",
	"tvdpll_mainpll_d2_ck",
	"mainpll_d2_d2"
};

static const char * const cam_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mmpll_d6",
	"mainpll_d3",
	"mmpll_d7",
	"univpll_d3",
	"mmpll_d5_d2",
	"adsppll_d5",
	"tvdpll_mainpll_d2_ck",
	"univpll_d3_d2"
};

static const char * const ccu_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mmpll_d6",
	"mainpll_d3",
	"mmpll_d7",
	"univpll_d3",
	"mmpll_d5_d2",
	"mainpll_d2_d2",
	"adsppll_d5",
	"univpll_d3_d2"
};

static const char * const dsp_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8",
	"univpll_d3_d4",
	"mainpll_d2_d4",
	"univpll_d3_d2",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3",
	"univpll_d3",
	"mmpll_d7",
	"mmpll_d6",
	"adsppll_d5",
	"tvdpll_ck",
	"univpll_d2",
	"adsppll_d4",
	"apupll_ck"
};

static const char * const dsp1_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8",
	"univpll_d3_d4",
	"mainpll_d2_d4",
	"univpll_d3_d2",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3",
	"univpll_d3",
	"mmpll_d7",
	"mmpll_d6",
	"adsppll_d5",
	"tvdpll_ck",
	"univpll_d2",
	"adsppll_d4",
	"apupll_ck"
};

static const char * const dsp2_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8",
	"univpll_d3_d4",
	"mainpll_d2_d4",
	"univpll_d3_d2",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3",
	"univpll_d3",
	"mmpll_d7",
	"mmpll_d6",
	"adsppll_d5",
	"tvdpll_ck",
	"univpll_d2",
	"adsppll_d4",
	"apupll_ck"
};

static const char * const dsp3_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8",
	"mainpll_d2_d4",
	"univpll_d3_d2",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3",
	"univpll_d3",
	"mmpll_d7",
	"mmpll_d6",
	"mainpll_d2",
	"tvdpll_ck",
	"tvdpll_mainpll_d2_ck",
	"univpll_d2",
	"adsppll_d4",
	"mmpll_d4"
};

static const char * const ipu_if_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8",
	"univpll_d3_d4",
	"mainpll_d2_d4",
	"univpll_d3_d2",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3",
	"univpll_d3",
	"mmpll_d7",
	"mmpll_d6",
	"adsppll_d5",
	"tvdpll_ck",
	"univpll_d2",
	"adsppll_d4",
	"apupll_ck"
};

static const char * const mfg_parents[] __initconst = {
	"clk26m",
	"mfgpll_ck",
	"univpll_d3",
	"mainpll_d5"
};

static const char * const f52m_mfg_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d2",
	"univpll_d3_d4",
	"univpll_d3_d8"
};

static const char * const camtg_parents[] __initconst = {
	"clk26m",
	"univpll_192m_d8",
	"univpll_d3_d8",
	"univpll_192m_d4",
	"univpll_d3_d16",
	"csw_f26m_ck_d2",
	"univpll_192m_d16",
	"univpll_192m_d32"
};

static const char * const camtg2_parents[] __initconst = {
	"clk26m",
	"univpll_192m_d8",
	"univpll_d3_d8",
	"univpll_192m_d4",
	"univpll_d3_d16",
	"csw_f26m_ck_d2",
	"univpll_192m_d16",
	"univpll_192m_d32"
};

static const char * const camtg3_parents[] __initconst = {
	"clk26m",
	"univpll_192m_d8",
	"univpll_d3_d8",
	"univpll_192m_d4",
	"univpll_d3_d16",
	"csw_f26m_ck_d2",
	"univpll_192m_d16",
	"univpll_192m_d32"
};

static const char * const camtg4_parents[] __initconst = {
	"clk26m",
	"univpll_192m_d8",
	"univpll_d3_d8",
	"univpll_192m_d4",
	"univpll_d3_d16",
	"csw_f26m_ck_d2",
	"univpll_192m_d16",
	"univpll_192m_d32"
};

static const char * const camtg5_parents[] __initconst = {
	"clk26m",
	"univpll_192m_d8",
	"univpll_d3_d8",
	"univpll_192m_d4",
	"univpll_d3_d16",
	"csw_f26m_ck_d2",
	"univpll_192m_d16",
	"univpll_192m_d32"
};

static const char * const uart_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d8"
};

static const char * const spi_parents[] __initconst = {
	"clk26m",
	"mainpll_d5_d2",
	"mainpll_d3_d4",
	"msdcpll_d4"
};

static const char * const msdc50_hclk_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d2",
	"mainpll_d3_d2"
};

static const char * const msdc50_0_parents[] __initconst = {
	"clk26m",
	"msdcpll_ck",
	"msdcpll_d2",
	"univpll_d2_d4",
	"mainpll_d3_d2",
	"univpll_d2_d2"
};

static const char * const msdc30_1_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d2",
	"mainpll_d3_d2",
	"mainpll_d7",
	"msdcpll_d2"
};

static const char * const audio_parents[] __initconst = {
	"clk26m",
	"mainpll_d5_d4",
	"mainpll_d7_d4",
	"mainpll_d2_d16"
};

static const char * const aud_intbus_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d4",
	"mainpll_d7_d2"
};


static const char * const fpwrap_ulposc_parents[] __initconst = {
	"osc_d10",
	"clk26m",
	"osc_d4",
	"osc_d8",
	"osc_d16"
};

static const char * const atb_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d2",
	"mainpll_d5"
};

static const char * const sspm_parents[] __initconst = {
	"clk26m",
	"univpll_d2_d4",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"mainpll_d3"
};

static const char * const dpi0_parents[] __initconst = {
	"clk26m",
	"tvdpll_d2",
	"tvdpll_d4",
	"tvdpll_d8",
	"tvdpll_d16"
};

static const char * const scam_parents[] __initconst = {
	"clk26m",
	"mainpll_d5_d2"
};

static const char * const disppwm_parents[] __initconst = {
	"clk26m",
	"univpll_d3_d4",
	"osc_d2",
	"osc_d4",
	"osc_d16"
};

static const char * const usb_top_parents[] __initconst = {
	"clk26m",
	"univpll_d5_d4",
	"univpll_d3_d4",
	"univpll_d5_d2"
};


static const char * const ssusb_top_xhci_parents[] __initconst = {
	"clk26m",
	"univpll_d5_d4",
	"univpll_d3_d4",
	"univpll_d5_d2"
};

static const char * const spm_parents[] __initconst = {
	"clk26m",
	"osc_d8",
	"mainpll_d2_d8"
};

static const char * const i2c_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d8",
	"univpll_d5_d2"
};

static const char * const seninf_parents[] __initconst = {
	"clk26m",
	"univpll_d7",
	"univpll_d3_d2",
	"univpll_d2_d2"
};

static const char * const seninf1_parents[] __initconst = {
	"clk26m",
	"univpll_d7",
	"univpll_d3_d2",
	"univpll_d2_d2"
};

static const char * const seninf2_parents[] __initconst = {
	"clk26m",
	"univpll_d7",
	"univpll_d3_d2",
	"univpll_d2_d2"
};

static const char * const dxcc_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d2",
	"mainpll_d2_d4",
	"mainpll_d2_d8"
};

static const char * const aud_eng1_parents[] __initconst = {
	"clk26m",
	"apll1_d2",
	"apll1_d4",
	"apll1_d8"
};

static const char * const aud_eng2_parents[] __initconst = {
	"clk26m",
	"apll2_d2",
	"apll2_d4",
	"apll2_d8"
};

static const char * const faes_ufsfde_parents[] __initconst = {
	"clk26m",
	"mainpll_d2",
	"mainpll_d2_d2",
	"mainpll_d3",
	"mainpll_d2_d4",
	"univpll_d3"
};

static const char * const fufs_parents[] __initconst = {
	"clk26m",
	"mainpll_d2_d4",
	"mainpll_d2_d8",
	"mainpll_d2_d16"
};

static const char * const aud_1_parents[] __initconst = {
	"clk26m",
	"apll1_ck"
};

static const char * const aud_2_parents[] __initconst = {
	"clk26m",
	"apll2_ck"
};

static const char * const adsp_parents[] __initconst = {
	"clk26m",
	"mainpll_d3",
	"univpll_d2_d4",
	"univpll_d2",
	"mmpll_d4",
	"adsppll_d4",
	"adsppll_d6"
};

static const char * const dpmaif_parents[] __initconst = {
	"clk26m",
	"univpll_d2_d4",
	"mainpll_d3",
	"mainpll_d2_d2",
	"univpll_d2_d2",
	"univpll_d3"
};

static const char * const venc_parents[] __initconst = {
	"clk26m",
	"mmpll_d7",
	"mainpll_d3",
	"univpll_d2_d2",
	"mainpll_d2_d2",
	"univpll_d3",
	"mmpll_d6",
	"mainpll_d5",
	"mainpll_d3_d2",
	"mmpll_d4_d2",
	"univpll_d2_d4",
	"mmpll_d5",
	"univpll_192m_d2"

};

static const char * const vdec_parents[] __initconst = {
	"clk26m",
	"univpll_d2_d4",
	"mainpll_d3",
	"univpll_d2_d2",
	"mainpll_d2_d2",
	"univpll_d3",
	"univpll_d5",
	"univpll_d5_d2",
	"mainpll_d2",
	"univpll_d2",
	"univpll_192m_d2"
};

static const char * const camtm_parents[] __initconst = {
	"clk26m",
	"univpll_d7",
	"univpll_d3_d2",
	"univpll_d2_d2"
};

static const char * const pwm_parents[] __initconst = {
	"clk26m",
	"univpll_d2_d8"
};

static const char * const audio_h_parents[] __initconst = {
	"clk26m",
	"univpll_d7",
	"apll1_ck",
	"apll2_ck"
};



static const char * const i2s0_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};

static const char * const i2s1_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};

static const char * const i2s2_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};

static const char * const i2s3_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};

static const char * const i2s4_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};

static const char * const i2s5_m_ck_parents[] __initconst = {
	"aud_1_sel",
	"aud_2_sel"
};
static const struct mtk_mux top_muxes[] __initconst = {
	/* CLK_CFG_0 */
	MUX_GATE_CLR_SET_UPD_FLAGS(TOP_MUX_AXI, "axi_sel", axi_parents,
		CK_CFG_0, CK_CFG_0_SET, CK_CFG_0_CLR,
		0, 2, 7,
		0x004, 0, CLK_IS_CRITICAL),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_MM, "mm_sel", mm_parents,
		CK_CFG_0, CK_CFG_0_SET, CK_CFG_0_CLR,
		8, 3, 15,
		0x004, 1),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SCP, "scp_sel", scp_parents,
		CK_CFG_0, CK_CFG_0_SET, CK_CFG_0_CLR,
		16, 3, 23,
		0x004, 2),

	/* CLK_CFG_1 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_IMG, "img_sel", img_parents,
		CK_CFG_1, CK_CFG_1_SET, CK_CFG_1_CLR,
		0, 3, 7,
		0x004, 4),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAM, "cam_sel", cam_parents,
		CK_CFG_1, CK_CFG_1_SET, CK_CFG_1_CLR,
		24, 4, 31,
		0x004, 7),

	/* CLK_CFG_2 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_DSP, "dsp_sel", dsp_parents,
		CK_CFG_2, CK_CFG_2_SET, CK_CFG_2_CLR,
		8, 4, 15,
		0x004, 9),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_DSP1, "dsp1_sel", dsp1_parents,
		CK_CFG_2, CK_CFG_2_SET, CK_CFG_2_CLR,
		16, 4, 23,
		0x004, 10),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_DSP2, "dsp2_sel", dsp2_parents,
		CK_CFG_2, CK_CFG_2_SET, CK_CFG_2_CLR,
		24, 4, 31,
		0x004, 11),

	/* CLK_CFG_3 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_IPU_IF, "ipu_if_sel", ipu_if_parents,
		CK_CFG_3, CK_CFG_3_SET, CK_CFG_3_CLR,
		8, 4, 15,
		0x004, 13),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_MFG, "mfg_sel", mfg_parents,
		CK_CFG_3, CK_CFG_3_SET, CK_CFG_3_CLR,
		16, 2, 23,
		0x004, 14),

	/* CLK_CFG_4 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTG, "camtg_sel", camtg_parents,
		CK_CFG_4, CK_CFG_4_SET, CK_CFG_4_CLR,
		0, 3, 7,
		0x004, 16),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTG2, "camtg2_sel", camtg2_parents,
		CK_CFG_4, CK_CFG_4_SET, CK_CFG_4_CLR,
		8, 3, 15,
		0x004, 17),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTG3, "camtg3_sel", camtg3_parents,
		CK_CFG_4, CK_CFG_4_SET, CK_CFG_4_CLR,
		16, 3, 23,
		0x004, 18),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTG4, "camtg4_sel", camtg4_parents,
		CK_CFG_4, CK_CFG_4_SET, CK_CFG_4_CLR,
		24, 3, 31,
		0x004, 19),

	/* CLK_CFG_5 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_UART, "uart_sel", uart_parents,
		CK_CFG_5, CK_CFG_5_SET, CK_CFG_5_CLR,
		0, 1, 7,
		0x004, 20),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SPI, "spi_sel", spi_parents,
		CK_CFG_5, CK_CFG_5_SET, CK_CFG_5_CLR,
		8, 2, 15,
		0x004, 21),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_MSDC50_0_HCLK, "msdc50_hclk_sel",
		msdc50_hclk_parents,
		CK_CFG_5, CK_CFG_5_SET, CK_CFG_5_CLR,
		16, 2, 23,
		0x004, 22),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_MSDC50_0, "msdc50_0_sel",
		msdc50_0_parents,
		CK_CFG_5, CK_CFG_5_SET, CK_CFG_5_CLR,
		24, 3, 31,
		0x004, 23),

	/* CLK_CFG_6 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_MSDC30_1, "msdc30_1_sel",
		msdc30_1_parents,
		CK_CFG_6, CK_CFG_6_SET, CK_CFG_6_CLR,
		0, 3, 7,
		0x004, 24),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUDIO, "audio_sel", audio_parents,
		CK_CFG_6, CK_CFG_6_SET, CK_CFG_6_CLR,
		8, 2, 15,
		0x004, 25),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUD_INTBUS, "aud_intbus_sel",
		aud_intbus_parents,
		CK_CFG_6, CK_CFG_6_SET, CK_CFG_6_CLR,
		16, 2, 23,
		0x004, 26),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_FPWRAP_ULPOSC, "fpwrap_ulposc_sel",
		fpwrap_ulposc_parents,
		CK_CFG_6, CK_CFG_6_SET, CK_CFG_6_CLR,
		24, 3, 31,
		0x004, 27),

	/* CLK_CFG_7 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_ATB, "atb_sel", atb_parents,
		CK_CFG_7, CK_CFG_7_SET, CK_CFG_7_CLR,
		0, 2, 7,
		0x004, 28),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SSPM, "sspm_sel", sspm_parents,
		CK_CFG_7, CK_CFG_7_SET, CK_CFG_7_CLR,
		8, 3, 15,
		0x004, 29),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_DPI0, "dpi0_sel", dpi0_parents,
		CK_CFG_7, CK_CFG_7_SET, CK_CFG_7_CLR,
		16, 3, 23,
		0x004, 30),

	/* CLK_CFG_8 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_DISP_PWM, "disppwm_sel", disppwm_parents,
		CK_CFG_8, CK_CFG_8_SET, CK_CFG_8_CLR,
		0, 3, 7,
		0x008, 1),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_USB_TOP, "usb_top_sel", usb_top_parents,
		CK_CFG_8, CK_CFG_8_SET, CK_CFG_8_CLR,
		8, 2, 15,
		0x008, 2),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SSUSB_TOP_XHCI, "ssusb_top_xhci_sel",
		ssusb_top_xhci_parents,
		CK_CFG_8, CK_CFG_8_SET, CK_CFG_8_CLR,
		16, 2, 23,
		0x008, 3),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SPM, "spm_sel", spm_parents,
		CK_CFG_8, CK_CFG_8_SET, CK_CFG_8_CLR,
		24, 2, 31,
		0x008, 4),

	/* CLK_CFG_9 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_I2C, "i2c_sel", i2c_parents,
		CK_CFG_9, CK_CFG_9_SET, CK_CFG_9_CLR,
		0, 2, 7,
		0x008, 5),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SENINF, "seninf_sel", seninf_parents,
		CK_CFG_9, CK_CFG_9_SET, CK_CFG_9_CLR,
		8, 2, 15,
		0x008, 6),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SENINF1, "seninf1_sel", seninf1_parents,
		CK_CFG_9, CK_CFG_9_SET, CK_CFG_9_CLR,
		16, 2, 23,
		0x008, 7),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_SENINF2, "seninf2_sel", seninf2_parents,
		CK_CFG_9, CK_CFG_9_SET, CK_CFG_9_CLR,
		24, 2, 31,
		0x008, 8),

	/* CLK_CFG_10 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_DXCC, "dxcc_sel", dxcc_parents,
		CK_CFG_10, CK_CFG_10_SET, CK_CFG_10_CLR,
		0, 2, 7,
		0x008, 9),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUD_ENG1, "aud_eng1_sel",
		aud_eng1_parents,
		CK_CFG_10, CK_CFG_10_SET, CK_CFG_10_CLR,
		8, 2, 15,
		0x008, 10),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUD_ENG2, "aud_eng2_sel",
		aud_eng2_parents,
		CK_CFG_10, CK_CFG_10_SET, CK_CFG_10_CLR,
		16, 2, 23,
		0x008, 11),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_FAES_UFSFDE, "faes_ufsfde_sel",
		faes_ufsfde_parents,
		CK_CFG_10, CK_CFG_10_SET, CK_CFG_10_CLR,
		24, 3, 31,
		0x008, 12),

	/* CLK_CFG_11 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_FUFS, "fufs_sel", fufs_parents,
		CK_CFG_11, CK_CFG_11_SET, CK_CFG_11_CLR,
		0, 2, 7,
		0x008, 13),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUD_1, "aud_1_sel", aud_1_parents,
		CK_CFG_11, CK_CFG_11_SET, CK_CFG_11_CLR,
		8, 1, 15,
		0x008, 14),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUD_2, "aud_2_sel", aud_2_parents,
		CK_CFG_11, CK_CFG_11_SET, CK_CFG_11_CLR,
		16, 1, 23,
		0x008, 15),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_ADSP, "adsp_sel", adsp_parents,
		CK_CFG_11, CK_CFG_11_SET, CK_CFG_11_CLR,
		24, 3, 31,
		0x008, 16),

	/* CLK_CFG_12 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_DPMAIF, "dpmaif_sel", dpmaif_parents,
		CK_CFG_12, CK_CFG_12_SET, CK_CFG_12_CLR,
		0, 3, 7,
		0x008, 17),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_VENC, "venc_sel", venc_parents,
		CK_CFG_12, CK_CFG_12_SET, CK_CFG_12_CLR,
		8, 4, 15,
		0x008, 18),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_VDEC, "vdec_sel", vdec_parents,
		CK_CFG_12, CK_CFG_12_SET, CK_CFG_12_CLR,
		16, 4, 23,
		0x008, 19),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTM, "camtm_sel", camtm_parents,
		CK_CFG_12, CK_CFG_12_SET, CK_CFG_12_CLR,
		24, 2, 31,
		0x004, 20),

	/* CLK_CFG_13 */
	MUX_GATE_CLR_SET_UPD(TOP_MUX_PWM, "pwm_sel", pwm_parents,
		CK_CFG_13, CK_CFG_13_SET, CK_CFG_13_CLR,
		0, 1, 7,
		0x008, 21),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_AUDIO_H, "audio_h_sel", audio_h_parents,
		CK_CFG_13, CK_CFG_13_SET, CK_CFG_13_CLR,
		8, 2, 15,
		0x008, 22),

	MUX_GATE_CLR_SET_UPD(TOP_MUX_CAMTG5, "camtg5_sel", camtg5_parents,
		CK_CFG_13, CK_CFG_13_SET, CK_CFG_13_CLR,
		24, 3, 31,
		0x008, 24),
};
static const struct mtk_composite top_audmuxes[] __initconst = {

	MUX(TOP_I2S0_M_SEL, "i2s0_m_ck_sel", i2s0_m_ck_parents, 0x320, 8, 1),
	MUX(TOP_I2S1_M_SEL, "i2s1_m_ck_sel", i2s1_m_ck_parents, 0x320, 9, 1),
	MUX(TOP_I2S2_M_SEL, "i2s2_m_ck_sel", i2s2_m_ck_parents, 0x320, 10, 1),
	MUX(TOP_I2S3_M_SEL, "i2s3_m_ck_sel", i2s3_m_ck_parents, 0x320, 11, 1),
	MUX(TOP_I2S4_M_SEL, "i2s4_m_ck_sel", i2s4_m_ck_parents, 0x320, 12, 1),
	MUX(TOP_I2S5_M_SEL, "i2s5_m_ck_sel", i2s5_m_ck_parents, 0x328, 20, 1),

	DIV_GATE(TOP_APLL12_DIV0, "apll12_div0", "i2s0_m_ck_sel",
		0x320, 2, 0x324, 8, 0),
	DIV_GATE(TOP_APLL12_DIV1, "apll12_div1", "i2s1_m_ck_sel",
		0x320, 3, 0x324, 8, 8),
	DIV_GATE(TOP_APLL12_DIV2, "apll12_div2", "i2s2_m_ck_sel",
		0x320, 4, 0x324, 8, 16),
	DIV_GATE(TOP_APLL12_DIV3, "apll12_div3", "i2s3_m_ck_sel",
		0x320, 5, 0x324, 8, 24),

	DIV_GATE(TOP_APLL12_DIV4, "apll12_div4", "i2s4_m_ck_sel",
		0x320, 6, 0x328, 8, 0),
	DIV_GATE(TOP_APLL12_DIVB, "apll12_divb", "apll12_div4",
		0x320, 7, 0x328, 8, 8),
	DIV_GATE(TOP_APLL12_DIV5, "apll12_div5", "i2s5_m_ck_sel",
		0x328, 16, 0x328, 4, 28),
};
static DEFINE_SPINLOCK(mt6785_clk_lock);

static int clk_mt6785_topckgen_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	void __iomem *base;
	int r;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	clk_data = mtk_alloc_clk_data(TOP_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_fixed_clks(fixed_clks, ARRAY_SIZE(fixed_clks),
				    clk_data);
	mtk_clk_register_factors(top_divs, ARRAY_SIZE(top_divs), clk_data);
	mtk_clk_register_muxes(&pdev->dev, top_muxes, ARRAY_SIZE(top_muxes),
			       node, &mt6785_clk_lock, clk_data);
	mtk_clk_register_composites(&pdev->dev, top_audmuxes,
				    ARRAY_SIZE(top_audmuxes), base,
				    &mt6785_clk_lock, clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}

/* PLL tuning range, from the 4.14 MT6785 clock driver. */
#define MT6785_PLL_FMAX		(3800UL * MHZ)
#define MT6785_PLL_FMIN		(1500UL * MHZ)
#define MT6785_INTEGER_BITS		8

#define PLL_B(_id, _name, _reg, _pwr_reg, _en_mask, _flags,	\
			_rst_bar_mask, _pcwbits, _pd_reg, _pd_shift,	\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift, _div_table) {		\
		.id = _id,						\
		.name = _name,						\
		.reg = _reg,						\
		.pwr_reg = _pwr_reg,					\
		.en_mask = _en_mask,					\
		.flags = _flags,					\
		.rst_bar_mask = _rst_bar_mask,				\
		.fmax = MT6785_PLL_FMAX,				\
		.fmin = MT6785_PLL_FMIN,				\
		.pcwbits = _pcwbits,					\
		.pcwibits = MT6785_INTEGER_BITS,			\
		.pd_reg = _pd_reg,					\
		.pd_shift = _pd_shift,					\
		.tuner_reg = _tuner_reg,				\
		.tuner_en_reg = _tuner_en_reg,				\
		.tuner_en_bit = _tuner_en_bit,				\
		.pcw_reg = _pcw_reg,					\
		.pcw_shift = _pcw_shift,				\
		.div_table = _div_table,				\
	}
#define PLL(_id, _name, _reg, _pwr_reg, _en_mask,			\
			_flags, _rst_bar_mask,				\
			_pcwbits,					\
			_pd_reg, _pd_shift,				\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift)				\
		PLL_B(_id, _name, _reg, _pwr_reg, _en_mask,		\
			_flags,	_rst_bar_mask,				\
			_pcwbits,					\
			_pd_reg, _pd_shift,				\
			_tuner_reg, _tuner_en_reg, _tuner_en_bit,	\
			_pcw_reg, _pcw_shift, NULL)
static const struct mtk_pll_data plls[] = {
	/* FIXME: need to fix flags/div_table/tuner_reg/table */
	PLL(APMIXED_MAINPLL, "mainpll", 0x0230, 0x023C, BIT(0),
		(HAVE_RST_BAR | PLL_AO), BIT(24),
		22,
		0x0234, 24,
		0, 0, 0,
		0x0234, 0),

	PLL(APMIXED_UNIV2PLL, "univ2pll", 0x0240, 0x024C, BIT(0),
		(HAVE_RST_BAR), BIT(24),
		22,
		0x0244, 24,
		0, 0, 0,
		0x0244, 0),

	PLL(APMIXED_MFGPLL, "mfgpll", 0x0250, 0x025C, BIT(0),
		0, 0,
		22,
		0x0254, 24,
		0, 0, 0,
		0x0254, 0),

	PLL(APMIXED_MSDCPLL, "msdcpll", 0x0260, 0x026C, BIT(0),
		0, 0,
		22,
		0x0264, 24,
		0, 0, 0,
		0x0264, 0),

	PLL(APMIXED_TVDPLL, "tvdpll", 0x0270, 0x027C, BIT(0),
		0, 0,
		22,
		0x0274, 24,
		0, 0, 0,
		0x0274, 0),

	PLL(APMIXED_APUPLL, "apupll", 0x02EC, 0x02F8, BIT(0),
		0, 0,
		22,
		0x02F0, 24,
		0, 0, 0,
		0x02F0, 0),

	PLL(APMIXED_ADSPPLL, "adsppll", 0x02b0, 0x02bC, BIT(0),
		(HAVE_RST_BAR), BIT(23),
		22,
		0x02b4, 24,
		0, 0, 0,
		0x02b4, 0),

	PLL(APMIXED_MMPLL, "mmpll", 0x0280, 0x028C, BIT(0),
		(HAVE_RST_BAR), BIT(23),
		22,
		0x0284, 24,
		0, 0, 0,
		0x0284, 0),

	PLL(APMIXED_APLL1, "apll1", 0x02C0, 0x02D0, BIT(0),
		0, 0,
		32,
		0x02C0, 1,
		0, 0x14, 0,
		0x02C4, 0),

	PLL(APMIXED_APLL2, "apll2", 0x02D4, 0x02E4, BIT(0),
		0, 0,
		32,
		0x02D4, 1,
		0, 0x14, 1,
		0x02D8, 0),
};
static int clk_mt6785_apmixed_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(APMIXED_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_plls(node, plls, ARRAY_SIZE(plls), clk_data);
	mtk_clk_register_gates(&pdev->dev, node, apmixed_clks,
			       ARRAY_SIZE(apmixed_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}

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

#define GATE_CAM_CHECK(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cam_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}
static const struct mtk_gate_regs cam_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};
#define GATE_CAM_DUMMY(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cam_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

static const struct mtk_gate cam_clks[] __initconst = {
	GATE_CAM_DUMMY(CAMSYS_LARB6_CGPDN, "camsys_larb6", "cam_sel", 0),
	GATE_CAM_DUMMY(CAMSYS_LARB7_CGPDN, "camsys_larb7", "cam_sel", 2),
	GATE_CAM_DUMMY(CAMSYS_GALS_CGPDN, "camsys_gals", "cam_sel", 4),
	GATE_CAM_CHECK(CAMSYS_CAM_CGPDN, "camsys_cam", "cam_sel", 6),
	GATE_CAM_CHECK(CAMSYS_CAMTG_CGPDN, "camsys_camtg", "cam_sel", 7),
	GATE_CAM_CHECK(CAMSYS_SENINF_CGPDN, "camsys_seninf", "cam_sel", 8),
	GATE_CAM_CHECK(CAMSYS_CAMSV0_CGPDN, "camsys_camsv0", "cam_sel", 9),
	GATE_CAM_CHECK(CAMSYS_CAMSV1_CGPDN, "camsys_camsv1", "cam_sel", 10),
	GATE_CAM_CHECK(CAMSYS_CCU_CGPDN, "camsys_ccu", "cam_sel", 12),
	GATE_CAM_CHECK(CAMSYS_FAKE_ENG_CGPDN, "camsys_fake_eng", "cam_sel", 13),
};

#define GATE_IMG(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &img_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,\
	}
static const struct mtk_gate_regs img_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate img_clks[] __initconst = {
	GATE_IMG(IMG_LARB5, "imgsys_larb5", "img_sel", 0),
	GATE_IMG(IMG_LARB4, "imgsys_larb4", "img_sel", 1),
	GATE_IMG(IMG_DIP, "imgsys_dip", "img_sel", 2),
	GATE_IMG(IMG_FDVT, "imgsys_fdvt", "img_sel", 3),
	GATE_IMG(IMG_DPE, "imgsys_dpe", "img_sel", 4),
	GATE_IMG(IMG_RSC, "imgsys_rsc", "img_sel", 5),
	GATE_IMG(IMG_MFB, "imgsys_mfb", "img_sel", 6),
	GATE_IMG(IMG_WPE_A, "imgsys_wpe_a", "img_sel", 7),
	GATE_IMG(IMG_WPE_B, "imgsys_wpe_b", "img_sel", 8),
	GATE_IMG(IMG_OWE, "imgsys_owe", "img_sel", 9),
};

#define GATE_MFG_CFG(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &mfg_cfg_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}
static const struct mtk_gate_regs mfg_cfg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate mfg_cfg_clks[] __initconst = {
	GATE_MFG_CFG(MFGCFG_BG3D, "mfg_cfg_bg3d", "mfg_sel", 0)
};

#define GATE_MM0(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm0_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}
static const struct mtk_gate_regs mm0_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};
#define GATE_MM0_DUMMY(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm0_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}
#define GATE_MM1(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm1_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}
static const struct mtk_gate_regs mm1_cg_regs = {
	.set_ofs = 0x114,
	.clr_ofs = 0x118,
	.sta_ofs = 0x110,
};

static const struct mtk_gate mm_clks[] __initconst = {
	/* MM0 */
	GATE_MM0_DUMMY(MMSYS_SMI_COMMON, "mm_smi_common", "mm_sel", 0),
	GATE_MM0_DUMMY(MMSYS_SMI_LARB0, "mm_smi_larb0", "mm_sel", 1),
	GATE_MM0_DUMMY(MMSYS_SMI_LARB1, "mm_smi_larb1", "mm_sel", 2),
	GATE_MM0_DUMMY(MMSYS_GALS_COMM0, "mm_gals_comm0", "mm_sel", 3),
	GATE_MM0_DUMMY(MMSYS_GALS_COMM1, "mm_gals_comm1", "mm_sel", 4),

	GATE_MM0_DUMMY(MMSYS_GALS_CCU2MM, "mm_gals_ccu2mm", "mm_sel", 5),
	GATE_MM0_DUMMY(MMSYS_GALS_IPU12MM, "mm_gals_ipu12mm", "mm_sel", 6),
	GATE_MM0_DUMMY(MMSYS_GALS_IMG2MM, "mm_gals_img2mm", "mm_sel", 7),


	GATE_MM0_DUMMY(MMSYS_GALS_CAM2MM, "mm_gals_cam2mm", "mm_sel", 8),
	GATE_MM0_DUMMY(MMSYS_GALS_IPU2MM, "mm_gals_ipu2mm", "mm_sel", 9),
	GATE_MM0(MMSYS_MDP_DL_TXCK, "mm_mdp_dl_txck", "mm_sel", 10),
	GATE_MM0(MMSYS_IPU_DL_TXCK, "mm_ipu_dl_txck", "mm_sel", 11),
	GATE_MM0(MMSYS_MDP_RDMA0, "mm_mdp_rdma0", "mm_sel", 12),
	GATE_MM0(MMSYS_MDP_RDMA1, "mm_mdp_rdma1", "mm_sel", 13),
	GATE_MM0(MMSYS_MDP_RSZ0, "mm_mdp_rsz0", "mm_sel", 14),
	GATE_MM0(MMSYS_MDP_RSZ1, "mm_mdp_rsz1", "mm_sel", 15),
	GATE_MM0(MMSYS_MDP_TDSHP, "mm_mdp_tdshp", "mm_sel", 16),
	GATE_MM0(MMSYS_MDP_WROT0, "mm_mdp_wrot0", "mm_sel", 17),
	GATE_MM0(MMSYS_MDP_WROT1, "mm_mdp_wrot1", "mm_sel", 18),

	GATE_MM0(MMSYS_FAKE_ENG, "mm_fake_eng", "mm_sel", 19),
	GATE_MM0(MMSYS_DISP_OVL0, "mm_disp_ovl0", "mm_sel", 20),
	GATE_MM0(MMSYS_DISP_OVL0_2L, "mm_disp_ovl0_2l", "mm_sel", 21),
	GATE_MM0(MMSYS_DISP_OVL1_2L, "mm_disp_ovl1_2l", "mm_sel", 22),
	GATE_MM0(MMSYS_DISP_RDMA0, "mm_disp_rdma0", "mm_sel", 23),
	GATE_MM0(MMSYS_DISP_RDMA1, "mm_disp_rdma1", "mm_sel", 24),
	GATE_MM0(MMSYS_DISP_WDMA0, "mm_disp_wdma0", "mm_sel", 25),
	GATE_MM0(MMSYS_DISP_COLOR0, "mm_disp_color0", "mm_sel", 26),
	GATE_MM0(MMSYS_DISP_CCORR0, "mm_disp_ccorr0", "mm_sel", 27),
	GATE_MM0(MMSYS_DISP_AAL0, "mm_disp_aal0", "mm_sel", 28),
	GATE_MM0(MMSYS_DISP_GAMMA0, "mm_disp_gamma0", "mm_sel", 29),
	GATE_MM0(MMSYS_DISP_DITHER0, "mm_disp_dither0", "mm_sel", 30),
	GATE_MM0(MMSYS_DISP_SPLIT, "mm_disp_split", "mm_sel", 31),
	/* MM1 */
	GATE_MM1(MMSYS_DSI0_MM_CK, "mm_dsi0_mmck", "mm_sel", 0),
	GATE_MM1(MMSYS_DSI0_IF_CK, "mm_dsi0_ifck", "mm_sel", 1),
	GATE_MM1(MMSYS_DPI_MM_CK, "mm_dpi_mmck", "mm_sel", 2),
	GATE_MM1(MMSYS_DPI_IF_CK, "mm_dpi_ifck", "dpi0_sel", 3),
	GATE_MM1(MMSYS_FAKE_ENG2, "mm_fake_eng2", "mm_sel", 4),
	GATE_MM1(MMSYS_MDP_DL_RX_CK, "mm_mdp_dl_rxck", "mm_sel", 5),
	GATE_MM1(MMSYS_IPU_DL_RX_CK, "mm_ipu_dl_rxck", "mm_sel", 6),
	GATE_MM1(MMSYS_26M, "mm_26m", "f_f26m_ck", 7),
	GATE_MM1(MMSYS_MMSYS_R2Y, "mm_mmsys_r2y", "mm_sel", 8),
	GATE_MM1(MMSYS_DISP_RSZ, "mm_disp_rsz", "mm_sel", 9),
	GATE_MM1(MMSYS_MDP_AAL, "mm_mdp_aal", "mm_sel", 10),
	GATE_MM1(MMSYS_MDP_HDR, "mm_mdp_hdr", "mm_sel", 11),
	GATE_MM1(MMSYS_DBI_MM_CK, "mm_dbi_mmck", "mm_sel", 12),
	GATE_MM1(MMSYS_DBI_IF_CK, "mm_dbi_ifck", "dpi0_sel", 13),

	GATE_MM1(MMSYS_DISP_POSTMASK0, "mm_disp_pm0", "mm_sel", 14),
	GATE_MM1(MMSYS_DISP_HRT_BW, "mm_disp_hrt_bw", "mm_sel", 15),
	GATE_MM1(MMSYS_DISP_OVL_FBDC, "mm_disp_ovl_fbdc", "mm_sel", 16),
};

#define GATE_VDEC0_I(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vdec0_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}
static const struct mtk_gate_regs vdec0_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x4,
	.sta_ofs = 0x0,
};
#define GATE_VDEC1_I(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vdec1_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}
static const struct mtk_gate_regs vdec1_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xc,
	.sta_ofs = 0x8,
};

static const struct mtk_gate vdec_clks[] __initconst = {
	/* VDEC0 */
	GATE_VDEC0_I(VDEC_VDEC, "vdec_cken", "vdec_sel", 0),
	/* VDEC1 */
	GATE_VDEC1_I(VDEC_LARB1, "vdec_larb1_cken", "vdec_sel", 0),
};

#define GATE_VENC_GLOBAL_CON_DUMMY(_id, _name, _parent, _shift) {	\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.regs = &venc_global_con_cg_regs,		\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}
static const struct mtk_gate_regs venc_global_con_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate venc_global_con_clks[] __initconst = {
	GATE_VENC_GLOBAL_CON_DUMMY(VENC_GCON_LARB, "venc_larb",
		"venc_sel", 0),
	GATE_VENC_GLOBAL_CON_DUMMY(VENC_GCON_VENC, "venc_venc",
		"venc_sel", 4),
	GATE_VENC_GLOBAL_CON_DUMMY(VENC_GCON_JPGENC, "venc_jpgenc",
		"venc_sel", 8),
	GATE_VENC_GLOBAL_CON_DUMMY(VENC_GCON_GALS, "venc_gals",
		"venc_sel", 28),
};

#define GATE_APU_CONN_DUMMY(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &apu_conn_cg_regs,	\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}
static const struct mtk_gate_regs apu_conn_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate apu_conn_clks[] __initconst = {
	GATE_APU_CONN_DUMMY(APU_CONN_APU_CG, "apu_conn_apu", "dsp1_sel", 0),
	GATE_APU_CONN_DUMMY(APU_CONN_AHB_CG, "apu_conn_ahb", "dsp_sel", 1),
	GATE_APU_CONN_DUMMY(APU_CONN_AXI_CG, "apu_conn_axi", "dsp_sel", 2),
	GATE_APU_CONN_DUMMY(APU_CONN_ISP_CG, "apu_conn_isp", "dsp_sel", 3),
	GATE_APU_CONN_DUMMY(APU_CONN_CAM_ADL_CG, "apu_conn_cam_adl",
		"dsp_sel", 4),
	GATE_APU_CONN_DUMMY(APU_CONN_IMG_ADL_CG, "apu_conn_img_adl",
		"dsp_sel", 5),
	GATE_APU_CONN_DUMMY(APU_CONN_EMI_26M_CG, "apu_conn_emi_26m",
		"dsp_sel", 6),
	GATE_APU_CONN_DUMMY(APU_CONN_VPU_UDI_CG, "apu_conn_vpu_udi",
		"dsp_sel", 7),
};

#define GATE_APU0(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &apu0_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,\
	}
static const struct mtk_gate_regs apu0_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate apu0_clks[] __initconst = {
	GATE_APU0(APU0_APU_CG, "apu0_apu", "dsp1_sel", 0),
	GATE_APU0(APU0_AXI_M_CG, "apu0_axi", "dsp1_sel", 1),
	GATE_APU0(APU0_JTAG_CG, "apu0_jtag", "dsp1_sel", 2),
};

#define GATE_APU1(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &apu1_cg_regs,		\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,\
	}
static const struct mtk_gate_regs apu1_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate apu1_clks[] __initconst = {
	GATE_APU1(APU1_APU_CG, "apu1_apu", "dsp2_sel", 0),
	GATE_APU1(APU1_AXI_M_CG, "apu1_axi", "dsp2_sel", 1),
	GATE_APU1(APU1_JTAG_CG, "apu1_jtag", "dsp2_sel", 2),
};

#define GATE_APU_VCORE(_id, _name, _parent, _shift) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &apu_vcore_cg_regs,	\
		.shift = _shift,		\
		.ops = &mtk_clk_gate_ops_setclr,\
	}
static const struct mtk_gate_regs apu_vcore_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate apu_vcore_clks[] __initconst = {
	GATE_APU_VCORE(APU_VCORE_AHB_CG, "apu_vcore_ahb", "ipu_if_sel", 0),
	GATE_APU_VCORE(APU_VCORE_AXI_CG, "apu_vcore_axi", "ipu_if_sel", 1),
	GATE_APU_VCORE(APU_VCORE_ADL_CG, "apu_vcore_adl", "ipu_if_sel", 2),
	GATE_APU_VCORE(APU_VCORE_QOS_CG, "apu_vcore_qos", "ipu_if_sel", 3),
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


static int clk_mt6785_camsys_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(CAMSYS_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, cam_clks,
			       ARRAY_SIZE(cam_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_imgsys_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(IMG_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, img_clks,
			       ARRAY_SIZE(img_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_mfg_cfg_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(MFGCFG_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, mfg_cfg_clks,
			       ARRAY_SIZE(mfg_cfg_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_mmsys_config_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(MMSYS_CONFIG_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, mm_clks,
			       ARRAY_SIZE(mm_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_vdec_gcon_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(VDEC_GCON_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, vdec_clks,
			       ARRAY_SIZE(vdec_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_venc_gcon_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(VENC_GCON_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, venc_global_con_clks,
			       ARRAY_SIZE(venc_global_con_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_apu_conn_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(APU_CONN_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, apu_conn_clks,
			       ARRAY_SIZE(apu_conn_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_apu0_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(APU0_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, apu0_clks,
			       ARRAY_SIZE(apu0_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_apu1_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(APU1_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, apu1_clks,
			       ARRAY_SIZE(apu1_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}


static int clk_mt6785_apu_vcore_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct clk_hw_onecell_data *clk_data;
	int r;

	clk_data = mtk_alloc_clk_data(APU_VCORE_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	mtk_clk_register_gates(&pdev->dev, node, apu_vcore_clks,
			       ARRAY_SIZE(apu_vcore_clks), clk_data);

	r = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (r)
		dev_err(&pdev->dev,
			"could not register clock provider: %d\n", r);

	return r;
}

static int clk_mt6785_probe(struct platform_device *pdev)
{
	int (*clk_probe)(struct platform_device *d);
	int r;

	clk_probe = of_device_get_match_data(&pdev->dev);
	if (!clk_probe)
		return -EINVAL;
	r = clk_probe(pdev);
	if (r)
		dev_err(&pdev->dev, "clock provider registration failed: %d\n", r);
	return r;
}

static const struct of_device_id of_match_clk_mt6785[] = {
	{
		.compatible = "mediatek,topckgen",
		.data = clk_mt6785_topckgen_probe,
	},
	{
		.compatible = "mediatek,apmixed",
		.data = clk_mt6785_apmixed_probe,
	},
	{
		.compatible = "mediatek,audio",
		.data = clk_mt6785_audio_probe,
	},
	{
		.compatible = "mediatek,camsys",
		.data = clk_mt6785_camsys_probe,
	},
	{
		.compatible = "mediatek,imgsys",
		.data = clk_mt6785_imgsys_probe,
	},
	{
		.compatible = "mediatek,mfgcfg",
		.data = clk_mt6785_mfg_cfg_probe,
	},
	{
		.compatible = "mediatek,mmsys_config",
		.data = clk_mt6785_mmsys_config_probe,
	},
	{
		.compatible = "mediatek,vdec_gcon",
		.data = clk_mt6785_vdec_gcon_probe,
	},
	{
		.compatible = "mediatek,venc_gcon",
		.data = clk_mt6785_venc_gcon_probe,
	},
	{
		.compatible = "mediatek,apu_conn",
		.data = clk_mt6785_apu_conn_probe,
	},
	{
		.compatible = "mediatek,apu0",
		.data = clk_mt6785_apu0_probe,
	},
	{
		.compatible = "mediatek,apu1",
		.data = clk_mt6785_apu1_probe,
	},
	{
		.compatible = "mediatek,apu_vcore",
		.data = clk_mt6785_apu_vcore_probe,
	},
	/* sentinel */
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6785);

static struct platform_driver clk_mt6785_drv = {
	.probe = clk_mt6785_probe,
	.driver = {
		.name = "clk-mt6785",
		.of_match_table = of_match_clk_mt6785,
	},
};
module_platform_driver(clk_mt6785_drv);
