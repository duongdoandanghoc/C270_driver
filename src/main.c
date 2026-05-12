/*
 * main.c — C270 V4L2 Driver Entry Point
 *
 * Entry point cho ứng dụng C270 sử dụng V4L2 thay vì libusb.
 * Parse CLI arguments, mở /dev/videoX, capture MJPEG frames,
 * push tới RTSP server và/hoặc local SDL2 display.
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. print_usage()   — CLI help
 *   2. parse_args()    — getopt_long argument parsing
 *   3. sig_handler()   — SIGINT/SIGTERM graceful shutdown
 *   4. main()          — V4L2 init → capture loop → cleanup
 *
 * ══════════════════════════════════════════════════════════════
 * VỊ TRÍ TRONG PIPELINE
 * ══════════════════════════════════════════════════════════════
 *
 *   /dev/videoX (uvcvideo hoặc mycam.ko)
 *       │  MJPEG frames
 *       ▼
 *   ┌───────────────────────────┐
 *   │  main.c  ◄────────────────│── FILE NÀY
 *   │    v4l2_device_read_frame │
 *   │    → stream_push_mjpeg   │
 *   │    → display_show_mjpeg  │
 *   └──────────┬────────────────┘
 *              │
 *              ▼
 *   c270_stream.c (RTSP)  /  c270_display.c (SDL2)
 */

#include "c270_v4l2.h"
#include "c270_display.h"
#include "c270_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <poll.h>

/* ── Global state ── */
static volatile int g_running = 1;

static V4L2Device     g_v4l2;
static DisplayContext g_display;
static StreamContext  g_stream;

/* ── Application config ── */
typedef struct {
    char     dev_path[32];   /* /dev/videoX */
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    int      port;
    char     mount_point[64];
    char     codec[16];
    char     password[64];
    int      no_display;
    int      no_stream;
} AppConfig;

static AppConfig g_app;

/* ────────────────────────────────────────────────────────────
 * Usage / Help
 * ──────────────────────────────────────────────────────────── */
static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("C270 V4L2 Driver — MJPEG camera streaming via /dev/videoX\n\n");
    printf("Device options:\n");
    printf("  -d, --device PATH    V4L2 device       (default: /dev/video0)\n");
    printf("  -W, --width N        Video width        (default: 640)\n");
    printf("  -H, --height N       Video height       (default: 480)\n");
    printf("  -f, --fps N          Frame rate         (default: 30)\n");
    printf("\nStreaming options:\n");
    printf("  -p, --port N         RTSP port          (default: 8554)\n");
    printf("  -m, --mount PATH     RTSP mount point   (default: /camera0)\n");
    printf("  -C, --codec CODEC    h264 or h265       (default: h264)\n");
    printf("  -P, --password PWD   RTSP password      (default: none)\n");
    printf("\nFeature toggles:\n");
    printf("      --no-display     Disable SDL local display\n");
    printf("      --no-stream      Disable RTSP streaming\n");
    printf("      --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                                   # MJPEG 640x480 H264\n", prog);
    printf("  %s -d /dev/video2 -C h265            # Custom device + H265\n", prog);
    printf("  %s --no-display -p 9554              # Headless mode\n", prog);
}

/* ────────────────────────────────────────────────────────────
 * Parse CLI arguments
 * ──────────────────────────────────────────────────────────── */
static AppConfig parse_args(int argc, char *argv[]) {
    AppConfig app;
    memset(&app, 0, sizeof(app));
    /* Defaults */
    strncpy(app.dev_path, "/dev/video0", sizeof(app.dev_path) - 1);
    app.width  = 640;
    app.height = 480;
    app.fps    = 30;
    app.port   = 8554;
    strncpy(app.mount_point, "/camera0", sizeof(app.mount_point) - 1);
    strncpy(app.codec, "h264", sizeof(app.codec) - 1);

    static struct option long_options[] = {
        {"device",      required_argument, 0, 'd'},
        {"width",       required_argument, 0, 'W'},
        {"height",      required_argument, 0, 'H'},
        {"fps",         required_argument, 0, 'f'},
        {"port",        required_argument, 0, 'p'},
        {"mount",       required_argument, 0, 'm'},
        {"codec",       required_argument, 0, 'C'},
        {"password",    required_argument, 0, 'P'},
        {"no-display",  no_argument,       0, 'D'},
        {"no-stream",   no_argument,       0, 'S'},
        {"help",        no_argument,       0, 0xFF},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "d:W:H:f:p:m:C:P:DS",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'd': strncpy(app.dev_path, optarg, sizeof(app.dev_path) - 1); break;
        case 'W': app.width  = (uint16_t)atoi(optarg); break;
        case 'H': app.height = (uint16_t)atoi(optarg); break;
        case 'f': app.fps    = (uint8_t)atoi(optarg);  break;
        case 'p': app.port   = atoi(optarg); break;
        case 'm': strncpy(app.mount_point, optarg, sizeof(app.mount_point) - 1); break;
        case 'C': strncpy(app.codec, optarg, sizeof(app.codec) - 1); break;
        case 'P': strncpy(app.password, optarg, sizeof(app.password) - 1); break;
        case 'D': app.no_display = 1; break;
        case 'S': app.no_stream  = 1; break;
        default:
            print_usage(argv[0]);
            exit(0);
        }
    }
    return app;
}

