# C270 Driver Migration: Userspace libusb → Kernel V4L2/VB2

## 1. Bối Cảnh & Vấn Đề

### Hiện trạng
Driver hiện tại hoạt động hoàn toàn ở userspace qua `libusb-1.0`:
```
USB Camera → libusb isochronous transfers → c270_capture.c (UVC parse + MJPEG decode → RGB24)
  → c270_stream.c (GStreamer appsrc → H264 → RTSP)
  → c270_display.c (SDL2 local display)
```

### Vấn đề
1. **Tearing sau ~frame 6000** — driver hiện ép alt setting = 10 ("comfort mode" 2x headroom), vi phạm chuẩn UVC
2. **Mentor yêu cầu:** Không được ép alt setting — chỉ dùng alt setting mặc định mà kernel/camera negotiate qua Probe/Commit
3. **Sử dụng `malloc()` không phù hợp** — cần dùng VB2 framework quản lý DMA buffer

### Mục tiêu
Chuyển sang kiến trúc **kernel module** chuẩn Linux V4L2:
```
USB Camera → kernel URB (isoc 8000 pkt/s)
  → mycam_urb_complete() (softirq, không sleep)
  → Parse UVC header → FID toggle → frame boundary
  → Tích lũy payload vào VB2 buffer → vb2_buffer_done()
  → Userspace: /dev/videoX → GStreamer v4l2src → jpegdec → RTSP
```

---

## 2. Ràng Buộc Thiết Kế

| Ràng buộc | Chi tiết |
|-----------|----------|
| **Không ép alt setting** | Probe/Commit → kernel chọn alt nhỏ nhất có `wMaxPacketSize >= dwMaxPayloadTransferSize` |
| **Không dùng malloc** | Frame buffer: VB2 DMA (MMAP). URB metadata: `kmalloc(GFP_KERNEL)`. URB completion: `GFP_ATOMIC` nếu cần |
| **Output MJPEG thô** | Kernel module chỉ gom MJPEG frame, KHÔNG decode. Decode ở userspace (GStreamer `jpegdec`) |
| **IRQ-safe** | `mycam_urb_complete()` chạy trong softirq — không được sleep, không mutex, không `kmalloc(GFP_KERNEL)` |
| **V4L2 controls riêng** | Streaming pipeline trước, controls (brightness/exposure) là task phụ sau |
| **Documentation bắt buộc** | Mỗi file có header block (cấu trúc + pipeline). Mỗi hàm có block comment (tác dụng, tác động, context) |

---

## 2.1. Quy Tắc Documentation

### File Header — Bắt buộc ở đầu mỗi file

Mỗi file `.c` phải bắt đầu bằng block comment mô tả:
1. **Tên module** và vai trò trong hệ thống
2. **Cấu trúc file** — liệt kê các section/function chính
3. **Vị trí trong pipeline** — file này nằm ở đâu trong luồng data

**Ví dụ cho kernel module `mycam_urb.c`:**
```c
/*
 * mycam_urb.c — URB Management & UVC Header Parsing
 *
 * Module này quản lý toàn bộ giao tiếp USB isochronous với camera:
 * nhận raw packets từ USB Host Controller, parse UVC payload header,
 * tích lũy payload thành MJPEG frame hoàn chỉnh, rồi deliver qua VB2.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. mycam_urb_alloc()      — Cấp phát URB pool + transfer buffers
 *   2. mycam_urb_submit()     — Submit URBs lên USB HC
 *   3. mycam_urb_complete()   — ISR callback: parse UVC header, gom payload
 *   4. mycam_frame_done()     — Deliver frame hoàn chỉnh → VB2 buffer
 *   5. mycam_urb_kill()       — Hủy tất cả URBs đang pending
 *   6. mycam_urb_free()       — Giải phóng URB pool
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   USB Camera (C270)
 *       │  isochronous packets (8000 packets/giây)
 *       ▼
 *   ┌─────────────────────────────┐
 *   │  USB Host Controller (HW)   │
 *   └──────────┬──────────────────┘
 *              │  URB completion interrupt
 *              ▼
 *   ┌─────────────────────────────┐
 *   │  mycam_urb_complete()  ◄────│── FILE NÀY (softirq context)
 *   │    parse UVC header         │
 *   │    FID toggle → new frame   │
 *   │    accumulate payload       │
 *   │    EOF → mycam_frame_done() │
 *   └──────────┬──────────────────┘
 *              │  vb2_buffer_done()
 *              ▼
 *   ┌─────────────────────────────┐
 *   │  VB2 Queue (mycam_vb2.c)   │
 *   │    → done_list              │
 *   │    → wake up poll/read      │
 *   └──────────┬──────────────────┘
 *              │  VIDIOC_DQBUF
 *              ▼
 *   ┌─────────────────────────────┐
 *   │  Userspace (GStreamer)       │
 *   │    v4l2src → jpegdec → ...  │
 *   └─────────────────────────────┘
 */
```

