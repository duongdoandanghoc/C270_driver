#include "c270_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <jpeglib.h>

/* ────────────────────────────────────────────────────────────
 * Internal: timestamp milliseconds
 * ──────────────────────────────────────────────────────────── */
static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ────────────────────────────────────────────────────────────
 * Internal: Custom JPEG error handler
 *
 * UVC MJPEG over isochronous USB thường có minor corruption
 * (extraneous bytes trước RST markers) do packet loss.
 * libjpeg xử lý được nhưng spam stderr → suppress warnings.
 * Fatal errors → longjmp về caller.
 * ──────────────────────────────────────────────────────────── */
#include <setjmp.h>

typedef struct {
    struct jpeg_error_mgr pub;     /* libjpeg error manager */
    jmp_buf               setjmp_buf;  /* longjmp target */
} JpegErrorContext;

static void jpeg_error_exit_handler(j_common_ptr cinfo) {
    JpegErrorContext *ctx = (JpegErrorContext *)cinfo->err;
    /* Không print — caller sẽ xử lý */
    longjmp(ctx->setjmp_buf, 1);
}

static void jpeg_output_message_suppress(j_common_ptr cinfo) {
    (void)cinfo;   /* Suppress tất cả warning messages */
}

/* ────────────────────────────────────────────────────────────
 * Internal: Decode MJPEG → RGB24 dùng libjpeg-turbo
 * ──────────────────────────────────────────────────────────── */
