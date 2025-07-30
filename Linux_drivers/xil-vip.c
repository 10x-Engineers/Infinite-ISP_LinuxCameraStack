// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Xil VIP IP
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
#include "infinite_isp_register.h"
#include "linux/isp_init.h"

/* Register register map */

#define VIP_REG_INT_STATUS_BIT_FRAME_START (1<<0)
#define VIP_REG_INT_STATUS_BIT_FRAME_DONE  (1<<1)

#define VIP_REG_INT_MASK_BIT_FRAME_START   (1<<0)
#define VIP_REG_INT_MASK_BIT_FRAME_DONE    (1<<1)
////////////////////////////////////

#define VIP_PAD_SINK		0
#define VIP_PAD_SOURCE		1
#define VIP_MEDIA_PADS		2


/**
 * struct vip_state - CSI-2 Rx device structure
 * @subdev: The v4l2 subdev structure
 * @format: Active V4L2 formats on each pad
 * @default_format: Default V4L2 format
 * @dev: Platform structure
 * @clks: array of clocks
 * @iomem: Base address of subsystem
 * @lock: mutex for accessing this structure
 * @pads: media pads
 * @streaming: Flag for storing streaming state
 *
 * This structure contains the device driver related parameters
 */
struct vip_state {
	struct v4l2_subdev subdev;
	struct v4l2_mbus_framefmt pad_format[VIP_MEDIA_PADS];
	struct device *dev;
	void __iomem *iomem;
	u32 bits;
	/* used to protect access to this struct */
	struct mutex lock;
	struct media_pad pads[VIP_MEDIA_PADS];
	bool streaming;
};

static inline struct vip_state *
to_vipstate(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct vip_state, subdev);
}

/*
 * Register related operations
 */
static inline u32 vip_read(struct vip_state *vip, u32 addr)
{
	return ioread32(vip->iomem + addr);
}

static inline void vip_write(struct vip_state *vip, u32 addr,
				   u32 value)
{
	iowrite32(value, vip->iomem + addr);
}

static int vip_log_status(struct v4l2_subdev *sd)
{
	struct vip_state *vip = to_vipstate(sd);
	struct device *dev = vip->dev;

	mutex_lock(&vip->lock);

	u32 width  = INFINITE_ISP_VIP_READ_REG(vip->iomem, vip_config, VIP_WIDTH);
	u32 height = INFINITE_ISP_VIP_READ_REG(vip->iomem, vip_config, VIP_HEIGHT);
	u32 bits   = INFINITE_ISP_VIP_READ_REG(vip->iomem, vip_config, VIP_BITS);
	dev_info(dev, "VIP %u x %u YUV%u",
			width, height, bits);

	mutex_unlock(&vip->lock);

	return 0;
}

static int vip_subscribe_event(struct v4l2_subdev *sd, struct v4l2_fh *fh,
			       struct v4l2_event_subscription *sub)
{
	if (sub->type != V4L2_EVENT_FRAME_SYNC)
		return -EINVAL;

	/* V4L2_EVENT_FRAME_SYNC doesn't require an id, so zero should be set */
	if (sub->id != 0)
		return -EINVAL;

	return v4l2_event_subscribe(fh, sub, 0, NULL);
}

