/*
 * c270_v4l2.c — V4L2 Device Management
 *
 * Thay thế c270_usb.c + c270_uvc.c. Mở /dev/videoX, negotiate
 * format MJPEG 640x480 qua VIDIOC_S_FMT, quản lý MMAP buffers,
 * và cung cấp capture loop DQBUF/QBUF.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. v4l2_device_open()         — open() + VIDIOC_QUERYCAP
 *   2. v4l2_device_set_format()   — VIDIOC_S_FMT
 *   3. v4l2_device_init_buffers() — REQBUFS + QUERYBUF + mmap + QBUF
 *   4. v4l2_device_start()        — VIDIOC_STREAMON
 *   5. v4l2_device_read_frame()   — DQBUF + re-QBUF
 *   6. v4l2_device_stop()         — VIDIOC_STREAMOFF
 *   7. v4l2_device_close()        — munmap + close
 *   8. v4l2_device_get_fps()      — measured FPS
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
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

/* ────────────────────────────────────────────────────────────
 * Internal: timestamp milliseconds (for FPS calculation)
 * ──────────────────────────────────────────────────────────── */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * v4l2_device_open — Mở V4L2 device và kiểm tra capabilities
 *
 * TÁC DỤNG:
 *   open() device path, verify V4L2_CAP_VIDEO_CAPTURE và
 *   V4L2_CAP_STREAMING flags trong capabilities.
 *
 * TÁC ĐỘNG:
 *   - dev->fd được set nếu thành công
 *   - dev->dev_path được copy
 *   - Tất cả field khác zeroed
 *
 * CONTEXT:
 *   Process context, có thể sleep (open syscall)
 *
 * @dev:  V4L2 device context (output, sẽ bị memset 0)
 * @path: đường dẫn device, vd "/dev/video0"
 * @return: 0 OK, -1 lỗi
 */
int v4l2_device_open(V4L2Device *dev, const char *path)
{
    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    snprintf(dev->dev_path, sizeof(dev->dev_path), "%s", path);

    dev->fd = open(path, O_RDWR | O_NONBLOCK);
    if (dev->fd < 0) {
        fprintf(stderr, "[V4L2] Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(dev->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "[V4L2] QUERYCAP failed: %s\n", strerror(errno));
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "[V4L2] %s is not a video capture device\n", path);
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "[V4L2] %s does not support streaming I/O\n", path);
        close(dev->fd);
        dev->fd = -1;
        return -1;
    }

    printf("[V4L2] Opened %s: %s (%s)\n", path, cap.card, cap.driver);
    printf("[V4L2]   Bus: %s\n", cap.bus_info);
    return 0;
}

/*
 * v4l2_device_set_format — Set pixel format và resolution
 *
 * TÁC DỤNG:
 *   Gọi VIDIOC_S_FMT để negotiate format với kernel driver.
 *   Kernel có thể điều chỉnh width/height nếu camera không
 *   hỗ trợ chính xác giá trị yêu cầu.
 *
 * TÁC ĐỘNG:
 *   - dev->width, dev->height, dev->pixfmt cập nhật từ kernel response
 *   - Giá trị có thể khác với w, h nếu kernel adjust
 *
 * CONTEXT:
 *   Process context, có thể sleep (ioctl)
 *
 * @dev:    V4L2 device (phải đã open)
 * @w:      desired width (640)
 * @h:      desired height (480)
 * @pixfmt: pixel format (V4L2_PIXFMT_MJPEG hoặc V4L2_PIX_FMT_MJPEG)
 * @return: 0 OK, -1 lỗi
 */
