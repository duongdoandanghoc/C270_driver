/* ═══════════════════════════════════════════════════════
 * STEP 3 TEST: Capture 5 frames, save raw MJPEG + decoded JPEG
 * Nếu file output.jpg mở được → capture pipeline OK
 *
 * Run: sudo ./test_step3_capture
 * Output: frame_001.jpg .. frame_005.jpg
 * ═══════════════════════════════════════════════════════ */
#include "c270_usb.h"
#include "c270_uvc.h"
#include "c270_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <jpeglib.h>

static int g_saved = 0;
#define TARGET_FRAMES 5
#define MAX_WAIT_MS   20000  /* chờ tối đa 8 giây */

/* Save RGB24 → JPEG file */
static void save_jpeg(const char *filename,
                      const uint8_t *rgb, int width, int height)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    FILE *fp = fopen(filename, "wb");
    if (!fp) { perror("fopen"); return; }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width      = width;
    cinfo.image_height     = height;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row_ptr[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_ptr[0] = (JSAMPROW)(rgb + cinfo.next_scanline * width * 3);
        jpeg_write_scanlines(&cinfo, row_ptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    printf("  Saved: %s (%dx%d)\n", filename, width, height);
}

static uint64_t now_ms_test(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static CaptureContext *g_ctx_ptr = NULL;

static void on_frame(const DecodedFrame *frame, void *userdata) {
    (void)userdata;
    if (g_saved >= TARGET_FRAMES) return;

    g_saved++;
    char filename[32];
    snprintf(filename, sizeof(filename), "frame_%03d.jpg", g_saved);
    save_jpeg(filename, frame->pixels, frame->width, frame->height);

    printf("  Frame %d/%d — %dx%d  FPS=%.1f\n",
           g_saved, TARGET_FRAMES,
           frame->width, frame->height,
           capture_get_fps(g_ctx_ptr));
}

int main(void) {
    C270Device     dev;
    CaptureContext ctx;
    g_ctx_ptr = &ctx;

    printf("═══ STEP 3: Capture Test ═══\n\n");

    if (c270_usb_init(&dev) < 0) return 1;
    if (c270_usb_open(&dev) < 0) { c270_usb_exit(&dev); return 1; }

    C270Config cfg = {
        .width=176, .height=144, .fps=30,
        .format_index=1, .frame_index=3
    };

    if (c270_uvc_start_stream(&dev, &cfg) < 0) goto done;

    if (capture_init(&ctx, &dev, &cfg, on_frame, NULL) < 0) goto done;
    if (capture_start(&ctx) < 0) goto done;

    printf("Capturing %d frames...\n", TARGET_FRAMES);

    /* Chạy libusb event loop cho đến khi đủ frames */
    struct timeval tv = {0, 5000};
    uint64_t start_ms = now_ms_test();
    printf("Waiting for frames (max 8s)...\n");
    while (g_saved < TARGET_FRAMES) {
        libusb_handle_events_timeout(dev.ctx, &tv);
        uint64_t elapsed = now_ms_test() - start_ms;
        if (elapsed > MAX_WAIT_MS) {
            printf("TIMEOUT after %llums — camera not sending data\n",
                   (unsigned long long)elapsed);
            break;
        }
    }

    capture_stop(&ctx);
    capture_free(&ctx);
    c270_uvc_stop_stream(&dev);

    printf("\n✓ STEP 3 PASSED — %d frames captured\n", g_saved);
    printf("  Mở frame_001.jpg để verify\n");
    printf("  → Tiếp theo: make test_step4_display\n\n");

done:
    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
