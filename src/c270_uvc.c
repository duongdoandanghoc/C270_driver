#include "c270_uvc.h"
#include <stdio.h>
#include <string.h>

/* ────────────────────────────────────────────────────────────
 * Internal helper: gửi UVC control request
 * ──────────────────────────────────────────────────────────── */
static int uvc_control_transfer(C270Device *dev,
                                uint8_t  req_type,
                                uint8_t  request,
                                uint16_t cs,        /* Control Selector */
                                uint8_t  entity_id,
                                uint8_t  iface,
                                uint8_t *data,
                                uint16_t length)
{
    uint16_t wValue = (cs << 8) | 0x00;
    uint16_t wIndex = ((uint16_t)entity_id << 8) | iface;

    int r = libusb_control_transfer(
        dev->handle,
        req_type,
        request,
        wValue,
        wIndex,
        data, length,
        1000    /* timeout ms */
    );

    if (r < 0) {
        fprintf(stderr, "[UVC] control_transfer failed: %s  "
                "(req=%02x cs=%02x entity=%d)\n",
                libusb_error_name(r), request, cs, entity_id);
    }
    return r;
}

/* Camera Terminal Controls (CT) — Entity ID=1 */
#define CT_ENTITY_ID                          1
#define CT_AE_MODE_CONTROL                    0x02
#define CT_EXPOSURE_TIME_ABSOLUTE_CONTROL     0x04

/* ────────────────────────────────────────────────────────────
 * Probe / Commit
 * ──────────────────────────────────────────────────────────── */
int c270_uvc_probe(C270Device *dev, UVCProbeCommit *probe, const C270Config *cfg) {
    memset(probe, 0, sizeof(*probe));

    /* Hint: chỉ dùng bFrameIndex và dwFrameInterval */
    probe->bmHint         = 0x0001;
    probe->bFormatIndex   = cfg->format_index;   /* 1=YUYV, 2=MJPEG */
    probe->bFrameIndex    = cfg->frame_index;    /* tùy resolution */

    /* Frame interval: 100ns units.  30fps = 1/30s = 333333 * 100ns */
    probe->dwFrameInterval = (uint32_t)(10000000 / cfg->fps);

    /* Gửi SET_CUR probe */
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        VS_PROBE_CONTROL, 0, UVC_STREAMING_INTERFACE,
        (uint8_t *)probe, sizeof(*probe));
    if (r < 0) return -1;

    /* Đọc lại GET_CUR — camera có thể điều chỉnh giá trị */
    memset(probe, 0, sizeof(*probe));
    r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_GET, UVC_GET_CUR,
        VS_PROBE_CONTROL, 0, UVC_STREAMING_INTERFACE,
        (uint8_t *)probe, sizeof(*probe));
    if (r < 0) return -1;

    printf("[UVC] Probe accepted by camera\n");
    c270_uvc_print_probe(probe);
    return 0;
}

int c270_uvc_commit(C270Device *dev, UVCProbeCommit *commit) {
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        VS_COMMIT_CONTROL, 0, UVC_STREAMING_INTERFACE,
        (uint8_t *)commit, sizeof(*commit));
    if (r < 0) return -1;
    printf("[UVC] Commit sent — camera will start sending video\n");
    return 0;
}

/* ────────────────────────────────────────────────────────────
 * Processing Unit Controls
 * ──────────────────────────────────────────────────────────── */
int c270_uvc_set_brightness(C270Device *dev, int16_t value) {
    uint8_t data[2] = { value & 0xFF, (value >> 8) & 0xFF };
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        PU_BRIGHTNESS_CONTROL, PU_ENTITY_ID, UVC_CONTROL_INTERFACE,
        data, 2);
    if (r == 2) printf("[UVC] Brightness set to %d\n", value);
    return (r == 2) ? 0 : -1;
}