static int decode_mjpeg(const uint8_t *jpeg_data, size_t jpeg_size,
                        DecodedFrame *out)
{
    struct jpeg_decompress_struct cinfo;
    JpegErrorContext jerr;

    /* Setup custom error handler với longjmp recovery */
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = jpeg_error_exit_handler;
    jerr.pub.output_message = jpeg_output_message_suppress;

    if (setjmp(jerr.setjmp_buf)) {
        /* Fatal JPEG error — cleanup và return lỗi */
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);

    jpeg_mem_src(&cinfo, (unsigned char *)jpeg_data, (unsigned long)jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int width  = (int)cinfo.output_width;
    int height = (int)cinfo.output_height;

    size_t needed = (size_t)(width * height * 3);
    if (!out->pixels) {
        out->pixels = malloc(needed);
        if (!out->pixels) {
            jpeg_destroy_decompress(&cinfo);
            return -1;
        }
    }

    out->width  = width;
    out->height = height;

    JSAMPROW row_pointer[1];
    int row_stride = width * 3;

    while (cinfo.output_scanline < cinfo.output_height) {
        row_pointer[0] = out->pixels +
                         cinfo.output_scanline * row_stride;
        jpeg_read_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* ────────────────────────────────────────────────────────────
 * Internal: Decode YUYV → RGB24 (không cần library)
 *
 * YUYV format (còn gọi là YUY2):
 *   Mỗi 4 bytes = 2 pixels
 *   [Y0][U][Y1][V]
 *   Pixel0 = (Y0, U, V)
 *   Pixel1 = (Y1, U, V)
 *
 * Công thức YUV→RGB (BT.601):
 *   R = Y + 1.402*(V-128)
 *   G = Y - 0.344*(U-128) - 0.714*(V-128)
 *   B = Y + 1.772*(U-128)
 * ──────────────────────────────────────────────────────────── */
static inline uint8_t clamp_u8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static int decode_yuyv(const uint8_t *yuyv, size_t yuyv_size,
                       DecodedFrame *out, int width, int height)
{
    size_t expected = (size_t)(width * height * 2);
    if (yuyv_size < expected) return -1;

    size_t rgb_size = (size_t)(width * height * 3);
    if (!out->pixels) {
        out->pixels = malloc(rgb_size);
        if (!out->pixels) return -1;
    }

    out->width  = width;
    out->height = height;

    /* Step 1: decode YUYV → RGB thẳng */
    uint8_t *dst = out->pixels;
    const uint8_t *src = yuyv;
    int total_pixels = width * height;

    for (int i = 0; i < total_pixels; i += 2) {
        int y0 = src[0], u = src[1], y1 = src[2], v = src[3];
        src += 4;
        int d = u - 128, e = v - 128;

        int c = y0 - 16;
        dst[0] = clamp_u8((298*c + 409*e + 128) >> 8);
        dst[1] = clamp_u8((298*c - 100*d - 208*e + 128) >> 8);
        dst[2] = clamp_u8((298*c + 516*d + 128) >> 8);
        dst += 3;

        c = y1 - 16;
        dst[0] = clamp_u8((298*c + 409*e + 128) >> 8);
        dst[1] = clamp_u8((298*c - 100*d - 208*e + 128) >> 8);
        dst[2] = clamp_u8((298*c + 516*d + 128) >> 8);
        dst += 3;
    }

    /* Step 2: flip horizontal in-place — swap pixel trái/phải mỗi dòng */
    int row_bytes = width * 3;
    for (int row = 0; row < height; row++) {
        uint8_t *left  = out->pixels + row * row_bytes;
        uint8_t *right = left + (width - 1) * 3;
        while (left < right) {
            /* swap 3 bytes (1 pixel) */
            uint8_t tmp[3];
            tmp[0]=left[0]; tmp[1]=left[1]; tmp[2]=left[2];
            left[0]=right[0]; left[1]=right[1]; left[2]=right[2];
            right[0]=tmp[0]; right[1]=tmp[1]; right[2]=tmp[2];
            left  += 3;
            right -= 3;
        }
    }
    return 0;
}

/* ────────────────────────────────────────────────────────────
 * Internal: Auto-detect format và decode
 * ──────────────────────────────────────────────────────────── */
static int decode_frame(const uint8_t *data, size_t size,
                        DecodedFrame *out, int width, int height)
{
    /* JPEG magic: ff d8 ff */
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8) {
        return decode_mjpeg(data, size, out);
    }
    /* YUYV: không có magic bytes, detect bằng size */
    if (size >= (size_t)(width * height * 2)) {
        static int logged = 0;
        if (!logged) {
            printf("[CAP] Format: YUYV (uncompressed)\n");
            logged = 1;
        }
        return decode_yuyv(data, size, out, width, height);
    }
    return -1;
}

/* ────────────────────────────────────────────────────────────
 * Internal: Xử lý UVC payload packet
 *
 * UVC payload format:
 *   byte 0: HLE (Header Length, thường = 12)
 *   byte 1: bmHeaderInfo
 *     bit0: FID (Frame ID — toggle mỗi frame mới)
 *     bit1: EOF (End of Frame)
 *     bit2: PTS present
 *     bit3: SCR present
 *     bit5: STI (Still Image)
 *     bit6: ERR
 *     bit7: EOH
 *   bytes 2..(HLE-1): optional PTS, SCR
 *   bytes HLE..(len-1): payload data (MJPEG chunk)
 * ──────────────────────────────────────────────────────────── */
static void process_uvc_payload(CaptureContext *ctx,
                                const uint8_t *data, int length)
{
    if (length < 2) return;

    uint8_t hle         = data[0];   /* header length */
    uint8_t bmi         = data[1];   /* bmHeaderInfo */

    /* debug removed */

    if (hle > length) return;        /* malformed */

    uint8_t frame_id    = bmi & 0x01;
    uint8_t eof         = (bmi >> 1) & 0x01;
    uint8_t err         = (bmi >> 6) & 0x01;

    if (err) return;   /* packet lỗi, bỏ qua */

    const uint8_t *payload     = data + hle;
    int            payload_len = length - hle;

    FrameBuffer *fb = &ctx->frame_buf;

    /* Frame ID toggle → frame mới bắt đầu */
    if (frame_id != fb->last_frame_id) {
        fb->size          = 0;   /* reset buffer */
        fb->last_frame_id = frame_id;
    }

    /* Append payload */
    if (payload_len > 0 &&
        fb->size + (size_t)payload_len <= sizeof(fb->data))
    {
        memcpy(fb->data + fb->size, payload, payload_len);
        fb->size += payload_len;
    }

    /* EOF → frame hoàn chỉnh (MJPEG)
     * Hoặc FID toggle với buffer đủ lớn → frame hoàn chỉnh (YUYV)
     */
    int yuyv_complete = (fb->size >= (size_t)(ctx->cfg.width * ctx->cfg.height * 2));
    if ((eof || yuyv_complete) && fb->size > 0) {
        ctx->decoded.timestamp_ms = now_ms();
        ctx->decoded.frame_number = ctx->frame_count++;

        if (decode_frame(fb->data, fb->size, &ctx->decoded,
                          ctx->cfg.width, ctx->cfg.height) == 0) {
            /* Tính FPS mỗi 30 frames */
            if (ctx->frame_count % 30 == 0) {
                uint64_t now = now_ms();
                uint64_t elapsed = now - ctx->fps_timer_ms;
                if (elapsed > 0)
                    ctx->current_fps = 30000.0f / (float)elapsed;
                ctx->fps_timer_ms = now;
            }

            /* Gọi callback */
            if (ctx->on_frame)
                ctx->on_frame(&ctx->decoded, ctx->userdata);
        }
        fb->size = 0;
    }
}

/* ────────────────────────────────────────────────────────────
 * Isochronous transfer callback (gọi từ libusb event thread)
 * ──────────────────────────────────────────────────────────── */
static void LIBUSB_CALL iso_callback(struct libusb_transfer *transfer) {
    CaptureContext *ctx = (CaptureContext *)transfer->user_data;

    if (!ctx->running) return;

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED ||
        transfer->status == LIBUSB_TRANSFER_ERROR)  /* partial OK */ {

        /* Duyệt từng iso packet trong transfer */
        int offset = 0;
        for (int i = 0; i < transfer->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *pkt =
                &transfer->iso_packet_desc[i];

            if (pkt->status == LIBUSB_TRANSFER_COMPLETED && pkt->actual_length > 0) {
                process_uvc_payload(ctx,
                                    transfer->buffer + offset,
                                    (int)pkt->actual_length);
            }
            offset += pkt->length;
        }
    }

    /* Re-submit transfer nếu vẫn đang chạy */
    if (ctx->running) {
        int r = libusb_submit_transfer(transfer);
        if (r < 0)
            fprintf(stderr, "[CAP] resubmit failed: %s\n",
                    libusb_error_name(r));
    }
}

/* ────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────── */
int capture_init(CaptureContext *ctx, C270Device *dev,
                 const C270Config *cfg,
                 FrameCallback cb, void *userdata)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->dev       = dev;
    ctx->cfg       = *cfg;
    ctx->on_frame  = cb;
    ctx->userdata  = userdata;
    ctx->fps_timer_ms = now_ms();
    printf("[CAP] Capture context initialized\n");
    return 0;
}