**Ví dụ cho userspace `c270_v4l2_capture.c`:**
```c
/*
 * c270_v4l2_capture.c — V4L2 Capture via /dev/videoX
 *
 * Module này mở V4L2 device, set format MJPEG 640x480, quản lý
 * MMAP buffers qua ioctl, và chạy capture loop DQBUF/QBUF.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. v4l2_capture_open()    — Mở /dev/videoX, set MJPEG format
 *   2. v4l2_capture_init()    — REQBUFS + mmap + QBUF
 *   3. v4l2_capture_start()   — STREAMON
 *   4. v4l2_capture_read()    — DQBUF → lấy MJPEG frame → QBUF
 *   5. v4l2_capture_stop()    — STREAMOFF
 *   6. v4l2_capture_close()   — munmap + close fd
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   /dev/videoX (kernel driver: uvcvideo hoặc mycam.ko)
 *       │  MJPEG frames qua V4L2 MMAP
 *       ▼
 *   ┌─────────────────────────────┐
 *   │  c270_v4l2_capture.c  ◄─────│── FILE NÀY
 *   │    DQBUF → MJPEG raw data   │
 *   │    callback → stream/display │
 *   └──────────┬──────────────────┘
 *              │  on_frame(mjpeg_data, size)
 *              ▼
 *   ┌─────────────────────────────┐
 *   │  c270_stream.c (RTSP)       │
 *   │  c270_display.c (SDL2)      │
 *   └─────────────────────────────┘
 */
```

### Function Comment — Bắt buộc trước mỗi hàm

Mỗi hàm (public hoặc static) phải có block comment gồm:
1. **Tác dụng** — hàm này làm gì
2. **Tác động** — ảnh hưởng tới state nào, side effects gì
3. **Context** — chạy trong context nào (process/IRQ/softirq), có được sleep không
4. **Parameters** — giải thích tham số nếu không rõ ràng
5. **Return** — giá trị trả về và ý nghĩa

**Ví dụ:**
```c
/*
 * mycam_urb_complete — URB completion callback
 *
 * TÁC DỤNG:
 *   Được gọi bởi USB HC khi một isochronous transfer hoàn thành.
 *   Duyệt từng iso packet, parse UVC payload header, tích lũy
 *   payload data vào frame_acc. Khi phát hiện frame boundary
 *   (FID toggle hoặc EOF), gọi mycam_frame_done() để deliver.
 *
 * TÁC ĐỘNG:
 *   - Ghi vào cam->frame_acc (data, size, last_fid)
 *   - Gọi mycam_frame_done() → lấy buffer từ buf_list → vb2_buffer_done()
 *   - Re-submit URB (usb_submit_urb GFP_ATOMIC)
 *
 * CONTEXT:
 *   Softirq/interrupt — KHÔNG ĐƯỢC sleep, KHÔNG mutex,
 *   KHÔNG kmalloc(GFP_KERNEL). Chỉ dùng spinlock_irqsave.
 *
 * @urb: URB đã hoàn thành, chứa iso packet descriptors + buffer
 */
static void mycam_urb_complete(struct urb *urb)
```

