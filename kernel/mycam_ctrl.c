// SPDX-License-Identifier: GPL-2.0
/*
 * mycam_ctrl.c — V4L2 Control Handler for Logitech C270
 *
 * Implement V4L2 controls (brightness, contrast, saturation, gain,
 * sharpness, exposure) bằng cách gửi UVC Processing Unit và
 * Camera Terminal control requests qua USB control endpoint.
 *
 * ══════════════════════════════════════════════════════════════
 * UVC CONTROL ARCHITECTURE
 * ══════════════════════════════════════════════════════════════
 *
 *   V4L2 control framework (v4l2_ctrl_handler)
 *       │  v4l2_ctrl_new_std()
 *       ▼
 *   mycam_ctrl_s_ctrl()
 *       │  UVC SET_CUR request
 *       ▼
 *   ┌────────────────────────────────────────────────┐
 *   │  Camera C270 UVC Units:                        │
 *   │    Processing Unit (ID=2): brightness, contrast│
 *   │    Camera Terminal (ID=1): exposure, auto_exp   │
 *   └────────────────────────────────────────────────┘
 *
 * UVC control requests go to interface 0 (VideoControl),
 * NOT interface 1 (VideoStreaming).
 *
 * ══════════════════════════════════════════════════════════════
 * C270 UNIT IDs (from lsusb -v)
 * ══════════════════════════════════════════════════════════════
 *   Input Terminal (Camera): bTerminalID = 1
 *   Processing Unit:         bUnitID     = 2
 *   Output Terminal:         bTerminalID = 3
 */

#include "mycam.h"
#include <linux/slab.h>
#include <media/v4l2-ctrls.h>

/* ── UVC Unit IDs (C270 specific, from USB descriptors) ── */
#define UVC_PU_ID           2   /* Processing Unit */
#define UVC_CT_ID           1   /* Camera Terminal (Input Terminal) */

/* ── UVC Control Selectors ── */
/* Processing Unit controls (UVC 1.5 spec Table 4-2) */
#define UVC_PU_BRIGHTNESS_CONTROL           0x02
#define UVC_PU_CONTRAST_CONTROL             0x03
#define UVC_PU_SATURATION_CONTROL           0x07
#define UVC_PU_SHARPNESS_CONTROL            0x08
#define UVC_PU_GAIN_CONTROL                 0x04
#define UVC_PU_GAMMA_CONTROL                0x09

/* Camera Terminal controls (UVC 1.5 spec Table 4-7) */
#define UVC_CT_AE_MODE_CONTROL              0x02
#define UVC_CT_EXPOSURE_TIME_ABS_CONTROL    0x04

/* ── UVC Request codes ── */
#define UVC_RC_SET_CUR          0x01
#define UVC_RC_GET_CUR          0x81
#define UVC_RC_GET_MIN          0x82
#define UVC_RC_GET_MAX          0x83
#define UVC_RC_GET_DEF          0x87

/*
 * mycam_uvc_unit_ctrl — Send UVC control request to a specific Unit
 *
 * TÁC DỤNG:
 *   Gửi SET_CUR hoặc GET_CUR request tới Processing Unit hoặc
 *   Camera Terminal. Khác với mycam_uvc_ctrl_msg (gửi tới VS interface),
 *   hàm này gửi tới VC interface (interface 0).
 *
 * NOTE:
 *   Buffer PHẢI là kmalloc'd (DMA-safe). Stack buffer bị cấm.
 *   wValue = selector << 8
 *   wIndex = unit_id << 8 | VC_interface_number
 *
 * CONTEXT: Process context (ioctl handler). Có thể sleep.
 */
static int mycam_uvc_unit_ctrl(struct mycam_device *cam,
			       u8 request, u8 unit_id, u8 selector,
			       void *data, u16 size)
{
	u8  type;
	int ret;

	if (request == UVC_RC_SET_CUR)
		type = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	else
		type = USB_DIR_IN  | USB_TYPE_CLASS | USB_RECIP_INTERFACE;

	ret = usb_control_msg(cam->udev,
			      (request == UVC_RC_SET_CUR)
				? usb_sndctrlpipe(cam->udev, 0)
				: usb_rcvctrlpipe(cam->udev, 0),
			      request,
			      type,
			      (u16)(selector << 8),      /* wValue */
			      (u16)(unit_id << 8) | MYCAM_CTRL_IFACE, /* wIndex */
			      data, size,
			      5000);  /* 5s timeout */

	if (ret < 0)
		dev_err(&cam->udev->dev,
			"UVC unit ctrl unit=%d sel=0x%02x req=0x%02x: %d\n",
			unit_id, selector, request, ret);
	else
		ret = 0;  /* normalize byte count → 0 */

	return ret;
}

/*
 * mycam_uvc_set_le16 — Set a 16-bit LE UVC control value
 *
 * Most C270 Processing Unit controls are 16-bit signed LE.
 * This helper allocates DMA-safe buffer, writes value, sends SET_CUR.
 */
static int mycam_uvc_set_le16(struct mycam_device *cam,
			      u8 unit_id, u8 selector, s16 value)
{
	__le16 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	*buf = cpu_to_le16(value);
	ret = mycam_uvc_unit_ctrl(cam, UVC_RC_SET_CUR, unit_id, selector,
				  buf, sizeof(*buf));
	kfree(buf);
	return ret;
}

/*
 * mycam_uvc_set_u8 — Set an 8-bit UVC control value
 *
 * Auto Exposure mode is a single byte control.
 */
