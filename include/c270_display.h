#ifndef C270_DISPLAY_H
#define C270_DISPLAY_H

#include "c270_capture.h"
#include <stdint.h>

typedef struct {
    void    *sdl_window;    /* SDL_Window*  — void* để tránh include SDL2 ở đây */
    void    *sdl_renderer;  /* SDL_Renderer* */
    void    *sdl_texture;   /* SDL_Texture*  */
    int      width;
    int      height;
    int      is_init;
} DisplayContext;

/* ── Public API ── */
int  display_init(DisplayContext *disp, int width, int height,
                  const char *title);
void display_show_frame(DisplayContext *disp,
                        const DecodedFrame *frame,
                        float fps,
                        const char *camera_id);
int  display_poll_events(DisplayContext *disp);  /* return 0=ok, -1=quit */
void display_free(DisplayContext *disp);

#endif /* C270_DISPLAY_H */