int c270_uvc_get_brightness(C270Device *dev, int16_t *value) {
    uint8_t data[2] = {0};
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_GET, UVC_GET_CUR,
        PU_BRIGHTNESS_CONTROL, PU_ENTITY_ID, UVC_CONTROL_INTERFACE,
        data, 2);
    if (r == 2) {
        *value = (int16_t)(data[0] | (data[1] << 8));
        printf("[UVC] Brightness = %d\n", *value);
        return 0;
    }
    return -1;
}

int c270_uvc_set_contrast(C270Device *dev, int16_t value) {
    uint8_t data[2] = { value & 0xFF, (value >> 8) & 0xFF };
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        PU_CONTRAST_CONTROL, PU_ENTITY_ID, UVC_CONTROL_INTERFACE,
        data, 2);
    return (r == 2) ? 0 : -1;
}

int c270_uvc_set_saturation(C270Device *dev, int16_t value) {
    uint8_t data[2] = { value & 0xFF, (value >> 8) & 0xFF };
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        PU_SATURATION_CONTROL, PU_ENTITY_ID, UVC_CONTROL_INTERFACE,
        data, 2);
    return (r == 2) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────
 * Exposure Controls
 * ──────────────────────────────────────────────────────────── */
int c270_uvc_set_exposure(C270Device *dev, int32_t value) {
    /* Trước tiên tắt auto-exposure (mode=1: manual) */
    uint8_t ae_mode = 1;
    uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        CT_AE_MODE_CONTROL, CT_ENTITY_ID, UVC_CONTROL_INTERFACE,
        &ae_mode, 1);

    /* Set exposure time (100us units) */
    uint8_t data[4] = {
        value & 0xFF, (value >> 8) & 0xFF,
        (value >> 16) & 0xFF, (value >> 24) & 0xFF
    };
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        CT_EXPOSURE_TIME_ABSOLUTE_CONTROL, CT_ENTITY_ID, UVC_CONTROL_INTERFACE,
        data, 4);
    if (r == 4) printf("[UVC] Exposure set to %d (manual mode)\n", value);
    return (r == 4) ? 0 : -1;
}

int c270_uvc_set_auto_exposure(C270Device *dev) {
    /* mode=8: aperture priority (auto) */
    uint8_t ae_mode = 8;
    int r = uvc_control_transfer(dev,
        UVC_REQ_TYPE_SET, UVC_SET_CUR,
        CT_AE_MODE_CONTROL, CT_ENTITY_ID, UVC_CONTROL_INTERFACE,
        &ae_mode, 1);
    if (r == 1) printf("[UVC] Auto-exposure enabled\n");
    return (r == 1) ? 0 : -1;
}

/* ────────────────────────────────────────────────────────────
 * Start / Stop video stream
 * ──────────────────────────────────────────────────────────── */