static int vip_start_stream(struct vip_state *vip)
{
	struct REG_Infinite_ISP_VIP *infinite_isp_vip;

	infinite_isp_vip = kzalloc(sizeof(*infinite_isp_vip), GFP_KERNEL);
	if (!infinite_isp_vip) {
		dev_err(vip->dev, "Failed to allocate infinite_isp_vip");
		return -ENOMEM;
	}
	u32 in_width   = vip->pad_format[VIP_PAD_SINK].width;
	u32 in_height  = vip->pad_format[VIP_PAD_SINK].height;
	u32 out_width  = vip->pad_format[VIP_PAD_SOURCE].width;
	u32 out_height = vip->pad_format[VIP_PAD_SOURCE].height;
	u32 out_code   = vip->pad_format[VIP_PAD_SOURCE].code;

	u32 scale_h = in_width / out_width;
	u32 scale_v = in_height / out_height;

	u32 top_en = 0;

	if (out_code == MEDIA_BUS_FMT_UYVY8_1X16) {
		infinite_isp_vip->yuvconvformat.YUV444TO422 = 1;
	} else if (out_code == MEDIA_BUS_FMT_VYYUYY8_1X24) {
		infinite_isp_vip->yuvconvformat.YUV444TO422 = 0;
	} else {
		pr_err("Unsupported output format %08X", out_code);
	}
	// if (in_width != out_width || in_height != out_height) {
	// 	top_en |= VIP_REG_TOP_EN_BIT_CROP_EN;
	// }
	// if (scale_h > 1 && scale_v > 1) {
	// 	top_en |= VIP_REG_TOP_EN_BIT_DSCALE_EN;
	// }
	// vip_write(vip, VIP_REG_TOP_EN, top_en);

	// if (top_en & VIP_REG_TOP_EN_BIT_DSCALE_EN) {
	// 	u32 scale_val = scale_h < scale_v ? scale_h : scale_v;
	// 	vip_write(vip, VIP_REG_CROP_X, (in_width-out_width*scale_val)/2);
	// 	vip_write(vip, VIP_REG_CROP_Y, (in_height-out_height*scale_val)/2);
	// 	vip_write(vip, VIP_REG_CROP_W, out_width*scale_val);
	// 	vip_write(vip, VIP_REG_CROP_H, out_height*scale_val);
	// 	vip_write(vip, VIP_REG_DSCALE_H, scale_val-1);
	// 	vip_write(vip, VIP_REG_DSCALE_V, scale_val-1);
	// } else {
	// 	vip_write(vip, VIP_REG_CROP_X, (in_width-out_width)/2);
	// 	vip_write(vip, VIP_REG_CROP_Y, (in_height-out_height)/2);
	// 	vip_write(vip, VIP_REG_CROP_W, out_width);
	// 	vip_write(vip, VIP_REG_CROP_H, out_height);
	// }

	// vip_write(vip, VIP_REG_INT_STATUS, 0);
	// vip_write(vip, VIP_REG_INT_MASK, ~(VIP_REG_INT_MASK_BIT_FRAME_START|VIP_REG_INT_MASK_BIT_FRAME_DONE));
	// vip_write(vip, VIP_REG_RESET, 0);

	vip->streaming = true;

	return 0;
}

static void vip_stop_stream(struct vip_state *vip)
{
	// vip_write(vip, VIP_REG_RESET, 1);
	// vip_write(vip, VIP_REG_INT_MASK, ~0U);
	// vip_write(vip, VIP_REG_INT_STATUS, 0);

	vip->streaming = false;
}

static int vip_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vip_state *vip = to_vipstate(sd);
	int ret = 0;

	mutex_lock(&vip->lock);

	if (enable == vip->streaming) {
		goto stream_done;
	}

	if (enable) {
		ret = vip_start_stream(vip);
	} else {
		vip_stop_stream(vip);
	}

stream_done:
	mutex_unlock(&vip->lock);
	return ret;
}

static void vip_queue_event_frame_sync(struct vip_state *vip, u32 frame_seq)
{
	struct v4l2_event event = {
		.type = V4L2_EVENT_FRAME_SYNC,
	};
	event.u.frame_sync.frame_sequence = frame_seq;

	v4l2_event_queue(vip->subdev.devnode, &event);
}

/**
 * vip_irq_handler - Interrupt handler for CSI-2
 * @irq: IRQ number
 * @data: Pointer to device state
 *
 * In the interrupt handler, a list of event counters are updated for
 * corresponding interrupts. This is useful to get status / debug.
 *
 * Return: IRQ_HANDLED after handling interrupts
 */
// static irqreturn_t vip_irq_handler(int irq, void *data)
// {
// 	struct vip_state *vip = (struct vip_state *)data;
// 	//struct device *dev = vip->dev;
// 	u32 status;

// 	status = vip_read(vip, VIP_REG_INT_STATUS);
// 	vip_write(vip, VIP_REG_INT_STATUS, 0);

// 	if (status & VIP_REG_INT_STATUS_BIT_FRAME_START) {
// 		//dev_info(dev, "IRQ FRAME_START");
// 		vip_queue_event_frame_sync(vip, 0);
// 	}

