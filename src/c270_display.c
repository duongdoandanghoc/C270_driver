/*
 * c270_display.c — SDL2 Display with TTF Text Rendering
 *
 * Hiển thị video camera qua SDL2 + SDL2_ttf.
 * Khi camera bị rút/lỗi, hiện TEXT trực tiếp trên cửa sổ camera
 * (không chỉ đổi màu nền hay in terminal).
 *
 * ══════════════════════════════════════════════════════════════
 * TEXT RENDERING
 * ══════════════════════════════════════════════════════════════
 *   Sử dụng SDL2_ttf + font DejaVu Sans Bold (có sẵn trên Ubuntu).
 *   Khi hiện status:
 *     - Dòng 1 (to):   icon + tên status (vd: "⚠ MẤT KẾT NỐI")
 *     - Dòng 2 (nhỏ):  message chi tiết (vd: "Camera đã bị rút! Chờ...")
 *     - Dòng 3 (nhỏ):  timestamp
 *   Tất cả render trực tiếp trên cửa sổ SDL2, người dùng nhìn
 *   thấy ngay mà không cần xem terminal.
 */

#include "c270_display.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ── Font paths (Ubuntu/Debian standard) ── */
#define FONT_PATH_BOLD   "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
#define FONT_PATH_NORMAL "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define FONT_SIZE_LARGE  28
#define FONT_SIZE_SMALL  18

/* ── Internal font handles stored in DisplayContext via opaque pointers ── */
/* We use the existing void* fields in DisplayContext for simplicity,
 * and add font pointers as module-level statics since there's only
 * one display context in the app. */
static TTF_Font *g_font_large = NULL;
static TTF_Font *g_font_small = NULL;
static int       g_ttf_init   = 0;

/*
 * init_ttf — Initialize SDL2_ttf and load fonts
 *
 * Gọi 1 lần trong display_init(). Nếu font không tìm thấy,
 * text rendering sẽ bị disable (fallback về title bar).
 */
static void init_ttf(void)
{
    if (g_ttf_init) return;

    if (TTF_Init() < 0) {
        fprintf(stderr, "[DISP] TTF_Init failed: %s\n", TTF_GetError());
        return;
    }
    g_ttf_init = 1;

    g_font_large = TTF_OpenFont(FONT_PATH_BOLD, FONT_SIZE_LARGE);
    if (!g_font_large) {
        fprintf(stderr, "[DISP] Cannot open font %s: %s\n",
                FONT_PATH_BOLD, TTF_GetError());
    }

    g_font_small = TTF_OpenFont(FONT_PATH_NORMAL, FONT_SIZE_SMALL);
    if (!g_font_small) {
        fprintf(stderr, "[DISP] Cannot open font %s: %s\n",
                FONT_PATH_NORMAL, TTF_GetError());
    }

    if (g_font_large && g_font_small)
        printf("[DISP] TTF fonts loaded OK\n");
}

/*
 * cleanup_ttf — Free fonts and quit TTF
 */
static void cleanup_ttf(void)
{
    if (g_font_large) { TTF_CloseFont(g_font_large); g_font_large = NULL; }
    if (g_font_small) { TTF_CloseFont(g_font_small); g_font_small = NULL; }
    if (g_ttf_init) { TTF_Quit(); g_ttf_init = 0; }
}

/*
 * render_text_centered — Render a line of text centered horizontally
 *
 * @renderer: SDL renderer
 * @font:     TTF font to use
 * @text:     UTF-8 text string
 * @y:        vertical position (top of text)
 * @color:    text color
 * @win_w:    window width (for centering)
 */
