#include "c270_usb.h"
#include "c270_uvc.h"
#include "c270_capture.h"
#include "c270_display.h"
#include "c270_stream.h"
#include "c270_hotplug.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <libusb-1.0/libusb.h>

/* ── Global state để signal handler có thể truy cập ── */
static volatile int g_running = 1;

static C270Device     g_dev;
static CaptureContext g_capture;
static DisplayContext g_display;
static StreamContext  g_stream;
static HotplugContext g_hotplug;

/* ── Application config (from CLI arguments) ── */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    uint8_t  format_index;   /* 1=YUYV, 2=MJPEG */
    uint8_t  frame_index;
    int      port;
    char     mount_point[64];
    char     codec[16];      /* h264 hoặc h265 */
    char     password[64];   /* RTSP password (empty = no auth) */
    int16_t  brightness;
    int16_t  contrast;
    int32_t  exposure;       /* 0 = auto, >0 = manual (100us units) */
    int      no_display;
    int      no_stream;
    int      no_hotplug;
    int      list_cameras;
} AppConfig;

static AppConfig g_app;

/* ────────────────────────────────────────────────────────────
 * List USB UVC cameras
 * ──────────────────────────────────────────────────────────── */
static void list_usb_cameras(void) {
    libusb_context *ctx = NULL;
    libusb_init(&ctx);

    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);

    printf("═══ Available USB Cameras ═══\n\n");
    int found = 0;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) < 0) continue;

        /* Check for Video class devices (0x0E = Misc, 0xEF = multi-function)
         * or check interface descriptors for Video class */
        struct libusb_config_descriptor *config = NULL;
        if (libusb_get_active_config_descriptor(devs[i], &config) < 0) continue;

        int is_video = 0;
        for (int j = 0; j < config->bNumInterfaces; j++) {
            for (int k = 0; k < config->interface[j].num_altsetting; k++) {
                if (config->interface[j].altsetting[k].bInterfaceClass == 0x0E) {
                    is_video = 1; break;
                }
            }
            if (is_video) break;
        }

        if (is_video) {
            found++;
            const char *name = "Unknown";
            if (desc.idVendor == 0x046d && desc.idProduct == 0x0825)
                name = "Logitech C270";
            else if (desc.idVendor == 0x046d)
                name = "Logitech Camera";

            printf("  [%d] %04x:%04x  %s  (bus %d, port %d)\n",
                   found, desc.idVendor, desc.idProduct, name,
                   libusb_get_bus_number(devs[i]),
                   libusb_get_port_number(devs[i]));
        }
        libusb_free_config_descriptor(config);
    }

    if (found == 0)
        printf("  (Không tìm thấy camera USB nào)\n");
    else
        printf("\n  Tổng: %d camera(s)\n", found);

    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);
}

/* ────────────────────────────────────────────────────────────
 * Usage / Help
 * ──────────────────────────────────────────────────────────── */
static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("C270 Custom UVC Driver — MJPEG/YUYV camera streaming\n\n");
    printf("Video options:\n");
    printf("  -W, --width N        Video width       (default: 640)\n");
    printf("  -H, --height N       Video height      (default: 480)\n");
    printf("  -f, --fps N          Frame rate        (default: 30)\n");
    printf("  -F, --format FMT     mjpeg or yuyv     (default: mjpeg)\n");
    printf("  -i, --frame-index N  UVC frame index   (default: auto)\n");
    printf("\nCamera controls:\n");
    printf("  -b, --brightness N   Brightness 0-255  (default: 128)\n");
    printf("  -c, --contrast N     Contrast 0-255    (default: 32)\n");
    printf("  -e, --exposure N     Exposure (100us)  (default: auto)\n");
    printf("\nStreaming options:\n");
    printf("  -p, --port N         RTSP port         (default: 8554)\n");
    printf("  -m, --mount PATH     RTSP mount point  (default: /camera0)\n");
    printf("  -C, --codec CODEC    h264 or h265      (default: h264)\n");
    printf("  -P, --password PWD   RTSP password     (default: none)\n");
    printf("\nFeature toggles:\n");
    printf("      --list           List available USB cameras\n");
    printf("      --no-display     Disable SDL local display\n");
    printf("      --no-stream      Disable RTSP streaming\n");
    printf("      --no-hotplug     Disable auto-reconnect watchdog\n");
    printf("      --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                                   # MJPEG 640x480 H264\n", prog);
    printf("  %s -C h265 -P secret                 # H265 + password\n", prog);
    printf("  %s -e 500 -b 200                     # Manual exposure\n", prog);
    printf("  %s --list                            # List cameras\n", prog);
    printf("  %s --no-display -p 9554              # Headless mode\n", prog);
}

