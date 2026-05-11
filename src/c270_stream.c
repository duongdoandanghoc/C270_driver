#include "c270_stream.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ────────────────────────────────────────────────────────────
 * GMainLoop thread — chạy GStreamer event loop
 * ──────────────────────────────────────────────────────────── */
static void *gst_loop_thread(void *arg) {
    GMainLoop *loop = (GMainLoop *)arg;
    g_main_loop_run(loop);
    return NULL;
}

/* ────────────────────────────────────────────────────────────
 * Media factory: tạo pipeline khi client connect
 *
 * Pipeline:
 *   appsrc (push RGB24 frames)
 *     → videoconvert
 *     → x264enc (zerolatency)
 *     → rtph264pay
 *     → (RTSP server gửi đi)
 * ──────────────────────────────────────────────────────────── */

typedef struct {
    StreamContext *stream_ctx;
    int            width;
    int            height;
    int            fps;
} FactoryData;

/* ────────────────────────────────────────────────────────────
 * Media unprepared callback — khi client disconnect, pipeline teardown
 * ──────────────────────────────────────────────────────────── */
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data) {
    (void)media;
    StreamContext *ctx = (StreamContext *)user_data;
    ctx->appsrc = NULL;   /* pipeline đã chết, ngừng push */
    printf("[STREAM] Client disconnected — appsrc cleared\n");
}

static void media_configure_cb(GstRTSPMediaFactory *factory,
                                GstRTSPMedia        *media,
                                gpointer             user_data)
{
    FactoryData *fdata = (FactoryData *)user_data;
    StreamContext *ctx = fdata->stream_ctx;

    GstElement *element = gst_rtsp_media_get_element(media);

    /* Lấy appsrc element từ pipeline */
    GstElement *appsrc = gst_bin_get_by_name_recurse_up(
        GST_BIN(element), "vsrc");

    if (!appsrc) {
        fprintf(stderr, "[STREAM] Cannot find appsrc element 'vsrc'\n");
        gst_object_unref(element);
        return;
    }

    /* Set caps cho appsrc */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format",    G_TYPE_STRING,  "RGB",
        "width",     G_TYPE_INT,     fdata->width,
        "height",    G_TYPE_INT,     fdata->height,
        "framerate", GST_TYPE_FRACTION, fdata->fps, 1,
        NULL);

    g_object_set(appsrc,
        "caps",          caps,
        "format",        GST_FORMAT_TIME,
        "is-live",       TRUE,
        "do-timestamp",  TRUE,
        NULL);
    gst_caps_unref(caps);

    ctx->appsrc = appsrc;   /* lưu lại để push frames sau này */

    /* Đăng ký cleanup khi pipeline teardown (client disconnect) */
    g_signal_connect(media, "unprepared",
                     G_CALLBACK(media_unprepared_cb), ctx);

    gst_object_unref(element);
    printf("[STREAM] Media configured — appsrc ready\n");
}