```c
/*
 * mycam_frame_done — Deliver một MJPEG frame hoàn chỉnh tới VB2
 *
 * TÁC DỤNG:
 *   Lấy buffer đầu tiên từ buf_list (queued bởi userspace QBUF),
 *   copy dữ liệu từ frame_acc vào VB2 buffer, set timestamp +
 *   sequence number, rồi gọi vb2_buffer_done(DONE) để đánh thức
 *   userspace đang block trên DQBUF/poll.
 *
 * TÁC ĐỘNG:
 *   - Lấy + xóa 1 entry từ cam->buf_list (spinlock protected)
 *   - memcpy frame_acc.data → VB2 plane buffer
 *   - vb2_buffer_done() → buffer chuyển sang done_list
 *   - Reset frame_acc (size=0, has_data=false)
 *   - Nếu buf_list rỗng: drop frame (không có buffer để chứa)
 *
 * CONTEXT:
 *   Gọi từ mycam_urb_complete() — softirq, KHÔNG được sleep.
 *
 * @cam: device context chứa buf_list và frame_acc
 */
static void mycam_frame_done(struct mycam_device *cam)
```

```c
/*
 * v4l2_capture_read — Dequeue một MJPEG frame từ kernel
 *
 * TÁC DỤNG:
 *   Gọi VIDIOC_DQBUF để lấy buffer chứa MJPEG frame hoàn chỉnh
 *   từ kernel driver. Buffer đã được mmap nên data có thể đọc
 *   trực tiếp qua pointer. Sau khi xử lý xong, gọi VIDIOC_QBUF
 *   để trả buffer lại cho kernel.
 *
 * TÁC ĐỘNG:
 *   - Block cho đến khi có frame (hoặc timeout)
 *   - Frame data nằm tại buffers[buf.index], kích thước buf.bytesused
 *   - Gọi on_frame callback → stream_push_frame / display_show_frame
 *   - Re-enqueue buffer → kernel có thể ghi frame tiếp theo vào
 *
 * CONTEXT:
 *   Process context (main thread capture loop). Có thể sleep (block trên ioctl).
 *
 * @ctx: V4L2 capture context chứa fd, buffers, callback
 * @return: 0 thành công, -1 lỗi hoặc timeout
 */
int v4l2_capture_read(V4L2CaptureContext *ctx)
```

---

## 3. Phased Migration: C → B → A

### 3.1. Phase C — Refactor Userspace: libusb → V4L2 Input

**Mục tiêu:** Validate pipeline `v4l2src → jpegdec → videoconvert` hoạt động với stock `uvcvideo` kernel driver trước khi viết kernel module.

**Thay đổi code:**

| File hiện tại | Hành động | File mới |
|---------------|-----------|----------|
| `src/c270_usb.c` | Thay thế | `src/c270_v4l2.c` — mở `/dev/videoX`, `VIDIOC_S_FMT` MJPEG 640x480 |
| `src/c270_capture.c` | Thay thế | `src/c270_v4l2_capture.c` — `REQBUFS` / `QBUF` / `DQBUF` / mmap |
| `src/c270_uvc.c` | Bỏ | Probe/Commit do kernel xử lý |
| `src/c270_stream.c` | Sửa nhẹ | Input MJPEG thô thay vì RGB24. Pipeline: `appsrc → jpegparse → jpegdec → videoconvert → x264enc → rtph264pay` |
| `src/c270_display.c` | Sửa nhẹ | Input MJPEG → decode bằng libjpeg hoặc bỏ local display (dùng GStreamer sink) |
| `src/main.c` | Refactor | Đổi init sequence sang V4L2 |
| `include/*.h` | Cập nhật | Bỏ libusb types, thêm V4L2 types |

**Dependencies thay đổi:**
- Bỏ: `libusb-1.0-0-dev`
- Bỏ: `libjpeg-turbo8-dev` (decode ở GStreamer)
- Giữ: `libsdl2-dev`, GStreamer, RTSP server
- Thêm: `linux/videodev2.h` (có sẵn trong kernel headers)

**V4L2 capture flow:**
```c
// 1. Open device
int fd = open("/dev/video0", O_RDWR);

// 2. Set format
struct v4l2_format fmt = {0};
fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
fmt.fmt.pix.width = 640;
fmt.fmt.pix.height = 480;
fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
ioctl(fd, VIDIOC_S_FMT, &fmt);

// 3. Request buffers (kernel alloc MMAP)
struct v4l2_requestbuffers req = {0};
req.count = 4;
req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
req.memory = V4L2_MEMORY_MMAP;
ioctl(fd, VIDIOC_REQBUFS, &req);

// 4. mmap each buffer
for (int i = 0; i < req.count; i++) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    buffers[i] = mmap(NULL, buf.length, PROT_READ|PROT_WRITE,
                      MAP_SHARED, fd, buf.m.offset);
    ioctl(fd, VIDIOC_QBUF, &buf);  // enqueue
}

// 5. Start streaming
enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMON, &type);

// 6. Capture loop
while (running) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_DQBUF, &buf);        // dequeue frame
    // buffers[buf.index] chứa MJPEG data, buf.bytesused bytes
    process_mjpeg_frame(buffers[buf.index], buf.bytesused);
    ioctl(fd, VIDIOC_QBUF, &buf);         // re-enqueue
}
```

