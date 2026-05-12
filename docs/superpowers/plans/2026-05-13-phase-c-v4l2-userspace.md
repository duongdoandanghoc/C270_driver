# Phase C: V4L2 Userspace Migration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor userspace app from libusb isochronous to V4L2 `/dev/videoX` input, validated with stock `uvcvideo` kernel driver.

**Architecture:** Replace `c270_usb.c` + `c270_uvc.c` + `c270_capture.c` (libusb) with `c270_v4l2.c` (V4L2 ioctl). Stream module receives MJPEG raw instead of RGB24. Display decodes MJPEG via libjpeg. Hotplug uses `/dev/videoX` presence check instead of libusb scan.

**Tech Stack:** C99, V4L2 (linux/videodev2.h), GStreamer RTSP, SDL2, libjpeg-turbo

---

### Task C1: Create V4L2 Device Header

**Files:**
- Create: `include/c270_v4l2.h`

- [ ] **Step 1: Write the header file**

```c
#ifndef C270_V4L2_H
#define C270_V4L2_H

/*
 * c270_v4l2.h — V4L2 Device Abstraction
 *
 * Thay thế c270_usb.h + c270_uvc.h cho pipeline V4L2.
 * Cung cấp interface mở/đóng /dev/videoX và set format MJPEG.
 */

#include <stdint.h>
#include <linux/videodev2.h>

#define V4L2_NUM_BUFFERS  4
#define V4L2_MAX_FRAME_SIZE (640 * 480 * 2)

/* Mapped buffer info */
typedef struct {
    void   *start;
    size_t  length;
} V4L2MappedBuffer;

/* V4L2 device context — replaces C270Device */
typedef struct {
    int                 fd;           /* /dev/videoX file descriptor */
    char                dev_path[32]; /* e.g. "/dev/video0" */
    uint16_t            width;
    uint16_t            height;
    uint8_t             fps;
    uint32_t            pixfmt;       /* V4L2_PIX_FMT_MJPEG */
    V4L2MappedBuffer    buffers[V4L2_NUM_BUFFERS];
    uint32_t            n_buffers;
    int                 is_streaming;
} V4L2Device;

/* Public API */
int  v4l2_device_open(V4L2Device *dev, const char *path);
int  v4l2_device_set_format(V4L2Device *dev, uint16_t w, uint16_t h, uint32_t pixfmt);
int  v4l2_device_init_buffers(V4L2Device *dev);
int  v4l2_device_start(V4L2Device *dev);
int  v4l2_device_read_frame(V4L2Device *dev, uint8_t **data, uint32_t *size);
int  v4l2_device_stop(V4L2Device *dev);
void v4l2_device_close(V4L2Device *dev);

#endif /* C270_V4L2_H */
```

- [ ] **Step 2: Commit**

```bash
git add include/c270_v4l2.h
git commit -m "feat(phase-c): add V4L2 device header replacing libusb types"
```

---

### Task C2: Implement V4L2 Device Module

**Files:**
- Create: `src/c270_v4l2.c`

- [ ] **Step 1: Write the implementation**

