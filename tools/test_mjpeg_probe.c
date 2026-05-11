/*
 * test_mjpeg_probe.c — Test xem camera có chấp nhận MJPEG qua UVC probe không
 *
 * Build: cd build && cmake .. && make test_mjpeg_probe
 * Run:   sudo ./build/test_mjpeg_probe
 */
#include "c270_usb.h"
#include "c270_uvc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static void hex_dump(const uint8_t *data, int len) {
    printf("    ");
    for (int i = 0; i < len; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n    ");
    }
    printf("\n");
}

static int do_full_probe(C270Device *dev, int fmt_idx, int frm_idx,
                          const char *label) {
    printf("\n╔═══════════════════════════════════════════╗\n");
    printf("║  PROBE: %s (fmt=%d, frm=%d)\n", label, fmt_idx, frm_idx);
    printf("╚═══════════════════════════════════════════╝\n");

    UVCProbeCommit probe;
    memset(&probe, 0, sizeof(probe));
    probe.bmHint         = 0x0001;
    probe.bFormatIndex   = fmt_idx;
    probe.bFrameIndex    = frm_idx;
    probe.dwFrameInterval = 333333; /* 30fps */

    /* SET_CUR PROBE */
    int r = libusb_control_transfer(dev->handle,
        0x21, 0x01, (0x01 << 8), 0x0001,
        (uint8_t *)&probe, sizeof(probe), 1000);
    if (r < 0) {
        printf("  ✗ SET_CUR failed: %s\n", libusb_error_name(r));
        return -1;
    }

    /* GET_CUR PROBE */
    UVCProbeCommit resp;
    memset(&resp, 0, sizeof(resp));
    r = libusb_control_transfer(dev->handle,
        0xA1, 0x81, (0x01 << 8), 0x0001,
        (uint8_t *)&resp, sizeof(resp), 1000);
    if (r < 0) {
        printf("  ✗ GET_CUR failed: %s\n", libusb_error_name(r));
        return -1;
    }

    printf("  Gửi:  FormatIndex=%d  FrameIndex=%d\n", fmt_idx, frm_idx);
    printf("  Nhận: FormatIndex=%d  FrameIndex=%d\n",
           resp.bFormatIndex, resp.bFrameIndex);
    printf("        dwFrameInterval      = %u (%.1f fps)\n",
           resp.dwFrameInterval,
           resp.dwFrameInterval ? 10000000.0f / resp.dwFrameInterval : 0);
    printf("        dwMaxVideoFrameSize  = %u bytes\n", resp.dwMaxVideoFrameSize);
    printf("        dwMaxPayloadTransfer = %u bytes\n", resp.dwMaxPayloadTransferSize);

    if (resp.bFormatIndex != fmt_idx) {
        printf("  ✗ Camera OVERRIDE: %d → %d\n", fmt_idx, resp.bFormatIndex);
    } else {
        printf("  ✓ Camera CHẤP NHẬN format %d (%s)\n", fmt_idx, label);
    }

    return resp.bFormatIndex;
}