**Kiểm chứng Phase C:**
```bash
# Bind lại uvcvideo
sudo modprobe uvcvideo
# Verify /dev/video0 xuất hiện
v4l2-ctl --list-devices
# Test stream
gst-launch-1.0 v4l2src device=/dev/video0 ! image/jpeg,width=640,height=480 ! jpegdec ! autovideosink
```

---

### 3.2. Phase B — Custom Kernel Module `mycam.ko`

**Mục tiêu:** Viết kernel module thay thế `uvcvideo`, đăng ký `/dev/videoX`, implement đúng flow trong diagram.

**Cấu trúc:**
```
kernel/
├── Kbuild                # kernel build rules
├── Makefile              # top-level make (gọi kernel build system)
├── mycam.h               # shared structs, constants
├── mycam_main.c          # module_init/exit, usb_driver probe/disconnect
├── mycam_video.c         # video_device, v4l2_ioctl_ops, v4l2_file_operations
├── mycam_vb2.c           # vb2_ops: queue_setup, buf_queue, start/stop_streaming
├── mycam_urb.c           # URB alloc/submit, completion handler, UVC header parse
└── mycam_ctrl.c          # v4l2_ctrl_handler (Phase riêng, sau khi stream ổn)
```

#### 3.2.1. mycam.h — Shared Data Structures

```c
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#define MYCAM_VID           0x046d
#define MYCAM_PID           0x0825
#define MYCAM_MAX_FRAME     (640 * 480 * 2)  /* MJPEG worst case */
#define MYCAM_ISO_PKTS      32
#define MYCAM_NUM_URBS      8
#define MYCAM_CTRL_IFACE    0
#define MYCAM_STREAM_IFACE  1
#define MYCAM_ISO_EP        0x81

/* UVC Probe/Commit (26 bytes, UVC 1.1) */
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
} __packed;

/* VB2 buffer wrapper */
struct mycam_buffer {
    struct vb2_v4l2_buffer vb;
    struct list_head       list;
};

/* Frame accumulator (dùng cho URB completion) */
struct mycam_frame_acc {
    u8    *data;           /* kmalloc'd once at start_streaming */
    u32    size;           /* current accumulated bytes */
    u32    max_size;
    u8     last_fid;       /* last Frame ID bit */
    bool   has_data;
    u32    sequence;       /* frame sequence counter */
};

/* Per-device context */
struct mycam_device {
    struct usb_device      *udev;
    struct usb_interface   *intf;
    struct v4l2_device      v4l2_dev;
    struct video_device     vdev;
    struct vb2_queue        vb2_queue;
    struct mutex            vb2_lock;      /* protects vb2 queue ops */

    /* Buffer management */
    struct list_head        buf_list;      /* queued mycam_buffers */
    spinlock_t              buf_lock;      /* protects buf_list (IRQ safe) */

    /* URB pool */
    struct urb             *urbs[MYCAM_NUM_URBS];
    u8                     *urb_bufs[MYCAM_NUM_URBS];
    u16                     max_pkt_size;
    u8                      alt_setting;

    /* Frame accumulator */
    struct mycam_frame_acc  frame_acc;

    /* Stream state */
    bool                    streaming;

    /* Format */
    u32                     width;
    u32                     height;
    u32                     pixfmt;       /* V4L2_PIX_FMT_MJPEG */
};
```

#### 3.2.2. Module Lifecycle

