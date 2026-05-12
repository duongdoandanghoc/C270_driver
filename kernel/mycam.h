/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mycam.h — Shared Data Structures & Constants for mycam.ko
 *
 * Header dùng chung cho tất cả file .c trong module mycam.ko.
 * Định nghĩa device context, VB2 buffer wrapper, UVC structs,
 * và frame accumulator.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC MODULE
 * ══════════════════════════════════════════════════════════════
 *
 *   mycam_main.c   — module_init/exit, usb_driver probe/disconnect
 *   mycam_video.c  — video_device, v4l2_ioctl_ops, v4l2_file_ops
 *   mycam_vb2.c    — vb2_ops: queue_setup, buf_queue, start/stop
 *   mycam_urb.c    — URB alloc/submit/complete, UVC header parse
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   USB C270 Camera
 *       │  isochronous packets (8000 pkt/s, alt setting do kernel chọn)
 *       ▼
 *   ┌────────────────────────────────┐
 *   │  mycam_urb.c (softirq)         │
 *   │    parse UVC header            │
 *   │    FID toggle → frame boundary │
 *   │    accumulate → frame_acc      │
 *   └──────────┬─────────────────────┘
 *              │ vb2_buffer_done()
 *              ▼
 *   ┌────────────────────────────────┐
 *   │  VB2 Queue (mycam_vb2.c)       │
 *   │    MMAP buffers                │
 *   └──────────┬─────────────────────┘
 *              │ VIDIOC_DQBUF
 *              ▼
 *   /dev/videoX → Userspace (c270_app V4L2 pipeline)
 */

#ifndef MYCAM_H
#define MYCAM_H

#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

/* ── Device identity ── */
#define MYCAM_VID              0x046d
#define MYCAM_PID              0x0825
#define MYCAM_MODULE_NAME      "mycam"

/* ── USB interface numbers (C270 UVC descriptor) ── */
#define MYCAM_CTRL_IFACE       0   /* VideoControl interface */
#define MYCAM_STREAM_IFACE     1   /* VideoStreaming interface */
#define MYCAM_ISO_EP_ADDR      0x81 /* isochronous IN endpoint */

/* ── URB configuration ── */
#define MYCAM_NUM_URBS         8    /* concurrent URBs in flight */
#define MYCAM_ISO_PKTS         32   /* iso packets per URB */

/* ── Frame / buffer sizing ── */
#define MYCAM_DEF_WIDTH        640
#define MYCAM_DEF_HEIGHT       480
/* MJPEG worst case: ~0.5 byte/pixel heuristic với safety margin */
#define MYCAM_MAX_FRAME_SZ     (640 * 480 * 2)

/* ── UVC Probe/Commit control (UVC 1.1, 26 bytes) ── */
struct mycam_uvc_probe {
	__le16 bmHint;
	__u8   bFormatIndex;
	__u8   bFrameIndex;
	__le32 dwFrameInterval;
	__le16 wKeyFrameRate;
	__le16 wPFrameRate;
	__le16 wCompQuality;
	__le16 wCompWindowSize;
	__le16 wDelay;
	__le32 dwMaxVideoFrameSize;
	__le32 dwMaxPayloadTransferSize;
	/* UVC 1.1 fields beyond 26 bytes not needed here */
} __packed;

#define MYCAM_UVC_PROBE_SZ     26

/* UVC control selectors */
#define UVC_VS_PROBE_CONTROL   0x01
#define UVC_VS_COMMIT_CONTROL  0x02

/* UVC request types */
#define UVC_SET_CUR  0x01
#define UVC_GET_CUR  0x81

/* ── VB2 buffer wrapper ──
 * Embeds vb2_v4l2_buffer + linked list node for buf_list management
 */
struct mycam_buffer {
	struct vb2_v4l2_buffer  vb;    /* MUST be first member */
	struct list_head        list;  /* link in cam->buf_list */
};

/* ── Frame accumulator ──
 * Tích lũy iso payload thành MJPEG frame hoàn chỉnh.
 * Chỉ alloc/free tại start/stop_streaming (không trong IRQ).
 */
struct mycam_frame_acc {
	u8    *data;       /* kmalloc'd buffer, kích thước max_size */
	u32    size;       /* bytes tích lũy hiện tại */
	u32    max_size;   /* MYCAM_MAX_FRAME_SZ */
	u8     last_fid;   /* FID bit của packet trước — detect toggle */
	bool   has_data;   /* có dữ liệu đang tích lũy không */
	u32    sequence;   /* frame sequence counter cho VB2 */
	u32    dropped;    /* số frames bị drop do thiếu buffer */
};

/* ── Per-device context (mycam_device) ──
 * Trung tâm của module: chứa toàn bộ state của một camera instance.
 * Được alloc bởi mycam_probe(), free bởi mycam_disconnect() qua kref.
 */
struct mycam_device {
	/* USB references */
	struct usb_device       *udev;
	struct usb_interface    *intf;

	/* V4L2 subsystem */
	struct v4l2_device       v4l2_dev;
	struct video_device      vdev;       /* /dev/videoX */
	struct mutex             vdev_lock;  /* serialize v4l2 ops (non-IRQ) */

	/* VB2 queue */
	struct vb2_queue         vb2_queue;
	struct mutex             vb2_lock;   /* vb2_queue lock (ioctl serializer) */

	/* Buffer list — queued buffers from VIDIOC_QBUF */
	struct list_head         buf_list;
	spinlock_t               buf_lock;   /* IRQ-safe, protects buf_list */

	/* URB pool */
	struct urb              *urbs[MYCAM_NUM_URBS];
	u8                      *urb_bufs[MYCAM_NUM_URBS]; /* kmalloc'd transfer bufs */
	u32                      urb_buf_size; /* packets * max_pkt_size */
	u16                      max_pkt_size; /* effective wMaxPacketSize of chosen alt */
	u8                       alt_setting;  /* USB alt setting in use */

	/* Frame accumulator */
	struct mycam_frame_acc   frame_acc;

	/* Stream state — check with READ_ONCE/WRITE_ONCE if accessed from IRQ */
	bool                     streaming;

	/* Format (negotiated at S_FMT) */
	u32                      width;
	u32                      height;
	u32                      pixfmt;     /* V4L2_PIX_FMT_MJPEG */
	u32                      sequence;   /* frame counter for userspace */
};

/* ── Function declarations — per-file interfaces ── */

/* mycam_video.c */
int  mycam_video_init(struct mycam_device *cam);
void mycam_video_cleanup(struct mycam_device *cam);

/* mycam_vb2.c */
int  mycam_vb2_init(struct mycam_device *cam);

/* mycam_urb.c */
int  mycam_urb_alloc_and_submit(struct mycam_device *cam);
void mycam_urb_kill_and_free(struct mycam_device *cam);

/* UVC helpers (in mycam_main.c or mycam_urb.c) */
int  mycam_uvc_probe_commit(struct mycam_device *cam,
			    struct mycam_uvc_probe *probe_out);
int  mycam_select_alt(struct mycam_device *cam, u32 max_payload);

#endif /* MYCAM_H */
