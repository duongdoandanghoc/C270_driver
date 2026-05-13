#include "c270_display.h"
#include <SDL2/SDL.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

int display_init(DisplayContext *disp, int width, int height,
                 const char *title)
{
    memset(disp, 0, sizeof(*disp));

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[DISP] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->sdl_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, 0);
    if (!disp->sdl_window) {
        fprintf(stderr, "[DISP] CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->sdl_renderer = SDL_CreateRenderer(
        (SDL_Window *)disp->sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!disp->sdl_renderer) {
        fprintf(stderr, "[DISP] CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Texture RGB24 — size sẽ update khi có frame đầu tiên */
    disp->sdl_texture = SDL_CreateTexture(
        (SDL_Renderer *)disp->sdl_renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        640, 480);  /* C270 MJPEG resolution */
    if (!disp->sdl_texture) {
        fprintf(stderr, "[DISP] CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->width   = width;
    disp->height  = height;
    disp->is_init = 1;

    /* Clear màn hình đen ngay khi init */
    SDL_SetRenderDrawColor((SDL_Renderer*)disp->sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear((SDL_Renderer*)disp->sdl_renderer);
    SDL_RenderPresent((SDL_Renderer*)disp->sdl_renderer);

    printf("[DISP] SDL2 display initialized %dx%d\n", width, height);
    return 0;
}

void display_show_frame(DisplayContext *disp,
                        const DecodedFrame *frame,
                        float fps,
                        const char *camera_id)
{
    if (!disp->is_init) return;

    SDL_Renderer *renderer = (SDL_Renderer *)disp->sdl_renderer;
    SDL_Texture  *texture  = (SDL_Texture  *)disp->sdl_texture;

    /* Upload RGB pixels vào texture */
    SDL_UpdateTexture(texture, NULL,
                      frame->pixels,
                      frame->width * 3);   /* pitch = width * 3 bytes */

    SDL_RenderClear(renderer);
    /* Scale lên 4x để dễ nhìn hơn */
    SDL_Rect dst = {0, 0,
        ((SDL_Window*)disp->sdl_window) ? disp->width : frame->width*4,
        ((SDL_Window*)disp->sdl_window) ? disp->height : frame->height*4};
    SDL_GetWindowSize((SDL_Window*)disp->sdl_window, &dst.w, &dst.h);
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    /* ── Overlay: FPS + Timestamp + Camera ID ──
     * SDL2 không có built-in font → dùng SDL_SetRenderDrawColor
     * để vẽ rectangle làm background, in text qua title bar.
     * Để render text đẹp hơn cần SDL2_ttf — hiện tại dùng
     * window title để hiển thị overlay info (đủ dùng cho demo).
     */
    char title_buf[128];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    snprintf(title_buf, sizeof(title_buf),
             "C270 Driver | %s | FPS: %.1f | Frame#%u | %s",
             camera_id, fps, frame->frame_number, time_str);

    SDL_SetWindowTitle((SDL_Window *)disp->sdl_window, title_buf);

    /* Semi-transparent overlay bar ở góc trên trái */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);  /* đen 63% opacity */
    SDL_Rect bar = {0, 0, 320, 20};
    SDL_RenderFillRect(renderer, &bar);

    SDL_RenderPresent(renderer);
}

/* ── Internal: JPEG error handler for display decode ── */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf               setjmp_buf;
} DispJpegError;

static void disp_jpeg_error_exit(j_common_ptr cinfo) {
    DispJpegError *err = (DispJpegError *)cinfo->err;
    longjmp(err->setjmp_buf, 1);
}

static void disp_jpeg_suppress(j_common_ptr cinfo) { (void)cinfo; }

/*
 * display_show_mjpeg — Show MJPEG frame (decode + render)
 *
 * TÁC DỤNG:
 *   Decode MJPEG data → RGB24 via libjpeg, rồi upload vào
 *   SDL2 texture và render giống display_show_frame.
 *
 * TÁC ĐỘNG:
 *   - Allocate/reuse internal decode_buf (disp->decode_buf)
 *   - Update SDL texture + window title overlay
 *
 * CONTEXT:
 *   Process context (main thread)
 */
void display_show_mjpeg(DisplayContext *disp,
                        const uint8_t *jpeg_data, uint32_t jpeg_size,
                        float fps, const char *camera_id)
{
    if (!disp->is_init || !jpeg_data || jpeg_size == 0) return;

    /* Allocate decode buffer if needed */
    size_t rgb_size = (size_t)(disp->width * disp->height * 3);
    if (!disp->decode_buf) {
        disp->decode_buf = malloc(rgb_size);
        if (!disp->decode_buf) return;
    }

    /* Decode MJPEG → RGB24 */
    struct jpeg_decompress_struct cinfo;
    DispJpegError jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = disp_jpeg_error_exit;
    jerr.pub.output_message = disp_jpeg_suppress;

    if (setjmp(jerr.setjmp_buf)) {
        jpeg_destroy_decompress(&cinfo);
        return;  /* corrupt frame, skip */
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg_data, jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int stride = (int)cinfo.output_width * 3;

    JSAMPROW row_ptr[1];
    while (cinfo.output_scanline < cinfo.output_height) {
        row_ptr[0] = disp->decode_buf + cinfo.output_scanline * stride;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    /* Render via SDL2 */
    SDL_Renderer *renderer = (SDL_Renderer *)disp->sdl_renderer;
    SDL_Texture  *texture  = (SDL_Texture  *)disp->sdl_texture;

    SDL_UpdateTexture(texture, NULL, disp->decode_buf, stride);
    SDL_RenderClear(renderer);

    SDL_Rect dst = {0, 0, 0, 0};
    SDL_GetWindowSize((SDL_Window*)disp->sdl_window, &dst.w, &dst.h);
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    /* Overlay */
    char title_buf[128];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    snprintf(title_buf, sizeof(title_buf),
             "C270 V4L2 | %s | FPS: %.1f | %s", camera_id, fps, time_str);
    SDL_SetWindowTitle((SDL_Window *)disp->sdl_window, title_buf);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_Rect bar = {0, 0, 320, 20};
    SDL_RenderFillRect(renderer, &bar);
    SDL_RenderPresent(renderer);
}

/*
 * display_show_status — Show status overlay on camera window
 *
 * TÁC DỤNG:
 *   Hiển thị thông báo trạng thái camera lên cửa sổ SDL2 thay vì
 *   chỉ in ra terminal. Người dùng sẽ thấy trực quan khi camera
 *   bị rút, đang reconnect, hoặc kết nối lại thành công.
 *
 *   Background color theo loại status:
 *     DISCONNECTED  → đỏ (200,30,30)
 *     RECONNECTING  → cam (200,140,0)
 *     CONNECTED     → xanh lá (30,160,60)
 *     ERROR         → đỏ đậm (140,10,10)
 *
 * TÁC ĐỘNG:
 *   - Clear cửa sổ, vẽ background + icon bar + message vào title
 *   - RenderPresent → cập nhật ngay trên màn hình
 *
 * CONTEXT: Main thread (gọi từ capture loop)
 */
void display_show_status(DisplayContext *disp, DisplayStatusType type,
                         const char *message)
{
    if (!disp->is_init) return;

    SDL_Renderer *renderer = (SDL_Renderer *)disp->sdl_renderer;

    /* Background color theo status type */
    uint8_t r, g, b;
    const char *icon;
    switch (type) {
    case DISP_STATUS_DISCONNECTED:
        r = 180; g = 30;  b = 30;  icon = "⚠ DISCONNECTED"; break;
    case DISP_STATUS_RECONNECTING:
        r = 200; g = 140; b = 0;   icon = "⟳ RECONNECTING"; break;
    case DISP_STATUS_CONNECTED:
        r = 30;  g = 160; b = 60;  icon = "✓ CONNECTED";    break;
    case DISP_STATUS_ERROR:
    default:
        r = 140; g = 10;  b = 10;  icon = "✗ ERROR";        break;
    }

    /* Clear with status color */
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderClear(renderer);

    /* Central dark bar for text readability */
    int win_w = 0, win_h = 0;
    SDL_GetWindowSize((SDL_Window*)disp->sdl_window, &win_w, &win_h);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);

    int bar_h = 80;
    int bar_y = (win_h - bar_h) / 2;
    SDL_Rect bar = {0, bar_y, win_w, bar_h};
    SDL_RenderFillRect(renderer, &bar);

    /* White icon/indicator circles */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
    int cx = win_w / 2;
    int cy = bar_y - 30;
    /* Draw camera icon (simple rectangle + circle) */
    SDL_Rect cam_body = {cx - 30, cy - 15, 60, 30};
    SDL_RenderFillRect(renderer, &cam_body);
    /* Lens circle (approximate with small filled rect) */
    SDL_Rect lens = {cx - 8, cy - 8, 16, 16};
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderFillRect(renderer, &lens);

    /* Window title carries the message text
     * (SDL2 doesn't have built-in text rendering without SDL2_ttf)
     */
    char title_buf[256];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
    snprintf(title_buf, sizeof(title_buf),
             "%s | %s | %s", icon, message, time_str);
    SDL_SetWindowTitle((SDL_Window *)disp->sdl_window, title_buf);

    SDL_RenderPresent(renderer);
}

int display_poll_events(DisplayContext *disp) {
    (void)disp;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_KEYDOWN &&
             event.key.keysym.sym == SDLK_ESCAPE))
        {
            return -1;   /* signal quit */
        }
    }
    return 0;
}

void display_free(DisplayContext *disp) {
    if (!disp->is_init) return;
    free(disp->decode_buf);
    disp->decode_buf = NULL;
    if (disp->sdl_texture)  SDL_DestroyTexture((SDL_Texture *)disp->sdl_texture);
    if (disp->sdl_renderer) SDL_DestroyRenderer((SDL_Renderer *)disp->sdl_renderer);
    if (disp->sdl_window)   SDL_DestroyWindow((SDL_Window *)disp->sdl_window);
    SDL_Quit();
    disp->is_init = 0;
    printf("[DISP] Display freed\n");
}