// 	if (status & VIP_REG_INT_STATUS_BIT_FRAME_DONE) {
// 		//XXX
// 		//dev_info(dev, "IRQ FRAME_DONE");
// 	}

// 	return IRQ_HANDLED;
// }

/**
 * vip_init_cfg - Initialise the pad format config to default
 * @sd: Pointer to V4L2 Sub device structure
 * @cfg: Pointer to sub device pad information structure
 *
 * This function is used to initialize the pad format with the default
 * values.
 *
 * Return: 0 on success
 */
static int vip_init_cfg(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *sd_state)
{
	struct vip_state *vip = to_vipstate(sd);
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	mutex_lock(&vip->lock);
	for (i = 0; i < VIP_MEDIA_PADS; i++) {
		format = v4l2_subdev_get_try_format(sd, sd_state, i);
		*format = vip->pad_format[i];
	}
	mutex_unlock(&vip->lock);

	return 0;
}

static const u32 vip_src_pad_fmts[] = {MEDIA_BUS_FMT_UYVY8_1X16, MEDIA_BUS_FMT_VYYUYY8_1X24, MEDIA_BUS_FMT_RBG888_1X24};

/*
 * vip_enum_mbus_code - Handle pixel format enumeration
 * @sd: pointer to v4l2 subdev structure
 * @cfg: V4L2 subdev pad configuration
 * @code: pointer to v4l2_subdev_mbus_code_enum structure
 *
 * Return: -EINVAL or zero on success
 */
static int vip_enum_mbus_code(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	struct vip_state *vip = to_vipstate(sd);

	if (code->pad == VIP_PAD_SINK && code->index == 0) {
		code->code = vip->pad_format[code->pad].code;
	} else if (code->pad == VIP_PAD_SOURCE && code->index < ARRAY_SIZE(vip_src_pad_fmts)) {
		code->code = vip_src_pad_fmts[code->index];
	} else {
		return -EINVAL;
	}

	return 0;
}

static int vip_enum_frame_size(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *state,
			       struct v4l2_subdev_frame_size_enum *fse)
{
	struct vip_state *vip = to_vipstate(sd);

	if (fse->index > 0 || fse->pad >= VIP_MEDIA_PADS) {
		return -EINVAL;
	}

	if (fse->pad == VIP_PAD_SINK && fse->index == 0) {
		fse->min_width  = vip->pad_format[fse->pad].width;
		fse->min_height = vip->pad_format[fse->pad].height;
		fse->max_width  = vip->pad_format[fse->pad].width;
		fse->max_height = vip->pad_format[fse->pad].height;
	} else if (fse->pad == VIP_PAD_SOURCE && fse->index == 0) {
		fse->min_width  = vip->pad_format[VIP_PAD_SINK].width / 4 / 4 * 4;
		fse->min_height = vip->pad_format[VIP_PAD_SINK].height / 4 / 4 * 4;
		fse->max_width  = vip->pad_format[VIP_PAD_SINK].width;
		fse->max_height = vip->pad_format[VIP_PAD_SINK].height;
	} else {
		return -EINVAL;
	}

	return 0;
}

static struct v4l2_mbus_framefmt *
__vip_get_pad_format(struct vip_state *vip,
			   struct v4l2_subdev_state *sd_state,
			   unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *get_fmt;

	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		get_fmt = v4l2_subdev_get_try_format(&vip->subdev,
						     sd_state, pad);
		break;
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		get_fmt = &vip->pad_format[pad];
		break;
	default:
		get_fmt = NULL;
		break;
	}

	return get_fmt;
}

/**
 * vip_get_format - Get the pad format
 * @sd: Pointer to V4L2 Sub device structure
 * @cfg: Pointer to sub device pad information structure
 * @fmt: Pointer to pad level media bus format
 *
 * This function is used to get the pad format information.
 *
 * Return: 0 on success
 */
