/*
 * main.c — C270 V4L2 Driver Entry Point (with auto-reconnect)
 *
 * Entry point cho ứng dụng C270 sử dụng V4L2 thay vì libusb.
 * Parse CLI arguments, mở /dev/videoX, capture MJPEG frames,
 * push tới RTSP server và/hoặc local SDL2 display.
 *
 * Khi camera bị rút ra (poll error / read error), driver sẽ:
 *   1. Hiện thông báo "DISCONNECTED" trên cửa sổ SDL2 (nền đỏ)
 *   2. Tự động thử reconnect mỗi 2 giây
 *   3. Hiện "RECONNECTING..." trên cửa sổ (nền cam)
 *   4. Khi kết nối lại → hiện "CONNECTED" (nền xanh) rồi resume stream
 *
 * ══════════════════════════════════════════════════════════════
 * CẤU TRÚC FILE
 * ══════════════════════════════════════════════════════════════
 *   1. print_usage()     — CLI help
 *   2. parse_args()      — getopt_long argument parsing
 *   3. sig_handler()     — SIGINT/SIGTERM graceful shutdown
 *   4. try_reconnect()   — close + reopen V4L2 device
 *   5. main()            — init → capture loop (with reconnect) → cleanup
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
#include <errno.h>
#include <time.h>

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

/* ── Reconnect timing ── */
#define RECONNECT_INTERVAL_MS  2000  /* thử lại mỗi 2 giây */
#define CONNECTED_SHOW_MS      1500  /* hiện "Connected" 1.5 giây */

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
    g_running = 0;
}

/*
 * try_open_device — Try to open and configure V4L2 device
 *
 * TÁC DỤNG:
 *   Mở device, set format MJPEG, init MMAP buffers, start streaming.
 *   Nếu bất kỳ bước nào fail → close lại và return -1.
 *
 * @return: 0 OK, -1 fail (device chưa sẵn sàng)
 */
static int try_open_device(void) {
    if (v4l2_device_open(&g_v4l2, g_app.dev_path) < 0)
        return -1;
    if (v4l2_device_set_format(&g_v4l2, g_app.width, g_app.height,
                               V4L2_PIXFMT_MJPEG) < 0) {
        v4l2_device_close(&g_v4l2);
        return -1;
    }
    if (v4l2_device_init_buffers(&g_v4l2) < 0) {
        v4l2_device_close(&g_v4l2);
        return -1;
    }
    if (v4l2_device_start(&g_v4l2) < 0) {
        v4l2_device_close(&g_v4l2);
        return -1;
    }
    return 0;
}

/*
 * now_ms — Get current time in milliseconds
 */