/*
 * sig_handler — Graceful shutdown on SIGINT/SIGTERM
 *
 * TÁC DỤNG: Set g_running = 0 để main loop thoát
 * CONTEXT:  Signal handler context
 */
static void sig_handler(int sig) {
    (void)sig;
    printf("\n[MAIN] Shutting down...\n");
    g_running = 0;
}

/* ────────────────────────────────────────────────────────────
 * Main
 * ──────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    g_app = parse_args(argc, argv);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("╔══════════════════════════════════╗\n");
    printf("║  C270 V4L2 Driver v3.0 (Phase C) ║\n");
    printf("╚══════════════════════════════════╝\n");
    printf("[MAIN] Device: %s  MJPEG %dx%d @ %dfps\n",
           g_app.dev_path, g_app.width, g_app.height, g_app.fps);
    printf("[MAIN] RTSP: %s  Display: %s  Codec: %s\n",
           g_app.no_stream  ? "OFF" : "ON",
           g_app.no_display ? "OFF" : "ON",
           g_app.codec);
    printf("\n");

    /* ── Step 1: Open V4L2 device ── */
    if (v4l2_device_open(&g_v4l2, g_app.dev_path) < 0) goto cleanup;

    /* ── Step 2: Set MJPEG format ── */
    if (v4l2_device_set_format(&g_v4l2, g_app.width, g_app.height,
                               V4L2_PIXFMT_MJPEG) < 0) goto cleanup;

    /* ── Step 3: Init MMAP buffers ── */
    if (v4l2_device_init_buffers(&g_v4l2) < 0) goto cleanup;

    /* ── Step 4: Init display (optional) ── */
    if (!g_app.no_display) {
        if (display_init(&g_display, g_v4l2.width, g_v4l2.height,
                         "C270 V4L2 Driver") < 0) goto cleanup;
    }

    /* ── Step 5: Init RTSP stream (optional) ── */
    if (!g_app.no_stream) {
        if (stream_init(&g_stream, g_app.port, g_app.mount_point,
                        g_v4l2.width, g_v4l2.height, g_app.fps,
                        g_app.codec, g_app.password) < 0) goto cleanup;
    }

    /* ── Step 6: Start streaming ── */
    if (v4l2_device_start(&g_v4l2) < 0) goto cleanup;

    printf("\n[MAIN] Running. Press Ctrl+C or ESC to quit.\n\n");

    /* ── Main capture loop ── */
    struct pollfd pfd;
    pfd.fd     = g_v4l2.fd;
    pfd.events = POLLIN;

    while (g_running) {
        /* Wait for frame ready (50ms timeout) */
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (g_running) perror("[MAIN] poll");
            break;
        }
        if (pr == 0) {
            /* Timeout — check display events */
            if (!g_app.no_display && display_poll_events(&g_display) < 0)
                g_running = 0;
            continue;
        }

        /* Read MJPEG frame */
        uint8_t *mjpeg_data = NULL;
        uint32_t mjpeg_size = 0;
        int ret = v4l2_device_read_frame(&g_v4l2, &mjpeg_data, &mjpeg_size);

        if (ret == 0 && mjpeg_data && mjpeg_size > 0) {
            float fps = v4l2_device_get_fps(&g_v4l2);

            if (!g_app.no_stream)
                stream_push_mjpeg(&g_stream, mjpeg_data, mjpeg_size);

            if (!g_app.no_display)
                display_show_mjpeg(&g_display, mjpeg_data, mjpeg_size,
                                   fps, "C270-V4L2");
        } else if (ret == -1) {
            fprintf(stderr, "[MAIN] Read frame error, stopping\n");
            break;
        }
        /* ret == -2: EAGAIN, just continue */

        if (!g_app.no_display && display_poll_events(&g_display) < 0)
            g_running = 0;
    }

    /* ── Cleanup ── */
cleanup:
    v4l2_device_stop(&g_v4l2);
    if (!g_app.no_stream) {
        stream_stop(&g_stream);
        stream_free(&g_stream);
    }
    if (!g_app.no_display)
        display_free(&g_display);
    v4l2_device_close(&g_v4l2);

    printf("[MAIN] Goodbye.\n");
    return 0;
}