static int vip_get_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct vip_state *vip = to_vipstate(sd);
	struct v4l2_mbus_framefmt *get_fmt;
	struct media_pad *pad;
	struct v4l2_subdev *connected_sd;
	int ret = 0;

	mutex_lock(&vip->lock);

	/* For pad 0, get the format from the connected sink pad */
	if (fmt->pad == 0) {
		/* Get the connected pad */
		pad = media_entity_remote_pad(&vip->pads[0]);
		if (!pad) {
			ret = -ENOLINK;
			goto unlock_get_format;
		}

		/* Get the connected subdev */
		connected_sd = media_entity_to_v4l2_subdev(pad->entity);
		if (!connected_sd) {
			ret = -ENODEV;
			dev_err(vip->dev, "No connected subdev for pad %d", fmt->pad);
			goto unlock_get_format;
		}

		/* Get the format from the connected subdev's pad */
		fmt->pad = pad->index;
		ret = v4l2_subdev_call(connected_sd, pad, get_fmt, NULL, fmt);
		if (ret) {
			dev_err(vip->dev, "Failed to get format from connected subdev for pad %d", fmt->pad);
			ret = -EINVAL;
			goto unlock_get_format;
		}

		/* Restore the original pad number for logging */
		fmt->pad = 0;
	} else {
		/* For other pads, get the format normally */
		get_fmt = __vip_get_pad_format(vip, sd_state, fmt->pad,
					     fmt->which);
		if (!get_fmt) {
			ret = -EINVAL;
			goto unlock_get_format;
		}
		fmt->format = *get_fmt;
	}

	dev_info(vip->dev, "VIP pad %d format: code=%08X, width=%u, height=%u",
		fmt->pad, fmt->format.code, fmt->format.width, fmt->format.height);

unlock_get_format:
	mutex_unlock(&vip->lock);

	return ret;
}