static int mycam_uvc_set_u8(struct mycam_device *cam,
			    u8 unit_id, u8 selector, u8 value)
{
	u8 *buf;
	int ret;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	*buf = value;
	ret = mycam_uvc_unit_ctrl(cam, UVC_RC_SET_CUR, unit_id, selector,
				  buf, sizeof(*buf));
	kfree(buf);
	return ret;
}

/*
 * mycam_ctrl_s_ctrl — v4l2_ctrl_ops .s_ctrl callback
 *
 * TÁC DỤNG:
 *   Được gọi bởi V4L2 control framework khi userspace set control value
 *   (VIDIOC_S_CTRL / VIDIOC_S_EXT_CTRLS).
 *
 * TÁC ĐỘNG:
 *   Gửi UVC SET_CUR request tới camera hardware qua USB control pipe.
 *
 * CONTEXT: Process context (ioctl, serialized bởi ctrl handler lock)
 */
static int mycam_ctrl_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mycam_device *cam = container_of(ctrl->handler,
						struct mycam_device,
						ctrl_handler);

	switch (ctrl->id) {
	/* ── Processing Unit controls (16-bit LE) ── */
	case V4L2_CID_BRIGHTNESS:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_BRIGHTNESS_CONTROL,
					  (s16)ctrl->val);
	case V4L2_CID_CONTRAST:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_CONTRAST_CONTROL,
					  (s16)ctrl->val);
	case V4L2_CID_SATURATION:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_SATURATION_CONTROL,
					  (s16)ctrl->val);
	case V4L2_CID_SHARPNESS:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_SHARPNESS_CONTROL,
					  (s16)ctrl->val);
	case V4L2_CID_GAIN:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_GAIN_CONTROL,
					  (s16)ctrl->val);
	case V4L2_CID_GAMMA:
		return mycam_uvc_set_le16(cam, UVC_PU_ID,
					  UVC_PU_GAMMA_CONTROL,
					  (s16)ctrl->val);

	/* ── Camera Terminal controls ── */
	case V4L2_CID_EXPOSURE_AUTO:
		/*
		 * V4L2: 0=AUTO, 1=MANUAL, 2=SHUTTER_PRIORITY, 3=APERTURE_PRIORITY
		 * UVC:  0x01=Manual, 0x02=Auto, 0x04=Shutter, 0x08=Aperture
		 * C270 hỗ trợ: Manual (1) và Aperture Priority (8)
		 */
		{
			u8 uvc_ae;
			switch (ctrl->val) {
			case V4L2_EXPOSURE_MANUAL:
				uvc_ae = 0x01; break;
			case V4L2_EXPOSURE_APERTURE_PRIORITY:
			default:
				uvc_ae = 0x08; break;
			}
			return mycam_uvc_set_u8(cam, UVC_CT_ID,
						UVC_CT_AE_MODE_CONTROL, uvc_ae);
		}
	case V4L2_CID_EXPOSURE_ABSOLUTE:
		/*
		 * V4L2: exposure in 100µs units
		 * UVC:  same unit (dwExposureTimeAbsolute)
		 * C270 range: 9..625 (= 0.9ms..62.5ms)
		 */
		return mycam_uvc_set_le16(cam, UVC_CT_ID,
					  UVC_CT_EXPOSURE_TIME_ABS_CONTROL,
					  (s16)ctrl->val);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops mycam_ctrl_ops = {
	.s_ctrl = mycam_ctrl_s_ctrl,
};

/*
 * mycam_ctrl_init — Initialize V4L2 control handler
 *
 * TÁC DỤNG:
 *   Tạo v4l2_ctrl_handler, đăng ký các controls với range/default
 *   lấy từ C270 USB descriptor (xác nhận qua v4l2-ctl trên uvcvideo).
 *
 * TÁC ĐỘNG:
 *   cam->ctrl_handler initialized, controls registered
 *   vdev->ctrl_handler được gán
 *
 * CONTEXT: Process context (probe, sau video_init)
 *
 * @cam: device context
 * @return: 0 OK, < 0 lỗi
 */
int mycam_ctrl_init(struct mycam_device *cam)
{
	struct v4l2_ctrl_handler *hdl = &cam->ctrl_handler;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);

	/* ── Processing Unit Controls ── */
	/* Ranges lấy từ: v4l2-ctl -d /dev/video0 --list-ctrls (uvcvideo) */

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_BRIGHTNESS, -64, 64, 1, 0);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_CONTRAST, 0, 95, 1, 0);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_SATURATION, 0, 100, 1, 64);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_SHARPNESS, 1, 7, 1, 2);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_GAIN, 1, 8, 1, 1);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_GAMMA, 100, 300, 1, 100);

	/* ── Camera Terminal Controls ── */
	v4l2_ctrl_new_std_menu(hdl, &mycam_ctrl_ops,
			       V4L2_CID_EXPOSURE_AUTO,
			       V4L2_EXPOSURE_APERTURE_PRIORITY,
			       0,  /* mask: tất cả modes available */
			       V4L2_EXPOSURE_APERTURE_PRIORITY);

	v4l2_ctrl_new_std(hdl, &mycam_ctrl_ops,
			  V4L2_CID_EXPOSURE_ABSOLUTE, 9, 625, 1, 156);

	if (hdl->error) {
		ret = hdl->error;
		dev_err(&cam->udev->dev,
			"ctrl handler init failed: %d\n", ret);
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	cam->vdev.ctrl_handler = hdl;

	dev_info(&cam->udev->dev,
		 "mycam: V4L2 controls registered\n");
	return 0;
}

/*
 * mycam_ctrl_cleanup — Free V4L2 control handler
 *
 * CONTEXT: Process context (disconnect)
 */
void mycam_ctrl_cleanup(struct mycam_device *cam)
{
	v4l2_ctrl_handler_free(&cam->ctrl_handler);
}
