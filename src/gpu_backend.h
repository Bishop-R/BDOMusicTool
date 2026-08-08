/* gpu_backend.h — SDL_GPU renderer backend for Muse (behind MUSE_GPU).
 *
 * When MUSE_GPU is defined, ui_render.c's primitives emit into this backend's
 * per-frame batch instead of calling SDL_Renderer, and app.c drives the frame
 * lifecycle. When MUSE_GPU is NOT defined, everything here is an inline no-op so
 * the normal build is untouched. */
#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;

#ifdef MUSE_GPU

/* lifecycle */
bool gpu_init(struct SDL_Window *window);
void gpu_shutdown(void);
void gpu_frame_begin(void);
void gpu_frame_end(void);          /* uploads the batch, draws, presents */
bool gpu_active(void);             /* true once gpu_init succeeded */
void gpu_test_frame(void);         /* Phase-0 self-contained test scene (still available) */

/* atlas dimensions for glyph UV (0 if the atlas failed to load) */
void gpu_atlas_size(int *w, int *h);

/* Render the current frame's batch to an offscreen texture, download it, and
 * return a NEW surface (caller owns/frees it) — for the self-capture recorder
 * and screenshots. Call after the frame's draws, before gpu_frame_end. NULL if
 * unavailable. Forward-declared surface so this header pulls in no SDL. */
struct SDL_Surface;
struct SDL_Surface *gpu_grab_surface(void);

/* --- emit primitives (top-left origin pixels; colours 0..255) --- */
void gpu_emit_rrect(float x, float y, float w, float h, float rad,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/* note shape: TL/TR/BR corners rounded, bottom-left square (piano-roll notes) */
void gpu_emit_note(float x, float y, float w, float h, float rad,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gpu_emit_rrect_grad(float x, float y, float w, float h, float rad, int horizontal,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                         uint8_t br, uint8_t bg, uint8_t bb, uint8_t ba);
/* outline: if it matches the most recent fill rect, it upgrades that instance
 * to a single fill+border (no conflation); otherwise a border-only instance. */
void gpu_emit_rrect_outline(float x, float y, float w, float h, float rad, float borderW,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gpu_emit_arc(float cx, float cy, float radius, float strokeHalf,
                  float a0deg, float a1deg, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gpu_emit_seg(float x0, float y0, float x1, float y1, float halfThick,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/* glyph: dest rect (px) + source rect in atlas px + colour */
void gpu_emit_glyph(float dx, float dy, float dw, float dh,
                    float sx, float sy, float sw, float sh,
                    uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/* flat colored triangles — a drop-in for SDL_RenderGeometry (untextured). */
struct SDL_Vertex;
void gpu_emit_geometry(const struct SDL_Vertex *v, int nv, const int *idx, int ni);

/* GPU images (SDL_RenderTexture routing): load a surface → opaque handle. */
void *gpu_image_from_surface(struct SDL_Surface *surf);
void  gpu_image_set_mod(void *img, uint8_t r, uint8_t g, uint8_t b);
void  gpu_image_set_alpha(void *img, uint8_t a);
void  gpu_image_free(void *img);
void  gpu_emit_image(void *img, float dx, float dy, float dw, float dh, int has_src, float sx, float sy, float sw, float sh);
void  gpu_emit_image_rotated(void *img, float dx, float dy, float dw, float dh, float angle_deg, float pivx, float pivy);
/* render-to-texture (SDL_SetRenderTarget): create a target image, redirect the batch to it. */
void *gpu_create_target(int w, int h);
void  gpu_target_push(void *img);
void  gpu_target_pop(void);
/* clip (scissor); nests via intersection */
void gpu_push_clip(float x, float y, float w, float h);
void gpu_pop_clip(void);

/* raw SDL_Render* routing (used by gpu_sdl.h) — tracked draw colour + SDL-style clip */
void gpu_set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gpu_raw_line(float x0, float y0, float x1, float y1);
void gpu_raw_fillrect(float x, float y, float w, float h);
void gpu_raw_rect(float x, float y, float w, float h);
void gpu_raw_point(float x, float y);
void gpu_set_clip(int x, int y, int w, int h);
void gpu_clear_clip(void);

#else  /* !MUSE_GPU — inline no-ops */

static inline bool gpu_init(struct SDL_Window *w) { (void)w; return false; }
static inline void gpu_shutdown(void) {}
static inline void gpu_frame_begin(void) {}
static inline void gpu_frame_end(void) {}
static inline bool gpu_active(void) { return false; }
static inline void gpu_test_frame(void) {}
static inline struct SDL_Surface *gpu_grab_surface(void) { return 0; }
static inline void *gpu_image_from_surface(struct SDL_Surface *s){ (void)s; return 0; }
static inline void gpu_image_set_mod(void *i, uint8_t r, uint8_t g, uint8_t b){ (void)i;(void)r;(void)g;(void)b; }
static inline void gpu_image_set_alpha(void *i, uint8_t a){ (void)i;(void)a; }
static inline void gpu_image_free(void *i){ (void)i; }
static inline void gpu_emit_image(void *i, float a, float b, float c, float d, int e, float f, float g, float h, float j){ (void)i;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)j; }
static inline void gpu_emit_image_rotated(void *i, float a, float b, float c, float d, float e, float f, float g){ (void)i;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
static inline void *gpu_create_target(int w, int h){ (void)w;(void)h; return 0; }
static inline void gpu_target_push(void *i){ (void)i; }
static inline void gpu_target_pop(void){}

#endif /* MUSE_GPU */

#endif /* GPU_BACKEND_H */

