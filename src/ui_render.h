#ifndef MUSE_UI_RENDER_H
#define MUSE_UI_RENDER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* Text rendering */
bool  text_init(SDL_Renderer *r, const char *font_path);
void  text_shutdown(void);
void  draw_text(SDL_Renderer *r, const char *str, float x, float y,
                float size, uint8_t cr, uint8_t cg, uint8_t cb);
void  draw_text_right(SDL_Renderer *r, const char *str, float x, float y,
                      float size, uint8_t cr, uint8_t cg, uint8_t cb);
void  draw_text_centered(SDL_Renderer *r, const char *str,
                         float cx, float cy, float size,
                         uint8_t cr, uint8_t cg, uint8_t cb);
float text_width(const char *str, float size);
float text_line_height(float size);
void  draw_text_bold(SDL_Renderer *r, const char *str, float x, float y,
                     float size, uint8_t cr, uint8_t cg, uint8_t cb);

/* Drawing primitives */
void draw_filled_rect(SDL_Renderer *r, float x, float y, float w, float h,
                      uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca);
void draw_rect_outline(SDL_Renderer *r, float x, float y, float w, float h,
                       uint8_t cr, uint8_t cg, uint8_t cb);
void draw_hline(SDL_Renderer *r, float x1, float x2, float y,
                uint8_t cr, uint8_t cg, uint8_t cb);
void draw_vline(SDL_Renderer *r, float x, float y1, float y2,
                uint8_t cr, uint8_t cg, uint8_t cb);
void draw_rounded_rect(SDL_Renderer *r, float x, float y, float w, float h,
                       float radius, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca);
void draw_rounded_rect_outline(SDL_Renderer *r, float x, float y, float w, float h,
                               float radius, uint8_t cr, uint8_t cg, uint8_t cb);
void draw_circle_filled(SDL_Renderer *r, float cx, float cy, float radius,
                        uint8_t cr, uint8_t cg, uint8_t cb);
void draw_circle_outline(SDL_Renderer *r, float cx, float cy, float radius,
                         uint8_t cr, uint8_t cg, uint8_t cb);

/* Horizontal gradient fill */
void draw_hgradient_rect(SDL_Renderer *r, float x, float y, float w, float h,
                         uint8_t r0, uint8_t g0, uint8_t b0,
                         uint8_t r1, uint8_t g1, uint8_t b1);

/* Widgets */
typedef struct { float x, y, w, h; } UiRect;
bool  ui_rect_contains(UiRect rc, float px, float py);

/* Dropdown arrow (filled triangle) */
void draw_dropdown_arrow(SDL_Renderer *r, float cx, float cy, float size,
                         uint8_t cr, uint8_t cg, uint8_t cb);
/* Button with rounded corners */
void draw_ctk_button(SDL_Renderer *r, UiRect rc, const char *label,
                     float font_sz, bool hovered, bool active);
/* Entry field: rounded, border, dark interior */
void draw_ctk_entry(SDL_Renderer *r, UiRect rc, const char *text,
                    float font_sz, bool focused);
/* Slider - gold thumb gets darker when dragging */
void draw_ctk_slider(SDL_Renderer *r, float x, float y, float w,
                     float value, float min_val, float max_val, bool active);
/* Radio button: circle with optional fill */
void draw_ctk_radio(SDL_Renderer *r, float x, float y, const char *label,
                    float font_sz, bool selected);
/* Scrollbar track + thumb */
void draw_scrollbar_v(SDL_Renderer *r, float x, float y, float h,
                      float view_frac, float scroll_frac, bool hovered);
void draw_scrollbar_h(SDL_Renderer *r, float x, float y, float w,
                      float view_frac, float scroll_frac, bool hovered);

/* Clipping */
void push_clip(SDL_Renderer *r, float x, float y, float w, float h);
void pop_clip(SDL_Renderer *r);

/* Instrument icons */
SDL_Texture *get_instrument_icon(uint8_t inst_id);
void init_instrument_icons(SDL_Renderer *r, const char *icons_dir);
void cleanup_instrument_icons(void);

/* Load a PNG as an SDL_Surface (caller must SDL_DestroySurface). Returns NULL on failure. */
SDL_Surface *load_png_surface(const char *path);
SDL_Surface *load_png_surface_mem(const void *data, int len);

/* Living Canvas FX - the aurora/glow stuff during playback */
void draw_aurora_blob(SDL_Renderer *r, float cx, float cy, float radius,
                      uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha);

#endif /* MUSE_UI_RENDER_H */
