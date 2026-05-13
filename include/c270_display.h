#ifndef C270_DISPLAY_H
#define C270_DISPLAY_H

#include <stdint.h>
#include <stddef.h>

/* DecodedFrame — legacy struct used by display_show_frame (backward compat) */
typedef struct {
    uint8_t  *pixels;      /* RGB24 data, width*height*3 bytes */
    int       width;
    int       height;
    uint64_t  timestamp_ms;
    uint32_t  frame_number;
} DecodedFrame;

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
void display_show_frame(DisplayContext *disp,
                        const DecodedFrame *frame,
                        float fps,
                        const char *camera_id);

/* Show MJPEG frame (decodes internally via libjpeg) */
void display_show_mjpeg(DisplayContext *disp,
                        const uint8_t *jpeg_data, uint32_t jpeg_size,
                        float fps, const char *camera_id);

int  display_poll_events(DisplayContext *disp);  /* return 0=ok, -1=quit */
void display_free(DisplayContext *disp);

/* Show status overlay (disconnect/reconnect messages) */
typedef enum {
    DISP_STATUS_DISCONNECTED,   /* camera rút ra — nền đỏ */
    DISP_STATUS_RECONNECTING,   /* đang reconnect — nền cam */
    DISP_STATUS_CONNECTED,      /* kết nối lại thành công — nền xanh */
    DISP_STATUS_ERROR            /* lỗi — nền đỏ đậm */
} DisplayStatusType;
void display_show_status(DisplayContext *disp, DisplayStatusType type,
                         const char *message);

#endif /* C270_DISPLAY_H */
