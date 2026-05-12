#ifndef C270_DISPLAY_H
#define C270_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    void    *sdl_window;    /* SDL_Window*  — void* để tránh include SDL2 ở đây */
    void    *sdl_renderer;  /* SDL_Renderer* */
    void    *sdl_texture;   /* SDL_Texture*  */
    uint8_t *decode_buf;    /* internal RGB buffer for MJPEG decode */
    int      width;
    int      height;
    int      is_init;
} DisplayContext;

/* ── Public API ── */
int  display_init(DisplayContext *disp, int width, int height,
                  const char *title);

/* Show decoded RGB24 frame (legacy) */
struct DecodedFrame;
void display_show_frame(DisplayContext *disp,
                        const struct DecodedFrame *frame,
                        float fps,
                        const char *camera_id);

/* Show MJPEG frame (decodes internally via libjpeg) */
void display_show_mjpeg(DisplayContext *disp,
                        const uint8_t *jpeg_data, uint32_t jpeg_size,
                        float fps, const char *camera_id);

int  display_poll_events(DisplayContext *disp);  /* return 0=ok, -1=quit */
void display_free(DisplayContext *disp);

#endif /* C270_DISPLAY_H */