```
insmod mycam.ko
  → mycam_init() → usb_register(&mycam_usb_driver)

Camera plugged in (VID:PID match)
  → mycam_probe()
    → v4l2_device_register()
    → vb2_queue_init() — type=CAPTURE, memory=MMAP, io_modes=MMAP|READ
    → video_register_device() → /dev/videoX xuất hiện

Userspace: open /dev/videoX
  → VIDIOC_S_FMT → mycam_s_fmt() — set MJPEG 640x480
  → VIDIOC_REQBUFS → vb2_reqbufs() — VB2 alloc MMAP buffers
  → VIDIOC_QBUF × N → vb2_qbuf() → mycam_buf_queue() → thêm vào buf_list
  → VIDIOC_STREAMON → vb2_streamon() → mycam_start_streaming()
    → UVC Probe/Commit (usb_control_msg)
    → Chọn alt setting (chuẩn: nhỏ nhất >= dwMaxPayloadTransferSize)
    → usb_set_interface(udev, STREAM_IFACE, alt)
    → Alloc + submit URBs

USB HC nhận iso packets
  → mycam_urb_complete() [softirq]
    → Parse UVC header: byte[0]=len, byte[1]=flags
    → Check ERR bit (bit 6) → skip if error
    → FID bit (bit 0) toggle → frame boundary
    → Accumulate payload vào frame_acc
    → Khi FID đổi hoặc EOF:
      → Lấy buffer từ buf_list (spin_lock_irqsave)
      → memcpy frame_acc → vb2 buffer
      → vb2_buffer_done(VB2_BUF_STATE_DONE)
      → Reset frame_acc
    → usb_submit_urb() để re-submit

Userspace: VIDIOC_DQBUF → nhận MJPEG frame → mmap xử lý

VIDIOC_STREAMOFF → mycam_stop_streaming()
  → usb_kill_urb() × N
  → usb_set_interface(alt=0)
  → Free URBs
  → Return all queued buffers với VB2_BUF_STATE_ERROR

Camera unplugged
  → mycam_disconnect()
    → video_unregister_device()
    → v4l2_device_unregister()

rmmod mycam.ko
  → mycam_exit() → usb_deregister()
```

#### 3.2.3. Alt Setting Selection (Chuẩn Kernel)

```c
/* Trong mycam_start_streaming(): */
static int select_alt_setting(struct mycam_device *cam, u32 max_payload)
{
    struct usb_host_interface *alts;
    struct usb_interface *intf;
    int i, best_alt = -1;
    u16 best_pkt = 0xFFFF;

    intf = cam->udev->actconfig->interface[MYCAM_STREAM_IFACE];

    for (i = 0; i < intf->num_altsetting; i++) {
        alts = &intf->altsetting[i];
        if (alts->desc.bNumEndpoints == 0)
            continue;
        u16 pkt = le16_to_cpu(alts->endpoint[0].desc.wMaxPacketSize);
        u16 size = pkt & 0x07FF;
        u8  mult = ((pkt >> 11) & 0x03) + 1;
        u16 effective = size * mult;

        /* Chọn alt nhỏ nhất đủ bandwidth */
        if (effective >= max_payload && size < best_pkt) {
            best_alt = alts->desc.bAlternateSetting;
            best_pkt = size;
        }
    }
    return best_alt;  /* -1 if none found */
}
```

#### 3.2.4. URB Completion Handler (IRQ-safe)

