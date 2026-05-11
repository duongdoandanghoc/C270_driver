#include "c270_hotplug.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RECONNECT_DELAY_MIN_MS   500
#define RECONNECT_DELAY_MAX_MS   30000

static const char *state_names[] = {
    "DISCONNECTED",
    "CONNECTING",
    "STREAMING",
    "ERROR"
};

/* ────────────────────────────────────────────────────────────
 * Internal: kiểm tra C270 có available trên USB bus không
 * (dùng libusb_get_device_list — không cần mở device)
 * ──────────────────────────────────────────────────────────── */
static int camera_is_available(C270Device *dev) {
    libusb_device **list;
    ssize_t count = libusb_get_device_list(dev->ctx, &list);
    int found = 0;

    for (ssize_t i = 0; i < count; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == 0) {
            if (desc.idVendor == C270_VID && desc.idProduct == C270_PID) {
                found = 1;
                break;
            }
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

static void set_state(HotplugContext *ctx, CameraState new_state) {
    if (ctx->state == new_state) return;
    printf("[HOTPLUG] State: %s → %s\n",
           state_names[ctx->state], state_names[new_state]);
    ctx->state = new_state;
    if (ctx->on_state_change)
        ctx->on_state_change(new_state, ctx->userdata);
}

/* ────────────────────────────────────────────────────────────
 * Watchdog thread
 * ──────────────────────────────────────────────────────────── */
static void *watchdog_thread(void *arg) {
    HotplugContext *ctx = (HotplugContext *)arg;

    printf("[HOTPLUG] Watchdog thread started\n");

    while (ctx->running) {
        switch (ctx->state) {

        case CAM_STATE_DISCONNECTED:
            if (camera_is_available(ctx->dev)) {
                printf("[HOTPLUG] Camera detected, attempting connect...\n");
                set_state(ctx, CAM_STATE_CONNECTING);
            }
            break;

        case CAM_STATE_CONNECTING:
            if (c270_usb_open(ctx->dev) == 0) {
                ctx->reconnect_delay_ms  = RECONNECT_DELAY_MIN_MS;
                ctx->reconnect_attempts  = 0;
                set_state(ctx, CAM_STATE_STREAMING);
            } else {
                fprintf(stderr, "[HOTPLUG] Connect failed (attempt %d), "
                        "retry in %dms\n",
                        ctx->reconnect_attempts + 1,
                        ctx->reconnect_delay_ms);
                usleep(ctx->reconnect_delay_ms * 1000);

                /* Exponential backoff */
                ctx->reconnect_attempts++;
                ctx->reconnect_delay_ms *= 2;
                if (ctx->reconnect_delay_ms > RECONNECT_DELAY_MAX_MS)
                    ctx->reconnect_delay_ms = RECONNECT_DELAY_MAX_MS;

                set_state(ctx, CAM_STATE_DISCONNECTED);
            }
            break;

        case CAM_STATE_STREAMING:
            /* Health check: camera vẫn còn trên USB bus không? */
            if (!camera_is_available(ctx->dev)) {
                printf("[HOTPLUG] Camera unplugged!\n");
                /* Callback trước (capture_stop) rồi mới close device */
                set_state(ctx, CAM_STATE_DISCONNECTED);
                c270_usb_close(ctx->dev);
                ctx->reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;
            }
            break;

        case CAM_STATE_ERROR:
            fprintf(stderr, "[HOTPLUG] Error state — cleanup and retry\n");
            c270_usb_close(ctx->dev);
            usleep(ctx->reconnect_delay_ms * 1000);
            ctx->reconnect_delay_ms *= 2;
            if (ctx->reconnect_delay_ms > RECONNECT_DELAY_MAX_MS)
                ctx->reconnect_delay_ms = RECONNECT_DELAY_MAX_MS;
            set_state(ctx, CAM_STATE_DISCONNECTED);
            break;
        }

        usleep(500000);   /* check mỗi 500ms */
    }

    printf("[HOTPLUG] Watchdog thread exiting\n");
    return NULL;
}

/* ────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────── */
int hotplug_init(HotplugContext *ctx, C270Device *dev,
                 HotplugCallback cb, void *userdata)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->dev                = dev;
    ctx->state              = CAM_STATE_DISCONNECTED;
    ctx->on_state_change    = cb;
    ctx->userdata           = userdata;
    ctx->reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;
    printf("[HOTPLUG] Initialized\n");
    return 0;
}

int hotplug_start(HotplugContext *ctx) {
    ctx->running = 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, watchdog_thread, ctx) != 0) {
        fprintf(stderr, "[HOTPLUG] pthread_create failed\n");
        return -1;
    }
    ctx->watchdog_thread = (unsigned long)tid;
    return 0;
}

void hotplug_stop(HotplugContext *ctx) {
    ctx->running = 0;
    pthread_join((pthread_t)ctx->watchdog_thread, NULL);
    printf("[HOTPLUG] Watchdog stopped\n");
}

const char *hotplug_state_name(CameraState state) {
    if (state >= 0 && state < 4) return state_names[state];
    return "UNKNOWN";
}