```c
/*
 * c270_v4l2.c — V4L2 Device Management
 *
 * Thay thế c270_usb.c + c270_uvc.c. Mở /dev/videoX, negotiate
 * format MJPEG 640x480 qua VIDIOC_S_FMT, quản lý MMAP buffers.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. v4l2_device_open()         — open() + capability check
 *   2. v4l2_device_set_format()   — VIDIOC_S_FMT
 *   3. v4l2_device_init_buffers() — REQBUFS + QUERYBUF + mmap + QBUF
 *   4. v4l2_device_start()        — VIDIOC_STREAMON
 *   5. v4l2_device_read_frame()   — DQBUF + re-QBUF
 *   6. v4l2_device_stop()         — VIDIOC_STREAMOFF
 *   7. v4l2_device_close()        — munmap + close
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   /dev/videoX (uvcvideo hoặc mycam.ko)
 *       │  MJPEG frames qua MMAP
 *       ▼
 *   ┌───────────────────────────┐
 *   │  c270_v4l2.c  ◄───────────│── FILE NÀY
 *   │    DQBUF → MJPEG raw      │
 *   └──────────┬────────────────┘
 *              │
 *              ▼
 *   main.c → stream / display
 */

#include "c270_v4l2.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/*
 * v4l2_device_open — Mở V4L2 device và kiểm tra capabilities
 *
 * TÁC DỤNG: open() device path, verify V4L2_CAP_VIDEO_CAPTURE + STREAMING
 * TÁC ĐỘNG: dev->fd được set, dev->dev_path được copy
 * CONTEXT:  Process context, có thể sleep
 *
 * @dev:  V4L2 device context (output)
 * @path: đường dẫn device, vd "/dev/video0"
 * @return: 0 OK, -1 lỗi
 */
int v4l2_device_open(V4L2Device *dev, const char *path)
{
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->dev_path, sizeof(dev->dev_path), "%s", path);

    dev->fd = open(path, O_RDWR | O_NONBLOCK);
    if (dev->fd < 0) {
        fprintf(stderr, "[V4L2] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    if (ioctl(dev->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[V4L2] QUERYCAP failed: %s\n", strerror(errno));
        close(dev->fd);
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "[V4L2] %s is not a capture device\n", path);
        close(dev->fd);
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[V4L2] %s does not support streaming\n", path);
        close(dev->fd);
        return -1;
    }

    printf("[V4L2] Opened %s: %s (%s)\n", path, cap.card, cap.driver);
    return 0;
}

/*
 * v4l2_device_set_format — Set pixel format, resolution
 *
 * TÁC DỤNG: VIDIOC_S_FMT để negotiate MJPEG 640x480
 * TÁC ĐỘNG: dev->width/height/pixfmt updated từ kernel response
 * CONTEXT:  Process context
 */
int v4l2_device_set_format(V4L2Device *dev, uint16_t w, uint16_t h, uint32_t pixfmt)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(dev->fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[V4L2] S_FMT failed: %s\n", strerror(errno));
        return -1;
    }

    dev->width  = fmt.fmt.pix.width;
    dev->height = fmt.fmt.pix.height;
    dev->pixfmt = fmt.fmt.pix.pixelformat;

    printf("[V4L2] Format: %c%c%c%c %dx%d\n",
           pixfmt & 0xFF, (pixfmt >> 8) & 0xFF,
           (pixfmt >> 16) & 0xFF, (pixfmt >> 24) & 0xFF,
           dev->width, dev->height);
    return 0;
}

/*
 * v4l2_device_init_buffers — REQBUFS + mmap + enqueue
 *
 * TÁC DỤNG: Cấp phát MMAP buffers từ kernel, mmap vào userspace, QBUF
 * TÁC ĐỘNG: dev->buffers[] populated, dev->n_buffers set
 * CONTEXT:  Process context
 */
int v4l2_device_init_buffers(V4L2Device *dev)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = V4L2_NUM_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(dev->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "[V4L2] REQBUFS failed: %s\n", strerror(errno));
        return -1;
    }
    dev->n_buffers = req.count;
    printf("[V4L2] Allocated %d MMAP buffers\n", req.count);

    for (uint32_t i = 0; i < dev->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (ioctl(dev->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QUERYBUF %d failed\n", i);
            return -1;
        }

        dev->buffers[i].length = buf.length;
        dev->buffers[i].start = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, dev->fd, buf.m.offset);
        if (dev->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[V4L2] mmap buffer %d failed\n", i);
            return -1;
        }

        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QBUF %d failed\n", i);
            return -1;
        }
    }
    return 0;
}

/*
 * v4l2_device_start — VIDIOC_STREAMON
 *
 * TÁC DỤNG: Bắt đầu streaming, kernel driver bắt đầu fill buffers
 * TÁC ĐỘNG: dev->is_streaming = 1
 * CONTEXT:  Process context
 */
int v4l2_device_start(V4L2Device *dev)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[V4L2] STREAMON failed: %s\n", strerror(errno));
        return -1;
    }
    dev->is_streaming = 1;
    printf("[V4L2] Streaming started\n");
    return 0;
}

/*
 * v4l2_device_read_frame — DQBUF one frame + re-QBUF
 *
 * TÁC DỤNG: Dequeue buffer chứa MJPEG frame, trả pointer + size cho caller
 * TÁC ĐỘNG: Buffer tạm thời thuộc caller cho đến khi hàm được gọi lại
 *           (re-QBUF ngay sau khi lấy data)
 * CONTEXT:  Process context, block nếu chưa có frame
 *
 * @data: output — pointer tới MJPEG data (mmap'd, không cần free)
 * @size: output — kích thước MJPEG frame (bytes)
 * @return: 0 OK, -1 lỗi, -2 EAGAIN (no frame ready)
 */
int v4l2_device_read_frame(V4L2Device *dev, uint8_t **data, uint32_t *size)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return -2;
        fprintf(stderr, "[V4L2] DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    *data = (uint8_t *)dev->buffers[buf.index].start;
    *size = buf.bytesused;

    if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[V4L2] re-QBUF failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * v4l2_device_stop — VIDIOC_STREAMOFF
 *
 * TÁC DỤNG: Dừng streaming
 * TÁC ĐỘNG: dev->is_streaming = 0, kernel ngừng fill buffers
 * CONTEXT:  Process context
 */
int v4l2_device_stop(V4L2Device *dev)
{
    if (!dev->is_streaming) return 0;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(dev->fd, VIDIOC_STREAMOFF, &type) < 0) {
        fprintf(stderr, "[V4L2] STREAMOFF failed: %s\n", strerror(errno));
        return -1;
    }
    dev->is_streaming = 0;
    printf("[V4L2] Streaming stopped\n");
    return 0;
}

/*
 * v4l2_device_close — Cleanup: munmap + close fd
 *
 * TÁC DỤNG: Giải phóng tất cả MMAP buffers và đóng device
 * TÁC ĐỘNG: dev->fd = -1, dev->buffers[] unmapped
 * CONTEXT:  Process context
 */
void v4l2_device_close(V4L2Device *dev)
{
    if (dev->fd < 0) return;
    for (uint32_t i = 0; i < dev->n_buffers; i++) {
        if (dev->buffers[i].start && dev->buffers[i].start != MAP_FAILED)
            munmap(dev->buffers[i].start, dev->buffers[i].length);
    }
    close(dev->fd);
    dev->fd = -1;
    printf("[V4L2] Device closed\n");
}
```