```c
static void mycam_urb_complete(struct urb *urb)
{
    struct mycam_device *cam = urb->context;
    int i, offset = 0;

    if (!cam->streaming) return;
    if (urb->status < 0 && urb->status != -ENOENT) {
        /* resubmit on transient errors */
        goto resubmit;
    }

    for (i = 0; i < urb->number_of_packets; i++) {
        struct usb_iso_packet_descriptor *pkt = &urb->iso_frame_desc[i];
        u8 *data = urb->transfer_buffer + offset;
        int len = pkt->actual_length;

        if (pkt->status == 0 && len >= 2) {
            u8 hle = data[0];          /* header length */
            u8 bmi = data[1];          /* bmHeaderInfo */
            u8 fid = bmi & 0x01;       /* Frame ID */
            u8 eof = (bmi >> 1) & 0x01;
            u8 err = (bmi >> 6) & 0x01;

            if (!err && hle <= len) {
                u8 *payload = data + hle;
                int payload_len = len - hle;

                /* FID toggle → new frame */
                if (fid != cam->frame_acc.last_fid && cam->frame_acc.has_data) {
                    mycam_frame_done(cam);  /* deliver current frame */
                }
                cam->frame_acc.last_fid = fid;

                /* Accumulate payload */
                if (payload_len > 0 &&
                    cam->frame_acc.size + payload_len <= cam->frame_acc.max_size) {
                    memcpy(cam->frame_acc.data + cam->frame_acc.size,
                           payload, payload_len);
                    cam->frame_acc.size += payload_len;
                    cam->frame_acc.has_data = true;
                }

                /* EOF → deliver frame */
                if (eof && cam->frame_acc.has_data)
                    mycam_frame_done(cam);
            }
        }
        offset += pkt->length;
    }

resubmit:
    if (cam->streaming)
        usb_submit_urb(urb, GFP_ATOMIC);
}

/* Deliver accumulated frame to VB2 */
static void mycam_frame_done(struct mycam_device *cam)
{
    struct mycam_buffer *buf;
    unsigned long flags;

    spin_lock_irqsave(&cam->buf_lock, flags);
    if (list_empty(&cam->buf_list)) {
        spin_unlock_irqrestore(&cam->buf_lock, flags);
        cam->frame_acc.size = 0;
        cam->frame_acc.has_data = false;
        return;  /* no buffer available, drop frame */
    }
    buf = list_first_entry(&cam->buf_list, struct mycam_buffer, list);
    list_del(&buf->list);
    spin_unlock_irqrestore(&cam->buf_lock, flags);

    /* Copy frame data to VB2 buffer */
    void *vbuf = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
    memcpy(vbuf, cam->frame_acc.data, cam->frame_acc.size);
    vb2_set_plane_payload(&buf->vb.vb2_buf, 0, cam->frame_acc.size);

    buf->vb.vb2_buf.timestamp = ktime_get_ns();
    buf->vb.sequence = cam->frame_acc.sequence++;
    buf->vb.field = V4L2_FIELD_NONE;

    vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

    /* Reset accumulator */
    cam->frame_acc.size = 0;
    cam->frame_acc.has_data = false;
}
```

#### 3.2.5. VB2 Queue Ops

```c
static int mycam_queue_setup(struct vb2_queue *q,
                             unsigned int *nbuffers,
                             unsigned int *nplanes,
                             unsigned int sizes[],
                             struct device *alloc_devs[])
{
    *nplanes = 1;
    sizes[0] = MYCAM_MAX_FRAME;
    if (*nbuffers < 2) *nbuffers = 2;
    return 0;
}

static void mycam_buf_queue(struct vb2_buffer *vb)
{
    struct mycam_buffer *buf = container_of(
        to_vb2_v4l2_buffer(vb), struct mycam_buffer, vb);
    struct mycam_device *cam = vb2_get_drv_priv(vb->vb2_queue);
    unsigned long flags;

    spin_lock_irqsave(&cam->buf_lock, flags);
    list_add_tail(&buf->list, &cam->buf_list);
    spin_unlock_irqrestore(&cam->buf_lock, flags);
}

static int mycam_start_streaming(struct vb2_queue *q, unsigned int count)
{
    struct mycam_device *cam = vb2_get_drv_priv(q);
    /* 1. UVC Probe/Commit */
    /* 2. Select alt setting (chuẩn) */
    /* 3. usb_set_interface() */
    /* 4. Alloc frame_acc.data = kmalloc(MYCAM_MAX_FRAME, GFP_KERNEL) */
    /* 5. Alloc + submit URBs */
    cam->streaming = true;
    return 0;
}

static void mycam_stop_streaming(struct vb2_queue *q)
{
    struct mycam_device *cam = vb2_get_drv_priv(q);
    cam->streaming = false;
    /* 1. Kill all URBs */
    /* 2. usb_set_interface(alt=0) */
    /* 3. kfree(frame_acc.data) */
    /* 4. Return all queued buffers with VB2_BUF_STATE_ERROR */
}
```

---

### 3.3. Phase A — Cleanup

**Khi Phase B ổn định:**
- Xóa: `src/c270_usb.c`, `src/c270_uvc.c`, `src/c270_capture.c`
- Xóa: `include/c270_usb.h`, `include/c270_uvc.h`, `include/c270_capture.h`
- Giữ/refactor: `src/c270_stream.c` → input từ V4L2, hoặc bỏ luôn appsrc và dùng GStreamer pipeline thuần
- Giữ: `src/c270_display.c` → optional SDL2 viewer
- Update: `CMakeLists.txt` — bỏ libusb, libjpeg deps
- Update: `README.md`, `SESSION_NOTES.md`
- Bỏ: `unbind_c270.sh` (kernel module tự quản lý bind/unbind)

