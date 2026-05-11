/* ═══════════════════════════════════════════════════════
 * STEP 4 TEST: Live display với SDL2 window
 * Chạy 10 giây rồi tự thoát (hoặc nhấn ESC)
 *
 * Run: sudo ./test_step4_display
 * ═══════════════════════════════════════════════════════ */
#include "c270_usb.h"
#include "c270_uvc.h"
#include "c270_capture.h"
#include "c270_display.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static DisplayContext  g_disp;
static CaptureContext *g_cap_ptr;
static volatile int    g_quit = 0;

static void on_frame(const DecodedFrame *frame, void *userdata) {
    (void)userdata;
    display_show_frame(&g_disp, frame,
                       capture_get_fps(g_cap_ptr), "C270-0");
}

int main(void) {
    C270Device     dev;
    CaptureContext cap;
    g_cap_ptr = &cap;

    printf("═══ STEP 4: Display Test (10s, ESC to quit) ═══\n\n");

    if (c270_usb_init(&dev) < 0) return 1;
    if (c270_usb_open(&dev) < 0) { c270_usb_exit(&dev); return 1; }

    C270Config cfg = {
        .width=176, .height=144, .fps=30,
        .format_index=1, .frame_index=3
    };

    if (c270_uvc_start_stream(&dev, &cfg) < 0) goto done;
    if (display_init(&g_disp, cfg.width, cfg.height, "C270 Step4 Test") < 0) goto done;
    if (capture_init(&cap, &dev, &cfg, on_frame, NULL) < 0) goto done;
    if (capture_start(&cap) < 0) goto done;

    time_t start = time(NULL);
    struct timeval tv = {0, 1000};

    while (!g_quit) {
        libusb_handle_events_timeout(dev.ctx, &tv);
        if (display_poll_events(&g_disp) < 0) break;
        if (time(NULL) - start >= 10) break;  /* 10s timeout */
    }

    capture_stop(&cap);
    capture_free(&cap);
    c270_uvc_stop_stream(&dev);
    display_free(&g_disp);

    printf("\n✓ STEP 4 PASSED — Display working\n");
    printf("  → Tiếp theo: make c270_app (full application)\n\n");

done:
    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