int stream_init(StreamContext *ctx, int port, const char *mount_point,
                int width, int height, int fps,
                const char *codec, const char *password)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->port = port;
    snprintf(ctx->mount_point, sizeof(ctx->mount_point), "%s", mount_point);
    snprintf(ctx->codec, sizeof(ctx->codec), "%s", codec ? codec : "h264");
    if (password && password[0])
        snprintf(ctx->password, sizeof(ctx->password), "%s", password);

    gst_init(NULL, NULL);

    /* RTSP server */
    GstRTSPServer *server = gst_rtsp_server_new();
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    gst_rtsp_server_set_service(server, port_str);
    ctx->rtsp_server = server;

    /* RTSP Authentication (optional) */
    if (ctx->password[0]) {
        GstRTSPAuth *auth = gst_rtsp_auth_new();
        GstRTSPToken *token = gst_rtsp_token_new(
            GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "user", NULL);
        gchar *basic = gst_rtsp_auth_make_basic("admin", ctx->password);
        gst_rtsp_auth_add_basic(auth, basic, token);
        g_free(basic);
        gst_rtsp_token_unref(token);

        /* Set default permissions */
        GstRTSPPermissions *perms = gst_rtsp_permissions_new();
        gst_rtsp_permissions_add_role(perms, "user",
            GST_RTSP_PERM_MEDIA_FACTORY_ACCESS, G_TYPE_BOOLEAN, TRUE,
            GST_RTSP_PERM_MEDIA_FACTORY_CONSTRUCT, G_TYPE_BOOLEAN, TRUE,
            NULL);

        gst_rtsp_server_set_auth(server, auth);
        g_object_unref(auth);

        printf("[STREAM] RTSP auth enabled (user: admin)\n");

        /* Mount points */
        GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
        GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_permissions(factory, perms);
        gst_rtsp_permissions_unref(perms);

        /* Build pipeline based on codec */
        char pipeline[512];
        if (strcmp(ctx->codec, "h265") == 0) {
            snprintf(pipeline, sizeof(pipeline),
                "( appsrc name=vsrc ! "
                "videoconvert ! "
                "video/x-raw,format=I420 ! "
                "x265enc tune=zerolatency bitrate=1000 "
                "speed-preset=ultrafast ! "
                "rtph265pay name=pay0 pt=96 config-interval=1 )");
        } else {
            snprintf(pipeline, sizeof(pipeline),
                "( appsrc name=vsrc ! "
                "videoconvert ! "
                "video/x-raw,format=I420 ! "
                "x264enc tune=zerolatency bitrate=1000 "
                "key-int-max=15 speed-preset=ultrafast ! "
                "rtph264pay name=pay0 pt=96 config-interval=1 )");
        }

        gst_rtsp_media_factory_set_launch(factory, pipeline);
        gst_rtsp_media_factory_set_shared(factory, TRUE);

        FactoryData *fdata = malloc(sizeof(FactoryData));
        fdata->stream_ctx = ctx;
        fdata->width  = width;
        fdata->height = height;
        fdata->fps    = fps;

        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(media_configure_cb), fdata);

        gst_rtsp_mount_points_add_factory(mounts, mount_point, factory);
        g_object_unref(mounts);
    } else {
        /* No auth — simpler path */
        GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
        GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

        char pipeline[512];
        if (strcmp(ctx->codec, "h265") == 0) {
            snprintf(pipeline, sizeof(pipeline),
                "( appsrc name=vsrc ! "
                "videoconvert ! "
                "video/x-raw,format=I420 ! "
                "x265enc tune=zerolatency bitrate=1000 "
                "speed-preset=ultrafast ! "
                "rtph265pay name=pay0 pt=96 config-interval=1 )");
        } else {
            snprintf(pipeline, sizeof(pipeline),
                "( appsrc name=vsrc ! "
                "videoconvert ! "
                "video/x-raw,format=I420 ! "
                "x264enc tune=zerolatency bitrate=1000 "
                "key-int-max=15 speed-preset=ultrafast ! "
                "rtph264pay name=pay0 pt=96 config-interval=1 )");
        }

        gst_rtsp_media_factory_set_launch(factory, pipeline);
        gst_rtsp_media_factory_set_shared(factory, TRUE);

        FactoryData *fdata = malloc(sizeof(FactoryData));
        fdata->stream_ctx = ctx;
        fdata->width  = width;
        fdata->height = height;
        fdata->fps    = fps;

        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(media_configure_cb), fdata);

        gst_rtsp_mount_points_add_factory(mounts, mount_point, factory);
        g_object_unref(mounts);
    }

    /* Attach server vào GMainContext */
    gst_rtsp_server_attach(server, NULL);

    /* Start GMainLoop trong thread riêng */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    ctx->main_loop = loop;

    pthread_t tid;
    pthread_create(&tid, NULL, gst_loop_thread, loop);
    ctx->gst_thread  = (unsigned long)tid;
    ctx->is_running  = 1;

    printf("[STREAM] RTSP server started (codec: %s)\n", ctx->codec);
    stream_print_url(ctx);
    return 0;
}

void stream_push_frame(StreamContext *ctx, const DecodedFrame *frame) {
    if (!ctx->is_running || !ctx->appsrc) return;

    GstElement *appsrc = (GstElement *)ctx->appsrc;
    size_t size = (size_t)(frame->width * frame->height * 3);

    /* Copy frame vào GstBuffer */
    GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    memcpy(map.data, frame->pixels, size);
    gst_buffer_unmap(buf, &map);

    /* Push vào appsrc */
    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    if (ret != GST_FLOW_OK) {
        /* GST_FLOW_FLUSHING (-2) = pipeline teardown (client disconnect)
         * Clear appsrc để ngừng push cho đến khi client mới connect */
        ctx->appsrc = NULL;
        if (ret == GST_FLOW_FLUSHING) {
            printf("[STREAM] Pipeline flushing — waiting for new client\n");
        } else {
            fprintf(stderr, "[STREAM] push-buffer error: %d\n", ret);
        }
    }
}

void stream_stop(StreamContext *ctx) {
    if (!ctx->is_running) return;
    ctx->is_running = 0;

    if (ctx->main_loop) {
        g_main_loop_quit((GMainLoop *)ctx->main_loop);
    }
    pthread_join((pthread_t)ctx->gst_thread, NULL);
    printf("[STREAM] RTSP server stopped\n");
}

void stream_free(StreamContext *ctx) {
    if (ctx->main_loop) {
        g_main_loop_unref((GMainLoop *)ctx->main_loop);
        ctx->main_loop = NULL;
    }
    if (ctx->rtsp_server) {
        gst_object_unref(ctx->rtsp_server);
        ctx->rtsp_server = NULL;
    }
    printf("[STREAM] Stream resources freed\n");
}

void stream_print_url(const StreamContext *ctx) {
    printf("[STREAM] ── Connect clients with: ──────────────────\n");
    printf("[STREAM]   vlc rtsp://<IPC_IP>:%d%s\n",
           ctx->port, ctx->mount_point);
    printf("[STREAM]   ffplay rtsp://<IPC_IP>:%d%s\n",
           ctx->port, ctx->mount_point);
    printf("[STREAM]   gst-launch-1.0 rtspsrc location=rtsp://<IPC_IP>:%d%s "
           "! decodebin ! autovideosink\n",
           ctx->port, ctx->mount_point);
    printf("[STREAM] ──────────────────────────────────────────\n");
}