/* ────────────────────────────────────────────────────────────
 * Parse CLI arguments
 * ──────────────────────────────────────────────────────────── */
static AppConfig parse_args(int argc, char *argv[]) {
    AppConfig app;
    memset(&app, 0, sizeof(app));
    /* Defaults: MJPEG 640x480 @ 30fps, H264, no auth */
    app.width        = 640;
    app.height       = 480;
    app.fps          = 30;
    app.format_index = 2;     /* MJPEG */
    app.frame_index  = 1;     /* 640x480 cho MJPEG */
    app.port         = 8554;
    app.brightness   = 128;
    app.contrast     = 32;
    app.exposure     = 0;     /* 0 = auto */
    strncpy(app.mount_point, "/camera0", sizeof(app.mount_point) - 1);
    strncpy(app.codec, "h264", sizeof(app.codec) - 1);

    static struct option long_options[] = {
        {"width",       required_argument, 0, 'W'},
        {"height",      required_argument, 0, 'H'},
        {"fps",         required_argument, 0, 'f'},
        {"format",      required_argument, 0, 'F'},
        {"frame-index", required_argument, 0, 'i'},
        {"port",        required_argument, 0, 'p'},
        {"mount",       required_argument, 0, 'm'},
        {"codec",       required_argument, 0, 'C'},
        {"password",    required_argument, 0, 'P'},
        {"brightness",  required_argument, 0, 'b'},
        {"contrast",    required_argument, 0, 'c'},
        {"exposure",    required_argument, 0, 'e'},
        {"list",        no_argument,       0, 'L'},
        {"no-display",  no_argument,       0, 'D'},
        {"no-stream",   no_argument,       0, 'S'},
        {"no-hotplug",  no_argument,       0, 'N'},
        {"help",        no_argument,       0, 0xFF},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "W:H:f:F:i:p:m:C:P:b:c:e:LDSN",
                              long_options, NULL)) != -1) {
        switch (opt) {
        case 'W': app.width  = (uint16_t)atoi(optarg); break;
        case 'H': app.height = (uint16_t)atoi(optarg); break;
        case 'f': app.fps    = (uint8_t)atoi(optarg);  break;
        case 'F':
            if (strcmp(optarg, "yuyv") == 0 || strcmp(optarg, "YUYV") == 0) {
                app.format_index = 1;
                if (app.width == 640 && app.height == 480) {
                    app.width = 176; app.height = 144;
                    app.frame_index = 3;
                }
            } else {
                app.format_index = 2;
            }
            break;
        case 'i': app.frame_index = (uint8_t)atoi(optarg); break;
        case 'p': app.port = atoi(optarg); break;
        case 'm': strncpy(app.mount_point, optarg, sizeof(app.mount_point) - 1); break;
        case 'C': strncpy(app.codec, optarg, sizeof(app.codec) - 1); break;
        case 'P': strncpy(app.password, optarg, sizeof(app.password) - 1); break;
        case 'b': app.brightness = (int16_t)atoi(optarg); break;
        case 'c': app.contrast   = (int16_t)atoi(optarg); break;
        case 'e': app.exposure   = (int32_t)atoi(optarg); break;
        case 'L': app.list_cameras = 1; break;
        case 'D': app.no_display = 1; break;
        case 'S': app.no_stream  = 1; break;
        case 'N': app.no_hotplug = 1; break;
        default:
            print_usage(argv[0]);
            exit(0);
        }
    }
    return app;
}

/* ────────────────────────────────────────────────────────────
 * Frame callback — gọi mỗi khi có frame mới từ camera
 * ──────────────────────────────────────────────────────────── */
static void on_frame(const DecodedFrame *frame, void *userdata) {
    (void)userdata;
    float fps = capture_get_fps(&g_capture);

    if (!g_app.no_display)
        display_show_frame(&g_display, frame, fps, "C270-0");
    if (!g_app.no_stream)
        stream_push_frame(&g_stream, frame);
}

/* ────────────────────────────────────────────────────────────
 * Hotplug state change callback
 * ──────────────────────────────────────────────────────────── */
static void on_camera_state(CameraState state, void *userdata) {
    (void)userdata;
    printf("[MAIN] Camera state: %s\n", hotplug_state_name(state));

    if (state == CAM_STATE_STREAMING) {
        C270Config cfg = {
            .width        = g_app.width,
            .height       = g_app.height,
            .fps          = g_app.fps,
            .format_index = g_app.format_index,
            .frame_index  = g_app.frame_index
        };
        c270_uvc_start_stream(&g_dev, &cfg);
        capture_start(&g_capture);
        printf("[MAIN] Stream resumed after reconnect\n");
    }
    else if (state == CAM_STATE_DISCONNECTED) {
        capture_stop(&g_capture);
        printf("[MAIN] Stream paused — waiting for camera\n");
    }
}

