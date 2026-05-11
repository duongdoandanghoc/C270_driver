#ifndef C270_STREAM_H
#define C270_STREAM_H

#include "c270_capture.h"
#include <stdint.h>

typedef struct {
    void        *rtsp_server;   /* GstRTSPServer* */
    void        *appsrc;        /* GstElement* — appsrc để push frame vào */
    void        *pipeline;      /* GstElement* */
    void        *main_loop;     /* GMainLoop* */

    int          port;
    char         mount_point[64];
    char         codec[16];     /* "h264" hoặc "h265" */
    char         password[64];  /* RTSP password (empty = no auth) */
    int          is_running;

    /* Thread chạy GMainLoop */
    unsigned long gst_thread;   /* pthread_t */
} StreamContext;

/* ── Public API ── */
int  stream_init(StreamContext *ctx, int port, const char *mount_point,
                 int width, int height, int fps,
                 const char *codec, const char *password);
void stream_push_frame(StreamContext *ctx, const DecodedFrame *frame);
void stream_stop(StreamContext *ctx);
void stream_free(StreamContext *ctx);

/* URL để client connect */
void stream_print_url(const StreamContext *ctx);

#endif /* C270_STREAM_H */
