// SPDX-License-Identifier: GPL-2.0
/*
 * mycam_video.c — V4L2 Video Device Registration & IOCTL Handlers
 *
 * Đăng ký video_device với V4L2 subsystem → tạo /dev/videoX.
 * Implement các IOCTL cần thiết: QUERYCAP, ENUM_FMT, S/G_FMT,
 * REQBUFS, QBUF, DQBUF, STREAMON/OFF (phần lớn delegate tới VB2).
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. mycam_querycap()       — VIDIOC_QUERYCAP
 *   2. mycam_enum_fmt()       — VIDIOC_ENUM_FMT (MJPEG)
 *   3. mycam_g_fmt()          — VIDIOC_G_FMT
 *   4. mycam_s_fmt()          — VIDIOC_S_FMT (validate + set)
 *   5. mycam_try_fmt()        — VIDIOC_TRY_FMT
 *   6. mycam_video_init()     — tạo và đăng ký video_device
 *   7. mycam_video_cleanup()  — hủy đăng ký video_device
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   Userspace: open("/dev/videoX")
 *       │  VIDIOC_QUERYCAP, S_FMT, REQBUFS, STREAMON, DQBUF
 *       ▼
 *   ┌────────────────────────────────┐
 *   │  mycam_video.c  ◄──────────────│── FILE NÀY
 *   │    v4l2_ioctl_ops              │
 *   │    video_device registered     │
 *   └──────────┬─────────────────────┘
 *              │  VB2 ops (REQBUFS, STREAMON, DQBUF)
 *              ▼
 *   mycam_vb2.c → mycam_urb.c → camera
 */

#include "mycam.h"
#include <linux/slab.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-fh.h>

/* ── Supported formats ── */
static const struct v4l2_fmtdesc mycam_formats[] = {
	{
		.index       = 0,
		.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.flags       = V4L2_FMT_FLAG_COMPRESSED,
		.description = "Motion-JPEG",
		.pixelformat = V4L2_PIX_FMT_MJPEG,
	},
};

/*
 * mycam_querycap — VIDIOC_QUERYCAP
 *
 * TÁC DỤNG:
 *   Trả về thông tin driver cho userspace (tên, bus, capabilities).
 *   Bắt buộc phải implement.
 *
 * TÁC ĐỘNG:
 *   Populate cap struct — không thay đổi state
 * CONTEXT: Process context (ioctl)
 */
static int mycam_querycap(struct file *file, void *priv,
			  struct v4l2_capability *cap)
{
	struct mycam_device *cam = video_drvdata(file);

	strscpy(cap->driver, MYCAM_MODULE_NAME, sizeof(cap->driver));
	strscpy(cap->card, "Logitech C270 (mycam)", sizeof(cap->card));
	usb_make_path(cam->udev, cap->bus_info, sizeof(cap->bus_info));

	cap->device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

/*
 * mycam_enum_fmt — VIDIOC_ENUM_FMT
 *
 * TÁC DỤNG:
 *   Liệt kê các pixel format hỗ trợ. Driver chỉ hỗ trợ MJPEG.
 * CONTEXT: Process context
 */
static int mycam_enum_fmt(struct file *file, void *priv,
			  struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(mycam_formats))
		return -EINVAL;
	*f = mycam_formats[f->index];
	return 0;
}

/*
 * mycam_fill_fmt — Điền thông tin format cho MJPEG 640x480
 *
 * TÁC DỤNG: Helper nội bộ populate v4l2_pix_format
 */
static void mycam_fill_fmt(struct mycam_device *cam,
			   struct v4l2_pix_format *pix)
{
	pix->width        = cam->width;
	pix->height       = cam->height;
	pix->pixelformat  = V4L2_PIX_FMT_MJPEG;
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = 0;           /* compressed: no stride */
	pix->sizeimage    = MYCAM_MAX_FRAME_SZ;
	pix->colorspace   = V4L2_COLORSPACE_JPEG;
}

/*
 * mycam_g_fmt — VIDIOC_G_FMT
 *
 * TÁC DỤNG: Trả về format hiện tại của camera
 * CONTEXT: Process context
 */
static int mycam_g_fmt(struct file *file, void *priv,
		       struct v4l2_format *f)
{
	struct mycam_device *cam = video_drvdata(file);

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	mycam_fill_fmt(cam, &f->fmt.pix);
	return 0;
}

/*
 * mycam_try_fmt — VIDIOC_TRY_FMT
 *
 * TÁC DỤNG:
 *   Validate format mà không thay đổi state. Chỉ accept MJPEG,
 *   force về 640x480 nếu resolution khác.
 * CONTEXT: Process context
 */
static int mycam_try_fmt(struct file *file, void *priv,
			 struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/* Chỉ hỗ trợ MJPEG — các format khác bị reject */
	if (pix->pixelformat != V4L2_PIX_FMT_MJPEG) {
		pix->pixelformat = V4L2_PIX_FMT_MJPEG;
	}

	/* Clamp về 640x480 (resolution duy nhất hỗ trợ trong Phase B) */
	pix->width       = clamp_t(u32, pix->width,  1, MYCAM_DEF_WIDTH);
	pix->height      = clamp_t(u32, pix->height, 1, MYCAM_DEF_HEIGHT);
	pix->width       = MYCAM_DEF_WIDTH;
	pix->height      = MYCAM_DEF_HEIGHT;
	pix->field       = V4L2_FIELD_NONE;
	pix->bytesperline = 0;
	pix->sizeimage   = MYCAM_MAX_FRAME_SZ;
	pix->colorspace  = V4L2_COLORSPACE_JPEG;
	return 0;
}