/* ────────────────────────────────────────────────────────────
 * Signal handler
 * ──────────────────────────────────────────────────────────── */
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

    /* --list: chỉ liệt kê cameras rồi thoát */
    if (g_app.list_cameras) {
        list_usb_cameras();
        return 0;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    const char *fmt_name = (g_app.format_index == 2) ? "MJPEG" : "YUYV";
    printf("╔══════════════════════════════════╗\n");
    printf("║   C270 Custom UVC Driver v2.0    ║\n");
    printf("╚══════════════════════════════════╝\n");
    printf("[MAIN] Config: %s %dx%d @ %dfps  Codec: %s\n",
           fmt_name, g_app.width, g_app.height, g_app.fps, g_app.codec);
    printf("[MAIN] RTSP: %s  Display: %s  Hotplug: %s  Auth: %s\n",
           g_app.no_stream  ? "OFF" : "ON",
           g_app.no_display ? "OFF" : "ON",
           g_app.no_hotplug ? "OFF" : "ON",
           g_app.password[0] ? "ON" : "OFF");
    printf("\n");

    /* ── Step 1: USB init & open ── */
    if (c270_usb_init(&g_dev) < 0)  goto cleanup;
    if (c270_usb_open(&g_dev) < 0)  goto cleanup;
    c270_usb_print_info(&g_dev);

    /* ── Step 2: Configure camera ── */
    C270Config cfg = {
        .width        = g_app.width,
        .height       = g_app.height,
        .fps          = g_app.fps,
        .format_index = g_app.format_index,
        .frame_index  = g_app.frame_index
    };

    c270_uvc_set_brightness(&g_dev, g_app.brightness);
    c270_uvc_set_contrast(&g_dev, g_app.contrast);

    /* Exposure control */
    if (g_app.exposure > 0)
        c270_uvc_set_exposure(&g_dev, g_app.exposure);
    else
        c270_uvc_set_auto_exposure(&g_dev);

    /* ── Step 3: Start UVC video stream ── */
    if (c270_uvc_start_stream(&g_dev, &cfg) < 0) goto cleanup;

    /* ── Step 4: Init display (optional) ── */
    if (!g_app.no_display) {
        if (display_init(&g_display, g_app.width, g_app.height,
                         "C270 Driver") < 0) goto cleanup;
    }

    /* ── Step 5: Init RTSP stream (optional) ── */
    if (!g_app.no_stream) {
        if (stream_init(&g_stream, g_app.port, g_app.mount_point,
                        cfg.width, cfg.height, cfg.fps,
                        g_app.codec, g_app.password) < 0) goto cleanup;
    }

    /* ── Step 6: Init & start capture ── */
    if (capture_init(&g_capture, &g_dev, &cfg, on_frame, NULL) < 0) goto cleanup;
    if (capture_start(&g_capture) < 0) goto cleanup;

    /* ── Step 7: Start hotplug watchdog (optional) ── */
    if (!g_app.no_hotplug) {
        hotplug_init(&g_hotplug, &g_dev, on_camera_state, NULL);
        g_hotplug.state = CAM_STATE_STREAMING;
        hotplug_start(&g_hotplug);
        printf("[MAIN] Hotplug watchdog enabled\n");
    }

    printf("\n[MAIN] Running. Press Ctrl+C or ESC to quit.\n\n");

    /* ── Main loop ── */
    struct timeval tv;
    while (g_running) {
        tv.tv_sec  = 0;
        tv.tv_usec = 1000;
        libusb_handle_events_timeout(g_dev.ctx, &tv);

        if (!g_app.no_display) {
            if (display_poll_events(&g_display) < 0)
                g_running = 0;
        } else {
            usleep(1000);
        }
    }

    /* ── Cleanup ── */
cleanup:
    if (!g_app.no_hotplug)
        hotplug_stop(&g_hotplug);
    capture_stop(&g_capture);
    capture_free(&g_capture);
    c270_uvc_stop_stream(&g_dev);
    if (!g_app.no_stream) {
        stream_stop(&g_stream);
        stream_free(&g_stream);
    }
    if (!g_app.no_display)
        display_free(&g_display);
    c270_usb_close(&g_dev);
    c270_usb_exit(&g_dev);

    printf("[MAIN] Goodbye.\n");
    return 0;
}