---

## 4. GStreamer Pipeline (Userspace)

Sau khi có `/dev/videoX` (từ Phase C hoặc B):

```bash
# Pipeline đơn giản — local display
gst-launch-1.0 v4l2src device=/dev/video0 \
  ! image/jpeg,width=640,height=480,framerate=30/1 \
  ! jpegdec \
  ! videoconvert \
  ! autovideosink

# Pipeline RTSP server
gst-launch-1.0 v4l2src device=/dev/video0 \
  ! image/jpeg,width=640,height=480,framerate=30/1 \
  ! jpegdec \
  ! videoconvert \
  ! x264enc tune=zerolatency bitrate=2000 \
  ! rtph264pay \
  ! udpsink host=127.0.0.1 port=5000
```

Hoặc giữ C app với RTSP server GStreamer:
```c
// GStreamer pipeline string cho RTSP factory
"appsrc name=src is-live=true format=time "
"! image/jpeg,width=640,height=480,framerate=30/1 "
"! jpegdec "
"! videoconvert "
"! x264enc tune=zerolatency bitrate=2000 speed-preset=ultrafast "
"! rtph264pay name=pay0 pt=96"
```

---

## 5. Memory Model

| Thành phần | Allocation | Context | Lifecycle |
|------------|-----------|---------|-----------|
| VB2 frame buffers | VB2 framework (MMAP) | `VIDIOC_REQBUFS` | Userspace controls |
| `frame_acc.data` | `kmalloc(GFP_KERNEL)` | `start_streaming()` | `start` → `stop` |
| URB structs | `usb_alloc_urb()` | `start_streaming()` | `start` → `stop` |
| URB transfer buffers | `kmalloc(GFP_KERNEL)` | `start_streaming()` | `start` → `stop` |
| `mycam_device` | `kzalloc(GFP_KERNEL)` | `probe()` | `probe` → `disconnect` |
| Userspace mmap'd buffers | Kernel MMAP | `mmap()` | Userspace controls |

**Nguyên tắc:**
- Frame data: **KHÔNG bao giờ malloc/kmalloc** — VB2 quản lý
- `frame_acc.data`: kmalloc **một lần** tại `start_streaming`, kfree tại `stop_streaming`
- URB completion handler: chỉ dùng `GFP_ATOMIC` nếu cần alloc (nhưng thiết kế này không cần alloc trong IRQ)

---

## 6. Rủi Ro & Giảm Thiểu

| Rủi ro | Mức | Giảm thiểu |
|--------|-----|------------|
| Kernel panic do bug trong module | Cao | Test trên VM trước, dùng `pr_debug` + `dmesg` |
| Alt setting mặc định gây tearing | Trung bình | Phase C validate với stock `uvcvideo` trước |
| Camera firmware quirk | Thấp | C270 là device phổ biến, UVC 1.1 compliant |
| Race condition buf_list | Trung bình | `spinlock_irqsave` cho mọi access từ IRQ context |

---

## 7. Thứ Tự Thực Hiện

1. **Phase C** (ưu tiên cao nhất)
   - Task C1: Viết `c270_v4l2.c` — open/close V4L2 device
   - Task C2: Viết `c270_v4l2_capture.c` — REQBUFS/QBUF/DQBUF/mmap loop
   - Task C3: Sửa `c270_stream.c` — nhận MJPEG thô thay vì RGB24
   - Task C4: Sửa `main.c` — V4L2 init flow
   - Task C5: Test với stock `uvcvideo` + GStreamer

2. **Phase B** (sau khi Phase C pass)
   - Task B1: Scaffolding `kernel/` — Makefile, mycam.h, mycam_main.c (probe/disconnect)
   - Task B2: mycam_video.c — V4L2 device registration
   - Task B3: mycam_vb2.c — VB2 queue ops
   - Task B4: mycam_urb.c — URB + UVC header parse + frame assembly
   - Task B5: Integration test — insmod → v4l2-ctl → gst-launch
   - Task B6: (Optional) mycam_ctrl.c — V4L2 controls

3. **Phase A** (sau khi Phase B stable)
   - Task A1: Xóa legacy libusb code
   - Task A2: Update docs
