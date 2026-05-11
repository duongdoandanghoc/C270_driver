#include "c270_display.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
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
    if (disp->sdl_texture)  SDL_DestroyTexture((SDL_Texture *)disp->sdl_texture);
    if (disp->sdl_renderer) SDL_DestroyRenderer((SDL_Renderer *)disp->sdl_renderer);
    if (disp->sdl_window)   SDL_DestroyWindow((SDL_Window *)disp->sdl_window);
    SDL_Quit();
    disp->is_init = 0;
    printf("[DISP] Display freed\n");
}
