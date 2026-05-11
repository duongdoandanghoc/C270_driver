/* ═══════════════════════════════════════════════════════
 * STEP 1 TEST: USB open/close + print device info
 * Chạy cái này đầu tiên — nếu pass thì USB layer OK
 *
 * Build: cmake .. && make test_step1_usb
 * Run:   sudo ./test_step1_usb
 * ═══════════════════════════════════════════════════════ */
#include "c270_usb.h"
#include <stdio.h>

int main(void) {
    C270Device dev;

    printf("═══ STEP 1: USB Layer Test ═══\n\n");

    /* Init libusb */
    if (c270_usb_init(&dev) < 0) {
        fprintf(stderr, "FAIL: libusb init\n");
        return 1;
    }

    /* Mở camera */
    if (c270_usb_open(&dev) < 0) {
        fprintf(stderr, "FAIL: open camera\n");
        fprintf(stderr, "\nTroubleshooting:\n");
        fprintf(stderr, "  1. lsusb | grep 046d\n");
        fprintf(stderr, "  2. ls /sys/bus/usb/drivers/uvcvideo/\n");
        fprintf(stderr, "  3. echo <path> > /sys/bus/usb/drivers/uvcvideo/unbind\n");
        fprintf(stderr, "  4. sudo ./test_step1_usb\n");
        c270_usb_exit(&dev);
        return 1;
    }

    /* In thông tin device */
    c270_usb_print_info(&dev);

    printf("\n✓ STEP 1 PASSED — USB layer working\n");
    printf("  → Tiếp theo: make test_step2_uvc\n\n");

    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