static int vip_set_format(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_format *fmt)
{
	struct vip_state *vip = to_vipstate(sd);
	struct v4l2_mbus_framefmt *__format;
	int ret = 0, index = 0;

	mutex_lock(&vip->lock);

	__format = __vip_get_pad_format(vip, sd_state,
					      fmt->pad, fmt->which);
	if (!__format) {
		ret = -EINVAL;
		goto unlock_set_format;
	}

	/* only source pad format can be updated */
	if (fmt->pad != VIP_PAD_SOURCE) {
		fmt->format = *__format;
		goto unlock_set_format;
	}

	for (index = 0; index < ARRAY_SIZE(vip_src_pad_fmts); index ++) {
		if (fmt->format.code == vip_src_pad_fmts[index]) {
			__format->code = fmt->format.code;
			break;
		}
	}
	// if (fmt->format.width <= vip->pad_format[VIP_PAD_SINK].width && fmt->format.height <= vip->pad_format[VIP_PAD_SINK].height) {
	// 	__format->width  = fmt->format.width;
	// 	__format->height = fmt->format.height;
	// }
	__format->width  = fmt->format.width;
	__format->height = fmt->format.height;
	fmt->format = *__format;

	dev_info(vip->dev, "Set format for pad %d: code=%08X, width=%u, height=%u",
		fmt->pad, fmt->format.code, fmt->format.width, fmt->format.height);
unlock_set_format:
	mutex_unlock(&vip->lock);

	return ret;
}

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static const struct media_entity_operations vip_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static const struct v4l2_subdev_core_ops vip_core_ops = {
	.log_status = vip_log_status,
	.subscribe_event = vip_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops vip_video_ops = {
	.s_stream = vip_s_stream,
};

static const struct v4l2_subdev_pad_ops vip_pad_ops = {
	.init_cfg = vip_init_cfg,
	.enum_mbus_code = vip_enum_mbus_code,
	.enum_frame_size = vip_enum_frame_size,
	.get_fmt = vip_get_format,
	.set_fmt = vip_set_format,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops vip_ops = {
	.core = &vip_core_ops,
	.video = &vip_video_ops,
	.pad = &vip_pad_ops
};

static int vip_get_hw_format(struct vip_state *vip)
{
	struct device *dev = vip->dev;
	struct v4l2_mbus_framefmt *format = NULL;

	u32 width  = INFINITE_ISP_VIP_READ_REG(vip->iomem, vip_config, VIP_WIDTH);
	u32 height = INFINITE_ISP_VIP_READ_REG(vip->iomem, vip_config, VIP_HEIGHT);
	u32 bits   = 8;
	if (width < 1 || height < 1 || (bits != 8 && bits != 10 && bits != 12)) {
		dev_err(dev, "Invalid HW formats. Resolution %u x %u, YUV%u",
			width, height, bits);
		return -EINVAL;
	}

	dev_info(dev, "VIP HW formats. Resolution %u x %u, YUV%u",
		width, height, bits);
	
	vip->bits = bits;
	format = &vip->pad_format[VIP_PAD_SINK];

	switch (bits) {
		case 8:   format->code = MEDIA_BUS_FMT_YUV8_1X24;  break;
		case 10:  format->code = MEDIA_BUS_FMT_YUV10_1X30; break;
		default:  format->code = MEDIA_BUS_FMT_YUV12_1X36; break;
	}

	format->field      = V4L2_FIELD_NONE;
	format->colorspace = V4L2_COLORSPACE_SRGB;
	format->width      = width;
	format->height     = height;

	format = &vip->pad_format[VIP_PAD_SOURCE];

	*format = vip->pad_format[VIP_PAD_SINK];
	format->code = MEDIA_BUS_FMT_UYVY8_1X16;

	return 0;
}

static void isp_vip_init(struct REG_Infinite_ISP_VIP* infinite_isp_vip)
{
	unsigned int vip_top_en = 0;
	if(RGBC_EN){
		infinite_isp_vip->vip_config.VIP_TOP_EN.VIP_TOP_EN_RGBC_EN = 1;
	}
	if(IRC_EN){
		infinite_isp_vip->vip_config.VIP_TOP_EN.VIP_TOP_EN_IRC_EN = 1;
	}
	if(SCALE_EN){
		infinite_isp_vip->vip_config.VIP_TOP_EN.VIP_TOP_EN_SCALE_EN = 1;
	}
	if(OSD_EN){
		infinite_isp_vip->vip_config.VIP_TOP_EN.VIP_TOP_EN_OSD_EN = 1;
	}
	if(YUV_CONV_FMT_EN){
		infinite_isp_vip->vip_config.VIP_TOP_EN.VIP_TOP_EN_YUVConvFormat_EN = 1;
	}

	infinite_isp_vip->rgbc.in_conv_standard = rgbc_conv_standard;

	infinite_isp_vip->irc.CROP_X = width_start_idx;
	infinite_isp_vip->irc.CROP_Y = height_start_idx;
	infinite_isp_vip->irc.IRC_OUTPUT = 1;

	infinite_isp_vip->scale.s_in_crop_w = 1920;
	infinite_isp_vip->scale.s_in_crop_h = 1080;
	infinite_isp_vip->scale.s_out_crop_w = 1920;
	infinite_isp_vip->scale.s_out_crop_h = 1080;
	infinite_isp_vip->scale.dscale_w = 1;
	infinite_isp_vip->scale.dscale_h = 1;

	infinite_isp_vip->yuvconvformat.YUV444TO422 = yuv_444_to_422;

	{
		unsigned int osd_x = 16;
		unsigned int osd_y = 16;
		unsigned int osd_w = 128;
		unsigned int osd_h = 64;
		unsigned int osd_color_fg = ( (0x00<<16) | (0x00<<8) | (0xff) );	// R, G, B
		unsigned int osd_color_bg = ( (0xff<<16) | (0xff<<8) | (0xff) );	// R, G, B
		unsigned int osd_alpha = 50;

		infinite_isp_vip->osd.OSD_X = osd_x;
		infinite_isp_vip->osd.OSD_Y = osd_y;
		infinite_isp_vip->osd.OSD_W = osd_w;
		infinite_isp_vip->osd.OSD_H = osd_h;
		infinite_isp_vip->osd.OSD_COLOR_FG_B = 0xff;
		infinite_isp_vip->osd.OSD_COLOR_FG_G = 0x00;
		infinite_isp_vip->osd.OSD_COLOR_FG_R = 0x00;	// R, G, B
		infinite_isp_vip->osd.OSD_COLOR_BG_B = 0xff;
		infinite_isp_vip->osd.OSD_COLOR_BG_G = 0xff;
		infinite_isp_vip->osd.OSD_COLOR_BG_R = 0xff;	// R, G, B
		infinite_isp_vip->osd.ALPHA = osd_alpha;
	}
}

static int vip_initialize_hw(struct vip_state *vip)
{
	struct REG_Infinite_ISP_VIP *infinite_isp_vip;

	infinite_isp_vip = kzalloc(sizeof(*infinite_isp_vip), GFP_KERNEL);
	if (!infinite_isp_vip) {
		return -ENOMEM;
	}

	INFINITE_ISP_VIP_WRITE_REG(vip->iomem, vip_config, VIP_RESET, 1);

	isp_vip_init(infinite_isp_vip);

	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, vip_config, &infinite_isp_vip->vip_config);
	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, rgbc, &infinite_isp_vip->rgbc);
	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, irc, &infinite_isp_vip->irc);
	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, scale, &infinite_isp_vip->scale);
	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, yuvconvformat, &infinite_isp_vip->yuvconvformat);
	INFINITE_ISP_WRITE_VIP_REGs(vip->iomem, osd, &infinite_isp_vip->osd);
	INFINITE_ISP_VIP_WRITE_REG(vip->iomem, vip_config, VIP_RESET, 0);

	return 0;
}