static void render_text_centered(SDL_Renderer *renderer, TTF_Font *font,
                                 const char *text, int y,
                                 SDL_Color color, int win_w)
{
    if (!font || !text || !text[0]) return;

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    SDL_Rect dst;
    dst.w = surface->w;
    dst.h = surface->h;
    dst.x = (win_w - dst.w) / 2;
    dst.y = y;

    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/*
 * render_text_left — Render text aligned left with padding
 */
static void render_text_left(SDL_Renderer *renderer, TTF_Font *font,
                             const char *text, int x, int y,
                             SDL_Color color)
{
    if (!font || !text || !text[0]) return;

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    SDL_Rect dst = { x, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/*
 * render_text_right — Render text aligned to the right edge
 */
static void render_text_right(SDL_Renderer *renderer, TTF_Font *font,
                              const char *text, int right_x, int y,
                              SDL_Color color)
{
    if (!font || !text || !text[0]) return;

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    SDL_Rect dst = { right_x - surface->w, y, surface->w, surface->h };
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

/*
 * format_uptime — Format seconds into HH:MM:SS string
 */
static void format_uptime(uint32_t secs, char *buf, size_t buf_size)
{
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;
    snprintf(buf, buf_size, "%02u:%02u:%02u", h, m, s);
}

/*
 * render_uptime_badge — Draw uptime timer badge in bottom-right corner
 *
 * Vẽ 1 ô nhỏ semi-transparent ở góc dưới bên phải cửa sổ
 * chứa biểu tượng đồng hồ + thời gian HH:MM:SS.
 * Khi paused=1, hiện "(PAUSED)" bên cạnh.
 */
static void render_uptime_badge(SDL_Renderer *renderer, int win_w, int win_h,
                                uint32_t uptime_secs, int paused)
{
    if (!g_font_small) return;

    char uptime_str[32];
    format_uptime(uptime_secs, uptime_str, sizeof(uptime_str));

    char badge_text[64];
    if (paused)
        snprintf(badge_text, sizeof(badge_text), "UPTIME: %s (TAM DUNG)", uptime_str);
    else
        snprintf(badge_text, sizeof(badge_text), "UPTIME: %s", uptime_str);

    /* Measure text width for badge sizing */
    int text_w = 0, text_h = 0;
    TTF_SizeUTF8(g_font_small, badge_text, &text_w, &text_h);

    int pad = 8;
    int badge_w = text_w + pad * 2;
    int badge_h = text_h + pad;
    int badge_x = win_w - badge_w - 10;
    int badge_y = win_h - badge_h - 10;

    /* Semi-transparent dark background */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
    SDL_Rect bg = { badge_x, badge_y, badge_w, badge_h };
    SDL_RenderFillRect(renderer, &bg);

    /* Thin border */
    SDL_Color border_color;
    if (paused) {
        SDL_SetRenderDrawColor(renderer, 255, 160, 0, 180); /* orange */
        border_color = (SDL_Color){255, 200, 100, 255};
    } else {
        SDL_SetRenderDrawColor(renderer, 80, 200, 120, 180); /* green */
        border_color = (SDL_Color){140, 255, 180, 255};
    }
    SDL_RenderDrawRect(renderer, &bg);
    (void)border_color;

    /* Timer text */
    SDL_Color text_color;
    if (paused)
        text_color = (SDL_Color){255, 200, 100, 255}; /* orange-ish */
    else
        text_color = (SDL_Color){140, 255, 180, 255}; /* green-ish */

    render_text_left(renderer, g_font_small, badge_text,
                     badge_x + pad, badge_y + pad / 2, text_color);
}

/* ════════════════════════════════════════════════════════════ */

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

    /* Init TTF for text rendering */
    init_ttf();

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

    /* ── Overlay: FPS + Timestamp + Camera ID ── */
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
 * display_show_mjpeg — Show MJPEG frame (decode + render) with TTF overlay
 *
 * TÁC DỤNG:
 *   Decode MJPEG data → RGB24 via libjpeg, rồi upload vào
 *   SDL2 texture và render. Overlay FPS/timestamp bằng TTF text.
 */
void display_show_mjpeg(DisplayContext *disp,
                        const uint8_t *jpeg_data, uint32_t jpeg_size,
                        float fps, const char *camera_id,
                        uint32_t uptime_secs)
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
    int win_w, win_h;
    SDL_GetWindowSize((SDL_Window*)disp->sdl_window, &win_w, &win_h);
    dst.w = win_w;
    dst.h = win_h;
    SDL_RenderCopy(renderer, texture, NULL, &dst);

    /* ── TTF Overlay: FPS + timestamp trên cửa sổ ── */
    if (g_font_small) {
        /* Semi-transparent bar at top */
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
        SDL_Rect overlay_bar = {0, 0, win_w, 30};
        SDL_RenderFillRect(renderer, &overlay_bar);

        char overlay_text[128];
        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

        snprintf(overlay_text, sizeof(overlay_text),
                 "  %s  |  FPS: %.1f  |  %s", camera_id, fps, time_str);

        SDL_Color white = {255, 255, 255, 255};
        render_text_left(renderer, g_font_small, overlay_text, 8, 5, white);
    }

    /* ── Uptime badge: góc dưới phải ── */
    render_uptime_badge(renderer, win_w, win_h, uptime_secs, 0);

    /* Window title (backup for non-TTF) */
    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf),
             "C270 V4L2 | %s | FPS: %.1f", camera_id, fps);
    SDL_SetWindowTitle((SDL_Window *)disp->sdl_window, title_buf);

    SDL_RenderPresent(renderer);
}

/*
 * display_show_status — Hiển thị TEXT trạng thái trên cửa sổ camera
 *
 * TÁC DỤNG:
 *   Render TEXT trực tiếp lên cửa sổ SDL2 bằng SDL2_ttf.
 *   Người dùng nhìn vào cửa sổ camera sẽ đọc được thông báo
 *   (không cần nhìn terminal).
 *
 *   Layout:
 *   ┌─────────────────────────────────┐
 *   │         (nền màu)               │
 *   │                                 │
 *   │     ⚠ MẤT KẾT NỐI CAMERA      │  ← font lớn, trắng
 *   │                                 │
 *   │   Camera đã bị rút! Chờ kết    │  ← font nhỏ, trắng mờ
 *   │   nối lại...                    │
 *   │                                 │
 *   │            13:42:29             │  ← timestamp
 *   └─────────────────────────────────┘
 *
 * CONTEXT: Main thread (gọi từ capture loop)
 */
void display_show_status(DisplayContext *disp, DisplayStatusType type,
                         const char *message, uint32_t uptime_secs)
{
    if (!disp->is_init) return;

    SDL_Renderer *renderer = (SDL_Renderer *)disp->sdl_renderer;

    /* Background color theo status type */
    uint8_t r, g, b;
    const char *status_text;
    switch (type) {
    case DISP_STATUS_DISCONNECTED:
        r = 180; g = 30;  b = 30;
        status_text = "MAT KET NOI CAMERA";
        break;
    case DISP_STATUS_RECONNECTING:
        r = 200; g = 140; b = 0;
        status_text = "DANG KET NOI LAI...";
        break;
    case DISP_STATUS_CONNECTED:
        r = 30;  g = 160; b = 60;
        status_text = "DA KET NOI CAMERA";
        break;
    case DISP_STATUS_ERROR:
    default:
        r = 140; g = 10;  b = 10;
        status_text = "LOI CAMERA";
        break;
    }

    /* Clear with status background color */
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderClear(renderer);

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize((SDL_Window*)disp->sdl_window, &win_w, &win_h);

    /* ── Central dark panel for readability ── */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    int panel_w = win_w - 60;
    int panel_h = 160;
    int panel_x = 30;
    int panel_y = (win_h - panel_h) / 2;
    SDL_Rect panel = { panel_x, panel_y, panel_w, panel_h };
    SDL_RenderFillRect(renderer, &panel);

    /* Panel border */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);
    SDL_RenderDrawRect(renderer, &panel);

    if (g_font_large && g_font_small) {
        /* ── Line 1: Status title (large, white, centered) ── */
        SDL_Color white = {255, 255, 255, 255};
        render_text_centered(renderer, g_font_large,
                             status_text,
                             panel_y + 20, white, win_w);

        /* ── Line 2: Detail message (small, light gray, centered) ── */
        SDL_Color gray = {220, 220, 220, 255};
        render_text_centered(renderer, g_font_small,
                             message,
                             panel_y + 65, gray, win_w);

        /* ── Line 3: Timestamp (small, dim, centered) ── */
        time_t t = time(NULL);
        struct tm *tm_info = localtime(&t);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

        SDL_Color dim = {160, 160, 160, 255};
        render_text_centered(renderer, g_font_small,
                             time_str,
                             panel_y + 110, dim, win_w);
    }

    /* ── Uptime badge: góc dưới phải (paused khi disconnect) ── */
    int paused = (type == DISP_STATUS_DISCONNECTED ||
                  type == DISP_STATUS_RECONNECTING ||
                  type == DISP_STATUS_ERROR);
    render_uptime_badge(renderer, win_w, win_h, uptime_secs, paused);

    /* Window title (backup) */
    char title_buf[256];
    snprintf(title_buf, sizeof(title_buf), "%s | %s", status_text, message);
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
    cleanup_ttf();
    SDL_Quit();
    disp->is_init = 0;
    printf("[DISP] Display freed\n");
}