/*
 * mycam_s_fmt — VIDIOC_S_FMT
 *
 * TÁC DỤNG:
 *   Set format. Chỉ được gọi khi chưa streaming.
 *   Validate rồi update cam->width/height/pixfmt.
 *
 * TÁC ĐỘNG:
 *   cam->width, cam->height, cam->pixfmt updated
 *
 * CONTEXT: Process context (ioctl, serialized bởi vb2_lock)
 */
static int mycam_s_fmt(struct file *file, void *priv,
		       struct v4l2_format *f)
{
	struct mycam_device *cam = video_drvdata(file);
	int ret;

	/* Không cho phép đổi format khi đang stream */
	if (vb2_is_busy(&cam->vb2_queue))
		return -EBUSY;

	ret = mycam_try_fmt(file, priv, f);
	if (ret)
		return ret;

	cam->width  = f->fmt.pix.width;
	cam->height = f->fmt.pix.height;
	cam->pixfmt = f->fmt.pix.pixelformat;
	return 0;
}

/*
 * mycam_enum_input — VIDIOC_ENUMINPUT
 *
 * TÁC DỤNG: Liệt kê input sources. Camera có 1 input (USB camera).
 * CONTEXT: Process context
 */
static int mycam_enum_input(struct file *file, void *priv,
			    struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;

	inp->type   = V4L2_INPUT_TYPE_CAMERA;
	inp->status = 0;
	strscpy(inp->name, "Camera", sizeof(inp->name));
	return 0;
}

/*
 * mycam_g_input / mycam_s_input
 *
 * TÁC DỤNG: Get/Set input (chỉ có input 0, phải implement để comply V4L2)
 * CONTEXT: Process context
 */
static int mycam_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int mycam_s_input(struct file *file, void *priv, unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

/* ── IOCTL ops table ── */
static const struct v4l2_ioctl_ops mycam_ioctl_ops = {
	.vidioc_querycap      = mycam_querycap,
	.vidioc_enum_fmt_vid_cap = mycam_enum_fmt,
	.vidioc_g_fmt_vid_cap = mycam_g_fmt,
	.vidioc_s_fmt_vid_cap = mycam_s_fmt,
	.vidioc_try_fmt_vid_cap = mycam_try_fmt,
	.vidioc_enum_input    = mycam_enum_input,
	.vidioc_g_input       = mycam_g_input,
	.vidioc_s_input       = mycam_s_input,

	/* VB2 helpers — delegate tới vb2_ioctl_* */
	.vidioc_reqbufs       = vb2_ioctl_reqbufs,
	.vidioc_querybuf      = vb2_ioctl_querybuf,
	.vidioc_qbuf          = vb2_ioctl_qbuf,
	.vidioc_dqbuf         = vb2_ioctl_dqbuf,
	.vidioc_prepare_buf   = vb2_ioctl_prepare_buf,
	.vidioc_create_bufs   = vb2_ioctl_create_bufs,
	.vidioc_streamon      = vb2_ioctl_streamon,
	.vidioc_streamoff     = vb2_ioctl_streamoff,
	.vidioc_expbuf        = vb2_ioctl_expbuf,
};

/* ── File operations ── */
static const struct v4l2_file_operations mycam_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/*
 * mycam_video_init — Tạo và đăng ký video_device
 *
 * TÁC DỤNG:
 *   Khởi tạo struct video_device, set ioctl_ops và fops,
 *   gọi video_register_device() để tạo /dev/videoX.
 *
 * TÁC ĐỘNG:
 *   - cam->vdev initialized
 *   - /dev/videoX được tạo ra trong /dev
 *
 * CONTEXT: Process context (probe)
 *
 * @cam: device context (đã có v4l2_dev và vb2_queue)
 * @return: 0 OK, < 0 lỗi
 */
int mycam_video_init(struct mycam_device *cam)
{
	struct video_device *vdev = &cam->vdev;
	int ret;

	strscpy(vdev->name, "Logitech C270 (mycam)", sizeof(vdev->name));
	vdev->v4l2_dev   = &cam->v4l2_dev;
	vdev->fops       = &mycam_fops;
	vdev->ioctl_ops  = &mycam_ioctl_ops;
	vdev->release    = video_device_release_empty;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	vdev->lock       = &cam->vdev_lock;
	vdev->queue      = &cam->vb2_queue;

	video_set_drvdata(vdev, cam);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		dev_err(&cam->udev->dev,
			"video_register_device failed: %d\n", ret);
		return ret;
	}

	dev_info(&cam->udev->dev, "mycam: video device registered\n");
	return 0;
}

/*
 * mycam_video_cleanup — Hủy đăng ký video_device
 *
 * TÁC DỤNG:
 *   Gọi video_unregister_device() → /dev/videoX biến mất.
 *   Userspace app đang mở sẽ nhận lỗi khi gọi ioctl tiếp theo.
 *
 * TÁC ĐỘNG:
 *   - /dev/videoX bị xóa
 *   - Không free cam ở đây (do mycam_disconnect làm)
 *
 * CONTEXT: Process context (disconnect)
 */
void mycam_video_cleanup(struct mycam_device *cam)
{
	video_unregister_device(&cam->vdev);
	dev_info(&cam->udev->dev, "mycam: video device unregistered\n");
}
