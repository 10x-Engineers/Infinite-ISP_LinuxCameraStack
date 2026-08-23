// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Xil ISP Lite IP
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/v4l2-subdev.h>
#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>
#include <media/v4l2-ctrls.h>
#include <linux/xil-isp-lite.h>
#include <linux/isp_init.h>
#include "infinite_isp_register.h"
#include <linux/debugfs.h>
#include "xil-isp-irq.h"

#define ISP_REG_INT_STATUS_BIT_FRAME_START  (1<<0)
#define ISP_REG_INT_STATUS_BIT_FRAME_DONE   (1<<1)
#define ISP_REG_INT_STATUS_BIT_AE_DONE      (1<<2)
#define ISP_REG_INT_STATUS_BIT_AWB_DONE     (1<<3)

#define ISP_REG_INT_MASK_BIT_FRAME_START    (1<<0)
#define ISP_REG_INT_MASK_BIT_FRAME_DONE     (1<<1)
#define ISP_REG_INT_MASK_BIT_AE_DONE        (1<<2)
#define ISP_REG_INT_MASK_BIT_AWB_DONE       (1<<3)

////////////////////////////////////////////////////////////////////////////

#define ISP_PAD_SINK		0
#define ISP_PAD_SOURCE		1
#define ISP_PAD_SOURCE_2	2
//#define ISP_MEDIA_PADS		2
#define ISP_MEDIA_PADS		3

#define ISP_DRIVER_NAME		"xil-isp-lite"
#define ISP_BUS_NAME		"platform:" ISP_DRIVER_NAME
#define ISP_STAT_DEV_NAME	ISP_DRIVER_NAME "_stat"


struct isp_stat_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list_node;
};

struct isp_stat_node {
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex vlock; /* ioctl serialization mutex */
	struct media_pad pad;
	spinlock_t lock; /* locks the buffers list 'stat' */
	struct list_head buf_list;
	bool streaming;
	u32 dropped_frames;
};

struct isp_state {
	struct v4l2_subdev subdev;
	struct v4l2_mbus_framefmt pad_format[ISP_MEDIA_PADS];
	struct device *dev;
	struct clk_bulk_data *clks;
	void  *isp_base;
	void  *luts_base;
	u32 bits;
	/* used to protect access to this struct */
	struct mutex lock;
	struct media_pad pads[ISP_MEDIA_PADS];
	bool streaming;
	u32 frame_sequence;
	u32 int_status;

	struct v4l2_ctrl_handler config_ctrls;
	struct isp_stat_node    stat_node;

    struct dentry *debug_dir;
    u32 debug_reg_offset;
	u8 is_lut;
    spinlock_t reg_lock;

	bool irq_source_enabled;
	bool irq_dispatcher_owner;
};

#define DEFINE_ISP_GET_FUNC(module, reg_name) \
static int isp_get_##module(struct isp_state *isp, struct REG_##reg_name *reg) \
{ \
    INFINITE_ISP_READ_MODULE_REGs(isp->isp_base, module, (void*)reg); \
    return 0; \
}
#define DEFINE_ISP_SET_FUNC(module, reg_name) \
static int isp_set_##module(struct isp_state *isp, const struct REG_##reg_name *reg) \
{ \
    INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, module, (void*)reg); \
    return 0; \
}

#define DEFINE_ISP_FUNC(module, reg_name) \
	DEFINE_ISP_GET_FUNC(module, reg_name) \
	DEFINE_ISP_SET_FUNC(module, reg_name)

DEFINE_ISP_FUNC(config, CONFIG)
DEFINE_ISP_FUNC(dpc, DPC)
DEFINE_ISP_FUNC(blc, BLC)
DEFINE_ISP_FUNC(ae, AE)
DEFINE_ISP_FUNC(dgain, DGAIN)
DEFINE_ISP_FUNC(lsc, LSC)
DEFINE_ISP_FUNC(awb, AWB)
DEFINE_ISP_FUNC(wb, WB)
DEFINE_ISP_FUNC(cfa, CFA)
DEFINE_ISP_FUNC(ccm, CCM)
DEFINE_ISP_FUNC(csc, CSC)
DEFINE_ISP_FUNC(ldci, LDCI)
DEFINE_ISP_FUNC(sharp, SHARP)
DEFINE_ISP_FUNC(bnr, BNR)
DEFINE_ISP_FUNC(_2dnr, 2DNR)


static inline struct isp_state *to_ispstate(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct isp_state, subdev);
}

/////////////////////////////////////////////////////////////////////////////
// ISP config ctrls

struct xil_isp_reg {
    unsigned int reg;
    unsigned int val;
};

