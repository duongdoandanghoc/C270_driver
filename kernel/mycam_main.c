// SPDX-License-Identifier: GPL-2.0
/*
 * mycam_main.c — Module Entry Point & USB Driver Lifecycle
 *
 * Điểm vào của kernel module mycam.ko. Đăng ký USB driver với kernel,
 * xử lý probe() khi camera được cắm vào, và disconnect() khi rút ra.
 * Thực hiện UVC Probe/Commit và chọn alt setting theo chuẩn kernel.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. mycam_uvc_ctrl_msg()    — wrapper gửi UVC control request
 *   2. mycam_uvc_probe_commit()— negotiate format với camera
 *   3. mycam_select_alt()      — chọn alt setting tối thiểu đủ bandwidth
 *   4. mycam_probe()           — USB device probe (camera cắm vào)
 *   5. mycam_disconnect()      — USB disconnect (camera rút ra)
 *   6. mycam_init/exit()       — module_init / module_exit
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   Kernel USB subsystem
 *       │  device match (VID:PID)
 *       ▼
 *   ┌──────────────────────────────┐
 *   │  mycam_probe()  ◄────────────│── FILE NÀY
 *   │    kzalloc mycam_device      │
 *   │    v4l2_device_register()    │
 *   │    mycam_vb2_init()          │
 *   │    mycam_video_init()        │
 *   │    → /dev/videoX xuất hiện   │
 *   └──────────────────────────────┘
 *
 *   Khi STREAMON:
 *       mycam_uvc_probe_commit()   — negotiate MJPEG format
 *       mycam_select_alt()         — chọn alt setting
 *       mycam_urb_alloc_and_submit() (mycam_vb2.c → mycam_urb.c)
 */

#include "mycam.h"
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("C270 Driver Project");
MODULE_DESCRIPTION("Custom UVC V4L2 driver for Logitech C270");
MODULE_VERSION("0.1");

