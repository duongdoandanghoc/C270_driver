#ifndef C270_CAPTURE_H
#define C270_CAPTURE_H

#include "c270_usb.h"
#include "c270_uvc.h"
#include <stdint.h>
#include <stddef.h>

/* ── Isochronous transfer config ── */
#define ISO_NUM_TRANSFERS    8      /* số lượng transfer buffer đồng thời */
#define ISO_PACKETS_PER_XFER 32       /* packets mỗi transfer */
#define ISO_PACKET_SIZE      960    /* bytes mỗi packet (C270 max) */

/* ── Frame buffer ── */
#define FRAME_BUFFER_SIZE    (640 * 480 * 3)  /* giữ lớn cho mọi resolution */  /* worst case raw */

typedef struct {
    uint8_t  data[FRAME_BUFFER_SIZE];
    size_t   size;         /* bytes đã ghi vào buffer */
    uint8_t  last_frame_id;
    int      is_complete;
} FrameBuffer;

/* ── Decoded frame (RGB) ── */
typedef struct {
    uint8_t  *pixels;      /* RGB24 data, width*height*3 bytes */
    int       width;
    int       height;
    uint64_t  timestamp_ms;
    uint32_t  frame_number;
} DecodedFrame;

/* ── Callback khi có frame mới ── */
typedef void (*FrameCallback)(const DecodedFrame *frame, void *userdata);

/* ── Capture context ── */
typedef struct {
    C270Device              *dev;
    C270Config               cfg;

    struct libusb_transfer  *transfers[ISO_NUM_TRANSFERS];
    uint8_t                 *transfer_bufs[ISO_NUM_TRANSFERS];

    FrameBuffer              frame_buf;
    DecodedFrame             decoded;

    FrameCallback            on_frame;
    void                    *userdata;

    volatile int             running;
    uint32_t                 frame_count;
    uint64_t                 fps_timer_ms;
    float                    current_fps;
} CaptureContext;

/* ── Public API ── */
int  capture_init(CaptureContext *ctx, C270Device *dev,
                  const C270Config *cfg,
                  FrameCallback cb, void *userdata);

int  capture_start(CaptureContext *ctx);
void capture_stop(CaptureContext *ctx);
void capture_free(CaptureContext *ctx);

/* Lấy FPS hiện tại */
float capture_get_fps(CaptureContext *ctx);

#endif /* C270_CAPTURE_H */
