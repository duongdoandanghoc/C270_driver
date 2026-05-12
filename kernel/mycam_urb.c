// SPDX-License-Identifier: GPL-2.0
/*
 * mycam_urb.c — URB Management & UVC Payload Header Parsing
 *
 * Trái tim của driver: quản lý USB isochronous transfers, parse
 * UVC payload header, tích lũy payload thành MJPEG frame hoàn chỉnh,
 * deliver frame tới VB2 queue.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. mycam_frame_done()         — deliver MJPEG frame → VB2
 *   2. mycam_urb_complete()       — ISR: parse UVC header, gom payload
 *   3. mycam_urb_alloc_and_submit()— alloc URB pool + submit
 *   4. mycam_urb_kill_and_free()  — kill + free URB pool
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   USB C270 Camera
 *       │  isochronous packets (8000 pkt/s)
 *       ▼
 *   ┌───────────────────────────────────────┐
 *   │  USB Host Controller (HW)             │
 *   └──────────────┬────────────────────────┘
 *                  │  URB completion interrupt
 *                  ▼
 *   ┌───────────────────────────────────────┐
 *   │  mycam_urb_complete()  ◄──────────────│── FILE NÀY (softirq)
 *   │                                       │
 *   │  for each iso packet:                 │
 *   │    UVC header: hle=data[0], bmi=data[1]│
 *   │    FID bit (bit 0): toggle = new frame│
 *   │    ERR bit (bit 6): skip if set       │
 *   │    EOF bit (bit 1): force deliver     │
 *   │    payload = data[hle..len]           │
 *   │    → mycam_frame_acc                  │
 *   │    → if FID toggle: mycam_frame_done()│
 *   │                                       │
 *   │  re-submit URB (GFP_ATOMIC)           │
 *   └──────────────┬────────────────────────┘
 *                  │  vb2_buffer_done(VB2_BUF_STATE_DONE)
 *                  ▼
 *   ┌───────────────────────────────────────┐
 *   │  VB2 Queue → done_list               │
 *   │  wake up poll() / VIDIOC_DQBUF        │
 *   └──────────────┬────────────────────────┘
 *                  │
 *                  ▼
 *   Userspace: c270_app V4L2 pipeline
 *
 * ══════════════════════════════════════════════════════════════
 * UVC PAYLOAD HEADER FORMAT (UVC 1.1 §2.4.3.3)
 * ══════════════════════════════════════════════════════════════
 *
 *   Byte 0: HLE  — Header Length Extension (= tổng length của header)
 *   Byte 1: bmHeaderInfo bits:
 *     bit 0: FID  — Frame ID (toggle mỗi frame mới)
 *     bit 1: EOF  — End of Frame
 *     bit 2: PTS  — Presentation Time Stamp present
 *     bit 3: SCR  — Source Clock Reference present
 *     bit 4: RES  — Reserved
 *     bit 5: STI  — Still Image
 *     bit 6: ERR  — Error (bỏ qua packet này)
 *     bit 7: EOH  — End of Header
 *   Bytes 2..HLE-1: optional PTS, SCR fields
 *   Bytes HLE..len-1: MJPEG payload data
 */

#include "mycam.h"
#include <linux/slab.h>
#include <linux/usb.h>

/* UVC bmHeaderInfo bit masks */
#define UVC_HDR_FID  BIT(0)  /* Frame ID */
#define UVC_HDR_EOF  BIT(1)  /* End of Frame */
#define UVC_HDR_ERR  BIT(6)  /* Error */

/*
 * mycam_frame_done — Deliver MJPEG frame hoàn chỉnh tới VB2
 *
 * TÁC DỤNG:
 *   Lấy buffer đầu tiên từ cam->buf_list (đã enqueue bởi userspace),
 *   copy dữ liệu từ frame_acc vào VB2 plane buffer, set metadata
 *   (timestamp, sequence, field), rồi gọi vb2_buffer_done(DONE)
 *   để đánh thức userspace đang block trên DQBUF/poll.
 *
 * TÁC ĐỘNG:
 *   - Lấy + xóa 1 entry từ cam->buf_list (spinlock protected)
 *   - memcpy frame_acc.data → VB2 vmalloc buffer
 *   - vb2_buffer_done() → buffer chuyển sang done_list
 *   - frame_acc reset (size=0, has_data=false)
 *   - Nếu buf_list rỗng: frame bị DROP (counter++)
 *
 * CONTEXT:
 *   Gọi từ mycam_urb_complete() — softirq context.
 *   KHÔNG được sleep. Chỉ dùng spinlock_irqsave.
 *
 * @cam: device context chứa buf_list và frame_acc
 */