static int isp_config_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
    struct isp_state *isp = container_of(ctrl->handler, 
                                      struct isp_state, 
                                      config_ctrls);
	union REG_ISP_TOP_EN top_en;
    int ret = 0;

    switch (ctrl->id) {
	case V4L2_CID_AUTO_WHITE_BALANCE:
		top_en.TOP_EN_val = INFINITE_ISP_READ_REG(isp->isp_base, config,
						       TOP_EN);
		ctrl->val = top_en.TOP_EN_AWB_EN;
		break;
	case V4L2_CID_AUTOGAIN:
		ctrl->val = !INFINITE_ISP_READ_REG(isp->isp_base, dgain,
						     dgain_isManual);
		break;
	case V4L2_CID_RED_BALANCE:
		ctrl->val = INFINITE_ISP_READ_REG(isp->isp_base, wb, WB_RGAIN);
		break;
	case V4L2_CID_BLUE_BALANCE:
		ctrl->val = INFINITE_ISP_READ_REG(isp->isp_base, wb, WB_BGAIN);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ctrl->val = INFINITE_ISP_READ_REG(isp->isp_base, dgain,
						     dgain_index_out);
		break;
	case V4L2_CID_USER_XIL_ISP_LITE_ALL:
		memcpy_fromio(ctrl->p_new.p_u8, isp->isp_base,
			      sizeof(struct REG_Infinite_ISP));
		break;
    case V4L2_CID_USER_XIL_ISP_LITE_CONFIG:
        ret = isp_get_config(isp, (struct REG_CONFIG *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_DPC:
        ret = isp_get_dpc(isp, (struct REG_DPC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_BLC:
        ret = isp_get_blc(isp, (struct REG_BLC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_AE:
        ret = isp_get_ae(isp, (struct REG_AE *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_DGAIN:
        ret = isp_get_dgain(isp, (struct REG_DGAIN *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_LSC:
        ret = isp_get_lsc(isp, (struct REG_LSC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_AWB:
        ret = isp_get_awb(isp, (struct REG_AWB *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_WB:
        ret = isp_get_wb(isp, (struct REG_WB *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CFA:
        ret = isp_get_cfa(isp, (struct REG_CFA *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CCM:
        ret = isp_get_ccm(isp, (struct REG_CCM *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CSC:
        ret = isp_get_csc(isp, (struct REG_CSC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_LDCI:
        ret = isp_get_ldci(isp, (struct REG_LDCI *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_SHARP:
        ret = isp_get_sharp(isp, (struct REG_SHARP *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_BNR:
        ret = isp_get_bnr(isp, (struct REG_BNR *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_2DNR:
        ret = isp_get__2dnr(isp, (struct REG_2DNR *)ctrl->p_new.p_u8);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static int isp_config_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct isp_state *isp = container_of(ctrl->handler, 
                                      struct isp_state, 
                                      config_ctrls);
	union REG_ISP_TOP_EN top_en;
    int ret = 0;

    switch (ctrl->id) {
	case V4L2_CID_AUTO_WHITE_BALANCE:
		top_en.TOP_EN_val = INFINITE_ISP_READ_REG(isp->isp_base, config,
						       TOP_EN);
		top_en.TOP_EN_AWB_EN = !!ctrl->val;
		INFINITE_ISP_WRITE_REG(isp->isp_base, config, TOP_EN,
				       top_en.TOP_EN_val);
		break;
	case V4L2_CID_AUTOGAIN:
		INFINITE_ISP_WRITE_REG(isp->isp_base, dgain, dgain_isManual,
				       !ctrl->val);
		break;
	case V4L2_CID_RED_BALANCE:
		INFINITE_ISP_WRITE_REG(isp->isp_base, wb, WB_RGAIN, ctrl->val);
		break;
	case V4L2_CID_BLUE_BALANCE:
		INFINITE_ISP_WRITE_REG(isp->isp_base, wb, WB_BGAIN, ctrl->val);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		INFINITE_ISP_WRITE_REG(isp->isp_base, dgain, dgain_isManual, 1);
		INFINITE_ISP_WRITE_REG(isp->isp_base, dgain, dgain_man_index,
				       ctrl->val);
		break;
    case V4L2_CID_USER_XIL_ISP_LITE_CONFIG:
        ret = isp_set_config(isp, (const struct REG_CONFIG *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_DPC:
        ret = isp_set_dpc(isp, (const struct REG_DPC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_BLC:
        ret = isp_set_blc(isp, (const struct REG_BLC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_AE:
        ret = isp_set_ae(isp, (const struct REG_AE *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_DGAIN:
        ret = isp_set_dgain(isp, (const struct REG_DGAIN *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_LSC:
        ret = isp_set_lsc(isp, (const struct REG_LSC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_AWB:
        ret = isp_set_awb(isp, (const struct REG_AWB *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_WB:
        ret = isp_set_wb(isp, (const struct REG_WB *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CFA:
        ret = isp_set_cfa(isp, (const struct REG_CFA *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CCM:
        ret = isp_set_ccm(isp, (const struct REG_CCM *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_CSC:
        ret = isp_set_csc(isp, (const struct REG_CSC *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_LDCI:
        ret = isp_set_ldci(isp, (const struct REG_LDCI *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_SHARP:
        ret = isp_set_sharp(isp, (const struct REG_SHARP *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_BNR:
        ret = isp_set_bnr(isp, (const struct REG_BNR *)ctrl->p_new.p_u8);
        break;
    case V4L2_CID_USER_XIL_ISP_LITE_2DNR:
        ret = isp_set__2dnr(isp, (const struct REG_2DNR *)ctrl->p_new.p_u8);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}


static const struct v4l2_ctrl_ops isp_config_ctrl_ops = {
	.g_volatile_ctrl = isp_config_g_volatile_ctrl,
	.s_ctrl = isp_config_s_ctrl,
};

struct isp_config_custom_ctrl {
	const char *name;
	u32 id;
	u32 size;
	u32 flags;
};

static const struct isp_config_custom_ctrl custom_ctrls[] = {
	{
		.name	= "ISP Full Register Control",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_ALL,
		.size	= sizeof(struct REG_Infinite_ISP),
		.flags	= V4L2_CTRL_FLAG_READ_ONLY
	}, {
		.name	= "ISP Configuration Control",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_CONFIG,
		.size	= sizeof(struct REG_CONFIG),
		.flags	= 0
	}, {
		.name	= "DPC - Defective Pixel Correction",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_DPC,
		.size	= sizeof(struct REG_DPC),
		.flags	= 0
	}, {
		.name	= "BLC - Black Level Correction",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_BLC,
		.size	= sizeof(struct REG_BLC),
		.flags	= 0
	}, {
		.name	= "AE - Auto Exposure",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_AE,
		.size	= sizeof(struct REG_AE),
		.flags	= 0
	}, {
		.name	= "DGAIN - Digital Gain",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_DGAIN,
		.size	= sizeof(struct REG_DGAIN),
		.flags	= 0
	}, {
		.name	= "LSC - Lens Shading Correction",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_LSC,
		.size	= sizeof(struct REG_LSC),
		.flags	= 0
	}, {
		.name	= "AWB - Auto White Balance",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_AWB,
		.size	= sizeof(struct REG_AWB),
		.flags	= 0
	}, {
		.name	= "WB - White Balance",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_WB,
		.size	= sizeof(struct REG_WB),
		.flags	= 0
	}, {
		.name	= "CFA - Color Filter Array",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_CFA,
		.size	= sizeof(struct REG_CFA),
		.flags	= 0
	}, {
		.name	= "CCM - Color Correction Matrix",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_CCM,
		.size	= sizeof(struct REG_CCM),
		.flags	= 0
	}, {
		.name	= "CSC - Color Space Conversion",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_CSC,
		.size	= sizeof(struct REG_CSC),
		.flags	= 0
	}, {
		.name	= "LDCI - Local Dynamic Contrast Improvement",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_LDCI,
		.size	= sizeof(struct REG_LDCI),
		.flags	= 0
	}, {
		.name	= "Sharpness Control",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_SHARP,
		.size	= sizeof(struct REG_SHARP),
		.flags	= 0
	}, {
		.name	= "BNR - Bayer Noise Reduction",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_BNR,
		.size	= sizeof(struct REG_BNR),
		.flags	= 0
	}, {
		.name	= "2DNR - 2D Noise Reduction",
		.id	= V4L2_CID_USER_XIL_ISP_LITE_2DNR,
		.size	= sizeof(struct REG_2DNR),
		.flags	= 0
	},
};

static int isp_config_init_ctrl_handler(struct isp_state *isp)
{
	struct v4l2_ctrl_handler *ctrl_handler = &isp->config_ctrls;
	struct v4l2_ctrl *ctrl;
	int ret;
	unsigned int i;

	/* Use this ctrl template to assign custom ISP ctrls. */
	struct v4l2_ctrl_config ctrl_template = {
		.ops		= &isp_config_ctrl_ops,
		.type		= V4L2_CTRL_TYPE_U8,
		.def		= 0,
		.min		= 0x00,
		.max		= 0xff,
		.step		= 1,
	};

	/* Standard tuning controls plus the legacy register-block controls. */
	ret = v4l2_ctrl_handler_init(ctrl_handler,
				     ARRAY_SIZE(custom_ctrls) + 5);
	if (ret) {
		dev_err(isp->dev, "ctrl_handler init failed (%d)", ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(custom_ctrls); i++) {
		ctrl_template.name = custom_ctrls[i].name;
		ctrl_template.id = custom_ctrls[i].id;
		ctrl_template.dims[0] = custom_ctrls[i].size;
		ctrl_template.flags = custom_ctrls[i].flags |
			V4L2_CTRL_FLAG_VOLATILE;
		if (!(custom_ctrls[i].flags & V4L2_CTRL_FLAG_READ_ONLY))
			ctrl_template.flags |= V4L2_CTRL_FLAG_EXECUTE_ON_WRITE;
		v4l2_ctrl_new_custom(ctrl_handler, &ctrl_template, NULL);
	}

#define ISP_NEW_STD_CTRL(_id, _min, _max, _step, _def) do { \
	ctrl = v4l2_ctrl_new_std(ctrl_handler, &isp_config_ctrl_ops, (_id), \
				 (_min), (_max), (_step), (_def)); \
	if (ctrl) \
		ctrl->flags |= V4L2_CTRL_FLAG_VOLATILE | \
			       V4L2_CTRL_FLAG_EXECUTE_ON_WRITE; \
} while (0)

	ISP_NEW_STD_CTRL(V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, !!AWB_EN);
	ISP_NEW_STD_CTRL(V4L2_CID_AUTOGAIN, 0, 1, 1, !DGAIN_isManual);
	ISP_NEW_STD_CTRL(V4L2_CID_RED_BALANCE, 0, 4095, 1, r_gain);
	ISP_NEW_STD_CTRL(V4L2_CID_BLUE_BALANCE, 0, 4095, 1, b_gain);
	/* The hardware exposes a 100-entry digital-gain LUT by index. */
	ISP_NEW_STD_CTRL(V4L2_CID_DIGITAL_GAIN, 0, 99, 1, current_gain);

#undef ISP_NEW_STD_CTRL

	if (ctrl_handler->error) {
		ret = ctrl_handler->error;
		dev_err(isp->dev, "controls init failed (%d)", ret);
		v4l2_ctrl_handler_free(ctrl_handler);
		return ret;
	}

	return 0;
}

/////////////////////////////////////////////////////////////////////////////
// ISP statistics meta node

#define ISP_STAT_REQ_BUFS_MIN 2
#define ISP_STAT_REQ_BUFS_MAX 8

static int isp_stat_enum_fmt_meta_cap(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct video_device *video = video_devdata(file);
	//struct isp_stat_node *node = video_get_drvdata(video);

	if (f->index > 0 || f->type != video->queue->type)
		return -EINVAL;

	f->pixelformat = V4L2_META_FMT_XIL_ISP_LITE_STAT;
	return 0;
}

static int isp_stat_g_fmt_meta_cap(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct video_device *video = video_devdata(file);
	//struct isp_stat_node *node = video_get_drvdata(video);
	struct v4l2_meta_format *meta = &f->fmt.meta;

	if (f->type != video->queue->type)
		return -EINVAL;

	memset(meta, 0, sizeof(*meta));
	meta->dataformat = V4L2_META_FMT_XIL_ISP_LITE_STAT;
	meta->buffersize = sizeof(struct xil_isp_lite_stat_result);

	return 0;
}

static int isp_stat_querycap(struct file *file,
				 void *priv, struct v4l2_capability *cap)
{
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, ISP_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	strscpy(cap->bus_info, ISP_BUS_NAME, sizeof(cap->bus_info));

	return 0;
}

/* ISP video device IOCTLs */
static const struct v4l2_ioctl_ops isp_stat_ioctl = {
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_enum_fmt_meta_cap = isp_stat_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = isp_stat_g_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = isp_stat_g_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = isp_stat_g_fmt_meta_cap,
	.vidioc_querycap = isp_stat_querycap,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations isp_stat_fops = {
	.owner = THIS_MODULE,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.open = v4l2_fh_open,
	.release = vb2_fop_release
};

static int isp_stat_vb2_queue_setup(struct vb2_queue *vq,
					unsigned int *num_buffers,
					unsigned int *num_planes,
					unsigned int sizes[],
					struct device *alloc_devs[])
{
	*num_planes = 1;

	*num_buffers = clamp_t(u32, *num_buffers, ISP_STAT_REQ_BUFS_MIN, ISP_STAT_REQ_BUFS_MAX);

	sizes[0] = sizeof(struct xil_isp_lite_stat_result);

	return 0;
}

static void isp_stat_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct isp_stat_buffer *buffer =
		container_of(vbuf, struct isp_stat_buffer, vb);
	struct vb2_queue *vq = vb->vb2_queue;
	struct isp_stat_node *node = vq->drv_priv;


	spin_lock_irq(&node->lock);
	list_add_tail(&buffer->list_node, &node->buf_list);
	spin_unlock_irq(&node->lock);
}

static int isp_stat_vb2_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct xil_isp_lite_stat_result))
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, sizeof(struct xil_isp_lite_stat_result));

	return 0;
}

static int isp_stat_vb2_start_streaming(struct vb2_queue *vq,
					unsigned int count)
{
	struct isp_stat_node *node = vq->drv_priv;
	struct isp_state *isp = container_of(node, struct isp_state, stat_node);
	unsigned long flags;

	/* Keep the interrupt source quiet while resetting the frame state. */
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_MASK, ~0U);
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_STATUS, 0);

	spin_lock_irqsave(&node->lock, flags);
	node->streaming = true;
	node->dropped_frames = 0;
	isp->frame_sequence = 0;
	isp->int_status = 0;
	spin_unlock_irqrestore(&node->lock, flags);
	if (xil_isp_irq_enable(XIL_ISP_IRQ_SOURCE_ISP)) {
		spin_lock_irqsave(&node->lock, flags);
		node->streaming = false;
		spin_unlock_irqrestore(&node->lock, flags);
		return -ENODEV;
	}
	isp->irq_source_enabled = true;

	/* Mask bits are active-high. AWB_DONE is not connected in this RTL. */
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_MASK,
		~(ISP_REG_INT_MASK_BIT_FRAME_START |
		  ISP_REG_INT_MASK_BIT_FRAME_DONE  |
		  ISP_REG_INT_MASK_BIT_AE_DONE));

	dev_info(isp->dev, "ISP statistics streaming started (%u buffers)",
		 count);
	return 0;
}

static void isp_stat_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct isp_stat_node *node = vq->drv_priv;
	struct isp_state *isp = container_of(node, struct isp_state, stat_node);
	struct isp_stat_buffer *buffer;
	unsigned long flags;

	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_MASK, ~0U);

	spin_lock_irqsave(&node->lock, flags);
	node->streaming = false;
	spin_unlock_irqrestore(&node->lock, flags);

	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_STATUS, 0);
	if (isp->irq_source_enabled) {
		xil_isp_irq_disable(XIL_ISP_IRQ_SOURCE_ISP);
		isp->irq_source_enabled = false;
	}
	isp->int_status = 0;

	for (;;) {
		spin_lock_irqsave(&node->lock, flags);
		if (list_empty(&node->buf_list)) {
			spin_unlock_irqrestore(&node->lock, flags);
			break;
		}
		buffer = list_first_entry(&node->buf_list, struct isp_stat_buffer, list_node);
		list_del(&buffer->list_node);
		spin_unlock_irqrestore(&node->lock, flags);
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}

	dev_info(isp->dev, "ISP statistics streaming stopped");
}

static const struct vb2_ops isp_stat_vb2_ops = {
	.queue_setup = isp_stat_vb2_queue_setup,
	.buf_queue = isp_stat_vb2_buf_queue,
	.buf_prepare = isp_stat_vb2_buf_prepare,
	.start_streaming = isp_stat_vb2_start_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.stop_streaming = isp_stat_vb2_stop_streaming,
};

static int isp_stat_init_vb2_queue(struct isp_stat_node *node)
{
	struct vb2_queue *q = &node->queue;
	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	q->drv_priv = node;
	q->ops = &isp_stat_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct isp_stat_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &node->vlock;

	return vb2_queue_init(q);
}

static void isp_stat_send_measurement(struct isp_stat_node *node, u32 irq_status)
{
	struct isp_state *isp = container_of(node, struct isp_state, stat_node);
	struct isp_stat_buffer *buffer = NULL;
	unsigned int frame_sequence = isp->frame_sequence;
	u32 dropped_frames;
	u64 timestamp = ktime_get_ns();
	unsigned long flags;
	struct xil_isp_lite_stat_result *stat_result;
	union REG_ISP_TOP_EN top_en;

	spin_lock_irqsave(&node->lock, flags);
	if (!node->streaming) {
		spin_unlock_irqrestore(&node->lock, flags);
		return;
	}

	/* get one empty buffer */
	if (!list_empty(&node->buf_list)) {
		buffer = list_first_entry(&node->buf_list, struct isp_stat_buffer, list_node);
		list_del(&buffer->list_node);
	} else {
		node->dropped_frames++;
	}
	dropped_frames = node->dropped_frames;
	spin_unlock_irqrestore(&node->lock, flags);

	if (!buffer)
		return;

	stat_result = vb2_plane_vaddr(&buffer->vb.vb2_buf, 0);
	if (!stat_result) {
		vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		return;
	}

	memset(stat_result, 0, sizeof(*stat_result));
	stat_result->abi_version = XIL_ISP_LITE_STAT_ABI_VERSION;
	stat_result->record_size = sizeof(*stat_result);
	stat_result->frame_sequence = frame_sequence;
	stat_result->irq_status = irq_status;
	stat_result->timestamp_ns = timestamp;
	/*
	 * ae_response is a one-pixel-clock pulse and is almost always zero by
	 * the time the frame IRQ runs. Export the latched decision instead.
	 */
	stat_result->ae_response = INFINITE_ISP_READ_REG(isp->isp_base, ae,
							 ae_response_debug);
	stat_result->ae_skewness = INFINITE_ISP_READ_REG(isp->isp_base, ae,
							 ae_result_skewness);
	stat_result->ae_done = INFINITE_ISP_READ_REG(isp->isp_base, ae, ae_done);
	stat_result->awb_r_gain = INFINITE_ISP_READ_REG(isp->isp_base, awb,
							FINAL_RGAIN);
	stat_result->awb_b_gain = INFINITE_ISP_READ_REG(isp->isp_base, awb,
							FINAL_BGAIN);
	stat_result->dgain_index = INFINITE_ISP_READ_REG(isp->isp_base, dgain,
							 dgain_index_out);
	stat_result->wb_r_gain = INFINITE_ISP_READ_REG(isp->isp_base, wb, WB_RGAIN);
	stat_result->wb_b_gain = INFINITE_ISP_READ_REG(isp->isp_base, wb, WB_BGAIN);
	stat_result->dropped_frames = dropped_frames;
	top_en.TOP_EN_val = INFINITE_ISP_READ_REG(isp->isp_base, config, TOP_EN);
	stat_result->flags = 0;
	if (top_en.TOP_EN_AE_EN)
		stat_result->flags |= XIL_ISP_LITE_STAT_FLAG_AE_VALID;
	if (top_en.TOP_EN_AWB_EN &&
	    (stat_result->awb_r_gain || stat_result->awb_b_gain))
		stat_result->flags |= XIL_ISP_LITE_STAT_FLAG_AWB_VALID;
	if (top_en.TOP_EN_DGAIN_EN)
		stat_result->flags |= XIL_ISP_LITE_STAT_FLAG_DGAIN_VALID;

	vb2_set_plane_payload(&buffer->vb.vb2_buf, 0, sizeof(*stat_result));
	buffer->vb.sequence = frame_sequence;
	buffer->vb.vb2_buf.timestamp = timestamp;
	dev_dbg_ratelimited(isp->dev,
		"stat frame=%u ae=%u skew=%u awb=%u/%u dgain=%u dropped=%u",
		frame_sequence, stat_result->ae_response, stat_result->ae_skewness,
		stat_result->awb_r_gain, stat_result->awb_b_gain,
		stat_result->dgain_index, dropped_frames);
	vb2_buffer_done(&buffer->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

static int isp_stat_node_register(struct isp_state *isp, struct v4l2_device *v4l2_dev)
{
	struct isp_stat_node *node = &isp->stat_node;
	struct video_device *vdev = &node->vdev;
	int ret;

	mutex_init(&node->vlock);
	INIT_LIST_HEAD(&node->buf_list);
	spin_lock_init(&node->lock);

	//XXX
	strscpy(vdev->name, ISP_STAT_DEV_NAME, sizeof(vdev->name));

	video_set_drvdata(vdev, node);
	vdev->ioctl_ops = &isp_stat_ioctl;
	vdev->fops = &isp_stat_fops;
	vdev->release = video_device_release_empty;
	vdev->lock = &node->vlock;
	vdev->v4l2_dev = v4l2_dev; //XXX MUST BE SET
	vdev->ctrl_handler = &isp->config_ctrls; //XXX
	vdev->queue = &node->queue;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->vfl_dir =  VFL_DIR_RX;
	ret = isp_stat_init_vb2_queue(node);
	if (ret)
		goto err_mutex_destroy;
	video_set_drvdata(vdev, node);

	node->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &node->pad);
	if (ret)
		goto err_release_queue;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&vdev->dev,
			"failed to register %s, ret=%d\n", vdev->name, ret);
		goto err_cleanup_media_entity;
	}

	return 0;

err_cleanup_media_entity:
	media_entity_cleanup(&vdev->entity);
err_release_queue:
	vb2_queue_release(&node->queue);
err_mutex_destroy:
	mutex_destroy(&node->vlock);
	return ret;
}

static void isp_stat_node_unregister(struct isp_state *isp)
{
	struct isp_stat_node *node = &isp->stat_node;
	struct video_device *vdev = &node->vdev;

	vb2_video_unregister_device(vdev);
	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&node->vlock);
}

/////////////////////////////////////////////////////////////////////////////
// isp subdev

static int isp_log_status(struct v4l2_subdev *sd)
{
	struct isp_state *isp = to_ispstate(sd);
	struct device *dev = isp->dev;
	u32 width, height, bits, bayer;

	mutex_lock(&isp->lock);

	width  = INFINITE_ISP_READ_REG(isp->isp_base, config, SNS_WIDTH);
	height = INFINITE_ISP_READ_REG(isp->isp_base, config, SNS_HEIGHT);
	bits   = INFINITE_ISP_READ_REG(isp->isp_base, config, BITS);
	bayer  = INFINITE_ISP_READ_REG(isp->isp_base, config, BAYER);
	dev_info(dev, "ISP Lite %u x %u RAW%u Bayer %u, stat_result_size %lu",
			width, height, bits, bayer, sizeof(struct xil_isp_lite_stat_result));

	mutex_unlock(&isp->lock);

	return 0;
}

static int isp_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			       struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static int isp_start_stream(struct isp_state *isp)
{
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, RESET, 0);
	isp->streaming = true;

	return 0;
}

static void isp_stop_stream(struct isp_state *isp)
{
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, RESET, 1);
	isp->streaming = false;
}

static void isp_queue_event_frame_sync(struct isp_state *isp, u32 frame_seq)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};
	event.u.frame_sync.frame_sequence = frame_seq;

	v4l2_event_queue(isp->subdev.devnode, &event);
}

static irqreturn_t isp_irq_handler(void *data)
{
	struct isp_state *isp = (struct isp_state *)data;
	u32 status, mask, pending;

	status = INFINITE_ISP_READ_REG(isp->isp_base, config, INT_STATUS);
	mask = INFINITE_ISP_READ_REG(isp->isp_base, config, INT_MASK);
	pending = status & ~mask;
	if (status)
		INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_STATUS, 0);

	if (!pending) {
		return IRQ_NONE;
	}

	if ((pending & ISP_REG_INT_STATUS_BIT_FRAME_START) && isp->subdev.devnode) {
		isp_queue_event_frame_sync(isp, isp->frame_sequence);
	}

	isp->int_status |= pending;

	/*
	 * FRAME_DONE is the record boundary. AE_DONE is optional on the current
	 * bitstream and AWB_DONE is not connected, so neither may block metadata.
	 * Their sampled validity is reported in the metadata flags instead.
	 */
	if (pending & ISP_REG_INT_STATUS_BIT_FRAME_DONE) {
		isp_stat_send_measurement(&isp->stat_node, isp->int_status);
		isp->frame_sequence++;
		isp->int_status = 0;
	}

	return IRQ_HANDLED;
}

static int isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct isp_state *isp = to_ispstate(sd);
	int ret = 0;

	dev_info(isp->dev, "ISP %s streaming", enable ? "enable" : "disable");
	mutex_lock(&isp->lock);

	if (enable == isp->streaming) {
		goto stream_done;
	}

	if (enable) {
		ret = isp_start_stream(isp);
	} else {
		isp_stop_stream(isp);
	}
	dev_info(isp->dev, "ISP %s streaming ret: %d", enable ? "start" : "stop", ret);
stream_done:
	mutex_unlock(&isp->lock);
	return ret;
}

static struct v4l2_mbus_framefmt *__isp_get_pad_format(struct isp_state *isp,
			   struct v4l2_subdev_state *sd_state,
			   unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *get_fmt;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		get_fmt = v4l2_subdev_get_try_format(&isp->subdev,
						     sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		get_fmt = &isp->pad_format[pad];
		break;
	default:
		get_fmt = NULL;
		break;
	}

	return get_fmt;
}

static int isp_init_cfg(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state)
{
	struct isp_state *isp = to_ispstate(sd);
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	mutex_lock(&isp->lock);
	for (i = 0; i < ISP_MEDIA_PADS; i++) {
		format = v4l2_subdev_get_try_format(sd, sd_state, i);
		*format = isp->pad_format[i];
	}
	mutex_unlock(&isp->lock);

	return 0;
}

static int isp_get_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct isp_state *isp = to_ispstate(sd);
	struct v4l2_mbus_framefmt *get_fmt;
	int ret = 0;

	mutex_lock(&isp->lock);

	get_fmt = __isp_get_pad_format(isp, sd_state, fmt->pad,
					     fmt->which);
	if (!get_fmt) {
		ret = -EINVAL;
		goto unlock_get_format;
	}

	fmt->format = *get_fmt;

	dev_info(isp->dev, "Get format for pad %u: code=%x, width=%u, height=%u",
		fmt->pad, fmt->format.code, fmt->format.width, fmt->format.height);
unlock_get_format:
	mutex_unlock(&isp->lock);

	return ret;
}

static int isp_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct isp_state *isp = to_ispstate(sd);
	struct v4l2_mbus_framefmt *__format;
	int ret = 0;

	mutex_lock(&isp->lock);

	__format = __isp_get_pad_format(isp, sd_state,
					      fmt->pad, fmt->which);
	if (!__format) {
		ret = -EINVAL;
		goto unlock_set_format;
	}

	/* only sink pad format can be updated */
	if (fmt->pad != ISP_PAD_SINK) {
		fmt->format = *__format;
		goto unlock_set_format;
	}

	if ((isp->bits == 8 && fmt->format.code == MEDIA_BUS_FMT_Y8_1X8) ||
	    (isp->bits == 10 && fmt->format.code == MEDIA_BUS_FMT_Y10_1X10) ||
	    (isp->bits == 12 && fmt->format.code == MEDIA_BUS_FMT_Y12_1X12)) {
		__format->code = fmt->format.code;
	}
	__format->width  = fmt->format.width;
	__format->height = fmt->format.height;
	fmt->format = *__format;
	dev_info(isp->dev, "Set format for pad %u: code=%x, width=%u, height=%u",
		fmt->pad, fmt->format.code, fmt->format.width, fmt->format.height);

unlock_set_format:
	mutex_unlock(&isp->lock);

	return ret;
}

static int isp_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct isp_state *isp = to_ispstate(sd);

	if (code->index > 1 || code->pad >= ISP_MEDIA_PADS) {
		return -EINVAL;
	}

	if (code->index == 0) {
		code->code = isp->pad_format[code->pad].code;
	} else {
		code->code = isp->bits == 8 ? MEDIA_BUS_FMT_Y8_1X8 : (isp->bits == 10) ? MEDIA_BUS_FMT_Y10_1X10 : MEDIA_BUS_FMT_Y12_1X12;
	}

	return 0;
}

static int isp_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct isp_state *isp = to_ispstate(sd);

	if (fse->index > 0 || fse->pad >= ISP_MEDIA_PADS || fse->code != isp->pad_format[fse->pad].code) {
		return -EINVAL;
	}

	fse->min_width  = isp->pad_format[fse->pad].width;
	fse->max_width  = isp->pad_format[fse->pad].width;
	fse->min_height = isp->pad_format[fse->pad].height;
	fse->max_height = isp->pad_format[fse->pad].height;

	return 0;
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations isp_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static const struct v4l2_subdev_core_ops isp_core_ops = {
	.log_status = isp_log_status,
	.subscribe_event = isp_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops isp_video_ops = {
	.s_stream = isp_s_stream,
};

static const struct v4l2_subdev_pad_ops isp_pad_ops = {
	.init_cfg = isp_init_cfg,
	.enum_mbus_code = isp_enum_mbus_code,
	.enum_frame_size = isp_enum_frame_size,
	.get_fmt = isp_get_format,
	.set_fmt = isp_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops isp_ops = {
	.core = &isp_core_ops,
	.video = &isp_video_ops,
	.pad = &isp_pad_ops
};

static int isp_internal_registered (struct v4l2_subdev *sd)
{
	struct isp_state *isp = to_ispstate(sd);
	return isp_stat_node_register(isp, sd->v4l2_dev);
}

static void isp_internal_unregistered (struct v4l2_subdev *sd)
{
	struct isp_state *isp = to_ispstate(sd);
	isp_stat_node_unregister(isp);
}

static const struct v4l2_subdev_internal_ops isp_internal_ops = {
	.registered    = isp_internal_registered,
	.unregistered  = isp_internal_unregistered,
};

static int isp_get_hw_format(struct isp_state *isp)
{
	struct device *dev = isp->dev;
	struct v4l2_mbus_framefmt *format = NULL;

	const u32 raw8_codes[] =  {MEDIA_BUS_FMT_SRGGB8_1X8, MEDIA_BUS_FMT_SGRBG8_1X8, MEDIA_BUS_FMT_SGBRG8_1X8, MEDIA_BUS_FMT_SBGGR8_1X8};
	const u32 raw10_codes[] = {MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10};
	const u32 raw12_codes[] = {MEDIA_BUS_FMT_SRGGB12_1X12, MEDIA_BUS_FMT_SGRBG12_1X12, MEDIA_BUS_FMT_SGBRG12_1X12, MEDIA_BUS_FMT_SBGGR12_1X12};

	u32 width  = INFINITE_ISP_READ_REG(isp->isp_base, config, SNS_WIDTH);
	u32 height = INFINITE_ISP_READ_REG(isp->isp_base, config, SNS_HEIGHT);
	u32 bits   = INFINITE_ISP_READ_REG(isp->isp_base, config, BITS);
	u32 bayer  = INFINITE_ISP_READ_REG(isp->isp_base, config, BAYER);
	if (width < 1 || height < 1 || (bits != 8 && bits != 10 && bits != 12) || bayer >= 4) {
		dev_err(dev, "Invalid HW formats. Resolution %u x %u, RAW%u, Bayer %u",
			width, height, bits, bayer);
		return -EINVAL;
	}

	dev_info(dev, "ISP HW formats. Resolution %u x %u, RAW%u, Bayer %u",
		width, height, bits, bayer);
	
	isp->bits = bits;
	format = &isp->pad_format[ISP_PAD_SINK];

	switch (bits) {
		case 8:   format->code = raw8_codes[bayer];  break;
		case 10:  format->code = raw10_codes[bayer]; break;
		default:  format->code = raw12_codes[bayer]; break;
	}

	format->field      = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->width      = width;
	format->height     = height;

	format = &isp->pad_format[ISP_PAD_SOURCE];
	*format = isp->pad_format[ISP_PAD_SINK];
	/* The ISP core exports fixed 8-bit Y, U and V channels to each VIP. */
	format->code = MEDIA_BUS_FMT_YUV8_1X24;
#if ISP_MEDIA_PADS > 2
	format = &isp->pad_format[ISP_PAD_SOURCE_2];
	*format = isp->pad_format[ISP_PAD_SINK];
	format->code = MEDIA_BUS_FMT_YUV8_1X24;
#endif

	return 0;
}

//np.uint8(np.power(np.double(range(64))/63, 1/2.2) * 63)
static const unsigned gamma_table[] = {
        0,  9, 13, 15, 17, 19, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 33,
       34, 35, 36, 37, 38, 39, 39, 40, 41, 42, 42, 43, 44, 44, 45, 46, 46,
       47, 48, 48, 49, 50, 50, 51, 51, 52, 52, 53, 54, 54, 55, 55, 56, 56,
       57, 57, 58, 58, 59, 59, 60, 60, 61, 61, 62, 62, 63
};

//spaceKernel = x_bf_makeSpaceKern(7, 6, 31); print(spaceKernel)
static const unsigned char spaceWeightTbl_6[7*7] = {
		24, 26, 27, 27, 27, 26, 24,
		26, 28, 29, 29, 29, 28, 26,
		27, 29, 30, 31, 30, 29, 27,
		27, 29, 31, 31, 31, 29, 27,
		27, 29, 30, 31, 30, 29, 27,
		26, 28, 29, 29, 29, 28, 26,
		24, 26, 27, 27, 27, 26, 24
	};
//spaceKernel = x_bf_makeSpaceKern(7, 8, 31); print(spaceKernel)
static const unsigned char spaceWeightTbl_8[7*7] = {
		27, 28, 29, 29, 29, 28, 27,
		28, 29, 30, 30, 30, 29, 28,
		29, 30, 31, 31, 31, 30, 29,
		29, 30, 31, 31, 31, 30, 29,
		29, 30, 31, 31, 31, 30, 29,
		28, 29, 30, 30, 30, 29, 28,
		27, 28, 29, 29, 29, 28, 27
	};
//spaceKernel = x_bf_makeSpaceKern(7, 10, 31); print(spaceKernel)
static const unsigned char spaceWeightTbl_10[7*7] = {
		28, 29, 29, 30, 29, 29, 28,
		29, 30, 30, 30, 30, 30, 29,
		29, 30, 31, 31, 31, 30, 29,
		30, 30, 31, 31, 31, 30, 30,
		29, 30, 31, 31, 31, 30, 29,
		29, 30, 30, 30, 30, 30, 29,
		28, 29, 29, 30, 29, 29, 28
	};
//colorCurve = x_bf_makeColorCurve(9, 20, 6, 31); print(colorCurve)
static const unsigned char colorCurveTbl_6[9][2] = {
		{ 2, 29},
		{ 4, 25},
		{ 6, 19},
		{ 8, 13},
		{10,  8},
		{12,  4},
		{14,  2},
		{16,  1},
		{18,  0}
	};
//colorCurve = x_bf_makeColorCurve(9, 26, 8, 31); print(colorCurve)
static const unsigned char colorCurveTbl_8[9][2] = {
		{ 2, 30},
		{ 5, 25},
		{ 7, 21},
		{10, 14},
		{13,  8},
		{15,  5},
		{18,  2},
		{20,  1},
		{23,  0}
	};
//colorCurve = x_bf_makeColorCurve(9, 34, 10, 31); print(colorCurve)
static const unsigned char colorCurveTbl_10[9][2] = {
		{ 3, 30},
		{ 6, 26},
		{10, 19},
		{13, 13},
		{17,  7},
		{20,  4},
		{23,  2},
		{27,  1},
		{30,  0}
	};
static void isp_init_bnr(struct REG_Infinite_ISP* infinite_isp_reg)
{
	infinite_isp_reg->bnr.bnr_space_kernel_r00 = bnr_sk_r[0];
	infinite_isp_reg->bnr.bnr_space_kernel_r01 = bnr_sk_r[1];
	infinite_isp_reg->bnr.bnr_space_kernel_r02 = bnr_sk_r[2];
	infinite_isp_reg->bnr.bnr_space_kernel_r03 = bnr_sk_r[3];
	infinite_isp_reg->bnr.bnr_space_kernel_r04 = bnr_sk_r[4];
	infinite_isp_reg->bnr.bnr_space_kernel_r10 = bnr_sk_r[5];
	infinite_isp_reg->bnr.bnr_space_kernel_r11 = bnr_sk_r[6];
	infinite_isp_reg->bnr.bnr_space_kernel_r12 = bnr_sk_r[7];
	infinite_isp_reg->bnr.bnr_space_kernel_r13 = bnr_sk_r[8];
	infinite_isp_reg->bnr.bnr_space_kernel_r14 = bnr_sk_r[9];
	infinite_isp_reg->bnr.bnr_space_kernel_r20 = bnr_sk_r[10];
	infinite_isp_reg->bnr.bnr_space_kernel_r21 = bnr_sk_r[11];
	infinite_isp_reg->bnr.bnr_space_kernel_r22 = bnr_sk_r[12];
	infinite_isp_reg->bnr.bnr_space_kernel_r23 = bnr_sk_r[13];
	infinite_isp_reg->bnr.bnr_space_kernel_r24 = bnr_sk_r[14];
	infinite_isp_reg->bnr.bnr_space_kernel_r30 = bnr_sk_r[15];
	infinite_isp_reg->bnr.bnr_space_kernel_r31 = bnr_sk_r[16];
	infinite_isp_reg->bnr.bnr_space_kernel_r32 = bnr_sk_r[17];
	infinite_isp_reg->bnr.bnr_space_kernel_r33 = bnr_sk_r[18];
	infinite_isp_reg->bnr.bnr_space_kernel_r34 = bnr_sk_r[19];
	infinite_isp_reg->bnr.bnr_space_kernel_r40 = bnr_sk_r[20];
	infinite_isp_reg->bnr.bnr_space_kernel_r41 = bnr_sk_r[21];
	infinite_isp_reg->bnr.bnr_space_kernel_r42 = bnr_sk_r[22];
	infinite_isp_reg->bnr.bnr_space_kernel_r43 = bnr_sk_r[23];
	infinite_isp_reg->bnr.bnr_space_kernel_r44 = bnr_sk_r[24];

	infinite_isp_reg->bnr.bnr_space_kernel_g00 = bnr_sk_g[0];
	infinite_isp_reg->bnr.bnr_space_kernel_g01 = bnr_sk_g[1];
	infinite_isp_reg->bnr.bnr_space_kernel_g02 = bnr_sk_g[2];
	infinite_isp_reg->bnr.bnr_space_kernel_g03 = bnr_sk_g[3];
	infinite_isp_reg->bnr.bnr_space_kernel_g04 = bnr_sk_g[4];
	infinite_isp_reg->bnr.bnr_space_kernel_g10 = bnr_sk_g[5];
	infinite_isp_reg->bnr.bnr_space_kernel_g11 = bnr_sk_g[6];
	infinite_isp_reg->bnr.bnr_space_kernel_g12 = bnr_sk_g[7];
	infinite_isp_reg->bnr.bnr_space_kernel_g13 = bnr_sk_g[8];
	infinite_isp_reg->bnr.bnr_space_kernel_g14 = bnr_sk_g[9];
	infinite_isp_reg->bnr.bnr_space_kernel_g20 = bnr_sk_g[10];
	infinite_isp_reg->bnr.bnr_space_kernel_g21 = bnr_sk_g[11];
	infinite_isp_reg->bnr.bnr_space_kernel_g22 = bnr_sk_g[12];
	infinite_isp_reg->bnr.bnr_space_kernel_g23 = bnr_sk_g[13];
	infinite_isp_reg->bnr.bnr_space_kernel_g24 = bnr_sk_g[14];
	infinite_isp_reg->bnr.bnr_space_kernel_g30 = bnr_sk_g[15];
	infinite_isp_reg->bnr.bnr_space_kernel_g31 = bnr_sk_g[16];
	infinite_isp_reg->bnr.bnr_space_kernel_g32 = bnr_sk_g[17];
	infinite_isp_reg->bnr.bnr_space_kernel_g33 = bnr_sk_g[18];
	infinite_isp_reg->bnr.bnr_space_kernel_g34 = bnr_sk_g[19];
	infinite_isp_reg->bnr.bnr_space_kernel_g40 = bnr_sk_g[20];
	infinite_isp_reg->bnr.bnr_space_kernel_g41 = bnr_sk_g[21];
	infinite_isp_reg->bnr.bnr_space_kernel_g42 = bnr_sk_g[22];
	infinite_isp_reg->bnr.bnr_space_kernel_g43 = bnr_sk_g[23];
	infinite_isp_reg->bnr.bnr_space_kernel_g44 = bnr_sk_g[24];

	infinite_isp_reg->bnr.bnr_space_kernel_b00 = bnr_sk_b[0];
	infinite_isp_reg->bnr.bnr_space_kernel_b01 = bnr_sk_b[1];
	infinite_isp_reg->bnr.bnr_space_kernel_b02 = bnr_sk_b[2];
	infinite_isp_reg->bnr.bnr_space_kernel_b03 = bnr_sk_b[3];
	infinite_isp_reg->bnr.bnr_space_kernel_b04 = bnr_sk_b[4];
	infinite_isp_reg->bnr.bnr_space_kernel_b10 = bnr_sk_b[5];
	infinite_isp_reg->bnr.bnr_space_kernel_b11 = bnr_sk_b[6];
	infinite_isp_reg->bnr.bnr_space_kernel_b12 = bnr_sk_b[7];
	infinite_isp_reg->bnr.bnr_space_kernel_b13 = bnr_sk_b[8];
	infinite_isp_reg->bnr.bnr_space_kernel_b14 = bnr_sk_b[9];
	infinite_isp_reg->bnr.bnr_space_kernel_b20 = bnr_sk_b[10];
	infinite_isp_reg->bnr.bnr_space_kernel_b21 = bnr_sk_b[11];
	infinite_isp_reg->bnr.bnr_space_kernel_b22 = bnr_sk_b[12];
	infinite_isp_reg->bnr.bnr_space_kernel_b23 = bnr_sk_b[13];
	infinite_isp_reg->bnr.bnr_space_kernel_b24 = bnr_sk_b[14];
	infinite_isp_reg->bnr.bnr_space_kernel_b30 = bnr_sk_b[15];
	infinite_isp_reg->bnr.bnr_space_kernel_b31 = bnr_sk_b[16];
	infinite_isp_reg->bnr.bnr_space_kernel_b32 = bnr_sk_b[17];
	infinite_isp_reg->bnr.bnr_space_kernel_b33 = bnr_sk_b[18];
	infinite_isp_reg->bnr.bnr_space_kernel_b34 = bnr_sk_b[19];
	infinite_isp_reg->bnr.bnr_space_kernel_b40 = bnr_sk_b[20];
	infinite_isp_reg->bnr.bnr_space_kernel_b41 = bnr_sk_b[21];
	infinite_isp_reg->bnr.bnr_space_kernel_b42 = bnr_sk_b[22];
	infinite_isp_reg->bnr.bnr_space_kernel_b43 = bnr_sk_b[23];
	infinite_isp_reg->bnr.bnr_space_kernel_b44 = bnr_sk_b[24];

	infinite_isp_reg->bnr.bnr_color_curve_x_r_0 = bnr_cc_xr[0];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_1 = bnr_cc_xr[1];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_2 = bnr_cc_xr[2];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_3 = bnr_cc_xr[3];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_4 = bnr_cc_xr[4];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_5 = bnr_cc_xr[5];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_6 = bnr_cc_xr[6];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_7 = bnr_cc_xr[7];
	infinite_isp_reg->bnr.bnr_color_curve_x_r_8 = bnr_cc_xr[8];

	infinite_isp_reg->bnr.bnr_color_curve_y_r_0 = bnr_cc_yr[0];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_1 = bnr_cc_yr[1];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_2 = bnr_cc_yr[2];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_3 = bnr_cc_yr[3];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_4 = bnr_cc_yr[4];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_5 = bnr_cc_yr[5];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_6 = bnr_cc_yr[6];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_7 = bnr_cc_yr[7];
	infinite_isp_reg->bnr.bnr_color_curve_y_r_8 = bnr_cc_yr[8];

	infinite_isp_reg->bnr.bnr_color_curve_x_g_0 = bnr_cc_xg[0];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_1 = bnr_cc_xg[1];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_2 = bnr_cc_xg[2];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_3 = bnr_cc_xg[3];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_4 = bnr_cc_xg[4];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_5 = bnr_cc_xg[5];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_6 = bnr_cc_xg[6];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_7 = bnr_cc_xg[7];
	infinite_isp_reg->bnr.bnr_color_curve_x_g_8 = bnr_cc_xg[8];

	infinite_isp_reg->bnr.bnr_color_curve_y_g_0 = bnr_cc_yg[0];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_1 = bnr_cc_yg[1];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_2 = bnr_cc_yg[2];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_3 = bnr_cc_yg[3];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_4 = bnr_cc_yg[4];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_5 = bnr_cc_yg[5];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_6 = bnr_cc_yg[6];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_7 = bnr_cc_yg[7];
	infinite_isp_reg->bnr.bnr_color_curve_y_g_8 = bnr_cc_yg[8];

	infinite_isp_reg->bnr.bnr_color_curve_x_b_0 = bnr_cc_xb[0];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_1 = bnr_cc_xb[1];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_2 = bnr_cc_xb[2];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_3 = bnr_cc_xb[3];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_4 = bnr_cc_xb[4];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_5 = bnr_cc_xb[5];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_6 = bnr_cc_xb[6];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_7 = bnr_cc_xb[7];
	infinite_isp_reg->bnr.bnr_color_curve_x_b_8 = bnr_cc_xb[8];

	infinite_isp_reg->bnr.bnr_color_curve_y_b_0 = bnr_cc_yb[0];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_1 = bnr_cc_yb[1];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_2 = bnr_cc_yb[2];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_3 = bnr_cc_yb[3];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_4 = bnr_cc_yb[4];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_5 = bnr_cc_yb[5];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_6 = bnr_cc_yb[6];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_7 = bnr_cc_yb[7];
	infinite_isp_reg->bnr.bnr_color_curve_y_b_8 = bnr_cc_yb[8];
}

#include <linux/delay.h>
static int isp_initialize_hw(struct isp_state *isp)
{
	struct REG_Infinite_ISP *infinite_isp_reg;
	struct REG_Infinite_ISP_LUT *infinite_isp_lut;
	infinite_isp_reg = kzalloc(sizeof(*infinite_isp_reg), GFP_KERNEL);
	if (!infinite_isp_reg)
		return -ENOMEM;

	infinite_isp_lut = kzalloc(sizeof(*infinite_isp_lut), GFP_KERNEL);
	if (!infinite_isp_lut) {
		kfree(infinite_isp_reg);
		return -ENOMEM;
	}

	INFINITE_ISP_WRITE_REG(isp->isp_base, config, RESET, 1);

	msleep_interruptible(100);

	infinite_isp_reg->config.TOP_EN.TOP_EN_DPC_EN = DPC_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_BLC_EN = BLC_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_LINEAR_EN = BLC_LINEAR_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_OECF_EN = OECF_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_DGAIN_EN = DGAIN_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_LSC_EN = 0;
	infinite_isp_reg->config.TOP_EN.TOP_EN_BNR_EN = BNR_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_WB_EN = WB_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_GAMMA_EN = GAMMA_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_DEMOSIC_EN = CFA_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_CCM_EN = CCM_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_CSC_EN = CSC_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_LDCI_EN = 0;
	infinite_isp_reg->config.TOP_EN.TOP_EN_2DNR_EN = NR2D_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_SHARP_EN = SHARP_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_AE_EN = AE_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_AWB_EN = AWB_EN;
	infinite_isp_reg->config.TOP_EN.TOP_EN_CROP_EN = CROP_EN;

	infinite_isp_reg->dpc.DPC_THRESHOLD = dp_threshold;

	infinite_isp_reg->blc.BLC_R  = r_offset;
	infinite_isp_reg->blc.BLC_GB = gr_offset;
	infinite_isp_reg->blc.BLC_GR = gb_offset;
	infinite_isp_reg->blc.BLC_B  = b_offset;
	infinite_isp_reg->blc.LINEAR_R = linear_r;
	infinite_isp_reg->blc.LINEAR_GB = linear_gb;
	infinite_isp_reg->blc.LINEAR_GR = linear_gr;
	infinite_isp_reg->blc.LINEAR_B = linear_b;

	infinite_isp_reg->dgain.dgain_isManual = DGAIN_isManual;
	infinite_isp_reg->dgain.dgain_man_index = current_gain;
	memcpy(&infinite_isp_reg->dgain.dgain_array_0, gain_array, sizeof(gain_array));

	isp_init_bnr(infinite_isp_reg);

	infinite_isp_reg->wb.WB_RGAIN = r_gain;
	infinite_isp_reg->wb.WB_BGAIN = b_gain;

	infinite_isp_reg->ccm.ccm_rr = corrected_red[0];
	infinite_isp_reg->ccm.ccm_rg = corrected_red[1];
	infinite_isp_reg->ccm.ccm_rb = corrected_red[2];

	infinite_isp_reg->ccm.ccm_gr = corrected_green[0];
	infinite_isp_reg->ccm.ccm_gg = corrected_green[1];
	infinite_isp_reg->ccm.ccm_gb = corrected_green[2];

	infinite_isp_reg->ccm.ccm_br = corrected_blue[0];
	infinite_isp_reg->ccm.ccm_bg = corrected_blue[1];
	infinite_isp_reg->ccm.ccm_bb = corrected_blue[2];

	infinite_isp_reg->csc.csc_conv_std = csc_conv_standard;
	
	infinite_isp_reg->sharp.sharpen_strength = sharpen_strength;
	memcpy(&infinite_isp_reg->sharp.luma_kernel_00, luma_kernel, sizeof(luma_kernel));

	infinite_isp_reg->_2dnr.nr2d_diff_0 = nr2d_diff[0];
	infinite_isp_reg->_2dnr.nr2d_diff_1 = nr2d_diff[1];
	infinite_isp_reg->_2dnr.nr2d_diff_2 = nr2d_diff[2];
	infinite_isp_reg->_2dnr.nr2d_diff_3 = nr2d_diff[3];
	infinite_isp_reg->_2dnr.nr2d_diff_4 = nr2d_diff[4];
	infinite_isp_reg->_2dnr.nr2d_diff_5 = nr2d_diff[5];
	infinite_isp_reg->_2dnr.nr2d_diff_6 = nr2d_diff[6];
	infinite_isp_reg->_2dnr.nr2d_diff_7 = nr2d_diff[7];
	infinite_isp_reg->_2dnr.nr2d_diff_8 = nr2d_diff[8];
	infinite_isp_reg->_2dnr.nr2d_diff_9 = nr2d_diff[9];
	infinite_isp_reg->_2dnr.nr2d_diff_10 = nr2d_diff[10];
	infinite_isp_reg->_2dnr.nr2d_diff_11 = nr2d_diff[11];
	infinite_isp_reg->_2dnr.nr2d_diff_12 = nr2d_diff[12];
	infinite_isp_reg->_2dnr.nr2d_diff_13 = nr2d_diff[13];
	infinite_isp_reg->_2dnr.nr2d_diff_14 = nr2d_diff[14];
	infinite_isp_reg->_2dnr.nr2d_diff_15 = nr2d_diff[15];
	infinite_isp_reg->_2dnr.nr2d_diff_16 = nr2d_diff[16];
	infinite_isp_reg->_2dnr.nr2d_diff_17 = nr2d_diff[17];
	infinite_isp_reg->_2dnr.nr2d_diff_18 = nr2d_diff[18];
	infinite_isp_reg->_2dnr.nr2d_diff_19 = nr2d_diff[19];
	infinite_isp_reg->_2dnr.nr2d_diff_20 = nr2d_diff[20];
	infinite_isp_reg->_2dnr.nr2d_diff_21 = nr2d_diff[21];
	infinite_isp_reg->_2dnr.nr2d_diff_22 = nr2d_diff[22];
	infinite_isp_reg->_2dnr.nr2d_diff_23 = nr2d_diff[23];
	infinite_isp_reg->_2dnr.nr2d_diff_24 = nr2d_diff[24];
	infinite_isp_reg->_2dnr.nr2d_diff_25 = nr2d_diff[25];
	infinite_isp_reg->_2dnr.nr2d_diff_26 = nr2d_diff[26];
	infinite_isp_reg->_2dnr.nr2d_diff_27 = nr2d_diff[27];
	infinite_isp_reg->_2dnr.nr2d_diff_28 = nr2d_diff[28];
	
	infinite_isp_reg->_2dnr.nr2d_weight_0 = nr2d_weight[0];
	infinite_isp_reg->_2dnr.nr2d_weight_1 = nr2d_weight[1];
	infinite_isp_reg->_2dnr.nr2d_weight_2 = nr2d_weight[2];
	infinite_isp_reg->_2dnr.nr2d_weight_3 = nr2d_weight[3];
	infinite_isp_reg->_2dnr.nr2d_weight_4 = nr2d_weight[4];
	infinite_isp_reg->_2dnr.nr2d_weight_5 = nr2d_weight[5];
	infinite_isp_reg->_2dnr.nr2d_weight_6 = nr2d_weight[6];
	infinite_isp_reg->_2dnr.nr2d_weight_7 = nr2d_weight[7];
	infinite_isp_reg->_2dnr.nr2d_weight_8 = nr2d_weight[8];
	infinite_isp_reg->_2dnr.nr2d_weight_9 = nr2d_weight[9];
	infinite_isp_reg->_2dnr.nr2d_weight_10 = nr2d_weight[10];
	infinite_isp_reg->_2dnr.nr2d_weight_11 = nr2d_weight[11];
	infinite_isp_reg->_2dnr.nr2d_weight_12 = nr2d_weight[12];
	infinite_isp_reg->_2dnr.nr2d_weight_13 = nr2d_weight[13];
	infinite_isp_reg->_2dnr.nr2d_weight_14 = nr2d_weight[14];
	infinite_isp_reg->_2dnr.nr2d_weight_15 = nr2d_weight[15];
	infinite_isp_reg->_2dnr.nr2d_weight_16 = nr2d_weight[16];
	infinite_isp_reg->_2dnr.nr2d_weight_17 = nr2d_weight[17];
	infinite_isp_reg->_2dnr.nr2d_weight_18 = nr2d_weight[18];
	infinite_isp_reg->_2dnr.nr2d_weight_19 = nr2d_weight[19];
	infinite_isp_reg->_2dnr.nr2d_weight_20 = nr2d_weight[20];
	infinite_isp_reg->_2dnr.nr2d_weight_21 = nr2d_weight[21];
	infinite_isp_reg->_2dnr.nr2d_weight_22 = nr2d_weight[22];
	infinite_isp_reg->_2dnr.nr2d_weight_23 = nr2d_weight[23];
	infinite_isp_reg->_2dnr.nr2d_weight_24 = nr2d_weight[24];
	infinite_isp_reg->_2dnr.nr2d_weight_25 = nr2d_weight[25];
	infinite_isp_reg->_2dnr.nr2d_weight_26 = nr2d_weight[26];
	infinite_isp_reg->_2dnr.nr2d_weight_27 = nr2d_weight[27];
	infinite_isp_reg->_2dnr.nr2d_weight_28 = nr2d_weight[28];

	infinite_isp_reg->awb.AWB_UNDEREXPOSED_LIMIT = awb_underexposed_limit;
	infinite_isp_reg->awb.AWB_OVEREXPOSED_LIMIT = awb_overexposed_limit;
	infinite_isp_reg->awb.AWB_FRAMES = awb_frames;

	infinite_isp_reg->ae.center_illuminance = center_illuminance;
	infinite_isp_reg->ae.skewness = histogram_skewnes;
	infinite_isp_reg->ae.ae_crop_left = ae_crop_left;
	infinite_isp_reg->ae.ae_crop_right = ae_crop_right;
	infinite_isp_reg->ae.ae_crop_top = ae_crop_top;
	infinite_isp_reg->ae.ae_crop_bottom = ae_crop_bottom;

	memcpy(infinite_isp_lut->gamma_lut.GAMMA_LUT, gamma_lut_10, sizeof(gamma_lut_10));

	memcpy(infinite_isp_lut->oecf_luts.OECF_R_LUT, oecf_table, sizeof(oecf_table));
	memcpy(infinite_isp_lut->oecf_luts.OECF_GB_LUT, oecf_table, sizeof(oecf_table));
	memcpy(infinite_isp_lut->oecf_luts.OECF_GR_LUT, oecf_table, sizeof(oecf_table));
	memcpy(infinite_isp_lut->oecf_luts.OECF_B_LUT, oecf_table, sizeof(oecf_table));

	memcpy(infinite_isp_lut->vip1_osd_ram.VIP1_OSD_RAM, osd_bitmap_128x32, sizeof(osd_bitmap_128x32));
	memcpy(infinite_isp_lut->vip2_osd_ram.VIP2_OSD_RAM, osd_bitmap_128x32, sizeof(osd_bitmap_128x32));

	INFINITE_ISP_WRITE_REG(isp->isp_base, config, TOP_EN, infinite_isp_reg->config.TOP_EN.TOP_EN_val);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, dpc, &infinite_isp_reg->dpc);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, blc, &infinite_isp_reg->blc);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, dgain, &infinite_isp_reg->dgain);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, bnr, &infinite_isp_reg->bnr);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, wb, &infinite_isp_reg->wb);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, ccm, &infinite_isp_reg->ccm);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, csc, &infinite_isp_reg->csc);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, sharp, &infinite_isp_reg->sharp);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, _2dnr, &infinite_isp_reg->_2dnr);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, awb, &infinite_isp_reg->awb);
	INFINITE_ISP_WRITE_MODULE_REGs(isp->isp_base, ae, &infinite_isp_reg->ae);

	INFINITE_ISP_WRITE_LUT_REGs(isp->luts_base, gamma_lut, &infinite_isp_lut->gamma_lut);
	INFINITE_ISP_WRITE_LUT_REGs(isp->luts_base, oecf_luts, &infinite_isp_lut->oecf_luts);
	INFINITE_ISP_WRITE_LUT_REGs(isp->luts_base, vip1_osd_ram, &infinite_isp_lut->vip1_osd_ram);
	INFINITE_ISP_WRITE_LUT_REGs(isp->luts_base, vip2_osd_ram, &infinite_isp_lut->vip2_osd_ram);
	
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, RESET, 0);
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_MASK, ~0U);

	kfree(infinite_isp_lut);
	kfree(infinite_isp_reg);
	return 0;
}

static const struct clk_bulk_data isp_clks[] = {
	{ .id = "s00_axi_aclk" },
	{ .id = "pclk" },
};

static ssize_t offset_write(struct file *file, const char __user *user_buf,
                          size_t count, loff_t *ppos)
{
    struct isp_state *isp = file->private_data;
    char buf[16];
    unsigned long offset;
    int ret;

    if (count >= sizeof(buf))
        return -EINVAL;

    if (copy_from_user(buf, user_buf, count))
        return -EFAULT;

    buf[count] = '\0';

    ret = kstrtoul(buf, 0, &offset);
    if (ret)
        return ret;

    if (offset > 0xffff)
        return -EINVAL;

    isp->debug_reg_offset = offset;
	isp->is_lut = 0;
	pr_info("set debug_reg_offset: 0x%x", offset);
    return count;
}

static ssize_t vip_offset_write(struct file *file, const char __user *user_buf,
                          size_t count, loff_t *ppos)
{
    struct isp_state *isp = file->private_data;
    char buf[16];
    unsigned long offset;
    int ret;

    if (count >= sizeof(buf))
        return -EINVAL;

    if (copy_from_user(buf, user_buf, count))
        return -EFAULT;

    buf[count] = '\0';

    ret = kstrtoul(buf, 0, &offset);
    if (ret)
        return ret;

    if (offset > 0xffff)
        return -EINVAL;

    isp->debug_reg_offset = offset;
	isp->is_lut = 1;
	pr_info("set lut debug_reg_offset: 0x%x", offset);
    return count;
}

static ssize_t offset_read(struct file *file, char __user *user_buf,
                         size_t count, loff_t *ppos)
{
    struct isp_state *isp = file->private_data;
    char buf[30];
	void* base = isp->is_lut ? isp->luts_base : isp->isp_base;
	unsigned int module_offset = isp->is_lut ? 0x8000 : 0;
	unsigned long max_offset = isp->is_lut ? 0x17FFC : 0x2BFC;
    int len;
	int i = 0;

    if (*ppos > 0)
        return 0;

	if (isp->debug_reg_offset == 0xffff) {
		pr_info("dump isp debug %s registers", isp->is_lut ? "luts" : "isp");
		for (; i < max_offset; i+=4 * 4) {
			pr_info("0x%04x : 0x%08x 0x%08x 0x%08x 0x%08x", i + module_offset, 
				ioread32(base + i),
				ioread32(base + i + 4),
				ioread32(base + i + 8),
				ioread32(base + i + 12));
		}
		len = snprintf(buf, sizeof(buf), "dump done\n");
	} else
    	len = snprintf(buf, sizeof(buf), "0x%08x : 0x%08x\n", isp->debug_reg_offset, ioread32(base + isp->debug_reg_offset));    
    if (copy_to_user(user_buf, buf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

static const struct file_operations offset_fops = {
	.owner = THIS_MODULE,
    .open = simple_open,
    .read = offset_read,
    .write = offset_write,
    .llseek = default_llseek,
};

static const struct file_operations lut_offset_fops = {
	.owner = THIS_MODULE,
    .open = simple_open,
    .read = offset_read,
    .write = vip_offset_write,
    .llseek = default_llseek,
};

static int isp_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct isp_state *isp;
	int num_clks = ARRAY_SIZE(isp_clks);
	struct device *dev = &pdev->dev;
	int irq;
	int ret;

	isp = devm_kzalloc(dev, sizeof(*isp), GFP_KERNEL);
	if (!isp) {
		dev_err(dev, "No memory for isp");
		return -ENOMEM;
	}

	isp->dev = dev;

	isp->clks = devm_kmemdup(dev, isp_clks, sizeof(isp_clks), GFP_KERNEL);
	if (!isp->clks) {
		dev_err(dev, "No memory for isp clks");
		return -ENOMEM;
	}

    isp->isp_base = devm_platform_ioremap_resource_byname(pdev, "isp");
    if (IS_ERR(isp->isp_base)) {
        ret = PTR_ERR(isp->isp_base);
        dev_err(&pdev->dev, "Failed to map isp registers: %d\n", ret);
        return ret;
    }

    isp->luts_base = devm_platform_ioremap_resource_byname(pdev, "luts");
    if (IS_ERR(isp->luts_base)) {
        ret = PTR_ERR(isp->luts_base);
        dev_err(&pdev->dev, "Failed to map luts registers: %d\n", ret);
        return ret;
    }

	ret = devm_clk_bulk_get(dev, num_clks, isp->clks);
	if (ret) {
		dev_err(dev, "could not get clks");
		return ret;
	}

	/* TODO: Enable/disable clocks at stream on/off time. */
	ret = clk_bulk_prepare_enable(num_clks, isp->clks);
	if (ret) {
		dev_err(dev, "could not prepare enable clks");
		return ret;
	}

	mutex_init(&isp->lock);

	/* Initialize V4L2 subdevice and media entity */
	isp->pads[ISP_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	isp->pads[ISP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
#if ISP_MEDIA_PADS > 2
	isp->pads[ISP_PAD_SOURCE_2].flags = MEDIA_PAD_FL_SOURCE;
#endif

	/* Initialize the default format */
	ret = isp_get_hw_format(isp);
	if (ret < 0) {
		goto err_mutex_destroy;
	}

	/* Initialize the isp hardware */
	ret = isp_initialize_hw(isp);
	if (ret < 0) {
		goto err_mutex_destroy;
	}

	/* Initialize ctrl handler */
	ret = isp_config_init_ctrl_handler(isp);
	if (ret < 0) {
		goto err_mutex_destroy;
	}

	/* Initialize V4L2 subdevice and media entity */
	subdev = &isp->subdev;
	v4l2_subdev_init(subdev, &isp_ops);
	subdev->owner = THIS_MODULE;
	subdev->dev = dev;
	subdev->internal_ops = &isp_internal_ops;
	subdev->ctrl_handler = &isp->config_ctrls;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &isp_media_ops;
	subdev->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	v4l2_set_subdevdata(subdev, isp);

	ret = media_entity_pads_init(&subdev->entity, ISP_MEDIA_PADS,
				     isp->pads);
	if (ret < 0) {
		dev_err(dev, "init media entity pads fail");
		goto err_ctrl_free;
	}

	platform_set_drvdata(pdev, isp);

	/*
	 * Old overlays put the physical parent IRQ on the ISP node. New overlays
	 * give it to the dedicated dispatcher node, so only attach it here when
	 * the legacy interrupt property is present.
	 */
	if (of_find_property(dev->of_node, "interrupts", NULL)) {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0) {
			ret = irq;
			goto err_media_cleanup;
		}
		ret = xil_isp_irq_attach(dev, irq);
		if (ret) {
			dev_err(dev, "failed to attach IRQ dispatcher: %d", ret);
			goto err_media_cleanup;
		}
		isp->irq_dispatcher_owner = true;
	}

	ret = xil_isp_irq_register(XIL_ISP_IRQ_SOURCE_ISP,
				   isp_irq_handler, isp);
	if (ret) {
		dev_err(dev, "failed to register ISP IRQ source: %d", ret);
		goto err_irq_detach;
	}

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto err_irq_unregister;
	}

	isp->debug_dir = debugfs_create_dir("xil_isp", NULL);
	if (IS_ERR_OR_NULL(isp->debug_dir)) {
		dev_warn(dev, "failed to create debugfs directory\n");
		isp->debug_dir = NULL;
	} else {
		debugfs_create_file("reg_offset", 0644, isp->debug_dir, isp,
				    &offset_fops);
		debugfs_create_file("luts_reg_offset", 0644, isp->debug_dir, isp,
				    &lut_offset_fops);
	}

	dev_info(dev, ISP_DRIVER_NAME " driver probed!");

	return 0;
err_irq_unregister:
	xil_isp_irq_unregister(XIL_ISP_IRQ_SOURCE_ISP, isp);
err_irq_detach:
	if (isp->irq_dispatcher_owner) {
		xil_isp_irq_detach(dev);
		isp->irq_dispatcher_owner = false;
	}
err_media_cleanup:
	media_entity_cleanup(&subdev->entity);
err_ctrl_free:
	v4l2_ctrl_handler_free(&isp->config_ctrls);
err_mutex_destroy:
	mutex_destroy(&isp->lock);
	clk_bulk_disable_unprepare(num_clks, isp->clks);
	return ret;
}

static int isp_remove(struct platform_device *pdev)
{
	struct isp_state *isp = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &isp->subdev;
	int num_clks = ARRAY_SIZE(isp_clks);

	debugfs_remove_recursive(isp->debug_dir);
	isp->debug_dir = NULL;

	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_MASK, ~0U);
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, INT_STATUS, 0);
	if (isp->irq_source_enabled) {
		xil_isp_irq_disable(XIL_ISP_IRQ_SOURCE_ISP);
		isp->irq_source_enabled = false;
	}
	xil_isp_irq_unregister(XIL_ISP_IRQ_SOURCE_ISP, isp);
	if (isp->irq_dispatcher_owner) {
		xil_isp_irq_detach(isp->dev);
		isp->irq_dispatcher_owner = false;
	}

	mutex_lock(&isp->lock);
	INFINITE_ISP_WRITE_REG(isp->isp_base, config, RESET, 1);
	isp->streaming = false;
	mutex_unlock(&isp->lock);

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);
	v4l2_ctrl_handler_free(&isp->config_ctrls);
	mutex_destroy(&isp->lock);
	clk_bulk_disable_unprepare(num_clks, isp->clks);

	return 0;
}

static const struct of_device_id isp_of_id_table[] = {
	{ .compatible = "xlnx,xil-isp-lite-1.0", },
	{ }
};
MODULE_DEVICE_TABLE(of, isp_of_id_table);

static struct platform_driver isp_driver = {
	.driver = {
		.name		= ISP_DRIVER_NAME,
		.of_match_table	= isp_of_id_table,
	},
	.probe			= isp_probe,
	.remove			= isp_remove,
};

module_platform_driver(isp_driver);

MODULE_AUTHOR("xinquan bian <544177215@qq.com>");
MODULE_DESCRIPTION("Xil ISP Lite Driver");
MODULE_LICENSE("GPL v2");