- [ ] **Step 2: Commit**

```bash
git add src/c270_v4l2.c
git commit -m "feat(phase-c): implement V4L2 device module"
```

---

### Task C3: Update Stream Module for MJPEG Input

**Files:**
- Modify: `include/c270_stream.h`
- Modify: `src/c270_stream.c`

- [ ] **Step 1: Add MJPEG push function to header**

Add after existing `stream_push_frame`:
```c
/* Push raw MJPEG data (không cần decode trước) */
void stream_push_mjpeg(StreamContext *ctx, const uint8_t *data, uint32_t size);
```

- [ ] **Step 2: Change pipeline from RGB appsrc to MJPEG appsrc**

In `stream_init()`, change pipeline strings from:
```c
"( appsrc name=vsrc ! videoconvert ! video/x-raw,format=I420 ! x264enc ..."
```
To:
```c
"( appsrc name=vsrc ! image/jpeg,width=%d,height=%d,framerate=%d/1 ! "
"jpegdec ! videoconvert ! video/x-raw,format=I420 ! "
"x264enc tune=zerolatency bitrate=1000 key-int-max=15 speed-preset=ultrafast ! "
"rtph264pay name=pay0 pt=96 config-interval=1 )"
```

- [ ] **Step 3: Change media_configure_cb caps from RGB to MJPEG**

```c
GstCaps *caps = gst_caps_new_simple("image/jpeg",
    "width",     G_TYPE_INT,     fdata->width,
    "height",    G_TYPE_INT,     fdata->height,
    "framerate", GST_TYPE_FRACTION, fdata->fps, 1,
    NULL);
```

- [ ] **Step 4: Implement stream_push_mjpeg**

```c
/*
 * stream_push_mjpeg — Push raw MJPEG frame vào GStreamer pipeline
 *
 * TÁC DỤNG: Wrap MJPEG data vào GstBuffer, push vào appsrc
 * TÁC ĐỘNG: GStreamer pipeline nhận frame để decode + encode + RTSP
 * CONTEXT:  Process context (capture thread)
 */
void stream_push_mjpeg(StreamContext *ctx, const uint8_t *data, uint32_t size) {
    if (!ctx->is_running || !ctx->appsrc) return;
    GstElement *appsrc = (GstElement *)ctx->appsrc;

    GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    memcpy(map.data, data, size);
    gst_buffer_unmap(buf, &map);

    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    if (ret != GST_FLOW_OK) {
        ctx->appsrc = NULL;
        if (ret == GST_FLOW_FLUSHING)
            printf("[STREAM] Pipeline flushing — waiting for new client\n");
    }
}
```

- [ ] **Step 5: Commit**

```bash
git add include/c270_stream.h src/c270_stream.c
git commit -m "feat(phase-c): stream module accepts MJPEG input"
```

---

### Task C4: Update Display for MJPEG Input