/* ── USB device ID table ── */
static const struct usb_device_id mycam_id_table[] = {
	{ USB_DEVICE(MYCAM_VID, MYCAM_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, mycam_id_table);

/*
 * mycam_uvc_ctrl_msg — Gửi UVC VideoStreaming control request
 *
 * TÁC DỤNG:
 *   Wrapper cho usb_control_msg() theo format UVC class request.
 *   Dùng để Probe (GET_CUR/SET_CUR) và Commit (SET_CUR) format.
 *
 * TÁC ĐỘNG:
 *   Gửi synchronous USB control transfer tới camera.
 *   data buffer bị overwrite với response (nếu là GET request).
 *
 * CONTEXT:
 *   Process context (start_streaming). Có thể sleep.
 *
 * @cam:       device context
 * @request:   UVC_SET_CUR hoặc UVC_GET_CUR
 * @selector:  UVC_VS_PROBE_CONTROL hoặc UVC_VS_COMMIT_CONTROL
 * @data:      buffer chứa mycam_uvc_probe struct
 * @size:      sizeof(*data)
 * @return:    0 OK, < 0 lỗi
 */
static int mycam_uvc_ctrl_msg(struct mycam_device *cam,
			      u8 request, u8 selector,
			      void *data, u16 size)
{
	u8  type;
	int ret;

	/*
	 * LƯU Ý: data buffer PHẢI là kmalloc'd (DMA-safe).
	 * Kernel >=5.x: usb_control_msg() WARN nếu buffer nằm trên
	 * stack hoặc vmalloc space → gây EAGAIN hoặc data corruption.
	 * Caller chịu trách nhiệm đảm bảo buffer hợp lệ.
	 */

	/* UVC VideoStreaming interface request */
	if (request == UVC_SET_CUR)
		type = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	else
		type = USB_DIR_IN  | USB_TYPE_CLASS | USB_RECIP_INTERFACE;

	ret = usb_control_msg(cam->udev,
			      (request == UVC_SET_CUR)
				? usb_sndctrlpipe(cam->udev, 0)
				: usb_rcvctrlpipe(cam->udev, 0),
			      request,
			      type,
			      (u16)(selector << 8),  /* wValue */
			      MYCAM_STREAM_IFACE,    /* wIndex */
			      data, size,
			      5000);  /* 5s timeout */

	if (ret < 0)
		dev_err(&cam->udev->dev,
			"UVC ctrl msg req=0x%02x sel=0x%02x: %d\n",
			request, selector, ret);
	else
		ret = 0;  /* usb_control_msg trả bytes transferred khi OK → normalize về 0 */
	return ret;
}

/*
 * mycam_uvc_probe_commit — UVC Probe/Commit để negotiate MJPEG format
 *
 * TÁC DỤNG:
 *   1. SET_CUR PROBE: đề xuất format MJPEG 640x480 với camera
 *   2. GET_CUR PROBE: đọc lại response (camera có thể điều chỉnh)
 *   3. SET_CUR COMMIT: xác nhận format đã negotiate
 *
 * TÁC ĐỘNG:
 *   - Camera chuyển vào trạng thái sẵn sàng stream
 *   - probe_out->dwMaxPayloadTransferSize chứa băng thông cần thiết
 *     → dùng để chọn alt setting
 *
 * CONTEXT:
 *   Process context (start_streaming). Có thể sleep.
 *
 * @cam:       device context (đã có width, height, pixfmt)
 * @probe_out: output — chứa kết quả negotiate (đặc biệt dwMaxPayloadTransferSize)
 * @return:    0 OK, < 0 lỗi
 */
int mycam_uvc_probe_commit(struct mycam_device *cam,
			   struct mycam_uvc_probe *probe_out)
{
	struct mycam_uvc_probe *probe;
	int ret;

	/*
	 * usb_control_msg() yêu cầu buffer phải DMA-safe (kmalloc'd).
	 * Kernel >=5.x WARN + fail nếu buffer nằm trên stack.
	 * → Dùng kmalloc thay vì local variable.
	 */
	probe = kmalloc(sizeof(*probe), GFP_KERNEL);
	if (!probe)
		return -ENOMEM;

	memset(probe, 0, sizeof(*probe));
	probe->bmHint        = cpu_to_le16(0x01);  /* suggest dwFrameInterval */
	probe->bFormatIndex  = 1;  /* MJPEG format index (C270: index 1) */
	probe->bFrameIndex   = 1;  /* 640x480 frame index */
	probe->dwFrameInterval = cpu_to_le32(333333); /* 30fps = 10^7/30 = 333333 */

	/* Step 1: Propose format to camera */
	ret = mycam_uvc_ctrl_msg(cam, UVC_SET_CUR, UVC_VS_PROBE_CONTROL,
				 probe, MYCAM_UVC_PROBE_SZ);
	if (ret < 0)
		goto out_free;

	/* Step 2: Read back negotiated values */
	memset(probe, 0, sizeof(*probe));
	ret = mycam_uvc_ctrl_msg(cam, UVC_GET_CUR, UVC_VS_PROBE_CONTROL,
				 probe, MYCAM_UVC_PROBE_SZ);
	if (ret < 0)
		goto out_free;

	dev_info(&cam->udev->dev,
		 "UVC probe: dwMaxVideoFrameSize=%u dwMaxPayloadTransferSize=%u\n",
		 le32_to_cpu(probe->dwMaxVideoFrameSize),
		 le32_to_cpu(probe->dwMaxPayloadTransferSize));

	/* Step 3: Commit — lock in negotiated format */
	ret = mycam_uvc_ctrl_msg(cam, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL,
				 probe, MYCAM_UVC_PROBE_SZ);
	if (ret < 0)
		goto out_free;

	*probe_out = *probe;

out_free:
	kfree(probe);
	return ret;
}

/*
 * mycam_select_alt — Chọn USB alt setting tối thiểu đủ bandwidth
 *
 * TÁC DỤNG:
 *   Duyệt tất cả altsettings của streaming interface, tìm alt nhỏ nhất
 *   có effective wMaxPacketSize >= max_payload (dwMaxPayloadTransferSize
 *   từ UVC Probe/Commit).
 *
 *   "Nhỏ nhất đủ bandwidth" = tránh chiếm USB bus quá mức, giảm tearing.
 *   Đây là cách ĐÚNG theo chuẩn — KHÔNG ép alt lớn bừa bãi.
 *
 * TÁC ĐỘNG:
 *   - Gọi usb_set_interface() để switch alt setting
 *   - cam->alt_setting cập nhật
 *   - cam->max_pkt_size cập nhật
 *
 * CONTEXT:
 *   Process context. Có thể sleep.
 *
 * @cam:         device context
 * @max_payload: băng thông tối thiểu cần thiết (bytes/packet)
 * @return:      0 OK, -ENODEV nếu không tìm được alt phù hợp
 */
int mycam_select_alt(struct mycam_device *cam, u32 max_payload)
{
	struct usb_interface *intf;
	struct usb_host_interface *alts;
	int i, best_alt = -1;
	u16 best_pkt = 0xFFFF;

	intf = cam->intf;
	/* Find streaming interface */
	if (intf->cur_altsetting->desc.bInterfaceNumber != MYCAM_STREAM_IFACE) {
		dev_err(&cam->udev->dev, "Wrong interface number\n");
		return -EINVAL;
	}

	for (i = 0; i < intf->num_altsetting; i++) {
		struct usb_endpoint_descriptor *ep;
		u16 pkt_raw, pkt_size, mult, effective;

		alts = &intf->altsetting[i];

		/* Alt 0 = idle (no endpoint), skip */
		if (alts->desc.bNumEndpoints == 0)
			continue;

		ep = &alts->endpoint[0].desc;

		/* Only consider ISO IN endpoint */
		if (!usb_endpoint_is_isoc_in(ep))
			continue;

		pkt_raw  = le16_to_cpu(ep->wMaxPacketSize);
		pkt_size = pkt_raw & 0x07FF;
		mult     = ((pkt_raw >> 11) & 0x03) + 1;  /* 1, 2, or 3 */
		effective = pkt_size * mult;

		dev_dbg(&cam->udev->dev,
			"alt%d: pkt_size=%u mult=%u effective=%u\n",
			alts->desc.bAlternateSetting, pkt_size, mult, effective);

		/* Chọn alt nhỏ nhất đủ bandwidth */
		if (effective >= (u16)max_payload && pkt_size < best_pkt) {
			best_alt = alts->desc.bAlternateSetting;
			best_pkt = pkt_size;
			cam->max_pkt_size = effective;
		}
	}

	if (best_alt < 0) {
		dev_err(&cam->udev->dev,
			"No alt setting with pkt >= %u bytes\n", max_payload);
		return -ENODEV;
	}

	dev_info(&cam->udev->dev,
		 "Selected alt%d (effective_pkt=%u, need=%u)\n",
		 best_alt, cam->max_pkt_size, max_payload);

	cam->alt_setting = (u8)best_alt;
	return usb_set_interface(cam->udev, MYCAM_STREAM_IFACE, best_alt);
}

/*
 * mycam_probe — USB probe callback: camera được cắm vào
 *
 * TÁC DỤNG:
 *   Được gọi bởi USB core khi VID:PID match. Cấp phát mycam_device,
 *   đăng ký V4L2 device và video_device → /dev/videoX xuất hiện.
 *   Camera chưa stream ở đây — chỉ khởi tạo infrastructure.
 *
 * TÁC ĐỘNG:
 *   - Alloc mycam_device (kzalloc)
 *   - v4l2_device_register() — đăng ký vào V4L2 subsystem
 *   - mycam_vb2_init()       — khởi tạo VB2 queue
 *   - mycam_video_init()     — tạo /dev/videoX
 *   - usb_set_intfdata()     — lưu driver data
 *
 * CONTEXT:
 *   Process context. Có thể sleep.
 *
 * @intf:  USB interface (streaming interface được probe)
 * @id:    USB device ID entry đã match
 * @return: 0 OK, < 0 lỗi (USB core sẽ không bind driver)
 */
static int mycam_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct mycam_device *cam;
	int ret;

	/* Chỉ xử lý streaming interface (bInterfaceNumber == 1) */
	if (intf->cur_altsetting->desc.bInterfaceNumber != MYCAM_STREAM_IFACE) {
		dev_dbg(&intf->dev, "Skipping control interface\n");
		return -ENODEV;
	}

	dev_info(&udev->dev, "mycam: Logitech C270 detected (VID:%04x PID:%04x)\n",
		 id->idVendor, id->idProduct);

	/* Alloc device context */
	cam = kzalloc(sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->udev   = usb_get_dev(udev);
	cam->intf   = intf;
	cam->width  = MYCAM_DEF_WIDTH;
	cam->height = MYCAM_DEF_HEIGHT;
	cam->pixfmt = V4L2_PIX_FMT_MJPEG;

	spin_lock_init(&cam->buf_lock);
	mutex_init(&cam->vdev_lock);
	mutex_init(&cam->vb2_lock);
	INIT_LIST_HEAD(&cam->buf_list);

	/* Register V4L2 device */
	ret = v4l2_device_register(&intf->dev, &cam->v4l2_dev);
	if (ret) {
		dev_err(&udev->dev, "v4l2_device_register failed: %d\n", ret);
		goto err_free;
	}

	/* Init VB2 queue */
	ret = mycam_vb2_init(cam);
	if (ret) {
		dev_err(&udev->dev, "vb2 init failed: %d\n", ret);
		goto err_v4l2;
	}

	/* Register video device → /dev/videoX */
	ret = mycam_video_init(cam);
	if (ret) {
		dev_err(&udev->dev, "video init failed: %d\n", ret);
		goto err_v4l2;
	}

	/* Init V4L2 controls (brightness, contrast, exposure, etc.) */
	ret = mycam_ctrl_init(cam);
	if (ret) {
		dev_err(&udev->dev, "ctrl init failed: %d\n", ret);
		goto err_video;
	}

	usb_set_intfdata(intf, cam);

	dev_info(&udev->dev, "mycam: registered as /dev/video%d\n",
		 cam->vdev.num);
	return 0;

err_video:
	mycam_video_cleanup(cam);
err_v4l2:
	v4l2_device_unregister(&cam->v4l2_dev);
err_free:
	usb_put_dev(udev);
	kfree(cam);
	return ret;
}

/*
 * mycam_disconnect — USB disconnect callback: camera bị rút ra
 *
 * TÁC DỤNG:
 *   Được gọi khi camera bị rút hoặc rmmod. Dừng streaming nếu đang
 *   chạy, hủy đăng ký V4L2 device, giải phóng resources.
 *
 * TÁC ĐỘNG:
 *   - cam->streaming = false → URB completion handler sẽ dừng
 *   - mycam_video_cleanup() — video_unregister_device()
 *   - v4l2_device_unregister()
 *   - usb_put_dev() + kfree(cam)
 *
 * CONTEXT:
 *   Process context. Có thể sleep.
 */
static void mycam_disconnect(struct usb_interface *intf)
{
	struct mycam_device *cam = usb_get_intfdata(intf);

	if (!cam)
		return;

	dev_info(&cam->udev->dev, "mycam: disconnecting\n");

	/* Prevent new URB completions delivering frames */
	cam->streaming = false;

	mycam_ctrl_cleanup(cam);
	mycam_video_cleanup(cam);
	v4l2_device_unregister(&cam->v4l2_dev);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(cam->udev);
	kfree(cam);

	pr_info("mycam: device removed\n");
}

/* ── USB driver registration ── */
static struct usb_driver mycam_usb_driver = {
	.name       = MYCAM_MODULE_NAME,
	.id_table   = mycam_id_table,
	.probe      = mycam_probe,
	.disconnect = mycam_disconnect,
};

/*
 * mycam_init — module_init: đăng ký USB driver
 *
 * TÁC DỤNG: usb_register() → kernel bắt đầu match VID:PID
 * CONTEXT:  Kernel init / insmod
 */
static int __init mycam_init(void)
{
	int ret = usb_register(&mycam_usb_driver);
	if (ret)
		pr_err("mycam: usb_register failed: %d\n", ret);
	else
		pr_info("mycam: driver registered (VID:0x%04x PID:0x%04x)\n",
			MYCAM_VID, MYCAM_PID);
	return ret;
}

/*
 * mycam_exit — module_exit: hủy đăng ký USB driver
 *
 * TÁC DỤNG: usb_deregister() → kernel không match VID:PID nữa
 * CONTEXT:  rmmod
 */
static void __exit mycam_exit(void)
{
	usb_deregister(&mycam_usb_driver);
	pr_info("mycam: driver unregistered\n");
}

module_init(mycam_init);
module_exit(mycam_exit);
