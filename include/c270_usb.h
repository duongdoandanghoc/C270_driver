#ifndef C270_USB_H
#define C270_USB_H

#include <libusb-1.0/libusb.h>
#include <stdint.h>

/* ── Logitech C270 USB identifiers ── */
#define C270_VID  0x046d
#define C270_PID  0x0825

/* ── UVC interface numbers (từ USB descriptor) ── */
#define UVC_CONTROL_INTERFACE    0   /* VideoControl interface */
#define UVC_STREAMING_INTERFACE  1   /* VideoStreaming interface */

/* ── Isochronous endpoint của C270 ── */
#define C270_ISO_ENDPOINT        0x81  /* EP1 IN */

/* ── Alternate settings cho streaming interface ──
 *  alt=0  → bandwidth=0      (stop)
 *  alt=5  → 800 bytes eff.   (đủ cho YUYV 176x144)
 *  alt=9  → 1984 bytes eff.  (đủ cho MJPEG 640x480)
 *  alt=11 → 3060 bytes eff.  (max bandwidth)
 *  Xem lsusb -v để biết chính xác
 */
#define C270_ALT_SETTING_STOP    0
#define C270_ALT_SETTING_STREAM  7

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    int                   is_open;
    uint8_t               active_alt;
    uint8_t               bus;
    uint8_t               port;
} C270Device;

/* ── Public API ── */
int  c270_usb_init(C270Device *dev);
int  c270_usb_open(C270Device *dev);
void c270_usb_close(C270Device *dev);
void c270_usb_exit(C270Device *dev);

/* In thông tin device để debug */
void c270_usb_print_info(C270Device *dev);

#endif /* C270_USB_H */
