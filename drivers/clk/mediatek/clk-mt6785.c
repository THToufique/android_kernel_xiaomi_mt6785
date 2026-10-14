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