int v4l2_device_set_format(V4L2Device *dev, uint16_t w, uint16_t h, uint32_t pixfmt)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = w;
    fmt.fmt.pix.height      = h;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (ioctl(dev->fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "[V4L2] S_FMT failed: %s\n", strerror(errno));
        return -1;
    }

    dev->width  = (uint16_t)fmt.fmt.pix.width;
    dev->height = (uint16_t)fmt.fmt.pix.height;
    dev->pixfmt = fmt.fmt.pix.pixelformat;

    char fourcc[5] = {0};
    fourcc[0] = (char)(dev->pixfmt & 0xFF);
    fourcc[1] = (char)((dev->pixfmt >> 8) & 0xFF);
    fourcc[2] = (char)((dev->pixfmt >> 16) & 0xFF);
    fourcc[3] = (char)((dev->pixfmt >> 24) & 0xFF);

    printf("[V4L2] Format set: %s %dx%d (sizeimage=%u)\n",
           fourcc, dev->width, dev->height, fmt.fmt.pix.sizeimage);

    if (dev->width != w || dev->height != h) {
        printf("[V4L2] ⚠ Kernel adjusted resolution from %dx%d to %dx%d\n",
               w, h, dev->width, dev->height);
    }
    return 0;
}

/*
 * v4l2_device_init_buffers — REQBUFS + QUERYBUF + mmap + QBUF
 *
 * TÁC DỤNG:
 *   1. VIDIOC_REQBUFS: yêu cầu kernel cấp phát V4L2_NUM_BUFFERS MMAP buffers
 *   2. VIDIOC_QUERYBUF: lấy offset + length cho từng buffer
 *   3. mmap: map kernel buffers vào userspace address space
 *   4. VIDIOC_QBUF: enqueue tất cả buffers vào kernel queue
 *
 * TÁC ĐỘNG:
 *   - dev->buffers[] populated (start + length)
 *   - dev->n_buffers = số buffers thực tế (có thể < V4L2_NUM_BUFFERS)
 *   - Kernel sở hữu tất cả buffers (đã enqueue)
 *
 * CONTEXT:
 *   Process context, có thể sleep (mmap syscall)
 *
 * @dev: V4L2 device (phải đã set format)
 * @return: 0 OK, -1 lỗi
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

    if (req.count < 2) {
        fprintf(stderr, "[V4L2] Insufficient buffers: got %d, need >= 2\n", req.count);
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
            fprintf(stderr, "[V4L2] QUERYBUF %d failed: %s\n", i, strerror(errno));
            return -1;
        }

        dev->buffers[i].length = buf.length;
        dev->buffers[i].start = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, dev->fd, buf.m.offset);

        if (dev->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[V4L2] mmap buffer %d failed: %s\n", i, strerror(errno));
            dev->buffers[i].start = NULL;
            return -1;
        }

        /* Enqueue buffer cho kernel */
        if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[V4L2] QBUF %d failed: %s\n", i, strerror(errno));
            return -1;
        }
    }

    printf("[V4L2] All %d buffers mmap'd and enqueued\n", dev->n_buffers);
    return 0;
}

/*
 * v4l2_device_start — VIDIOC_STREAMON
 *
 * TÁC DỤNG:
 *   Bắt đầu streaming. Kernel driver sẽ bắt đầu fill các buffers
 *   đã enqueue với video frames từ camera.
 *
 * TÁC ĐỘNG:
 *   - dev->is_streaming = 1
 *   - dev->fps_timer_ms reset
 *   - Kernel bắt đầu DMA transfer vào MMAP buffers
 *
 * CONTEXT:
 *   Process context
 *
 * @dev: V4L2 device (phải đã init buffers)
 * @return: 0 OK, -1 lỗi
 */
int v4l2_device_start(V4L2Device *dev)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(dev->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "[V4L2] STREAMON failed: %s\n", strerror(errno));
        return -1;
    }
    dev->is_streaming = 1;
    dev->fps_timer_ms = now_ms();
    dev->frame_count  = 0;
    printf("[V4L2] Streaming started (%s %dx%d)\n",
           dev->pixfmt == V4L2_PIXFMT_MJPEG ? "MJPEG" : "other",
           dev->width, dev->height);
    return 0;
}

