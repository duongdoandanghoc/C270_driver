#ifndef C270_V4L2_H
#define C270_V4L2_H

/*
 * c270_v4l2.h — V4L2 Device Abstraction
 *
 * Thay thế c270_usb.h + c270_uvc.h cho pipeline V4L2.
 * Cung cấp interface mở/đóng /dev/videoX, set format MJPEG,
 * quản lý MMAP buffers, và capture loop DQBUF/QBUF.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC
 * ══════════════════════════════════════════════════════════════
 *   V4L2Device       — context chứa fd, buffers, format info
 *   V4L2MappedBuffer — mmap'd buffer (start pointer + length)
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   /dev/videoX (kernel: uvcvideo hoặc mycam.ko)
 *       │  MJPEG frames qua V4L2 MMAP
 *       ▼
 *   ┌───────────────────────────┐
 *   │  c270_v4l2.c (file này)   │
 *   │    open → set_format →    │
 *   │    init_buffers → start → │
 *   │    read_frame loop        │
 *   └──────────┬────────────────┘
 *              │  MJPEG raw data
 *              ▼
 *   main.c → c270_stream.c (RTSP)
 *          → c270_display.c (SDL2)
 */

#include <stdint.h>
#include <stddef.h>

/* ── Configuration ── */
#define V4L2_NUM_BUFFERS      4                  /* MMAP buffer count */
#define V4L2_MAX_FRAME_SIZE   (640 * 480 * 2)    /* MJPEG worst case */

/* ── V4L2 pixel format constants (avoid including kernel header in .h) ── */
#define V4L2_PIXFMT_MJPEG     0x47504A4D  /* 'MJPG' */
#define V4L2_PIXFMT_YUYV      0x56595559  /* 'YUYV' */

/* ── Mapped buffer ── */
typedef struct {
    void   *start;     /* mmap'd address */
    size_t  length;    /* buffer size in bytes */
} V4L2MappedBuffer;

/* ── V4L2 device context — replaces C270Device ── */
typedef struct {
    int                 fd;             /* /dev/videoX file descriptor */
    char                dev_path[32];   /* e.g. "/dev/video0" */
    uint16_t            width;
    uint16_t            height;
    uint8_t             fps;
    uint32_t            pixfmt;         /* V4L2_PIXFMT_MJPEG */
    V4L2MappedBuffer    buffers[V4L2_NUM_BUFFERS];
    uint32_t            n_buffers;      /* actual allocated count */
    int                 is_streaming;
    uint32_t            frame_count;    /* total frames captured */
    float               current_fps;    /* measured FPS */
    uint64_t            fps_timer_ms;   /* for FPS calculation */
} V4L2Device;

/* ── Public API ── */

/* Open V4L2 device and verify capabilities */
int  v4l2_device_open(V4L2Device *dev, const char *path);

/* Set pixel format and resolution via VIDIOC_S_FMT */
int  v4l2_device_set_format(V4L2Device *dev, uint16_t w, uint16_t h, uint32_t pixfmt);

/* Allocate MMAP buffers: REQBUFS + QUERYBUF + mmap + QBUF */
int  v4l2_device_init_buffers(V4L2Device *dev);

/* Start streaming: VIDIOC_STREAMON */
int  v4l2_device_start(V4L2Device *dev);

/* Read one frame: DQBUF + re-QBUF. Returns 0=ok, -1=error, -2=EAGAIN */
int  v4l2_device_read_frame(V4L2Device *dev, uint8_t **data, uint32_t *size);

/* Stop streaming: VIDIOC_STREAMOFF */
int  v4l2_device_stop(V4L2Device *dev);

/* Cleanup: munmap all buffers + close fd */
void v4l2_device_close(V4L2Device *dev);

/* Get current measured FPS */
float v4l2_device_get_fps(V4L2Device *dev);

#endif /* C270_V4L2_H */