static void mycam_frame_done(struct mycam_device *cam)
{
	struct mycam_buffer *buf;
	unsigned long flags;
	void *vbuf_data;

	/* Lấy buffer từ queue — spinlock bảo vệ buf_list */
	spin_lock_irqsave(&cam->buf_lock, flags);
	if (list_empty(&cam->buf_list)) {
		/* Không có buffer — drop frame */
		cam->frame_acc.dropped++;
		spin_unlock_irqrestore(&cam->buf_lock, flags);
		goto reset_acc;
	}
	buf = list_first_entry(&cam->buf_list, struct mycam_buffer, list);
	list_del(&buf->list);
	spin_unlock_irqrestore(&cam->buf_lock, flags);

	/* Copy MJPEG data vào VB2 buffer (vmalloc, luôn accessible từ softirq) */
	vbuf_data = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	if (vbuf_data && cam->frame_acc.size > 0) {
		memcpy(vbuf_data, cam->frame_acc.data, cam->frame_acc.size);
		vb2_set_plane_payload(&buf->vb.vb2_buf, 0, cam->frame_acc.size);
	}

	/* Set metadata */
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.sequence          = cam->frame_acc.sequence++;
	buf->vb.field             = V4L2_FIELD_NONE;

	/* Deliver buffer to VB2 — wakes up userspace DQBUF/poll */
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

reset_acc:
	cam->frame_acc.size     = 0;
	cam->frame_acc.has_data = false;
}

/*
 * mycam_urb_complete — URB completion callback (ISR)
 *
 * TÁC DỤNG:
 *   Được gọi bởi USB Host Controller khi một isochronous URB hoàn thành.
 *   Duyệt từng iso packet trong URB:
 *     1. Bỏ qua packet lỗi (status != 0 hoặc len < 2)
 *     2. Parse UVC payload header (HLE + bmHeaderInfo)
 *     3. Bỏ qua nếu ERR bit set
 *     4. Detect frame boundary: FID toggle → deliver current frame
 *     5. Tích lũy payload vào frame_acc
 *     6. Nếu EOF bit → deliver frame ngay
 *   Sau đó re-submit URB để tiếp tục nhận packets.
 *
 * TÁC ĐỘNG:
 *   - Ghi vào cam->frame_acc (data, size, last_fid, has_data)
 *   - Gọi mycam_frame_done() → lấy buf từ buf_list → vb2_buffer_done()
 *   - usb_submit_urb(GFP_ATOMIC) để re-submit
 *
 * CONTEXT:
 *   Softirq/interrupt context. KHÔNG được:
 *   - sleep / wait
 *   - gọi mutex_lock (chỉ spinlock_irqsave)
 *   - kmalloc(GFP_KERNEL)
 *   Chỉ dùng GFP_ATOMIC nếu cần alloc (không cần trong thiết kế này).
 *
 * @urb: URB đã hoàn thành, chứa iso_frame_desc[] + transfer_buffer
 */
static void mycam_urb_complete(struct urb *urb)
{
	struct mycam_device *cam = urb->context;
	int i, offset = 0;

	/* Kiểm tra streaming state (READ_ONCE để tránh compiler reorder) */
	if (!READ_ONCE(cam->streaming))
		return;

	/* Xử lý URB-level error */
	if (urb->status < 0) {
		switch (urb->status) {
		case -ENOENT:
		case -ECONNRESET:
		case -ESHUTDOWN:
			/* URB bị kill (stop_streaming) — không re-submit */
			return;
		default:
			/* Lỗi tạm thời (EOVERFLOW, v.v.) — re-submit */
			goto resubmit;
		}
	}

	/* Duyệt từng iso packet trong URB */
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *pkt = &urb->iso_frame_desc[i];
		u8 *data = (u8 *)urb->transfer_buffer + offset;
		int actual = pkt->actual_length;

		offset += pkt->length;  /* advance bằng nominal length */

		/* Bỏ qua packet lỗi hoặc quá ngắn để có header */
		if (pkt->status != 0 || actual < 2)
			continue;

		/* Parse UVC payload header */
		u8 hle = data[0];  /* Header Length Extension */
		u8 bmi = data[1];  /* bmHeaderInfo */
		u8 fid = bmi & UVC_HDR_FID;
		u8 eof = bmi & UVC_HDR_EOF;
		u8 err = bmi & UVC_HDR_ERR;

		/* Reject malformed header (hle > actual) */
		if (hle < 2 || hle > actual)
			continue;

		/* Bỏ qua packet có ERR bit */
		if (err)
			continue;

		/* FID toggle → frame boundary → deliver current frame */
		if (cam->frame_acc.has_data &&
		    fid != cam->frame_acc.last_fid) {
			mycam_frame_done(cam);
		}
		cam->frame_acc.last_fid = fid;

		/* Tích lũy payload */
		{
			u8 *payload     = data + hle;
			int payload_len = actual - hle;

			if (payload_len > 0) {
				u32 avail = cam->frame_acc.max_size
					    - cam->frame_acc.size;
				u32 copy  = (u32)payload_len < avail
					    ? (u32)payload_len : avail;
				if (copy > 0) {
					memcpy(cam->frame_acc.data
					       + cam->frame_acc.size,
					       payload, copy);
					cam->frame_acc.size += copy;
					cam->frame_acc.has_data = true;
				}
			}
		}

		/* EOF bit → deliver frame ngay (không đợi FID toggle) */
		if (eof && cam->frame_acc.has_data)
			mycam_frame_done(cam);
	}