/*
 * v4l2_device_read_frame — DQBUF one frame + re-QBUF
 *
 * TÁC DỤNG:
 *   Dequeue buffer chứa MJPEG frame hoàn chỉnh từ kernel.
 *   Trả pointer trực tiếp vào mmap'd memory (zero-copy).
 *   Re-enqueue buffer ngay sau khi lấy pointer.
 *
 * TÁC ĐỘNG:
 *   - *data trỏ vào mmap'd buffer (caller KHÔNG được free)
 *   - *size = kích thước MJPEG frame
 *   - dev->frame_count++
 *   - dev->current_fps cập nhật mỗi 30 frames
 *   - Buffer re-enqueue → kernel có thể ghi frame tiếp vào
 *
 * CONTEXT:
 *   Process context. Non-blocking nếu O_NONBLOCK (return -2 EAGAIN).
 *
 * ⚠ LƯU Ý: Vì buffer được re-QBUF ngay, data pointer chỉ valid
 *   cho đến lần gọi read_frame tiếp theo. Caller phải xử lý
 *   hoặc copy data trước khi gọi lại.
 *
 * @dev:  V4L2 device (phải đang streaming)
 * @data: output — pointer tới MJPEG data
 * @size: output — kích thước frame (bytes)
 * @return: 0 OK, -1 lỗi, -2 EAGAIN (no frame ready)
 */
int v4l2_device_read_frame(V4L2Device *dev, uint8_t **data, uint32_t *size)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(dev->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return -2;  /* no frame ready yet */
        fprintf(stderr, "[V4L2] DQBUF failed: %s\n", strerror(errno));
        return -1;
    }

    *data = (uint8_t *)dev->buffers[buf.index].start;
    *size = buf.bytesused;

    /* FPS measurement: mỗi 30 frames */
    dev->frame_count++;
    if (dev->frame_count % 30 == 0) {
        uint64_t now = now_ms();
        uint64_t elapsed = now - dev->fps_timer_ms;
        if (elapsed > 0)
            dev->current_fps = 30000.0f / (float)elapsed;
        dev->fps_timer_ms = now;
    }

    /* Re-enqueue buffer */
    if (ioctl(dev->fd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "[V4L2] re-QBUF failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * v4l2_device_stop — VIDIOC_STREAMOFF
 *
 * TÁC DỤNG:
 *   Dừng streaming. Kernel driver ngừng fill buffers.
 *
 * TÁC ĐỘNG:
 *   - dev->is_streaming = 0
 *   - Tất cả buffers trở lại trạng thái dequeued
 *
 * CONTEXT:
 *   Process context
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
    printf("[V4L2] Streaming stopped (%d frames captured)\n", dev->frame_count);
    return 0;
}

/*
 * v4l2_device_close — Cleanup: munmap + close fd
 *
 * TÁC DỤNG:
 *   Giải phóng tất cả MMAP buffers và đóng file descriptor.
 *
 * TÁC ĐỘNG:
 *   - Tất cả dev->buffers[] unmapped
 *   - dev->fd = -1
 *   - Device không còn sử dụng được
 *
 * CONTEXT:
 *   Process context
 */
void v4l2_device_close(V4L2Device *dev)
{
    if (dev->fd < 0) return;

    /* Munmap all buffers */
    for (uint32_t i = 0; i < dev->n_buffers; i++) {
        if (dev->buffers[i].start && dev->buffers[i].start != MAP_FAILED) {
            munmap(dev->buffers[i].start, dev->buffers[i].length);
            dev->buffers[i].start = NULL;
        }
    }

    close(dev->fd);
    printf("[V4L2] Device %s closed\n", dev->dev_path);
    dev->fd = -1;
}

/*
 * v4l2_device_get_fps — Lấy FPS đo được
 *
 * TÁC DỤNG: Trả về FPS hiện tại (cập nhật mỗi 30 frames)
 * CONTEXT:  Thread-safe (chỉ đọc float)
 */
float v4l2_device_get_fps(V4L2Device *dev)
{
    return dev->current_fps;
}
