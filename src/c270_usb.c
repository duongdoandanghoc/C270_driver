#include "c270_usb.h"
#include <stdio.h>
#include <string.h>

int c270_usb_init(C270Device *dev) {
    memset(dev, 0, sizeof(*dev));

    int r = libusb_init(&dev->ctx);
    if (r < 0) {
        fprintf(stderr, "[USB] libusb_init failed: %s\n", libusb_error_name(r));
        return -1;
    }
    libusb_set_option(dev->ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    printf("[USB] libusb initialized\n");
    return 0;
}

int c270_usb_open(C270Device *dev) {
    /* Tìm C270 theo VID:PID */
    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, C270_VID, C270_PID);
    if (!dev->handle) {
        fprintf(stderr, "[USB] Cannot open C270 (VID=%04x PID=%04x)\n",
                C270_VID, C270_PID);
        fprintf(stderr, "[USB] Checklist:\n");
        fprintf(stderr, "      1. lsusb | grep 046d  — camera có nhận không?\n");
        fprintf(stderr, "      2. ls /sys/bus/usb/drivers/uvcvideo/ — unbind chưa?\n");
        fprintf(stderr, "      3. Chạy với sudo nếu chưa setup udev rule\n");
        return -1;
    }

    /* Lấy bus/port để log */
    libusb_device *device = libusb_get_device(dev->handle);
    dev->bus  = libusb_get_bus_number(device);
    dev->port = libusb_get_port_number(device);
    printf("[USB] Found C270 on bus %d port %d\n", dev->bus, dev->port);

    /* Detach kernel driver nếu vẫn còn (safety) */
    for (int iface = 0; iface <= 1; iface++) {
        if (libusb_kernel_driver_active(dev->handle, iface) == 1) {
            printf("[USB] Detaching kernel driver from interface %d\n", iface);
            int r = libusb_detach_kernel_driver(dev->handle, iface);
            if (r < 0) {
                fprintf(stderr, "[USB] detach interface %d failed: %s\n",
                        iface, libusb_error_name(r));
                libusb_close(dev->handle);
                return -1;
            }
        }
    }

    /* Configuration 1 đã được kernel set sẵn — không cần set lại
     * set_configuration khi device đang active sẽ gây LIBUSB_ERROR_BUSY */
    int r;

    /* Claim control interface (interface 0) */
    r = libusb_claim_interface(dev->handle, UVC_CONTROL_INTERFACE);
    if (r < 0) {
        fprintf(stderr, "[USB] claim interface 0 failed: %s\n", libusb_error_name(r));
        libusb_close(dev->handle);
        return -1;
    }

    /* Claim streaming interface (interface 1) */
    r = libusb_claim_interface(dev->handle, UVC_STREAMING_INTERFACE);
    if (r < 0) {
        fprintf(stderr, "[USB] claim interface 1 failed: %s\n", libusb_error_name(r));
        libusb_release_interface(dev->handle, UVC_CONTROL_INTERFACE);
        libusb_close(dev->handle);
        return -1;
    }

    dev->is_open = 1;
    printf("[USB] C270 opened — interfaces 0 & 1 claimed\n");
    return 0;
}

void c270_usb_close(C270Device *dev) {
    if (!dev->is_open) return;
    libusb_release_interface(dev->handle, UVC_STREAMING_INTERFACE);
    libusb_release_interface(dev->handle, UVC_CONTROL_INTERFACE);
    libusb_close(dev->handle);
    dev->handle  = NULL;
    dev->is_open = 0;
    printf("[USB] C270 closed\n");
}

void c270_usb_exit(C270Device *dev) {
    if (dev->ctx) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }
}

void c270_usb_print_info(C270Device *dev) {
    if (!dev->is_open) {
        printf("[USB] Device not open\n");
        return;
    }

    libusb_device *device = libusb_get_device(dev->handle);
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(device, &desc);

    printf("[USB] ── Device Info ──────────────────\n");
    printf("[USB]   VID:PID   = %04x:%04x\n", desc.idVendor, desc.idProduct);
    printf("[USB]   Bus:Port  = %d:%d\n", dev->bus, dev->port);
    printf("[USB]   USB spec  = %x\n", desc.bcdUSB);
    printf("[USB]   Num confs = %d\n", desc.bNumConfigurations);

    /* In alternate settings của streaming interface */
    struct libusb_config_descriptor *config;
    libusb_get_active_config_descriptor(device, &config);

    printf("[USB]   Streaming interface alternate settings:\n");
    const struct libusb_interface *iface = &config->interface[UVC_STREAMING_INTERFACE];
    for (int a = 0; a < iface->num_altsetting; a++) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
        printf("[USB]     alt=%d  endpoints=%d\n", alt->bAlternateSetting,
               alt->bNumEndpoints);
        for (int e = 0; e < alt->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            printf("[USB]       EP 0x%02x  type=%d  maxPacket=%d\n",
                   ep->bEndpointAddress,
                   ep->bmAttributes & 0x03,
                   ep->wMaxPacketSize);
        }
    }
    libusb_free_config_descriptor(config);
    printf("[USB] ──────────────────────────────────\n");
}
