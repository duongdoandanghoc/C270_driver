// SPDX-License-Identifier: GPL-2.0
/*
 * mycam_vb2.c — VB2 Queue Operations
 *
 * Implement vb2_ops cho mycam: quản lý buffer lifecycle từ alloc
 * (REQBUFS) đến enqueue (QBUF) đến start/stop streaming.
 * VB2 framework tự lo MMAP — driver chỉ cần implement vb2_ops.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. mycam_queue_setup()    — VIDIOC_REQBUFS: size và count
 *   2. mycam_buf_init()       — buffer allocation (VB2 gọi)
 *   3. mycam_buf_queue()      — VIDIOC_QBUF: thêm buffer vào list
 *   4. mycam_start_streaming()— VIDIOC_STREAMON: URB submit
 *   5. mycam_stop_streaming() — VIDIOC_STREAMOFF: URB kill
 *   6. mycam_vb2_init()       — khởi tạo vb2_queue
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   Userspace: VIDIOC_REQBUFS → VIDIOC_QBUF × N → VIDIOC_STREAMON
 *       │
 *       ▼
 *   ┌────────────────────────────────┐
 *   │  mycam_vb2.c  ◄───────────────│── FILE NÀY
 *   │    queue_setup → VB2 alloc     │
 *   │    buf_queue → buf_list        │
 *   │    start_streaming → URBs      │
 *   └──────────┬─────────────────────┘
 *              │ mycam_urb_alloc_and_submit()
 *              ▼
 *   mycam_urb.c → USB HC → Camera
 *
 *   Ngược chiều (khi frame hoàn chỉnh):
 *   mycam_urb.c → vb2_buffer_done() → userspace DQBUF unblock
 */

#include "mycam.h"
#include <linux/slab.h>

/*
 * mycam_queue_setup — Xác định số buffer và kích thước
 *
 * TÁC DỤNG:
 *   VB2 gọi khi userspace thực hiện VIDIOC_REQBUFS.
 *   Driver báo cho VB2 biết cần 1 plane với kích thước MYCAM_MAX_FRAME_SZ.
 *
 * TÁC ĐỘNG:
 *   VB2 sẽ alloc *nbuffers MMAP buffers, mỗi buffer = sizes[0] bytes.
 *   *nbuffers có thể bị tăng lên nếu yêu cầu < 2.
 *
 * CONTEXT: Process context (ioctl)
 *
 * @q:         vb2_queue
 * @nbuffers:  in/out — số buffers yêu cầu, driver có thể tăng lên
 * @nplanes:   output — số planes (= 1 cho MJPEG)
 * @sizes:     output — kích thước mỗi plane (bytes)
 * @alloc_devs: không dùng (vmalloc allocator)
 */
static int mycam_queue_setup(struct vb2_queue *q,
			     unsigned int *nbuffers,
			     unsigned int *nplanes,
			     unsigned int sizes[],
			     struct device *alloc_devs[])
{
	/* Cần ít nhất 2 buffers để tránh stall */
	if (*nbuffers < 2)
		*nbuffers = 2;

	/* 1 plane chứa toàn bộ MJPEG frame */
	*nplanes = 1;
	sizes[0] = MYCAM_MAX_FRAME_SZ;

	return 0;
}

/*
 * mycam_buf_prepare — Validate buffer trước khi queue
 *
 * TÁC DỤNG:
 *   Kiểm tra plane size hợp lệ trước khi buffer được enqueue.
 *
 * CONTEXT: Process context (QBUF)
 */
static int mycam_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < MYCAM_MAX_FRAME_SZ) {
		pr_err("mycam: buffer too small: %lu < %u\n",
		       vb2_plane_size(vb, 0), MYCAM_MAX_FRAME_SZ);
		return -EINVAL;
	}
	vb2_set_plane_payload(vb, 0, 0);  /* reset payload size */
	return 0;
}

/*
 * mycam_buf_queue — Enqueue buffer vào driver's buf_list
 *
 * TÁC DỤNG:
 *   Được gọi khi userspace thực hiện VIDIOC_QBUF.
 *   Thêm buffer vào cam->buf_list để URB completion handler
 *   có thể lấy ra và fill với MJPEG frame.
 *
 * TÁC ĐỘNG:
 *   - buf->list được thêm vào tail của cam->buf_list
 *   - spinlock bảo vệ vì buf_list được access từ IRQ context
 *
 * CONTEXT:
 *   Process context (QBUF ioctl). Spinlock nên KHÔNG sleep.
 */
static void mycam_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct mycam_buffer *buf = container_of(vbuf, struct mycam_buffer, vb);
	struct mycam_device *cam = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long flags;

	spin_lock_irqsave(&cam->buf_lock, flags);
	list_add_tail(&buf->list, &cam->buf_list);
	spin_unlock_irqrestore(&cam->buf_lock, flags);
}

/*
 * mycam_start_streaming — VIDIOC_STREAMON
 *
 * TÁC DỤNG:
 *   1. Khởi tạo frame accumulator (kmalloc data buffer)
 *   2. Thực hiện UVC Probe/Commit → lấy dwMaxPayloadTransferSize
 *   3. Chọn alt setting tối thiểu đủ bandwidth
 *   4. usb_set_interface() → camera bắt đầu gửi iso packets
 *   5. Alloc và submit URBs
 *
 * TÁC ĐỘNG:
 *   - cam->frame_acc.data allocated
 *   - cam->streaming = true
 *   - URBs in flight → mycam_urb_complete() sẽ được gọi
 *
 * CONTEXT: Process context (ioctl). Có thể sleep.
 *
 * @q:     vb2_queue
 * @count: số buffers đã enqueue (không dùng trực tiếp)
 * @return: 0 OK, < 0 lỗi
 */