static uint64_t app_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ────────────────────────────────────────────────────────────
 * Main
 * ──────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    g_app = parse_args(argc, argv);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("╔══════════════════════════════════════════╗\n");
    printf("║  C270 V4L2 Driver v4.0 (Auto-Reconnect) ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("[MAIN] Device: %s  MJPEG %dx%d @ %dfps\n",
           g_app.dev_path, g_app.width, g_app.height, g_app.fps);
    printf("[MAIN] RTSP: %s  Display: %s  Codec: %s\n",
           g_app.no_stream  ? "OFF" : "ON",
           g_app.no_display ? "OFF" : "ON",
           g_app.codec);
    printf("\n");

    /* ── Step 1: Init display (optional) — init trước device để hiện status ── */
    if (!g_app.no_display) {
        if (display_init(&g_display, g_app.width, g_app.height,
                         "C270 V4L2 Driver") < 0) goto cleanup;
    }

    /* ── Step 2: Init RTSP stream (optional) ── */
    if (!g_app.no_stream) {
        if (stream_init(&g_stream, g_app.port, g_app.mount_point,
                        g_v4l2.width ? g_v4l2.width : g_app.width,
                        g_v4l2.height ? g_v4l2.height : g_app.height,
                        g_app.fps,
                        g_app.codec, g_app.password) < 0) goto cleanup;
    }

    /* ── Step 3: Open V4L2 device ── */
    if (!g_app.no_display) {
        display_show_status(&g_display, DISP_STATUS_RECONNECTING,
                            "Dang ket noi camera...", 0);
    }

    if (try_open_device() < 0) {
        fprintf(stderr, "[MAIN] Không mở được %s, chờ camera...\n",
                g_app.dev_path);
        if (!g_app.no_display) {
            display_show_status(&g_display, DISP_STATUS_DISCONNECTED,
                                "Camera chua duoc ket noi", 0);
        }
    } else {
        printf("[MAIN] Camera kết nối thành công!\n");
        if (!g_app.no_display) {
            display_show_status(&g_display, DISP_STATUS_CONNECTED,
                                "Camera da ket noi", 0);
            /* Show green status briefly */
            uint64_t show_until = app_now_ms() + CONNECTED_SHOW_MS;
            while (app_now_ms() < show_until && g_running) {
                if (!g_app.no_display && display_poll_events(&g_display) < 0) {
                    g_running = 0; break;
                }
                usleep(50000);
            }
        }
    }

    printf("\n[MAIN] Running. Press Ctrl+C or ESC to quit.\n\n");

    /* ── Main capture loop with auto-reconnect ── */
    struct pollfd pfd;
    int           camera_connected = (g_v4l2.fd >= 0);
    uint64_t      last_reconnect_attempt = 0;
    int           consecutive_errors = 0;
    int           total_reconnects = 0;

    /* ── Uptime tracking ──
     * uptime_accumulated_ms: tổng thời gian camera đã hoạt động (ms)
     * uptime_connect_time:   timestamp lúc connect gần nhất
     * Khi disconnect: cộng (now - connect_time) vào accumulated
     * Khi reconnect:  lưu connect_time mới
     */
    uint64_t      uptime_accumulated_ms = 0;
    uint64_t      uptime_connect_time = camera_connected ? app_now_ms() : 0;

    while (g_running) {
        /* ── Camera disconnected: show status + try reconnect ── */
        if (!camera_connected) {
            uint64_t now = app_now_ms();

            /* Poll display events (let user ESC to quit) */
            if (!g_app.no_display && display_poll_events(&g_display) < 0) {
                g_running = 0;
                break;
            }

            /* Try reconnect every RECONNECT_INTERVAL_MS */
            if (now - last_reconnect_attempt >= RECONNECT_INTERVAL_MS) {
                last_reconnect_attempt = now;

                printf("[MAIN] Đang thử kết nối lại camera...\n");
                if (!g_app.no_display) {
                    display_show_status(&g_display, DISP_STATUS_RECONNECTING,
                                        "Dang ket noi lai camera...",
                                        (uint32_t)(uptime_accumulated_ms / 1000));
                }

                if (try_open_device() == 0) {
                    /* ─── Reconnect thành công ─── */
                    camera_connected = 1;
                    consecutive_errors = 0;
                    total_reconnects++;
                    uptime_connect_time = app_now_ms(); /* resume timer */

                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "Camera da ket noi lai! (lan %d)", total_reconnects);
                    printf("[MAIN] %s\n", msg);

                    if (!g_app.no_display) {
                        display_show_status(&g_display, DISP_STATUS_CONNECTED, msg,
                                            (uint32_t)(uptime_accumulated_ms / 1000));
                        /* Show green briefly */
                        uint64_t show_until = app_now_ms() + CONNECTED_SHOW_MS;
                        while (app_now_ms() < show_until && g_running) {
                            if (display_poll_events(&g_display) < 0) {
                                g_running = 0; break;
                            }
                            usleep(50000);
                        }
                    }
                }
            }

            usleep(100000); /* 100ms sleep trong disconnect loop */
            continue;
        }

        /* ── Camera connected: normal capture loop ── */
        pfd.fd     = g_v4l2.fd;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, 50);

        if (pr < 0) {
            if (errno == EINTR) continue; /* signal interrupt */
            /* poll error → camera likely disconnected */
            fprintf(stderr, "[MAIN] poll error: %s → camera disconnected\n",
                    strerror(errno));
            goto handle_disconnect;
        }

        if (pr == 0) {
            /* Timeout — check display events */
            if (!g_app.no_display && display_poll_events(&g_display) < 0)
                g_running = 0;
            continue;
        }

        /* Check for POLLERR / POLLHUP — device removed */
        if (pfd.revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "[MAIN] POLLERR/POLLHUP → camera disconnected\n");
            goto handle_disconnect;
        }

        /* Read MJPEG frame */
        uint8_t *mjpeg_data = NULL;
        uint32_t mjpeg_size = 0;
        int ret = v4l2_device_read_frame(&g_v4l2, &mjpeg_data, &mjpeg_size);

        if (ret == 0 && mjpeg_data && mjpeg_size > 0) {
            consecutive_errors = 0;  /* reset error counter */
            float fps = v4l2_device_get_fps(&g_v4l2);

            if (!g_app.no_stream)
                stream_push_mjpeg(&g_stream, mjpeg_data, mjpeg_size);

            if (!g_app.no_display) {
                /* Compute live uptime */
                uint64_t live_uptime_ms = uptime_accumulated_ms +
                    (app_now_ms() - uptime_connect_time);
                display_show_mjpeg(&g_display, mjpeg_data, mjpeg_size,
                                   fps, "C270-V4L2",
                                   (uint32_t)(live_uptime_ms / 1000));
            }
        } else if (ret == -1) {
            consecutive_errors++;
            if (consecutive_errors >= 5) {
                fprintf(stderr,
                        "[MAIN] %d read errors liên tiếp → camera disconnected\n",
                        consecutive_errors);
                goto handle_disconnect;
            }
        }
        /* ret == -2: EAGAIN, just continue */

        if (!g_app.no_display && display_poll_events(&g_display) < 0)
            g_running = 0;

        continue;

    handle_disconnect:
        /* ─── Camera bị rút / lỗi → cleanup và chuyển sang reconnect ─── */
        printf("[MAIN] ⚠ Camera disconnected! Đang chờ kết nối lại...\n");

        v4l2_device_stop(&g_v4l2);
        v4l2_device_close(&g_v4l2);

        /* Pause uptime: save accumulated time */
        if (uptime_connect_time > 0) {
            uptime_accumulated_ms += (app_now_ms() - uptime_connect_time);
            uptime_connect_time = 0;
        }

        camera_connected = 0;
        consecutive_errors = 0;
        last_reconnect_attempt = 0;  /* try immediately next loop */

        if (!g_app.no_display) {
            display_show_status(&g_display, DISP_STATUS_DISCONNECTED,
                                "Camera da bi rut! Cho ket noi lai...",
                                (uint32_t)(uptime_accumulated_ms / 1000));
        }
    }

    /* ── Cleanup ── */
cleanup:
    if (camera_connected) {
        v4l2_device_stop(&g_v4l2);
    }
    if (!g_app.no_stream) {
        stream_stop(&g_stream);
        stream_free(&g_stream);
    }
    if (!g_app.no_display)
        display_free(&g_display);
    v4l2_device_close(&g_v4l2);

    printf("[MAIN] Goodbye. (Reconnects: %d)\n", total_reconnects);
    return 0;
}
