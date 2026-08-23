/* SPDX-License-Identifier: ((GPL-2.0+ WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * xil-isp-lite.h
 *
 * Xil ISP Lite driver - user space header file.
 *
 * Copyright © 2022- bxinquan Ltd.
 *
 * Author: Xinquan Bian (544177215@qq.com)
 *
 */

#ifndef __XIL_ISP_LITE_H_
#define __XIL_ISP_LITE_H_

#include <linux/v4l2-controls.h>

#define V4L2_CID_USER_XIL_ISP_LITE_BASE			(V4L2_CID_USER_BASE + 0x10e0)

#define V4L2_CID_USER_XIL_ISP_LITE_ALL		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 0)
#define V4L2_CID_USER_XIL_ISP_LITE_CONFIG	(V4L2_CID_USER_XIL_ISP_LITE_BASE + 1)
#define V4L2_CID_USER_XIL_ISP_LITE_DPC		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 2)
#define V4L2_CID_USER_XIL_ISP_LITE_BLC		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 3)
#define V4L2_CID_USER_XIL_ISP_LITE_AE		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 4)
#define V4L2_CID_USER_XIL_ISP_LITE_DGAIN	(V4L2_CID_USER_XIL_ISP_LITE_BASE + 5)
#define V4L2_CID_USER_XIL_ISP_LITE_LSC		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 6)
#define V4L2_CID_USER_XIL_ISP_LITE_AWB		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 7)
#define V4L2_CID_USER_XIL_ISP_LITE_WB		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 8)
#define V4L2_CID_USER_XIL_ISP_LITE_CFA		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 9)
#define V4L2_CID_USER_XIL_ISP_LITE_CCM		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 10)
#define V4L2_CID_USER_XIL_ISP_LITE_CSC		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 11)
#define V4L2_CID_USER_XIL_ISP_LITE_LDCI		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 12)
#define V4L2_CID_USER_XIL_ISP_LITE_SHARP	(V4L2_CID_USER_XIL_ISP_LITE_BASE + 13)
#define V4L2_CID_USER_XIL_ISP_LITE_BNR		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 14)
#define V4L2_CID_USER_XIL_ISP_LITE_2DNR		(V4L2_CID_USER_XIL_ISP_LITE_BASE + 15)

/*
 * XIL-ISP-LITE IP Modules
 *
 *        |------------- RAW -------------|   |------- RGB -------| |--------- YUV ---------|
 * RAW ==> DPC => BLC => BNR => DGAIN => Demosaic => WB => CCM => CSC => Gamma => 2DNR => EE ==> YUV
 *                                    |           |
 *                                    V           V
 *                                 STAT_AE    STAT_AWB
 * */


/* ISP Configure structure define */

struct xil_isp_lite_top {
	__u32 dpc_en		:1;
	__u32 blc_en		:1;
	__u32 bnr_en		:1;
	__u32 dgain_en		:1;
	__u32 demosaic_en	:1;
	__u32 wb_en		:1;
	__u32 ccm_en		:1;
	__u32 csc_en		:1;
	__u32 gamma_en		:1;
	__u32 nr2d_en		:1;
	__u32 ee_en		:1;
	__u32 stat_ae_en	:1;
	__u32 stat_awb_en	:1;
	__u32 padding		:19; /* Unused */
};

struct xil_isp_lite_dpc {
	__u32 enabled;
	__u16 threshold;
	__u16 padding;
};

struct xil_isp_lite_blc {
	__u32 enabled;
	__u16 black_level_r;
	__u16 black_level_gr;
	__u16 black_level_gb;
	__u16 black_level_b;
};

struct xil_isp_lite_bnr {
	__u32 enabled;
	__u32 nr_level   :4;
	__u32 padding    :28;
};

struct xil_isp_lite_dgain {
	__u32 enabled;
	__u8  gain;  //U4.4
	__u16 offset;
	__u8  padding;
};

struct xil_isp_lite_demosaic {
	__u32 enabled;
};

struct xil_isp_lite_wb {
	__u32 enabled;
	__u8  rgain; //U4.4
	__u8  ggain;
	__u8  bgain;
	__u8  padding;
};

struct xil_isp_lite_ccm {
	__u32 enabled;
	__s8  matrix[3*3];
	__u8  padding[3];
};

struct xil_isp_lite_csc {
	__u32 enabled;
};

struct xil_isp_lite_gamma {
	__u32 enabled;
	__u8  gamma_table[64];
};

struct xil_isp_lite_2dnr {
	__u32 enabled;
	__u8  space_weight[7*7];
	__u8  color_curve[9][2];
	__u8  padding;
};

struct xil_isp_lite_ee {
	__u32 enabled;
};

struct xil_isp_lite_stat_ae_cfg {
	__u32 enabled;
	__u16 rect_x;
	__u16 rect_y;
	__u16 rect_w;
	__u16 rect_h;
};

struct xil_isp_lite_stat_awb_cfg {
	__u32 enabled;
	__u16 thresh_min;
	__u16 thresh_max;
};


/* ISP statistics buffer define */

#define V4L2_META_FMT_XIL_ISP_LITE_STAT		v4l2_fourcc('X', 'I', 'S', 'P')

/*
 * Versioned, fixed-size frame statistics ABI.
 *
 * The current RTL does not expose histograms or channel sums. It exposes the
 * AE decision/skewness, the gains computed by AWB, and the effective digital
 * gain index. Keep this record flat and append-only so a future libcamera IPA
 * backend can consume it without depending on the private register layout.
 */
#define XIL_ISP_LITE_STAT_ABI_VERSION		1U

#define XIL_ISP_LITE_STAT_FLAG_AE_VALID		(1U << 0)
#define XIL_ISP_LITE_STAT_FLAG_AWB_VALID	(1U << 1)
#define XIL_ISP_LITE_STAT_FLAG_DGAIN_VALID	(1U << 2)

/* Values produced by isp_ae.v. */
#define XIL_ISP_LITE_AE_RESPONSE_NORMAL		0U
#define XIL_ISP_LITE_AE_RESPONSE_OVEREXPOSED	1U
#define XIL_ISP_LITE_AE_RESPONSE_HOLD		2U
#define XIL_ISP_LITE_AE_RESPONSE_UNDEREXPOSED	3U

struct xil_isp_lite_stat_result {
	__u32 abi_version;
	__u32 record_size;
	__u32 frame_sequence;
	__u32 irq_status;
	__u64 timestamp_ns;
	__u32 flags;
	__u32 ae_response;
	__u32 ae_skewness;
	__u32 ae_done;
	__u32 awb_r_gain;
	__u32 awb_b_gain;
	__u32 dgain_index;
	__u32 wb_r_gain;
	__u32 wb_b_gain;
	__u32 dropped_frames;
	__u32 reserved[8];
};

#endif /* __XIL_ISP_LITE_H_ */
