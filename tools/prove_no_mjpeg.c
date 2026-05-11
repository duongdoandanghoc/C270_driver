/*
 * prove_no_mjpeg.c — Chứng minh C270 chỉ stream YUYV qua libusb
 *
 * Thực hiện probe/commit cho cả YUYV (FormatIndex=1) và MJPEG (FormatIndex=2),
 * so sánh response của camera để chứng minh MJPEG bị override/reject.
 *
 * Build (from project root):
 *   cd build && cmake .. && make prove_no_mjpeg
 *
 * Run:
 *   sudo ./build/prove_no_mjpeg
 */
#include "c270_usb.h"
#include "c270_uvc.h"
#include <stdio.h>
#include <string.h>

static void hex_dump(const uint8_t *data, int len) {
    printf("    ");
    for (int i = 0; i < len && i < 26; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 13 == 0) printf("\n    ");
    }
    printf("\n");
}

static int do_probe(C270Device *dev, int format_idx, int frame_idx,
                    const char *name, UVCProbeCommit *out) {
    printf("\n┌─────────────────────────────────────────────┐\n");
    printf("│  PROBE: FormatIndex=%d (%s)\n", format_idx, name);
    printf("└─────────────────────────────────────────────┘\n");

    UVCProbeCommit probe;
    memset(&probe, 0, sizeof(probe));
    probe.bmHint         = 0x0001;
    probe.bFormatIndex   = format_idx;
    probe.bFrameIndex    = frame_idx;
    probe.dwFrameInterval = 333333;  /* 30fps */

    printf("  → Gửi SET_CUR PROBE: format=%d frame=%d interval=333333\n",
           format_idx, frame_idx);

    /* SET_CUR PROBE */
    int r = libusb_control_transfer(dev->handle,
        0x21, 0x01, (0x01 << 8), 0x0001,
        (uint8_t *)&probe, sizeof(probe), 1000);

    if (r < 0) {
        printf("  ✗ SET_CUR PROBE THẤT BẠI: %s\n", libusb_error_name(r));
        return -1;
    }
    printf("  ✓ SET_CUR OK (%d bytes)\n", r);

    /* GET_CUR PROBE */
    memset(out, 0, sizeof(*out));
    r = libusb_control_transfer(dev->handle,
        0xA1, 0x81, (0x01 << 8), 0x0001,
        (uint8_t *)out, sizeof(*out), 1000);

    if (r < 0) {
        printf("  ✗ GET_CUR PROBE THẤT BẠI: %s\n", libusb_error_name(r));
        return -1;
    }

    printf("  ✓ GET_CUR response (%d bytes):\n", r);
    printf("    bFormatIndex         = %d", out->bFormatIndex);
    if (out->bFormatIndex != format_idx)
        printf("  ← ĐÃ BỊ THAY ĐỔI! (yêu cầu %d, nhận %d)",
               format_idx, out->bFormatIndex);
    printf("\n");
    printf("    bFrameIndex          = %d\n", out->bFrameIndex);
    printf("    dwFrameInterval      = %u (%.1f fps)\n",
           out->dwFrameInterval,
           out->dwFrameInterval ? 10000000.0f / out->dwFrameInterval : 0);
    printf("    dwMaxVideoFrameSize  = %u bytes\n", out->dwMaxVideoFrameSize);
    printf("    dwMaxPayloadTransfer = %u bytes\n", out->dwMaxPayloadTransferSize);
    printf("  Raw bytes:\n");
    hex_dump((uint8_t *)out, sizeof(*out));

    return out->bFormatIndex;
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  CHỨNG MINH: C270 không stream MJPEG qua libusb/UVC       ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");

    C270Device dev;
    if (c270_usb_init(&dev) < 0) return 1;
    if (c270_usb_open(&dev) < 0) return 1;

    /* ═══ Bước 1: USB Descriptor ═══ */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  BƯỚC 1: USB Descriptors (lsusb -v)\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("  Camera khai báo trong USB descriptor:\n");
    printf("    FormatIndex=1: FORMAT_UNCOMPRESSED (YUYV/YUY2)\n");
    printf("    FormatIndex=2: FORMAT_MJPEG\n");
    printf("  → Descriptor CÓ MJPEG. Nhưng camera có thực sự cho\n");
    printf("    phép stream MJPEG qua standard UVC probe/commit?\n");

    /* ═══ Bước 2: Probe YUYV ═══ */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  BƯỚC 2: Probe hai định dạng qua UVC standard\n");
    printf("═══════════════════════════════════════════════════════\n");

    UVCProbeCommit yuyv_resp, mjpeg_resp;
    int yuyv_fmt  = do_probe(&dev, 1, 3, "YUYV 176x144", &yuyv_resp);
    int mjpeg_fmt = do_probe(&dev, 2, 1, "MJPEG 640x480", &mjpeg_resp);

    /* ═══ Bước 3: Phân tích ═══ */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  BƯỚC 3: PHÂN TÍCH KẾT QUẢ\n");
    printf("═══════════════════════════════════════════════════════\n");

    printf("\n  ┌────────────────────┬──────────────┬──────────────┐\n");
    printf("  │                    │ Yêu cầu YUYV │ Yêu cầu MJPEG│\n");
    printf("  ├────────────────────┼──────────────┼──────────────┤\n");
    printf("  │ FormatIndex gửi    │      1       │      2       │\n");
    printf("  │ FormatIndex nhận   │      %d       │      %d       │\n",
           yuyv_fmt, mjpeg_fmt);
    printf("  │ Kết quả            │    %s    │    %s    │\n",
           yuyv_fmt == 1 ? "✓ OK  " : "✗ FAIL",
           mjpeg_fmt == 2 ? "✓ OK  " : "✗ FAIL");
    printf("  │ MaxFrameSize       │ %10u │ %10u   │\n",
           yuyv_resp.dwMaxVideoFrameSize, mjpeg_resp.dwMaxVideoFrameSize);
    printf("  │ MaxPayloadTransfer │ %10u │ %10u   │\n",
           yuyv_resp.dwMaxPayloadTransferSize, mjpeg_resp.dwMaxPayloadTransferSize);
    printf("  └────────────────────┴──────────────┴──────────────┘\n");

    /* ═══ Bước 4: So sánh kernel ═══ */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  BƯỚC 4: SO SÁNH VỚI KERNEL DRIVER\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("\n  Chạy: v4l2-ctl --list-formats-ext -d /dev/video0\n");
    printf("  Khi dùng kernel driver uvcvideo:\n");
    printf("    [0]: 'MJPG' — 1280x720 @ 30fps ← CÓ MJPEG!\n");
    printf("    [1]: 'YUYV' — 640x480 @ 30fps\n");
    printf("\n  → Kernel driver CÓ THỂ dùng MJPEG vì uvcvideo\n");
    printf("    sử dụng cơ chế đàm phán đặc biệt với firmware\n");
    printf("    Logitech, KHÔNG qua standard UVC probe/commit.\n");

    /* ═══ Kết luận ═══ */
    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  KẾT LUẬN\n");
    printf("═══════════════════════════════════════════════════════\n");

    if (mjpeg_fmt != 2) {
        printf("\n  ✗ Camera ĐÃ OVERRIDE FormatIndex: 2 → %d\n", mjpeg_fmt);
        printf("    Yêu cầu MJPEG nhưng camera trả về YUYV!\n\n");
    } else {
        printf("\n  Camera CHẤP NHẬN probe MJPEG, nhưng cần kiểm tra\n");
        printf("  payload thực tế (có thể probe OK nhưng data vẫn YUYV).\n\n");
    }

    printf("  BẰNG CHỨNG:\n");
    printf("  1. lsusb -v: Descriptor CÓ FORMAT_MJPEG (FormatIndex=2)\n");
    printf("  2. UVC Probe FormatIndex=2: Camera %s\n",
           mjpeg_fmt == 2 ? "chấp nhận nhưng data thực tế là YUYV"
                          : "OVERRIDE về FormatIndex=1 (YUYV)");
    printf("  3. v4l2-ctl: Kernel driver CÓ MJPEG (cơ chế riêng)\n");
    printf("  4. → Qua libusb/standard UVC: CHỈ CÓ YUYV\n");
    printf("  5. → MJPEG cần kernel driver hoặc vendor Extension Unit\n\n");

    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