static int vip_probe(struct platform_device *pdev)
{
	struct v4l2_subdev *subdev;
	struct vip_state *vip;
	struct device *dev = &pdev->dev;
	int irq, ret;

	vip = devm_kzalloc(dev, sizeof(*vip), GFP_KERNEL);
	if (!vip) {
		dev_err(dev, "No memory for vip");
		return -ENOMEM;
	}

	vip->dev = dev;

	vip->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vip->iomem)) {
		dev_err(dev, "No iomem resource in DT");
		return PTR_ERR(vip->iomem);
	}

	mutex_init(&vip->lock);

	/* Initialize V4L2 subdevice and media entity */
	vip->pads[VIP_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	vip->pads[VIP_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	/* Initialize the default format */
	ret = vip_get_hw_format(vip);
	if (ret < 0) {
		goto error;
	}

	/* Initialize the vip hardware */
	ret = vip_initialize_hw(vip);
	if (ret < 0) {
		goto error;
	}

	/* Initialize V4L2 subdevice and media entity */
	subdev = &vip->subdev;
	v4l2_subdev_init(subdev, &vip_ops);
	subdev->dev = dev;
	strscpy(subdev->name, dev_name(dev), sizeof(subdev->name));
	subdev->flags |= V4L2_SUBDEV_FL_HAS_EVENTS | V4L2_SUBDEV_FL_HAS_DEVNODE;
	subdev->entity.ops = &vip_media_ops;
	v4l2_set_subdevdata(subdev, vip);

	ret = media_entity_pads_init(&subdev->entity, VIP_MEDIA_PADS,
				     vip->pads);
	if (ret < 0) {
		dev_err(dev, "init media entity pads fail");
		goto error;
	}

	platform_set_drvdata(pdev, vip);

	ret = v4l2_async_register_subdev(subdev);
	if (ret < 0) {
		dev_err(dev, "failed to register subdev\n");
		goto error;
	}

	dev_info(dev, "xil-vip driver probed!");

	return 0;
error:
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&vip->lock);
	return ret;
}

static int vip_remove(struct platform_device *pdev)
{
	struct vip_state *vip = platform_get_drvdata(pdev);
	struct v4l2_subdev *subdev = &vip->subdev;

	v4l2_async_unregister_subdev(subdev);
	media_entity_cleanup(&subdev->entity);
	mutex_destroy(&vip->lock);

	return 0;
}

static const struct of_device_id vip_of_id_table[] = {
	{ .compatible = "xlnx,xil-vip-1.0", },
	{ }
};
MODULE_DEVICE_TABLE(of, vip_of_id_table);

static struct platform_driver vip_driver = {
	.driver = {
		.name		= "xil-vip",
		.of_match_table	= vip_of_id_table,
	},
	.probe			= vip_probe,
	.remove			= vip_remove,
};

module_platform_driver(vip_driver);

MODULE_AUTHOR("xinquan bian <544177215@qq.com>");
MODULE_DESCRIPTION("Xil VIP Driver");
MODULE_LICENSE("GPL v2");