resubmit:
	/* Re-submit URB để tiếp tục nhận packets */
	if (READ_ONCE(cam->streaming)) {
		int ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret && ret != -ENODEV)
			dev_err_ratelimited(&cam->udev->dev,
					    "re-submit URB: %d\n", ret);
	}
}

/*
 * mycam_urb_alloc_and_submit — Cấp phát và submit URB pool
 *
 * TÁC DỤNG:
 *   Alloc MYCAM_NUM_URBS isochronous URBs, mỗi URB có MYCAM_ISO_PKTS
 *   iso packets. Submit tất cả lên USB Host Controller để bắt đầu
 *   nhận isochronous data từ camera.
 *
 * TÁC ĐỘNG:
 *   - cam->urbs[] populated
 *   - cam->urb_bufs[] allocated (kmalloc GFP_KERNEL)
 *   - URBs submitted → mycam_urb_complete() sẽ được gọi
 *
 * CONTEXT:
 *   Process context (start_streaming). Có thể sleep.
 *
 * @cam: device context (phải có max_pkt_size và alt_setting đã set)
 * @return: 0 OK, < 0 lỗi
 */
int mycam_urb_alloc_and_submit(struct mycam_device *cam)
{
	int i, j, ret;
	u32 buf_size;
	u8  ep_addr = MYCAM_ISO_EP_ADDR;

	/* Tính kích thước transfer buffer = packets × packet_size */
	buf_size = MYCAM_ISO_PKTS * cam->max_pkt_size;
	cam->urb_buf_size = buf_size;

	for (i = 0; i < MYCAM_NUM_URBS; i++) {
		struct urb *urb;
		u8 *buf;

		/* Alloc transfer buffer */
		buf = kmalloc(buf_size, GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto err_free;
		}
		cam->urb_bufs[i] = buf;

		/* Alloc URB */
		urb = usb_alloc_urb(MYCAM_ISO_PKTS, GFP_KERNEL);
		if (!urb) {
			ret = -ENOMEM;
			goto err_free;
		}
		cam->urbs[i] = urb;

		/* Điền URB fields */
		urb->dev             = cam->udev;
		urb->pipe            = usb_rcvisocpipe(cam->udev, ep_addr);
		urb->transfer_flags  = URB_ISO_ASAP;
		urb->transfer_buffer = buf;
		urb->transfer_buffer_length = buf_size;
		urb->number_of_packets      = MYCAM_ISO_PKTS;
		urb->complete        = mycam_urb_complete;
		urb->context         = cam;
		urb->interval        = 1;  /* micro-frame interval */

		/* Set iso packet lengths */
		for (j = 0; j < MYCAM_ISO_PKTS; j++) {
			urb->iso_frame_desc[j].offset = j * cam->max_pkt_size;
			urb->iso_frame_desc[j].length = cam->max_pkt_size;
		}

		/* Submit URB */
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret) {
			dev_err(&cam->udev->dev,
				"usb_submit_urb[%d] failed: %d\n", i, ret);
			goto err_free;
		}
	}

	dev_info(&cam->udev->dev,
		 "mycam: %d URBs submitted (buf_size=%u, pkts=%d)\n",
		 MYCAM_NUM_URBS, buf_size, MYCAM_ISO_PKTS);
	return 0;

err_free:
	/* Kill và free URBs đã alloc */
	for (j = 0; j < i; j++) {
		if (cam->urbs[j]) {
			usb_kill_urb(cam->urbs[j]);
			usb_free_urb(cam->urbs[j]);
			cam->urbs[j] = NULL;
		}
		kfree(cam->urb_bufs[j]);
		cam->urb_bufs[j] = NULL;
	}
	/* Free current (chưa submit) */
	kfree(cam->urb_bufs[i]);
	cam->urb_bufs[i] = NULL;
	return ret;
}

/*
 * mycam_urb_kill_and_free — Kill và free toàn bộ URB pool
 *
 * TÁC DỤNG:
 *   Gọi usb_kill_urb() cho mỗi URB (block cho đến khi completion
 *   handler kết thúc), sau đó free URB struct và transfer buffer.
 *
 * TÁC ĐỘNG:
 *   - cam->streaming phải = false TRƯỚC khi gọi hàm này
 *   - Sau khi return: không có URB nào còn in-flight
 *   - cam->urbs[] và cam->urb_bufs[] được zeroed
 *
 * CONTEXT:
 *   Process context (stop_streaming). Có thể sleep (usb_kill_urb blocks).
 *
 * @cam: device context
 */
void mycam_urb_kill_and_free(struct mycam_device *cam)
{
	int i;

	for (i = 0; i < MYCAM_NUM_URBS; i++) {
		if (cam->urbs[i]) {
			usb_kill_urb(cam->urbs[i]);   /* đợi completion */
			usb_free_urb(cam->urbs[i]);
			cam->urbs[i] = NULL;
		}
		kfree(cam->urb_bufs[i]);
		cam->urb_bufs[i] = NULL;
	}
	dev_info(&cam->udev->dev, "mycam: all URBs killed and freed\n");
}
