/* gpu_sdl.h — under MUSE_GPU, redefine the raw SDL_Render* primitives that
 * bypass the ui_render helpers so they route through the GPU batch (with a
 * tracked draw colour + SDL-style clip). Include this AFTER all other headers
 * in any .c file that calls SDL_RenderLine / SDL_RenderFillRect / etc. directly.
 *
 * When MUSE_GPU is not defined this header is empty — the real SDL functions
 * are used. Textures (SDL_RenderTexture) and render-targets (SDL_SetRenderTarget)
 * are NOT redirected here; those are handled separately. */
#ifndef GPU_SDL_H
#define GPU_SDL_H

#ifdef MUSE_GPU
#include <SDL3/SDL.h>
#include "gpu_backend.h"

static inline bool gsdl_setcol(SDL_Renderer *r, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca){ if(!gpu_active())return SDL_SetRenderDrawColor(r,cr,cg,cb,ca); gpu_set_draw_color(cr,cg,cb,ca); return true; }
static inline bool gsdl_setcolf(SDL_Renderer *r, float cr, float cg, float cb, float ca){ if(!gpu_active())return SDL_SetRenderDrawColorFloat(r,cr,cg,cb,ca); gpu_set_draw_color((Uint8)(cr*255+0.5f),(Uint8)(cg*255+0.5f),(Uint8)(cb*255+0.5f),(Uint8)(ca*255+0.5f)); return true; }
static inline bool gsdl_line(SDL_Renderer *r, float x1, float y1, float x2, float y2){ if(!gpu_active())return SDL_RenderLine(r,x1,y1,x2,y2); gpu_raw_line(x1,y1,x2,y2); return true; }
static inline bool gsdl_lines(SDL_Renderer *r, const SDL_FPoint *p, int n){ if(!gpu_active())return SDL_RenderLines(r,p,n); for(int i=0;i+1<n;i++)gpu_raw_line(p[i].x,p[i].y,p[i+1].x,p[i+1].y); return true; }
static inline bool gsdl_fillrect(SDL_Renderer *r, const SDL_FRect *rc){ if(!gpu_active())return SDL_RenderFillRect(r,rc); if(rc)gpu_raw_fillrect(rc->x,rc->y,rc->w,rc->h); return true; }
static inline bool gsdl_fillrects(SDL_Renderer *r, const SDL_FRect *rc, int n){ if(!gpu_active())return SDL_RenderFillRects(r,rc,n); for(int i=0;i<n;i++)gpu_raw_fillrect(rc[i].x,rc[i].y,rc[i].w,rc[i].h); return true; }
static inline bool gsdl_rect(SDL_Renderer *r, const SDL_FRect *rc){ if(!gpu_active())return SDL_RenderRect(r,rc); if(rc)gpu_raw_rect(rc->x,rc->y,rc->w,rc->h); return true; }
static inline bool gsdl_rects(SDL_Renderer *r, const SDL_FRect *rc, int n){ if(!gpu_active())return SDL_RenderRects(r,rc,n); for(int i=0;i<n;i++)gpu_raw_rect(rc[i].x,rc[i].y,rc[i].w,rc[i].h); return true; }
static inline bool gsdl_point(SDL_Renderer *r, float x, float y){ if(!gpu_active())return SDL_RenderPoint(r,x,y); gpu_raw_point(x,y); return true; }
static inline bool gsdl_points(SDL_Renderer *r, const SDL_FPoint *p, int n){ if(!gpu_active())return SDL_RenderPoints(r,p,n); for(int i=0;i<n;i++)gpu_raw_point(p[i].x,p[i].y); return true; }
static inline bool gsdl_geom(SDL_Renderer *r, SDL_Texture *t, const SDL_Vertex *v, int nv, const int *idx, int ni){ if(!gpu_active())return SDL_RenderGeometry(r,t,v,nv,idx,ni); if(!t)gpu_emit_geometry(v,nv,idx,ni); return true; }
static inline bool gsdl_clip(SDL_Renderer *r, const SDL_Rect *rc){ if(!gpu_active())return SDL_SetRenderClipRect(r,rc); if(rc)gpu_set_clip(rc->x,rc->y,rc->w,rc->h);else gpu_clear_clip(); return true; }
static inline bool gsdl_blend(SDL_Renderer *r, SDL_BlendMode m){ if(!gpu_active())return SDL_SetRenderDrawBlendMode(r,m); return true; }
/* SDL_Texture* under MUSE_GPU is a gpu_backend image handle (see gpu_image_from_surface). */
static inline bool gsdl_rendertex(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *src, const SDL_FRect *dst){
    if(!gpu_active())return SDL_RenderTexture(r,t,src,dst); if(t&&dst)gpu_emit_image((void*)t,dst->x,dst->y,dst->w,dst->h,src?1:0,src?src->x:0.f,src?src->y:0.f,src?src->w:0.f,src?src->h:0.f); return true; }
