#ifndef C270_HOTPLUG_H
#define C270_HOTPLUG_H

#include "c270_usb.h"
#include <stdint.h>

typedef enum {
    CAM_STATE_DISCONNECTED = 0,
    CAM_STATE_CONNECTING,
    CAM_STATE_STREAMING,
    CAM_STATE_ERROR
} CameraState;

typedef void (*HotplugCallback)(CameraState new_state, void *userdata);

typedef struct {
    C270Device       *dev;
    CameraState       state;
    HotplugCallback   on_state_change;
    void             *userdata;

    volatile int      running;
    unsigned long     watchdog_thread;  /* pthread_t */

    /* Exponential backoff */
    int               reconnect_delay_ms;
    int               reconnect_attempts;
} HotplugContext;

/* ── Public API ── */
int  hotplug_init(HotplugContext *ctx, C270Device *dev,
                  HotplugCallback cb, void *userdata);
int  hotplug_start(HotplugContext *ctx);
void hotplug_stop(HotplugContext *ctx);

const char *hotplug_state_name(CameraState state);

#endif /* C270_HOTPLUG_H */