**Files:**
- Modify: `include/c270_display.h`
- Modify: `src/c270_display.c`

- [ ] **Step 1: Add MJPEG display function to header**

```c
/* Show MJPEG frame (decode internally via libjpeg) */
void display_show_mjpeg(DisplayContext *disp,
                        const uint8_t *jpeg_data, uint32_t jpeg_size,
                        float fps, const char *camera_id);
```

- [ ] **Step 2: Implement display_show_mjpeg using libjpeg decode**

Reuse the existing `decode_mjpeg` logic from old `c270_capture.c` — decode MJPEG→RGB24 then call `display_show_frame` internally.

- [ ] **Step 3: Commit**

```bash
git add include/c270_display.h src/c270_display.c
git commit -m "feat(phase-c): display accepts MJPEG input"
```

---

### Task C5: Rewrite main.c for V4L2 Pipeline

**Files:**
- Modify: `src/main.c`

- [ ] **Step 1: Replace libusb includes with V4L2**

Replace:
```c
#include "c270_usb.h"
#include "c270_uvc.h"
#include "c270_capture.h"
```
With:
```c
#include "c270_v4l2.h"
```

- [ ] **Step 2: Replace global state**

Replace `C270Device g_dev` + `CaptureContext g_capture` with `V4L2Device g_v4l2`.

- [ ] **Step 3: Rewrite main() init sequence**

```c
// Old: c270_usb_init → c270_usb_open → c270_uvc_start_stream → capture_init → capture_start
// New: v4l2_device_open → v4l2_device_set_format → v4l2_device_init_buffers → v4l2_device_start

V4L2Device g_v4l2;
v4l2_device_open(&g_v4l2, "/dev/video0");
v4l2_device_set_format(&g_v4l2, 640, 480, V4L2_PIX_FMT_MJPEG);
v4l2_device_init_buffers(&g_v4l2);
v4l2_device_start(&g_v4l2);
```

- [ ] **Step 4: Rewrite main loop**

Replace libusb event loop with V4L2 poll + read:
```c
while (g_running) {
    uint8_t *mjpeg_data;
    uint32_t mjpeg_size;
    int ret = v4l2_device_read_frame(&g_v4l2, &mjpeg_data, &mjpeg_size);
    if (ret == 0) {
        if (!g_app.no_stream)
            stream_push_mjpeg(&g_stream, mjpeg_data, mjpeg_size);
        if (!g_app.no_display)
            display_show_mjpeg(&g_display, mjpeg_data, mjpeg_size, fps, "C270-0");
    } else if (ret == -2) {
        usleep(1000); /* EAGAIN — no frame yet */
    }
    if (!g_app.no_display && display_poll_events(&g_display) < 0)
        g_running = 0;
}
```

- [ ] **Step 5: Rewrite cleanup**

```c
v4l2_device_stop(&g_v4l2);
v4l2_device_close(&g_v4l2);
```

- [ ] **Step 6: Commit**

```bash
git add src/main.c
git commit -m "feat(phase-c): main.c uses V4L2 pipeline"
```

---

### Task C6: Update CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Replace source files and remove libusb dependency**

Replace in DRIVER_SOURCES:
```cmake
set(DRIVER_SOURCES
    src/c270_v4l2.c
    src/c270_display.c
    src/c270_stream.c
    src/c270_hotplug.c
)
```

Remove `pkg_check_modules(LIBUSB ...)`. Keep libjpeg for display decode.

- [ ] **Step 2: Update link libraries**

Remove `${LIBUSB_LIBRARIES}` from `target_link_libraries`.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat(phase-c): CMake uses V4L2 instead of libusb"
```

---

### Task C7: Integration Test

- [ ] **Step 1: Bind uvcvideo**

```bash
sudo modprobe uvcvideo
v4l2-ctl --list-devices
# Should show /dev/video0
```

- [ ] **Step 2: Verify GStreamer pipeline**

```bash
gst-launch-1.0 v4l2src device=/dev/video0 \
  ! image/jpeg,width=640,height=480,framerate=30/1 \
  ! jpegdec ! videoconvert ! autovideosink
```

- [ ] **Step 3: Build and run app**

```bash
cd build && cmake .. && make -j$(nproc)
sudo ./build/c270_app
```

- [ ] **Step 4: Verify RTSP**

```bash
ffplay rtsp://127.0.0.1:8554/camera0
```

- [ ] **Step 5: Commit final**

```bash
git add -A
git commit -m "feat(phase-c): V4L2 userspace migration complete"
```