int c270_uvc_start_stream(C270Device *dev, const C270Config *cfg) {
    UVCProbeCommit probe;
    memset(&probe, 0, sizeof(probe));

    /* Probe để lấy info nhưng không dựa vào MaxPayloadSize
     * vì camera C270 YUYV-only báo MaxPayloadSize=512 không chính xác */
    c270_uvc_probe(dev, &probe, cfg);
    c270_uvc_commit(dev, &probe);
    /* Ignore probe errors — camera vẫn stream được */

    /* Step 3: Tìm alt setting phù hợp với MaxPayloadTransferSize
     * Camera báo payload size cần thiết qua dwMaxPayloadTransferSize.
     * Phải chọn alt setting có wMaxPacketSize >= payload size đó.
     */
    uint32_t needed = probe.dwMaxPayloadTransferSize;
    if (needed == 0) needed = 512;  /* fallback */

    printf("[UVC] Camera needs payload = %u bytes/packet\n", needed);

    /* Duyệt tất cả alt settings để tìm cái phù hợp nhỏ nhất */
    libusb_device *device = libusb_get_device(dev->handle);
    struct libusb_config_descriptor *config = NULL;
    libusb_get_active_config_descriptor(device, &config);

    int best_alt = -1;
    uint16_t best_pkt = 0xFFFF;
    /* Cũng tìm alt với bandwidth headroom (2x) để giảm packet loss cho MJPEG */
    int comfort_alt = -1;
    uint16_t comfort_pkt = 0xFFFF;
    uint32_t comfort_needed = needed * 2;

    if (config) {
        const struct libusb_interface *iface =
            &config->interface[UVC_STREAMING_INTERFACE];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt =
                &iface->altsetting[a];
            if (alt->bNumEndpoints == 0) continue;
            uint16_t pkt = alt->endpoint[0].wMaxPacketSize & 0x07FF;
            uint8_t  mult = ((alt->endpoint[0].wMaxPacketSize >> 11) & 0x03) + 1;
            uint16_t effective = pkt * mult;
            printf("[UVC]   alt=%d  pkt=%u  mult=%u  effective=%u\n",
                   alt->bAlternateSetting, pkt, mult, effective);
            /* Minimum: effective >= needed */
            if (effective >= needed && pkt < best_pkt) {
                best_alt = alt->bAlternateSetting;
                best_pkt = pkt;
            }
            /* Comfort: effective >= 2x needed (headroom cho MJPEG) */
            if (effective >= comfort_needed && pkt < comfort_pkt) {
                comfort_alt = alt->bAlternateSetting;
                comfort_pkt = pkt;
            }
        }
        libusb_free_config_descriptor(config);
    }

    /* Ưu tiên comfort alt nếu có, fallback sang minimum */
    if (comfort_alt >= 0) {
        best_alt = comfort_alt;
        best_pkt = comfort_pkt;
        printf("[UVC] Using comfort alt (2x headroom) for reliable MJPEG\n");
    }

    if (best_alt < 0) {
        fprintf(stderr, "[UVC] No suitable alt setting found, using alt=11\n");
        best_alt = 11;
    }

    printf("[UVC] Selected alt=%d (maxPacket=%u)\n", best_alt, best_pkt);

    int r = libusb_set_interface_alt_setting(
        dev->handle, UVC_STREAMING_INTERFACE, best_alt);
    if (r < 0) {
        fprintf(stderr, "[UVC] set_alt_setting %d failed: %s\n",
                best_alt, libusb_error_name(r));
        return -1;
    }
    dev->active_alt = (uint8_t)best_alt;
    printf("[UVC] Stream started  %dx%d @ %dfps  alt=%d\n",
           cfg->width, cfg->height, cfg->fps, best_alt);
    return 0;
}

int c270_uvc_stop_stream(C270Device *dev) {
    /* Set alt=0 → camera ngừng gửi iso data */
    int r = libusb_set_interface_alt_setting(
        dev->handle,
        UVC_STREAMING_INTERFACE,
        C270_ALT_SETTING_STOP);
    if (r < 0) {
        fprintf(stderr, "[UVC] stop stream (alt=0) failed: %s\n",
                libusb_error_name(r));
        return -1;
    }
    printf("[UVC] Stream stopped\n");
    return 0;
}

/* ────────────────────────────────────────────────────────────
 * Debug print
 * ──────────────────────────────────────────────────────────── */
void c270_uvc_print_probe(const UVCProbeCommit *p) {
    printf("[UVC] ── Probe/Commit ───────────────────\n");
    printf("[UVC]   FormatIndex       = %d\n", p->bFormatIndex);
    printf("[UVC]   FrameIndex        = %d\n", p->bFrameIndex);
    printf("[UVC]   FrameInterval     = %u (%.1f fps)\n",
           p->dwFrameInterval,
           p->dwFrameInterval ? 10000000.0f / p->dwFrameInterval : 0);
    printf("[UVC]   MaxFrameSize      = %u bytes\n",  p->dwMaxVideoFrameSize);
    printf("[UVC]   MaxPayloadSize    = %u bytes\n",  p->dwMaxPayloadTransferSize);
    printf("[UVC] ──────────────────────────────────\n");
}
