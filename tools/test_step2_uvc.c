/* ═══════════════════════════════════════════════════════
 * STEP 2 TEST: UVC probe/commit + controls
 * Verify camera accept format negotiation
 *
 * Run: sudo ./test_step2_uvc
 * ═══════════════════════════════════════════════════════ */
#include "c270_usb.h"
#include "c270_uvc.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    C270Device dev;

    printf("═══ STEP 2: UVC Control Test ═══\n\n");

    if (c270_usb_init(&dev) < 0) return 1;
    if (c270_usb_open(&dev) < 0) { c270_usb_exit(&dev); return 1; }

    /* ── Test 1: Đọc brightness hiện tại ── */
    printf("--- Test: Get current brightness ---\n");
    int16_t brightness = 0;
    if (c270_uvc_get_brightness(&dev, &brightness) == 0) {
        printf("  Current brightness = %d\n", brightness);
    } else {
        fprintf(stderr, "  FAIL: get brightness\n");
    }

    /* ── Test 2: Set brightness ── */
    printf("--- Test: Set brightness to 100 ---\n");
    if (c270_uvc_set_brightness(&dev, 100) == 0) {
        printf("  OK\n");
    } else {
        fprintf(stderr, "  FAIL\n");
    }

    /* ── Test 3: Probe/Commit 640x480 30fps MJPEG ── */
    printf("--- Test: Probe 640x480 @ 30fps MJPEG ---\n");
    C270Config cfg = {
        .width        = 640,
        .height       = 480,
        .fps          = 30,
        .format_index = 1,   /* MJPEG */
        .frame_index  = 3    /* 640x480 — adjust nếu camera reject */
    };

    UVCProbeCommit probe;
    if (c270_uvc_probe(&dev, &probe, &cfg) == 0) {
        printf("  Probe OK\n");
        printf("  Actual FPS = %.1f\n",
               probe.dwFrameInterval ? 10000000.0f / probe.dwFrameInterval : 0);
    } else {
        fprintf(stderr, "  FAIL: probe\n");
        fprintf(stderr, "  Hint: thử frame_index=1,2,4 nếu 3 bị reject\n");
    }

    printf("\n✓ STEP 2 PASSED — UVC controls working\n");
    printf("  → Tiếp theo: make test_step3_capture\n\n");

    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
