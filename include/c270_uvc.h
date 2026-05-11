#ifndef C270_UVC_H
#define C270_UVC_H

#include "c270_usb.h"
#include <stdint.h>

/* ── UVC Request codes (USB Video Class spec 1.1) ── */
#define UVC_SET_CUR  0x01
#define UVC_GET_CUR  0x81
#define UVC_GET_MIN  0x82
#define UVC_GET_MAX  0x83
#define UVC_GET_DEF  0x87

/* ── bmRequestType ── */
#define UVC_REQ_TYPE_SET  0x21   /* host→device, class, interface */
#define UVC_REQ_TYPE_GET  0xA1   /* device→host, class, interface */

/* ── Processing Unit (PU) Controls — Entity ID 2 trên C270 ── */
#define PU_ENTITY_ID                 2
#define PU_BRIGHTNESS_CONTROL        0x02
#define PU_CONTRAST_CONTROL          0x03
#define PU_HUE_CONTROL               0x06
#define PU_SATURATION_CONTROL        0x07
#define PU_SHARPNESS_CONTROL         0x08
#define PU_GAMMA_CONTROL             0x09
#define PU_WHITE_BALANCE_TEMP_CTRL   0x0A
#define PU_GAIN_CONTROL              0x04

/* ── Camera Terminal (CT) Controls — Entity ID 1 ── */
#define CT_ENTITY_ID                        1
#define CT_EXPOSURE_TIME_ABSOLUTE_CONTROL   0x04
#define CT_FOCUS_ABSOLUTE_CONTROL           0x06

/* ── Video Probe/Commit Controls (Streaming interface) ── */
#define VS_PROBE_CONTROL   0x01
#define VS_COMMIT_CONTROL  0x02

/* Probe/Commit structure (UVC 1.1 — 26 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t bmHint;
    uint8_t  bFormatIndex;       /* 1=YUYV, 2=MJPEG trên C270 */
    uint8_t  bFrameIndex;        /* tùy resolution — xem lsusb -v */
    uint32_t dwFrameInterval;    /* 100ns units: 333333 = 30fps */
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
} UVCProbeCommit;

/* ── Camera config ── */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    uint8_t  format_index;   /* 1=YUYV, 2=MJPEG */
    uint8_t  frame_index;    /* tùy resolution — xem lsusb -v */
} C270Config;

/* ── Public API ── */

/* Probe/Commit — negotiate format với camera */
int c270_uvc_probe(C270Device *dev, UVCProbeCommit *probe, const C270Config *cfg);
int c270_uvc_commit(C270Device *dev, UVCProbeCommit *commit);

/* Control settings */
int c270_uvc_set_brightness(C270Device *dev, int16_t value);
int c270_uvc_get_brightness(C270Device *dev, int16_t *value);
int c270_uvc_set_contrast(C270Device *dev, int16_t value);
int c270_uvc_set_saturation(C270Device *dev, int16_t value);
int c270_uvc_set_exposure(C270Device *dev, int32_t value);
int c270_uvc_set_auto_exposure(C270Device *dev);

/* Start/stop video stream */
int c270_uvc_start_stream(C270Device *dev, const C270Config *cfg);
int c270_uvc_stop_stream(C270Device *dev);

/* In probe result để debug */
void c270_uvc_print_probe(const UVCProbeCommit *p);

#endif /* C270_UVC_H */