static int mycam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mycam_device *cam = vb2_get_drv_priv(q);
	struct mycam_uvc_probe probe;
	int ret;

	/* Alloc frame accumulator buffer (1 lần duy nhất) */
	cam->frame_acc.max_size = MYCAM_MAX_FRAME_SZ;
	cam->frame_acc.data = kmalloc(MYCAM_MAX_FRAME_SZ, GFP_KERNEL);
	if (!cam->frame_acc.data) {
		ret = -ENOMEM;
		goto err_return_bufs;
	}
	cam->frame_acc.size     = 0;
	cam->frame_acc.has_data = false;
	cam->frame_acc.last_fid = 0xFF;  /* invalid → first packet always "new" */
	cam->frame_acc.sequence = 0;
	cam->frame_acc.dropped  = 0;

	/* UVC Probe/Commit: negotiate MJPEG format */
	ret = mycam_uvc_probe_commit(cam, &probe);
	if (ret) {
		dev_err(&cam->udev->dev, "UVC probe/commit failed: %d\n", ret);
		goto err_free_acc;
	}

	/* Chọn alt setting theo chuẩn kernel */
	ret = mycam_select_alt(cam, le32_to_cpu(probe.dwMaxPayloadTransferSize));
	if (ret) {
		dev_err(&cam->udev->dev, "alt select failed: %d\n", ret);
		goto err_free_acc;
	}

	/* Bắt đầu URBs */
	cam->streaming = true;
	ret = mycam_urb_alloc_and_submit(cam);
	if (ret) {
		cam->streaming = false;
		dev_err(&cam->udev->dev, "URB alloc/submit failed: %d\n", ret);
		goto err_alt0;
	}

	dev_info(&cam->udev->dev, "mycam: streaming started\n");
	return 0;

err_alt0:
	usb_set_interface(cam->udev, MYCAM_STREAM_IFACE, 0);
err_free_acc:
	kfree(cam->frame_acc.data);
	cam->frame_acc.data = NULL;
err_return_bufs:
	/* Trả lại tất cả buffers đã enqueue với ERROR state */
	{
		struct mycam_buffer *buf, *tmp;
		unsigned long flags;
		spin_lock_irqsave(&cam->buf_lock, flags);
		list_for_each_entry_safe(buf, tmp, &cam->buf_list, list) {
			list_del(&buf->list);
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_QUEUED);
		}
		spin_unlock_irqrestore(&cam->buf_lock, flags);
	}
	return ret;
}

/*
 * mycam_stop_streaming — VIDIOC_STREAMOFF
 *
 * TÁC DỤNG:
 *   1. Set cam->streaming = false → URB completion handler dừng submit
 *   2. usb_kill_urb() × N → đợi tất cả URBs hoàn thành
 *   3. usb_set_interface(alt=0) → idle, giải phóng USB bandwidth
 *   4. Free URBs và accumulator buffer
 *   5. Trả lại tất cả buffers còn trong buf_list với VB2_BUF_STATE_ERROR
 *
 * TÁC ĐỘNG:
 *   - cam->streaming = false
 *   - Tất cả URBs killed và freed
 *   - cam->frame_acc.data freed
 *   - buf_list drained (ERROR state)
 *
 * CONTEXT: Process context (ioctl). Có thể sleep.
 */
static void mycam_stop_streaming(struct vb2_queue *q)
{
	struct mycam_device *cam = vb2_get_drv_priv(q);
	struct mycam_buffer *buf, *tmp;
	unsigned long flags;

	/* Signal URB completion handler to stop */
	cam->streaming = false;

	/* Kill + free URBs */
	mycam_urb_kill_and_free(cam);

	/* Idle alt setting: release USB bandwidth */
	usb_set_interface(cam->udev, MYCAM_STREAM_IFACE, 0);

	/* Free accumulator */
	kfree(cam->frame_acc.data);
	cam->frame_acc.data = NULL;

	dev_info(&cam->udev->dev,
		 "mycam: streaming stopped (frames=%u dropped=%u)\n",
		 cam->frame_acc.sequence, cam->frame_acc.dropped);

	/* Return all queued buffers to VB2 with ERROR */
	spin_lock_irqsave(&cam->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &cam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&cam->buf_lock, flags);
}

/* ── VB2 ops table ── */
static const struct vb2_ops mycam_vb2_ops = {
	.queue_setup     = mycam_queue_setup,
	.buf_prepare     = mycam_buf_prepare,
	.buf_queue       = mycam_buf_queue,
	.start_streaming = mycam_start_streaming,
	.stop_streaming  = mycam_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/*
 * mycam_vb2_init — Khởi tạo VB2 queue
 *
 * TÁC DỤNG:
 *   Cấu hình và khởi tạo vb2_queue cho MMAP capture.
 *   Dùng vmalloc allocator (phù hợp cho MJPEG, không cần DMA coherent).
 *
 * TÁC ĐỘNG:
 *   cam->vb2_queue được khởi tạo, sẵn sàng nhận REQBUFS
 *
 * CONTEXT: Process context (probe)
 */
int mycam_vb2_init(struct mycam_device *cam)
{
	struct vb2_queue *q = &cam->vb2_queue;
	int ret;

	q->type            = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes        = VB2_MMAP | VB2_READ;
	q->drv_priv        = cam;
	q->buf_struct_size = sizeof(struct mycam_buffer);
	q->ops             = &mycam_vb2_ops;
	q->mem_ops         = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock            = &cam->vb2_lock;
	q->dev             = &cam->udev->dev;

	ret = vb2_queue_init(q);
	if (ret)
		dev_err(&cam->udev->dev, "vb2_queue_init failed: %d\n", ret);
	return ret;
}