static inline bool gsdl_texmod(SDL_Texture *t, Uint8 r, Uint8 g, Uint8 b){ if(!gpu_active())return SDL_SetTextureColorMod(t,r,g,b); gpu_image_set_mod((void*)t,r,g,b); return true; }
static inline bool gsdl_texalpha(SDL_Texture *t, Uint8 a){ if(!gpu_active())return SDL_SetTextureAlphaMod(t,a); gpu_image_set_alpha((void*)t,a); return true; }
static inline bool gsdl_texblend(SDL_Texture *t, SDL_BlendMode m){ if(!gpu_active())return SDL_SetTextureBlendMode(t,m); return true; }
static inline bool gsdl_texscale(SDL_Texture *t, SDL_ScaleMode m){ if(!gpu_active())return SDL_SetTextureScaleMode(t,m); return true; }
static inline void gsdl_destroytex(SDL_Texture *t){ if(!gpu_active())SDL_DestroyTexture(t); }
/* render-to-texture: TARGET textures become gpu render targets; SetRenderTarget push/pops the batch. */
static inline SDL_Texture *gsdl_createtex(SDL_Renderer *r, SDL_PixelFormat f, int access, int w, int h){
    if(!gpu_active())return SDL_CreateTexture(r,f,access,w,h); return (access==SDL_TEXTUREACCESS_TARGET)?(SDL_Texture*)gpu_create_target(w,h):NULL; }
static inline SDL_Texture *gsdl_texsurface(SDL_Renderer *r, SDL_Surface *s){ if(!gpu_active())return SDL_CreateTextureFromSurface(r,s); return (SDL_Texture*)gpu_image_from_surface(s); }
static inline bool gsdl_settarget(SDL_Renderer *r, SDL_Texture *t){ if(!gpu_active())return SDL_SetRenderTarget(r,t); if(t)gpu_target_push((void*)t);else gpu_target_pop(); return true; }
static inline bool gsdl_rendertexrot(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *src, const SDL_FRect *dst,
        double angle, const SDL_FPoint *ctr, SDL_FlipMode flip){
    if(!gpu_active())return SDL_RenderTextureRotated(r,t,src,dst,angle,ctr,flip); (void)src;(void)flip;
    if(t&&dst){ float px=dst->x+(ctr?ctr->x:dst->w*0.5f), py=dst->y+(ctr?ctr->y:dst->h*0.5f);
        gpu_emit_image_rotated((void*)t, dst->x,dst->y,dst->w,dst->h, (float)angle, px,py); }
    return true; }
static inline bool gsdl_clear(SDL_Renderer *r){ if(!gpu_active())return SDL_RenderClear(r); gpu_frame_begin(); return true; }
static inline bool gsdl_present(SDL_Renderer *r){ if(!gpu_active())return SDL_RenderPresent(r); gpu_frame_end(); return true; }

#define SDL_SetRenderDrawColor       gsdl_setcol
#define SDL_SetRenderDrawColorFloat  gsdl_setcolf
#define SDL_RenderLine               gsdl_line
#define SDL_RenderLines              gsdl_lines
#define SDL_RenderFillRect           gsdl_fillrect
#define SDL_RenderFillRects          gsdl_fillrects
#define SDL_RenderRect               gsdl_rect
#define SDL_RenderRects              gsdl_rects
#define SDL_RenderPoint              gsdl_point
#define SDL_RenderPoints             gsdl_points
#define SDL_RenderGeometry           gsdl_geom
#define SDL_SetRenderClipRect        gsdl_clip
#define SDL_SetRenderDrawBlendMode   gsdl_blend
#define SDL_RenderTexture            gsdl_rendertex
#define SDL_SetTextureColorMod       gsdl_texmod
#define SDL_SetTextureAlphaMod       gsdl_texalpha
#define SDL_SetTextureBlendMode      gsdl_texblend
#define SDL_SetTextureScaleMode      gsdl_texscale
#define SDL_DestroyTexture           gsdl_destroytex
#define SDL_CreateTexture            gsdl_createtex
#define SDL_CreateTextureFromSurface gsdl_texsurface
#define SDL_SetRenderTarget          gsdl_settarget
#define SDL_RenderTextureRotated     gsdl_rendertexrot
#define SDL_RenderClear              gsdl_clear
#define SDL_RenderPresent            gsdl_present

#endif /* MUSE_GPU */
#endif /* GPU_SDL_H */