/* Thử stream thật và kiểm tra payload */
static void try_stream(C270Device *dev, int fmt_idx, int frm_idx,
                        const char *label) {
    printf("\n┌─────────────────────────────────────────────┐\n");
    printf("│  STREAM TEST: %s (fmt=%d, frm=%d)\n", label, fmt_idx, frm_idx);
    printf("└─────────────────────────────────────────────┘\n");

    /* Probe */
    UVCProbeCommit probe;
    memset(&probe, 0, sizeof(probe));
    probe.bmHint         = 0x0001;
    probe.bFormatIndex   = fmt_idx;
    probe.bFrameIndex    = frm_idx;
    probe.dwFrameInterval = 333333;

    libusb_control_transfer(dev->handle, 0x21, 0x01, (0x01 << 8), 0x0001,
        (uint8_t *)&probe, sizeof(probe), 1000);

    UVCProbeCommit resp;
    memset(&resp, 0, sizeof(resp));
    libusb_control_transfer(dev->handle, 0xA1, 0x81, (0x01 << 8), 0x0001,
        (uint8_t *)&resp, sizeof(resp), 1000);

    /* Commit */
    libusb_control_transfer(dev->handle, 0x21, 0x01, (0x02 << 8), 0x0001,
        (uint8_t *)&resp, sizeof(resp), 1000);

    /* Set alt=5 */
    int r = libusb_set_interface_alt_setting(dev->handle, 1, 5);
    if (r < 0) {
        printf("  ✗ Alt=5 failed: %s\n", libusb_error_name(r));
        return;
    }

    /* Đọc endpoint */
    libusb_device *device = libusb_get_device(dev->handle);
    struct libusb_config_descriptor *config;
    libusb_get_active_config_descriptor(device, &config);
    uint8_t ep = 0x81;
    if (config) {
        const struct libusb_interface_descriptor *alt =
            &config->interface[1].altsetting[5];
        if (alt->bNumEndpoints > 0)
            ep = alt->endpoint[0].bEndpointAddress;
        libusb_free_config_descriptor(config);
    }

    /* Thu thập ISO packets */
    int npkt = 64;
    int pkt_sz = 800;
    uint8_t *buf = calloc(1, npkt * pkt_sz);
    struct libusb_transfer *xfer = libusb_alloc_transfer(npkt);

    volatile int done = 0;
    int jpeg_found = 0;
    int data_pkts = 0;
    int empty_pkts = 0;

    /* Dùng synchronous bulk read thay vì async ISO để đơn giản */
    printf("  Thu thập packets (2 giây)...\n\n");

    /* Submit nhiều lần synchronously */
    struct timeval tv;
    int total_transfers = 0;
    time_t start = time(NULL);

    /* Allocate single transfer buffer */
    int single_sz = 8 * pkt_sz;
    uint8_t *sbuf = calloc(1, single_sz);

    while (time(NULL) - start < 2 && total_transfers < 10) {
        /* Synchronous ISO transfer bằng cách submit + wait */
        struct libusb_transfer *sx = libusb_alloc_transfer(8);
        volatile int sx_done = 0;

        /* Simple callback */
        typedef struct { volatile int *done_flag; int *jpeg; int *data; int *empty; const char *lbl; } CBData;
        /* Không dùng nested function, dùng global approach */

        libusb_fill_iso_transfer(sx, dev->handle, ep,
            sbuf, single_sz, 8, NULL, NULL, 1000);
        libusb_set_iso_packet_lengths(sx, pkt_sz);

        r = libusb_submit_transfer(sx);
        if (r < 0) {
            printf("  ✗ Submit failed: %s\n", libusb_error_name(r));
            libusb_free_transfer(sx);
            break;
        }

        /* Wait for transfer */
        tv.tv_sec = 0; tv.tv_usec = 500000;
        for (int w = 0; w < 20; w++) {
            tv.tv_sec = 0; tv.tv_usec = 50000;
            libusb_handle_events_timeout(dev->ctx, &tv);
            if (sx->status != LIBUSB_TRANSFER_COMPLETED &&
                sx->status != LIBUSB_TRANSFER_TIMED_OUT) {
                /* Still pending, check again */
            }
            /* Check if any packet has data */
            int has_data = 0;
            for (int i = 0; i < 8; i++) {
                if (sx->iso_packet_desc[i].actual_length > 0) has_data = 1;
            }
            if (has_data) break;
        }

        /* Analyze packets */
        for (int i = 0; i < 8; i++) {
            int alen = sx->iso_packet_desc[i].actual_length;
            if (alen <= 0) continue;

            uint8_t *pd = sbuf + (i * pkt_sz);

            if (alen <= 12) {
                empty_pkts++;
                continue;
            }

            uint8_t hdr_len = pd[0];
            if (hdr_len >= alen) { empty_pkts++; continue; }

            int payload_len = alen - hdr_len;
            uint8_t *payload = pd + hdr_len;

            if (payload_len > 0) {
                data_pkts++;
                if (total_transfers < 3) {
                    printf("  pkt[%d]: %d bytes, payload %d bytes, first: ",
                           i, alen, payload_len);
                    for (int b = 0; b < 8 && b < payload_len; b++)
                        printf("%02x ", payload[b]);

                    if (payload_len >= 2 && payload[0] == 0xFF && payload[1] == 0xD8) {
                        printf("← JPEG SOI (FF D8) !!!");
                        jpeg_found = 1;
                    }
                    printf("\n");
                }
            } else {
                empty_pkts++;
            }
        }

        libusb_free_transfer(sx);
        total_transfers++;
    }

    printf("\n  ── Kết quả stream %s ──\n", label);
    printf("  Transfers:     %d\n", total_transfers);
    printf("  Data packets:  %d\n", data_pkts);
    printf("  Empty packets: %d\n", empty_pkts);
    printf("  JPEG magic:    %s\n", jpeg_found ? "✓ CÓ (FF D8)" : "✗ KHÔNG");

    if (jpeg_found) {
        printf("\n  → Data thực tế LÀ MJPEG!\n");
    } else if (data_pkts > 0) {
        printf("\n  → Data thực tế KHÔNG phải MJPEG (không có FF D8)\n");
        printf("  → Nhiều khả năng là YUYV raw data\n");
    } else {
        printf("\n  → Không nhận được data (bandwidth issue?)\n");
    }

    free(sbuf);
    free(buf);

    /* Stop */
    libusb_set_interface_alt_setting(dev->handle, 1, 0);
}

int main(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  TEST: MJPEG có hoạt động qua libusb/UVC không?    ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    C270Device dev;
    if (c270_usb_init(&dev) < 0) return 1;
    if (c270_usb_open(&dev) < 0) return 1;

    /* ═══ Test 1: Probe cả 2 format ═══ */
    printf("\n══════════ PHẦN 1: PROBE TEST ══════════\n");
    int r1 = do_full_probe(&dev, 1, 3, "YUYV 176x144");
    int r2 = do_full_probe(&dev, 2, 1, "MJPEG 640x480");
    int r3 = do_full_probe(&dev, 2, 3, "MJPEG 176x144");

    /* ═══ Test 2: Stream thật ═══ */
    printf("\n══════════ PHẦN 2: STREAM TEST ══════════\n");

    if (r1 == 1) {
        try_stream(&dev, 1, 3, "YUYV");
    }

    /* Luôn thử MJPEG dù probe có override hay không */
    try_stream(&dev, 2, 1, "MJPEG 640x480");

    /* ═══ Kết luận ═══ */
    printf("\n══════════ KẾT LUẬN ══════════\n\n");
    if (r2 == 2 || r3 == 2) {
        printf("  Camera CHẤP NHẬN probe MJPEG.\n");
        printf("  → Bạn CÓ THỂ thử đổi format_index=2 trong code!\n");
        printf("  → Sửa main.c: .format_index = 2\n");
    } else {
        printf("  Camera OVERRIDE probe MJPEG về YUYV.\n");
        printf("  → Chỉ dùng được YUYV qua libusb.\n");
    }
    printf("\n");

    c270_usb_close(&dev);
    c270_usb_exit(&dev);
    return 0;
}