int capture_start(CaptureContext *ctx) {
    ctx->running = 1;

    /* ── Tự động detect maxPacket của alt setting đang dùng ──
     * Không hardcode ISO_PACKET_SIZE vì mỗi máy/port có thể khác nhau.
     * Đọc trực tiếp từ USB descriptor tại runtime.
     */
    libusb_device *device = libusb_get_device(ctx->dev->handle);
    struct libusb_config_descriptor *config = NULL;
    libusb_get_active_config_descriptor(device, &config);

    uint16_t max_packet = 0;
    uint8_t  alt_in_use = ctx->dev->active_alt;
    if (alt_in_use == 0) alt_in_use = C270_ALT_SETTING_STREAM;

    if (config) {
        const struct libusb_interface *iface =
            &config->interface[UVC_STREAMING_INTERFACE];
        /* Tìm alt setting đang dùng */
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor *alt =
                &iface->altsetting[a];
            if (alt->bAlternateSetting == alt_in_use &&
                alt->bNumEndpoints > 0)
            {
                /* USB 2.0 HS iso: wMaxPacketSize encoding
                 *   bit[10:0]  = actual packet size
                 *   bit[12:11] = extra transactions per microframe
                 * Kernel cần bit[10:0] thực tế, không phải raw value */
                uint16_t raw = alt->endpoint[0].wMaxPacketSize;
                max_packet = raw & 0x07FF;  /* mask lấy 11 bit thấp */
                break;
            }
        }
        libusb_free_config_descriptor(config);
    }

    if (max_packet == 0) {
        fprintf(stderr, "[CAP] Cannot detect maxPacket, using fallback 512\n");
        max_packet = 512;
    }

    printf("[CAP] Alt=%d  maxPacket=%u bytes\n", alt_in_use, max_packet);

    /* Allocate và submit tất cả transfer */
    for (int i = 0; i < ISO_NUM_TRANSFERS; i++) {
        ctx->transfer_bufs[i] =
            malloc(ISO_PACKETS_PER_XFER * max_packet);
        if (!ctx->transfer_bufs[i]) {
            fprintf(stderr, "[CAP] malloc transfer buf %d failed\n", i);
            ctx->running = 0;
            return -1;
        }

        ctx->transfers[i] = libusb_alloc_transfer(ISO_PACKETS_PER_XFER);
        if (!ctx->transfers[i]) {
            fprintf(stderr, "[CAP] alloc transfer %d failed\n", i);
            ctx->running = 0;
            return -1;
        }

        libusb_fill_iso_transfer(
            ctx->transfers[i],
            ctx->dev->handle,
            C270_ISO_ENDPOINT,
            ctx->transfer_bufs[i],
            ISO_PACKETS_PER_XFER * max_packet,
            ISO_PACKETS_PER_XFER,
            iso_callback,
            ctx,
            5000
        );
        libusb_set_iso_packet_lengths(ctx->transfers[i], max_packet);

        int r = libusb_submit_transfer(ctx->transfers[i]);
        if (r < 0) {
            fprintf(stderr, "[CAP] submit transfer %d failed: %s\n",
                    i, libusb_error_name(r));
            ctx->running = 0;
            return -1;
        }
    }

    printf("[CAP] %d iso transfers submitted (packet=%u bytes) — capturing...\n",
           ISO_NUM_TRANSFERS, max_packet);
    return 0;
}

void capture_stop(CaptureContext *ctx) {
    ctx->running = 0;

    /* Cancel tất cả transfers */
    for (int i = 0; i < ISO_NUM_TRANSFERS; i++) {
        if (ctx->transfers[i])
            libusb_cancel_transfer(ctx->transfers[i]);
    }

    /* Drain libusb events để callbacks finish */
    struct timeval tv = {0, 100000};  /* 100ms */
    for (int i = 0; i < 20; i++)
        libusb_handle_events_timeout(ctx->dev->ctx, &tv);

    printf("[CAP] Capture stopped\n");
}

void capture_free(CaptureContext *ctx) {
    for (int i = 0; i < ISO_NUM_TRANSFERS; i++) {
        if (ctx->transfers[i]) {
            libusb_free_transfer(ctx->transfers[i]);
            ctx->transfers[i] = NULL;
        }
        free(ctx->transfer_bufs[i]);
        ctx->transfer_bufs[i] = NULL;
    }
    free(ctx->decoded.pixels);
    ctx->decoded.pixels = NULL;
    printf("[CAP] Capture resources freed\n");
}

float capture_get_fps(CaptureContext *ctx) {
    return ctx->current_fps;
}
