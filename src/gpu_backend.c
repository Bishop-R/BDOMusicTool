/* gpu_backend.c — SDL_GPU renderer backend for Muse.
 *
 * Batches every UI primitive emitted during a frame (via the gpu_emit_* API
 * that ui_render.c calls under MUSE_GPU) into two instance streams — SDF shapes
 * and atlas-text glyphs — plus an ordered run list that preserves paint order
 * and per-run scissor (clip). gpu_frame_end uploads the batch and replays the
 * runs to the swapchain in one pass. Shapes/text render with the exact shaders
 * proven in gpu_render_poc (inst.*, text.*).
 *
 * Self-contained: SDL3 + libc + stb_image (impl lives in ui_render.c). Whole
 * file compiles to an empty TU when MUSE_GPU is undefined. */
#include "gpu_backend.h"

#ifdef MUSE_GPU

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "stb_image.h"   /* declarations only; STB_IMAGE_IMPLEMENTATION is in ui_render.c */

#define BG_HEX 0x15171C
#define FR(h) (((h)>>16)&255)/255.0f
#define FG(h) (((h)>>8)&255)/255.0f
#define FB(h) ((h)&255)/255.0f

/* instance layouts (must match inst.vert/frag and text.vert/frag) */
typedef struct { float box[4], parm[4], fill[4], grad[4], bord[4], arc[4]; } SInst; /* 96 B */
typedef struct { float dst[4], uv[4], col[4], rot[4]; } TInst;                       /* 64 B; rot=cos,sin,pivotx,pivoty (identity 1,0,0,0) */
typedef struct { int kind; int start, count; SDL_Rect sc; int full; void *tex; } Run;  /* kind 0=shape 1=glyph/image 2=line 3=geom; tex=atlas or image */

/* ---- module state ---- */
static SDL_GPUDevice           *g_dev = NULL;
static SDL_Window              *g_win = NULL;
static SDL_GPUGraphicsPipeline *g_sdf = NULL, *g_txt = NULL;
static SDL_GPUShader           *g_svs=NULL,*g_sfs=NULL,*g_tvs=NULL,*g_tfs=NULL;
static SDL_GPUTexture          *g_atlas = NULL;
static SDL_GPUSampler          *g_samp  = NULL;
static SDL_GPUSampler          *g_samp_lin = NULL;   /* linear filter for stretched images */
static int   g_aw = 0, g_ah = 0;
static bool  g_ready = false;

/* GPU images (instrument icons, spectrogram, etc. — anything drawn via SDL_RenderTexture) */
typedef struct { SDL_GPUTexture *tex; int w,h; uint8_t mr,mg,mb,ma; int is_target; } GImage;
static GImage **g_imgs=NULL; static int g_nimg=0, g_capimg=0;
static SDL_GPUTextureFormat g_pipefmt;   /* colour-target format of all pipelines (= swapchain) */
/* render-to-texture (SDL_SetRenderTarget): a sub-batch renders to an offscreen GImage */
static GImage *g_target=NULL;
static int g_ts_nsh,g_ts_ngl,g_ts_nlv,g_ts_ngv,g_ts_nrun,g_ts_cur,g_ts_clipd,g_ts_hassdlclip,g_ts_lastfill;
static void upload_batch(SDL_GPUCommandBuffer*cmd);
static void replay_range(SDL_GPUCommandBuffer*cmd,SDL_GPURenderPass*rp,int W,int H,int run0,int run1);

static SInst *g_sh=NULL; static int g_nsh=0, g_capsh=0;
static TInst *g_gl=NULL; static int g_ngl=0, g_capgl=0;
static Run   *g_run=NULL; static int g_nrun=0, g_caprun=0;
static int    g_cur = -1;                 /* index of the open run, or -1 */
static int    g_last_fill = -1;           /* shape index of the most recent fill (for outline merge) */
static float  g_lf_x, g_lf_y, g_lf_w, g_lf_h, g_lf_r;

/* clip stack (screen px, top-left) */
static SDL_Rect g_clip[32]; static int g_clipd = 0;
static uint8_t g_dcr=255,g_dcg=255,g_dcb=255,g_dca=255;   /* tracked SDL draw colour */
static SDL_Rect g_sdlclip; static int g_hassdlclip=0;      /* SDL_SetRenderClipRect state */

/* GPU vertex buffers (grow on demand) */
static SDL_GPUBuffer *g_shbuf=NULL; static Uint32 g_shcap=0;
static SDL_GPUBuffer *g_glbuf=NULL; static Uint32 g_glcap=0;
static SDL_GPUTransferBuffer *g_shtb=NULL; static Uint32 g_shtbcap=0;
static SDL_GPUTransferBuffer *g_gltb=NULL; static Uint32 g_gltbcap=0;

/* polyline ribbon: connected draw_aa_line segments are coalesced into one
   continuous stroke (uniform width, no per-joint conflation). */
static SDL_GPUGraphicsPipeline *g_line=NULL; static SDL_GPUShader *g_lvs=NULL,*g_lfs=NULL;
typedef struct { float pos[2]; float dist, halfw; float col[4]; } LVert;   /* 32 B */
static LVert *g_lv=NULL; static int g_nlv=0, g_caplv=0;
static SDL_GPUBuffer *g_lvbuf=NULL; static Uint32 g_lvcap=0;
static SDL_GPUTransferBuffer *g_lvtb=NULL; static Uint32 g_lvtbcap=0;
#define MAX_PL 65536
static float g_plx[MAX_PL], g_ply[MAX_PL]; static int g_pln=0;   /* current polyline points */
static float g_plhw=0; static uint8_t g_plr,g_plg,g_plb,g_pla;

/* colored-triangle geometry (ported SDL_RenderGeometry); reuses the LVert layout (pos.xy + col) */
static SDL_GPUGraphicsPipeline *g_geom=NULL; static SDL_GPUShader *g_gvs=NULL,*g_gfs=NULL;
static LVert *g_gv=NULL; static int g_ngv=0, g_capgv=0;
static SDL_GPUBuffer *g_gvbuf=NULL; static Uint32 g_gvcap=0;
static SDL_GPUTransferBuffer *g_gvtb=NULL; static Uint32 g_gvtbcap=0;

static void set4(float*p,float a,float b,float c,float d){p[0]=a;p[1]=b;p[2]=c;p[3]=d;}
static void col4(float*p,uint8_t r,uint8_t g,uint8_t b,uint8_t a){p[0]=r/255.0f;p[1]=g/255.0f;p[2]=b/255.0f;p[3]=a/255.0f;}

/* ---- batch plumbing ---- */
static bool rect_eq(const SDL_Rect*a,const SDL_Rect*b){return a->x==b->x&&a->y==b->y&&a->w==b->w&&a->h==b->h;}
static void cur_scissor(SDL_Rect *out, int *full) {
    if (g_clipd <= 0 && !g_hassdlclip) { *full = 1; out->x=out->y=out->w=out->h=0; return; }
    *full = 0;
    SDL_Rect r = (g_clipd>0) ? g_clip[g_clipd-1] : (SDL_Rect){ -100000, -100000, 300000, 300000 };
    if (g_hassdlclip) SDL_GetRectIntersection(&r, &g_sdlclip, &r);
    *out = r;
}
/* open/extend the run for (kind); returns the run to append into */
static bool route(int kind, int idx, void *tex) {
    SDL_Rect sc; int full; cur_scissor(&sc,&full);
    if (g_cur>=0 && g_run[g_cur].kind==kind && g_run[g_cur].full==full && g_run[g_cur].tex==tex &&
        (full || rect_eq(&g_run[g_cur].sc,&sc))) {
        g_run[g_cur].count++; return true;
    }
    if (g_nrun>=g_caprun) {
        int nc=g_caprun?g_caprun*2:256;
        Run *grown=realloc(g_run,(size_t)nc*sizeof(Run));
        if(!grown) return false;
        g_run=grown; g_caprun=nc;
    }
    g_run[g_nrun]=(Run){ .kind=kind,.start=idx,.count=1,.sc=sc,.full=full,.tex=tex };
    g_cur=g_nrun++;
    return true;
}
/* tessellate the current polyline into a ribbon (triangle strip) with miter
   joins + a per-vertex signed perpendicular distance for the AA, add a LINE run */
static void addrun_line(int vstart,int vcount){
    SDL_Rect sc; int full; cur_scissor(&sc,&full);
    if(g_nrun>=g_caprun){ int nc=g_caprun?g_caprun*2:256; Run*grown=realloc(g_run,(size_t)nc*sizeof(Run)); if(!grown)return; g_run=grown;g_caprun=nc; }
    g_run[g_nrun]=(Run){.kind=2,.start=vstart,.count=vcount,.sc=sc,.full=full};
    g_cur=-1; g_nrun++;
}
static void flush_line(void){
    if(g_pln<2){ g_pln=0; return; }
    float hw=g_plhw>0.5f?g_plhw:0.5f, ext=hw+0.75f;
    int need=g_nlv+2*g_pln;
    if(need>g_caplv){ int nc=g_caplv?g_caplv*2:4096; while(nc<need)nc*=2; LVert*grown=realloc(g_lv,(size_t)nc*sizeof(LVert)); if(!grown){g_pln=0;return;} g_lv=grown;g_caplv=nc; }
    int vstart=g_nlv;
    float cr=g_plr/255.0f,cg=g_plg/255.0f,cb=g_plb/255.0f,ca=g_pla/255.0f;
    for(int i=0;i<g_pln;i++){
        float d0x=0,d0y=0,d1x=0,d1y=0,l;
        if(i>0){ d0x=g_plx[i]-g_plx[i-1]; d0y=g_ply[i]-g_ply[i-1]; l=hypotf(d0x,d0y); if(l>1e-4f){d0x/=l;d0y/=l;} }
        if(i<g_pln-1){ d1x=g_plx[i+1]-g_plx[i]; d1y=g_ply[i+1]-g_ply[i]; l=hypotf(d1x,d1y); if(l>1e-4f){d1x/=l;d1y/=l;} }
        float mx=d0x+d1x,my=d0y+d1y; l=hypotf(mx,my);
        if(l<1e-4f){ mx=(i<g_pln-1)?d1x:d0x; my=(i<g_pln-1)?d1y:d0y; l=hypotf(mx,my); if(l<1e-4f)l=1; mx/=l;my/=l; } else { mx/=l;my/=l; }
        float nx=-my, ny=mx;                               /* averaged (miter) normal */
        float sdx=(i<g_pln-1)?d1x:d0x, sdy=(i<g_pln-1)?d1y:d0y;
        float dot=nx*(-sdy)+ny*sdx, scale=1.0f;
        if(fabsf(dot)>0.25f) scale=1.0f/fabsf(dot);
        if(scale>3.0f)scale=3.0f;
        float ox=nx*ext*scale, oy=ny*ext*scale;
        LVert*a=&g_lv[g_nlv++]; a->pos[0]=g_plx[i]+ox; a->pos[1]=g_ply[i]+oy; a->dist= ext; a->halfw=hw; a->col[0]=cr;a->col[1]=cg;a->col[2]=cb;a->col[3]=ca;
        LVert*b=&g_lv[g_nlv++]; b->pos[0]=g_plx[i]-ox; b->pos[1]=g_ply[i]-oy; b->dist=-ext; b->halfw=hw; b->col[0]=cr;b->col[1]=cg;b->col[2]=cb;b->col[3]=ca;
    }
    addrun_line(vstart, g_nlv-vstart);
    g_pln=0;
}

static SInst *push_shape(void) {
    flush_line();
    if (g_nsh>=g_capsh){ int nc=g_capsh?g_capsh*2:512; SInst*grown=realloc(g_sh,(size_t)nc*sizeof(SInst)); if(!grown)return NULL; g_sh=grown;g_capsh=nc; }
    int idx=g_nsh; if(!route(0,idx,NULL))return NULL; g_nsh++; SInst*q=&g_sh[idx]; *q=(SInst){0}; return q;
}
static TInst *push_glyph(void) {
    flush_line();
    if (g_ngl>=g_capgl){ int nc=g_capgl?g_capgl*2:1024; TInst*grown=realloc(g_gl,(size_t)nc*sizeof(TInst)); if(!grown)return NULL; g_gl=grown;g_capgl=nc; }
    int idx=g_ngl; if(!route(1,idx,g_atlas))return NULL; g_ngl++; TInst*q=&g_gl[idx]; *q=(TInst){0}; return q;
}

/* ---- public emit API ---- */
void gpu_atlas_size(int*w,int*h){ if(w)*w=g_aw; if(h)*h=g_ah; }
bool gpu_active(void){ return g_ready; }

void gpu_emit_rrect(float x,float y,float w,float h,float rad,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready||w<=0||h<=0) return;
    SInst*q=push_shape();
    if(!q)return;
    set4(q->box,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f); set4(q->parm,rad,0,0,0);
    col4(q->fill,r,g,b,a);
    g_last_fill=g_nsh-1; g_lf_x=x; g_lf_y=y; g_lf_w=w; g_lf_h=h; g_lf_r=rad;
}
void gpu_emit_note(float x,float y,float w,float h,float rad,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready||w<=0||h<=0) return;
    SInst*q=push_shape();
    if(!q)return;
    set4(q->box,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f); set4(q->parm,rad,0,3,0);   /* mode 3 = note (BL square) */
    col4(q->fill,r,g,b,a);
    g_last_fill=g_nsh-1; g_lf_x=x; g_lf_y=y; g_lf_w=w; g_lf_h=h; g_lf_r=rad;
}
void gpu_emit_rrect_grad(float x,float y,float w,float h,float rad,int horiz,
                         uint8_t tr,uint8_t tg,uint8_t tb,uint8_t ta,
                         uint8_t br,uint8_t bg,uint8_t bb,uint8_t ba){
    if(!g_ready||w<=0||h<=0) return;
    SInst*q=push_shape();
    if(!q)return;
    set4(q->box,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f); set4(q->parm,rad,0,0,horiz?2.0f:1.0f);
    col4(q->fill,tr,tg,tb,ta); col4(q->grad,br,bg,bb,ba);
    g_last_fill=-1;
}
void gpu_emit_rrect_outline(float x,float y,float w,float h,float rad,float bw,
                            uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready||w<=0||h<=0) return;
    if(bw<=0) bw=1.2f;
    /* merge into the immediately-preceding fill of the same rect → one silhouette */
    if(g_last_fill>=0 && g_last_fill==g_nsh-1 &&
       fabsf(g_lf_x-x)<0.5f&&fabsf(g_lf_y-y)<0.5f&&fabsf(g_lf_w-w)<0.5f&&fabsf(g_lf_h-h)<0.5f){
        SInst*q=&g_sh[g_last_fill]; q->parm[1]=bw; col4(q->bord,r,g,b,a); return;
    }
    /* border-only: transparent interior, single outer silhouette */
    SInst*q=push_shape();
    if(!q)return;
    set4(q->box,x+w*0.5f,y+h*0.5f,w*0.5f,h*0.5f); set4(q->parm,rad,bw,0,0);
    col4(q->fill,r,g,b,0); col4(q->bord,r,g,b,a);
    g_last_fill=-1;
}
void gpu_emit_arc(float cx,float cy,float radius,float sh,float a0,float a1,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready) return;
    SInst*q=push_shape();
    if(!q)return;
    set4(q->box,cx,cy,0,0); set4(q->parm,radius,0,1,0);
    col4(q->fill,r,g,b,a); set4(q->arc,a0,a1,sh,0);
    g_last_fill=-1;
}
void gpu_emit_seg(float x0,float y0,float x1,float y1,float ht,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready) return;
    /* coalesce connected same-style segments into one continuous polyline stroke */
    int cont = (g_pln>0 && g_plr==r && g_plg==g && g_plb==b && g_pla==a && g_plhw==ht &&
                fabsf(g_plx[g_pln-1]-x0)<0.6f && fabsf(g_ply[g_pln-1]-y0)<0.6f);
    if(cont){ if(g_pln<MAX_PL){ g_plx[g_pln]=x1; g_ply[g_pln]=y1; g_pln++; } }
    else { flush_line(); g_plx[0]=x0;g_ply[0]=y0; g_plx[1]=x1;g_ply[1]=y1; g_pln=2; g_plhw=ht; g_plr=r;g_plg=g;g_plb=b;g_pla=a; }
    if(g_pln>=MAX_PL){ float lx=g_plx[g_pln-1],ly=g_ply[g_pln-1]; flush_line(); g_plx[0]=lx;g_ply[0]=ly; g_pln=1; }
    g_last_fill=-1;
}
void gpu_emit_geometry(const SDL_Vertex *v, int nv, const int *idx, int ni){
    if(!g_ready||!v) return;
    flush_line();
    int count = idx ? ni : nv;
    if(count<3) return;
    if(g_ngv+count>g_capgv){ int nc=g_capgv?g_capgv*2:2048; while(nc<g_ngv+count)nc*=2; LVert*grown=realloc(g_gv,(size_t)nc*sizeof(LVert)); if(!grown)return; g_gv=grown;g_capgv=nc; }
    int vstart=g_ngv;
    for(int i=0;i<count;i++){
        const SDL_Vertex *s = &v[idx ? idx[i] : i];
        LVert *q = &g_gv[g_ngv++];
        q->pos[0]=s->position.x; q->pos[1]=s->position.y; q->dist=0; q->halfw=0;
        q->col[0]=s->color.r; q->col[1]=s->color.g; q->col[2]=s->color.b; q->col[3]=s->color.a;
    }
    SDL_Rect sc; int full; cur_scissor(&sc,&full);
    if(g_nrun>=g_caprun){ int nc=g_caprun?g_caprun*2:256; Run*grown=realloc(g_run,(size_t)nc*sizeof(Run)); if(!grown){g_ngv=vstart;return;} g_run=grown;g_caprun=nc; }
    g_run[g_nrun]=(Run){.kind=3,.start=vstart,.count=g_ngv-vstart,.sc=sc,.full=full};
    g_cur=-1; g_nrun++; g_last_fill=-1;
}
void gpu_emit_glyph(float dx,float dy,float dw,float dh,float sx,float sy,float sw,float sh,
                    uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    if(!g_ready||g_aw<=0||dw<=0||dh<=0) return;
    TInst*q=push_glyph();
    if(!q)return;
    set4(q->dst,dx,dy,dw,dh);
    set4(q->uv,sx/g_aw,sy/g_ah,(sx+sw)/g_aw,(sy+sh)/g_ah);
    col4(q->col,r,g,b,a);
    set4(q->rot,1,0,0,0);
    g_last_fill=-1;
}
void *gpu_image_from_surface(SDL_Surface *surf){
    if(!g_ready||!surf) return NULL;
    SDL_Surface *rgba = (surf->format==SDL_PIXELFORMAT_RGBA32) ? surf : SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
    if(!rgba) return NULL;
    int w=rgba->w, h=rgba->h;
    SDL_GPUTexture *tex=SDL_CreateGPUTexture(g_dev,&(SDL_GPUTextureCreateInfo){.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER,.width=(Uint32)w,.height=(Uint32)h,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1});
    SDL_GPUTransferBuffer *tb=SDL_CreateGPUTransferBuffer(g_dev,&(SDL_GPUTransferBufferCreateInfo){.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=(Uint32)(w*h*4)});
    if(tex&&tb){
        Uint8 *m=SDL_MapGPUTransferBuffer(g_dev,tb,false);
        for(int y=0;y<h;y++) memcpy(m+(size_t)y*w*4, (Uint8*)rgba->pixels+(size_t)y*rgba->pitch, (size_t)w*4);
        SDL_UnmapGPUTransferBuffer(g_dev,tb);
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(g_dev);
        SDL_GPUCopyPass *cp=SDL_BeginGPUCopyPass(cmd);
        SDL_UploadToGPUTexture(cp,&(SDL_GPUTextureTransferInfo){.transfer_buffer=tb,.pixels_per_row=(Uint32)w,.rows_per_layer=(Uint32)h},&(SDL_GPUTextureRegion){.texture=tex,.w=(Uint32)w,.h=(Uint32)h,.d=1},false);
        SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
    }
    if(tb) SDL_ReleaseGPUTransferBuffer(g_dev,tb);
    if(rgba!=surf) SDL_DestroySurface(rgba);
    if(!tex) return NULL;
    GImage *im=calloc(1,sizeof(GImage));
    if(!im){SDL_ReleaseGPUTexture(g_dev,tex);return NULL;}
    im->tex=tex; im->w=w; im->h=h; im->mr=im->mg=im->mb=im->ma=255;
    if(g_nimg>=g_capimg){ int nc=g_capimg?g_capimg*2:64; GImage**grown=realloc(g_imgs,(size_t)nc*sizeof(GImage*)); if(!grown){SDL_ReleaseGPUTexture(g_dev,tex);free(im);return NULL;} g_imgs=grown;g_capimg=nc; }
    g_imgs[g_nimg++]=im;
    return im;
}
void gpu_image_set_mod(void *h,uint8_t r,uint8_t g,uint8_t b){ if(h){GImage*im=h;im->mr=r;im->mg=g;im->mb=b;} }
void gpu_image_set_alpha(void *h,uint8_t a){ if(h)((GImage*)h)->ma=a; }
void gpu_image_free(void *h){
    if(!h) return; GImage *im=(GImage*)h;
    for(int i=0;i<g_nimg;i++){ if(g_imgs[i]==im){ g_imgs[i]=g_imgs[--g_nimg]; break; } }
    if(im->tex && g_dev) SDL_ReleaseGPUTexture(g_dev,im->tex);
    free(im);
}
void gpu_emit_image(void *himg, float dx,float dy,float dw,float dh, int has_src,float sx,float sy,float sw,float sh){
    if(!g_ready||!himg||dw<=0||dh<=0) return;
    GImage *im=(GImage*)himg; if(!im->tex) return;
    flush_line();
    if(g_ngl>=g_capgl){ int nc=g_capgl?g_capgl*2:1024; TInst*grown=realloc(g_gl,(size_t)nc*sizeof(TInst)); if(!grown)return; g_gl=grown;g_capgl=nc; }
    int idx=g_ngl; if(!route(1,idx,im->tex))return; g_ngl++;
    TInst*q=&g_gl[idx]; *q=(TInst){0};
    set4(q->dst,dx,dy,dw,dh);
    if(has_src) set4(q->uv, sx/im->w, sy/im->h, (sx+sw)/im->w, (sy+sh)/im->h);
    else        set4(q->uv, 0,0,1,1);
    q->col[0]=im->mr/255.0f; q->col[1]=im->mg/255.0f; q->col[2]=im->mb/255.0f; q->col[3]=im->ma/255.0f;
    set4(q->rot,1,0,0,0);
    g_last_fill=-1;
}
void gpu_emit_image_rotated(void *himg, float dx,float dy,float dw,float dh, float angle_deg, float pivx,float pivy){
    if(!g_ready||!himg||dw<=0||dh<=0) return;
    GImage *im=(GImage*)himg; if(!im->tex) return;
    flush_line();
    if(g_ngl>=g_capgl){ int nc=g_capgl?g_capgl*2:1024; TInst*grown=realloc(g_gl,(size_t)nc*sizeof(TInst)); if(!grown)return; g_gl=grown;g_capgl=nc; }
    int idx=g_ngl; if(!route(1,idx,im->tex))return; g_ngl++;
    TInst*q=&g_gl[idx]; *q=(TInst){0};
    set4(q->dst,dx,dy,dw,dh);
    set4(q->uv,0,0,1,1);
    q->col[0]=im->mr/255.0f; q->col[1]=im->mg/255.0f; q->col[2]=im->mb/255.0f; q->col[3]=im->ma/255.0f;
    float a=angle_deg*(float)M_PI/180.0f;   /* SDL rotates clockwise in screen (y-down) space */
    set4(q->rot, cosf(a), sinf(a), pivx, pivy);
    g_last_fill=-1;
}
void *gpu_create_target(int w,int h){
    if(!g_ready||w<=0||h<=0) return NULL;
    SDL_GPUTexture *tex=SDL_CreateGPUTexture(g_dev,&(SDL_GPUTextureCreateInfo){.type=SDL_GPU_TEXTURETYPE_2D,.format=g_pipefmt,.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=(Uint32)w,.height=(Uint32)h,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1});
    if(!tex) return NULL;
    GImage *im=calloc(1,sizeof(GImage));
    if(!im){SDL_ReleaseGPUTexture(g_dev,tex);return NULL;}
    im->tex=tex; im->w=w; im->h=h; im->mr=im->mg=im->mb=im->ma=255; im->is_target=1;
    if(g_nimg>=g_capimg){ int nc=g_capimg?g_capimg*2:64; GImage**grown=realloc(g_imgs,(size_t)nc*sizeof(GImage*)); if(!grown){SDL_ReleaseGPUTexture(g_dev,tex);free(im);return NULL;} g_imgs=grown;g_capimg=nc; }
    g_imgs[g_nimg++]=im;
    return im;
}
void gpu_target_push(void *himg){
    if(!g_ready) return;
    flush_line();
    g_ts_nsh=g_nsh; g_ts_ngl=g_ngl; g_ts_nlv=g_nlv; g_ts_ngv=g_ngv; g_ts_nrun=g_nrun;
    g_ts_cur=g_cur; g_ts_clipd=g_clipd; g_ts_hassdlclip=g_hassdlclip; g_ts_lastfill=g_last_fill;
    g_target=(GImage*)himg;
    g_cur=-1; g_clipd=0; g_hassdlclip=0; g_last_fill=-1;   /* sub-batch draws in target-local space, no clip */
}
void gpu_target_pop(void){
    if(!g_ready) return;
    flush_line();
    GImage *tg=g_target; g_target=NULL;
    int r0=g_ts_nrun, r1=g_nrun;
    if(tg && tg->tex && r1>r0){
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(g_dev);
        upload_batch(cmd);
        SDL_GPUColorTargetInfo cti={.texture=tg->tex,.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE,.clear_color=(SDL_FColor){0,0,0,0}};
        SDL_GPURenderPass *rp=SDL_BeginGPURenderPass(cmd,&cti,1,NULL);
        replay_range(cmd,rp,tg->w,tg->h,r0,r1);
        SDL_EndGPURenderPass(rp);
        SDL_GPUFence *f=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if(f){ SDL_WaitForGPUFences(g_dev,true,&f,1); SDL_ReleaseGPUFence(g_dev,f); }
    }
    /* discard the sub-batch (already rendered to the texture) and restore the main batch */
    g_nsh=g_ts_nsh; g_ngl=g_ts_ngl; g_nlv=g_ts_nlv; g_ngv=g_ts_ngv; g_nrun=g_ts_nrun;
    g_cur=g_ts_cur; g_clipd=g_ts_clipd; g_hassdlclip=g_ts_hassdlclip; g_last_fill=g_ts_lastfill;
}
void gpu_push_clip(float x,float y,float w,float h){
    flush_line();
    SDL_Rect nr={(int)(x+0.5f),(int)(y+0.5f),(int)(w+0.5f),(int)(h+0.5f)};
    if(g_clipd>0){ SDL_Rect r; SDL_GetRectIntersection(&g_clip[g_clipd-1],&nr,&r); nr=r; }
    if(g_clipd<32) g_clip[g_clipd++]=nr;
    g_last_fill=-1;
}
void gpu_pop_clip(void){ flush_line(); if(g_clipd>0) g_clipd--; g_last_fill=-1; }

/* --- raw SDL_Render* routing (via gpu_sdl.h): use the tracked draw colour --- */
void gpu_set_draw_color(uint8_t r,uint8_t g,uint8_t b,uint8_t a){ g_dcr=r;g_dcg=g;g_dcb=b;g_dca=a; }
void gpu_raw_line(float x0,float y0,float x1,float y1){
    if(fabsf(x0-x1)<0.01f){ float y=fminf(y0,y1); gpu_emit_rrect(x0,y,1.0f,fabsf(y1-y0)+1.0f,0,g_dcr,g_dcg,g_dcb,g_dca); }      /* vertical: crisp */
    else if(fabsf(y0-y1)<0.01f){ float x=fminf(x0,x1); gpu_emit_rrect(x,y0,fabsf(x1-x0)+1.0f,1.0f,0,g_dcr,g_dcg,g_dcb,g_dca); } /* horizontal: crisp */
    else gpu_emit_seg(x0,y0,x1,y1,0.6f,g_dcr,g_dcg,g_dcb,g_dca);                                                                /* diagonal: AA ribbon */
}
void gpu_raw_fillrect(float x,float y,float w,float h){ gpu_emit_rrect(x,y,w,h,0,g_dcr,g_dcg,g_dcb,g_dca); }
void gpu_raw_rect(float x,float y,float w,float h){ gpu_emit_rrect_outline(x,y,w,h,0,1.0f,g_dcr,g_dcg,g_dcb,g_dca); }
void gpu_raw_point(float x,float y){ gpu_emit_rrect(x,y,1.0f,1.0f,0,g_dcr,g_dcg,g_dcb,g_dca); }
void gpu_set_clip(int x,int y,int w,int h){ flush_line(); g_sdlclip=(SDL_Rect){x,y,w,h}; g_hassdlclip=1; }
void gpu_clear_clip(void){ flush_line(); g_hassdlclip=0; }

/* ---- shader / atlas loading ---- */
static Uint8 *load_file(const char*name,size_t*n){
    char p[512];
    snprintf(p,sizeof p,"./Data/%s",name);
    FILE*f=fopen(p,"rb"); if(!f)return NULL;
    fseek(f,0,SEEK_END); long s=ftell(f); fseek(f,0,SEEK_SET);
    if(s<=0){fclose(f);return NULL;}
    Uint8*buf=malloc((size_t)s); size_t rd=fread(buf,1,(size_t)s,f); fclose(f);
    if(rd!=(size_t)s){free(buf);return NULL;}
    *n=(size_t)s; return buf;
}
static SDL_GPUShader *mkshader(const char*name,SDL_GPUShaderStage st,Uint32 nu,Uint32 ns){
    size_t n=0; Uint8*code=load_file(name,&n);
    if(!code){ SDL_Log("gpu_backend: shader %s not found",name); return NULL; }
    SDL_GPUShader*sh=SDL_CreateGPUShader(g_dev,&(SDL_GPUShaderCreateInfo){
        .code_size=n,.code=code,.entrypoint="main",.format=SDL_GPU_SHADERFORMAT_SPIRV,
        .stage=st,.num_uniform_buffers=nu,.num_samplers=ns});
    free(code); return sh;
}
static SDL_GPUGraphicsPipeline *mkpipe(SDL_GPUShader*vs,SDL_GPUShader*fs,int nattr,SDL_GPUTextureFormat fmt,SDL_GPUVertexInputRate rate,SDL_GPUPrimitiveType prim){
    SDL_GPUColorTargetBlendState bl={.enable_blend=true,
        .src_color_blendfactor=SDL_GPU_BLENDFACTOR_SRC_ALPHA,.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,.color_blend_op=SDL_GPU_BLENDOP_ADD,
        .src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE,.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,.alpha_blend_op=SDL_GPU_BLENDOP_ADD};
    SDL_GPUColorTargetDescription ctd={.format=fmt,.blend_state=bl};
    SDL_GPUVertexBufferDescription vbd={.slot=0,.pitch=(Uint32)(nattr*16),.input_rate=rate};
    SDL_GPUVertexAttribute at[6];
    for(int i=0;i<nattr;i++) at[i]=(SDL_GPUVertexAttribute){.location=(Uint32)i,.buffer_slot=0,.format=SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,.offset=(Uint32)(i*16)};
    return SDL_CreateGPUGraphicsPipeline(g_dev,&(SDL_GPUGraphicsPipelineCreateInfo){
        .vertex_shader=vs,.fragment_shader=fs,
        .vertex_input_state={.vertex_buffer_descriptions=&vbd,.num_vertex_buffers=1,.vertex_attributes=at,.num_vertex_attributes=(Uint32)nattr},
        .primitive_type=prim,
        .rasterizer_state={.fill_mode=SDL_GPU_FILLMODE_FILL,.cull_mode=SDL_GPU_CULLMODE_NONE},
        .multisample_state={.sample_count=SDL_GPU_SAMPLECOUNT_1},
        .target_info={.color_target_descriptions=&ctd,.num_color_targets=1}});
}
static void load_atlas(void){
    size_t n=0; Uint8*png=load_file("font_atlas.png",&n);
    if(!png){ SDL_Log("gpu_backend: font_atlas.png not found — text disabled"); return; }
    int w=0,h=0,ch=0; Uint8*px=stbi_load_from_memory(png,(int)n,&w,&h,&ch,4);
    free(png);
    if(!px){ SDL_Log("gpu_backend: atlas decode failed"); return; }
    g_aw=w; g_ah=h;
    g_atlas=SDL_CreateGPUTexture(g_dev,&(SDL_GPUTextureCreateInfo){.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER,.width=(Uint32)w,.height=(Uint32)h,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1});
    Uint32 bytes=(Uint32)(w*h*4);
    SDL_GPUTransferBuffer*tb=SDL_CreateGPUTransferBuffer(g_dev,&(SDL_GPUTransferBufferCreateInfo){.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=bytes});
    memcpy(SDL_MapGPUTransferBuffer(g_dev,tb,false),px,bytes); SDL_UnmapGPUTransferBuffer(g_dev,tb);
    stbi_image_free(px);
    SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
    SDL_UploadToGPUTexture(cp,&(SDL_GPUTextureTransferInfo){.transfer_buffer=tb,.pixels_per_row=(Uint32)w,.rows_per_layer=(Uint32)h},&(SDL_GPUTextureRegion){.texture=g_atlas,.w=(Uint32)w,.h=(Uint32)h,.d=1},false);
    SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(g_dev,tb);
    g_samp=SDL_CreateGPUSampler(g_dev,&(SDL_GPUSamplerCreateInfo){.min_filter=SDL_GPU_FILTER_NEAREST,.mag_filter=SDL_GPU_FILTER_NEAREST,.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE});
    /* linear sampler for stretched images (icons, CQT spectrogram) — matches the old SDL_SCALEMODE_LINEAR */
    g_samp_lin=SDL_CreateGPUSampler(g_dev,&(SDL_GPUSamplerCreateInfo){.min_filter=SDL_GPU_FILTER_LINEAR,.mag_filter=SDL_GPU_FILTER_LINEAR,.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE});
    SDL_Log("gpu_backend: atlas %dx%d loaded",w,h);
}

bool gpu_init(struct SDL_Window*window){
    g_win=(SDL_Window*)window;
    if(!g_win){ SDL_Log("gpu_backend: NULL window"); return false; }
    g_dev=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,true,NULL);
    if(!g_dev){ SDL_Log("gpu_backend: CreateGPUDevice: %s",SDL_GetError()); return false; }
    if(!SDL_ClaimWindowForGPUDevice(g_dev,g_win)){ SDL_Log("gpu_backend: ClaimWindow: %s",SDL_GetError()); gpu_shutdown(); return false; }
    SDL_GPUTextureFormat fmt=SDL_GetGPUSwapchainTextureFormat(g_dev,g_win);
    if(fmt==SDL_GPU_TEXTUREFORMAT_INVALID){ SDL_Log("gpu_backend: bad swapchain fmt"); gpu_shutdown(); return false; }
    g_pipefmt=fmt;
    g_svs=mkshader("inst.vert.spv",SDL_GPU_SHADERSTAGE_VERTEX,1,0);
    g_sfs=mkshader("inst.frag.spv",SDL_GPU_SHADERSTAGE_FRAGMENT,0,0);
    g_tvs=mkshader("text.vert.spv",SDL_GPU_SHADERSTAGE_VERTEX,1,0);
    g_tfs=mkshader("text.frag.spv",SDL_GPU_SHADERSTAGE_FRAGMENT,0,1);
    if(!g_svs||!g_sfs||!g_tvs||!g_tfs){ gpu_shutdown(); return false; }
    g_sdf=mkpipe(g_svs,g_sfs,6,fmt,SDL_GPU_VERTEXINPUTRATE_INSTANCE,SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP);
    g_txt=mkpipe(g_tvs,g_tfs,4,fmt,SDL_GPU_VERTEXINPUTRATE_INSTANCE,SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP);
    g_lvs=mkshader("line.vert.spv",SDL_GPU_SHADERSTAGE_VERTEX,1,0);
    g_lfs=mkshader("line.frag.spv",SDL_GPU_SHADERSTAGE_FRAGMENT,0,0);
    if(g_lvs&&g_lfs) g_line=mkpipe(g_lvs,g_lfs,2,fmt,SDL_GPU_VERTEXINPUTRATE_VERTEX,SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP);
    g_gvs=mkshader("geom.vert.spv",SDL_GPU_SHADERSTAGE_VERTEX,1,0);
    g_gfs=mkshader("geom.frag.spv",SDL_GPU_SHADERSTAGE_FRAGMENT,0,0);
    if(g_gvs&&g_gfs) g_geom=mkpipe(g_gvs,g_gfs,2,fmt,SDL_GPU_VERTEXINPUTRATE_VERTEX,SDL_GPU_PRIMITIVETYPE_TRIANGLELIST);
    if(!g_sdf||!g_txt||!g_line||!g_geom){ SDL_Log("gpu_backend: pipeline: %s",SDL_GetError()); gpu_shutdown(); return false; }
    load_atlas();
    g_ready=true;
    SDL_Log("gpu_backend: ready (swapchain fmt %d)",(int)fmt);
    return true;
}

/* ---- frame ---- */
void gpu_frame_begin(void){
    g_nsh=g_ngl=g_nrun=0; g_nlv=0; g_ngv=0; g_pln=0; g_cur=-1; g_last_fill=-1; g_clipd=0; g_hassdlclip=0; g_target=NULL;
}
static void ensure_buf(SDL_GPUBuffer**buf,Uint32*cap,Uint32 need){
    if(need<=*cap && *buf) return;
    Uint32 nc=*cap?*cap:4096; while(nc<need) nc*=2;
    if(*buf) SDL_ReleaseGPUBuffer(g_dev,*buf);
    *buf=SDL_CreateGPUBuffer(g_dev,&(SDL_GPUBufferCreateInfo){.usage=SDL_GPU_BUFFERUSAGE_VERTEX,.size=nc}); *cap=nc;
}
static void ensure_tb(SDL_GPUTransferBuffer**tb,Uint32*cap,Uint32 need){
    if(need<=*cap && *tb) return;
    Uint32 nc=*cap?*cap:4096; while(nc<need) nc*=2;
    if(*tb) SDL_ReleaseGPUTransferBuffer(g_dev,*tb);
    *tb=SDL_CreateGPUTransferBuffer(g_dev,&(SDL_GPUTransferBufferCreateInfo){.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=nc}); *cap=nc;
}
/* upload the batch (call inside/around a copy pass on `cmd`) */
static void upload_batch(SDL_GPUCommandBuffer*cmd){
    flush_line();   /* commit any pending polyline into g_lv + a run */
    Uint32 sb=(Uint32)(g_nsh*sizeof(SInst)), gb=(Uint32)(g_ngl*sizeof(TInst)), lb=(Uint32)(g_nlv*sizeof(LVert));
    SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
    if(sb){ ensure_buf(&g_shbuf,&g_shcap,sb); ensure_tb(&g_shtb,&g_shtbcap,sb);
        memcpy(SDL_MapGPUTransferBuffer(g_dev,g_shtb,true),g_sh,sb); SDL_UnmapGPUTransferBuffer(g_dev,g_shtb);
        SDL_UploadToGPUBuffer(cp,&(SDL_GPUTransferBufferLocation){.transfer_buffer=g_shtb},&(SDL_GPUBufferRegion){.buffer=g_shbuf,.size=sb},true); }
    if(gb){ ensure_buf(&g_glbuf,&g_glcap,gb); ensure_tb(&g_gltb,&g_gltbcap,gb);
        memcpy(SDL_MapGPUTransferBuffer(g_dev,g_gltb,true),g_gl,gb); SDL_UnmapGPUTransferBuffer(g_dev,g_gltb);
        SDL_UploadToGPUBuffer(cp,&(SDL_GPUTransferBufferLocation){.transfer_buffer=g_gltb},&(SDL_GPUBufferRegion){.buffer=g_glbuf,.size=gb},true); }
    if(lb){ ensure_buf(&g_lvbuf,&g_lvcap,lb); ensure_tb(&g_lvtb,&g_lvtbcap,lb);
        memcpy(SDL_MapGPUTransferBuffer(g_dev,g_lvtb,true),g_lv,lb); SDL_UnmapGPUTransferBuffer(g_dev,g_lvtb);
        SDL_UploadToGPUBuffer(cp,&(SDL_GPUTransferBufferLocation){.transfer_buffer=g_lvtb},&(SDL_GPUBufferRegion){.buffer=g_lvbuf,.size=lb},true); }
    Uint32 vb=(Uint32)(g_ngv*sizeof(LVert));
    if(vb){ ensure_buf(&g_gvbuf,&g_gvcap,vb); ensure_tb(&g_gvtb,&g_gvtbcap,vb);
        memcpy(SDL_MapGPUTransferBuffer(g_dev,g_gvtb,true),g_gv,vb); SDL_UnmapGPUTransferBuffer(g_dev,g_gvtb);
        SDL_UploadToGPUBuffer(cp,&(SDL_GPUTransferBufferLocation){.transfer_buffer=g_gvtb},&(SDL_GPUBufferRegion){.buffer=g_gvbuf,.size=vb},true); }
    SDL_EndGPUCopyPass(cp);
}
/* replay runs into an active render pass */
static void replay_range(SDL_GPUCommandBuffer*cmd,SDL_GPURenderPass*rp,int W,int H,int run0,int run1){
    float inv[2]={2.0f/(float)W,2.0f/(float)H};
    SDL_PushGPUVertexUniformData(cmd,0,inv,sizeof inv);
    int pipe=-1; SDL_GPUTexture *btex=NULL;
    for(int i=run0;i<run1;i++){
        Run*r=&g_run[i];
        SDL_Rect sc = r->full ? (SDL_Rect){0,0,W,H} : r->sc;
        if(sc.x<0){sc.w+=sc.x;sc.x=0;} if(sc.y<0){sc.h+=sc.y;sc.y=0;}
        if(sc.x>W)sc.x=W; if(sc.y>H)sc.y=H;
        if(sc.x+sc.w>W)sc.w=W-sc.x; if(sc.y+sc.h>H)sc.h=H-sc.y;
        if(sc.w<0)sc.w=0; if(sc.h<0)sc.h=0;
        SDL_SetGPUScissor(rp,&sc);
        if(r->kind==0){
            if(pipe!=0){ SDL_BindGPUGraphicsPipeline(rp,g_sdf); SDL_BindGPUVertexBuffers(rp,0,&(SDL_GPUBufferBinding){.buffer=g_shbuf},1); pipe=0; }
            SDL_DrawGPUPrimitives(rp,4,r->count,0,r->start);
        } else if(r->kind==2){
            if(pipe!=2){ SDL_BindGPUGraphicsPipeline(rp,g_line); SDL_BindGPUVertexBuffers(rp,0,&(SDL_GPUBufferBinding){.buffer=g_lvbuf},1); pipe=2; }
            SDL_DrawGPUPrimitives(rp,r->count,1,r->start,0);   /* triangle strip, non-instanced */
        } else if(r->kind==3){
            if(pipe!=3){ SDL_BindGPUGraphicsPipeline(rp,g_geom); SDL_BindGPUVertexBuffers(rp,0,&(SDL_GPUBufferBinding){.buffer=g_gvbuf},1); pipe=3; }
            SDL_DrawGPUPrimitives(rp,r->count,1,r->start,0);   /* triangle list */
        } else {
            SDL_GPUTexture *tx=(SDL_GPUTexture*)r->tex; if(!tx||!g_samp) continue;
            if(pipe!=1){ SDL_BindGPUGraphicsPipeline(rp,g_txt); SDL_BindGPUVertexBuffers(rp,0,&(SDL_GPUBufferBinding){.buffer=g_glbuf},1); pipe=1; btex=NULL; }
            if(tx!=btex){ SDL_GPUSampler *smp=(tx==g_atlas||!g_samp_lin)?g_samp:g_samp_lin;   /* atlas=crisp text, images=linear */
                SDL_BindGPUFragmentSamplers(rp,0,&(SDL_GPUTextureSamplerBinding){.texture=tx,.sampler=smp},1); btex=tx; }
            SDL_DrawGPUPrimitives(rp,4,r->count,0,r->start);
        }
    }
}
static void replay(SDL_GPUCommandBuffer*cmd,SDL_GPURenderPass*rp,int W,int H){ replay_range(cmd,rp,W,H,0,g_nrun); }
/* offscreen render of the current batch → BMP, for MUSE_GPU_SHOT verification */
static void shot(const char*path){
    int W=0,H=0; SDL_GetWindowSizeInPixels(g_win,&W,&H);
    const char*ew=SDL_getenv("MUSE_GPU_W"),*eh=SDL_getenv("MUSE_GPU_H");
    if(ew&&*ew)W=atoi(ew); if(eh&&*eh)H=atoi(eh);
    if(W<=0||H<=0){W=1400;H=800;}
    SDL_GPUTextureFormat fmt=SDL_GetGPUSwapchainTextureFormat(g_dev,g_win);
    SDL_GPUTexture*tex=SDL_CreateGPUTexture(g_dev,&(SDL_GPUTextureCreateInfo){.type=SDL_GPU_TEXTURETYPE_2D,.format=fmt,.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=(Uint32)W,.height=(Uint32)H,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1});
    SDL_GPUTransferBuffer*down=SDL_CreateGPUTransferBuffer(g_dev,&(SDL_GPUTransferBufferCreateInfo){.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=(Uint32)(W*H*4)});
    SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(g_dev);
    upload_batch(cmd);
    SDL_GPUColorTargetInfo cti={.texture=tex,.clear_color={FR(BG_HEX),FG(BG_HEX),FB(BG_HEX),1},.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass*rp=SDL_BeginGPURenderPass(cmd,&cti,1,NULL);
    replay(cmd,rp,W,H); SDL_EndGPURenderPass(rp);
    SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(cp,&(SDL_GPUTextureRegion){.texture=tex,.w=(Uint32)W,.h=(Uint32)H,.d=1},&(SDL_GPUTextureTransferInfo){.transfer_buffer=down,.pixels_per_row=(Uint32)W,.rows_per_layer=(Uint32)H});
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence*fe=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_dev,true,&fe,1); SDL_ReleaseGPUFence(g_dev,fe);
    void*px=SDL_MapGPUTransferBuffer(g_dev,down,false);
    SDL_PixelFormat pf=(fmt==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)?SDL_PIXELFORMAT_BGRA32:SDL_PIXELFORMAT_RGBA32;
    SDL_Surface*s=SDL_CreateSurfaceFrom(W,H,pf,px,W*4); SDL_SaveBMP(s,path); SDL_DestroySurface(s);
    SDL_UnmapGPUTransferBuffer(g_dev,down); SDL_ReleaseGPUTransferBuffer(g_dev,down); SDL_ReleaseGPUTexture(g_dev,tex);
    SDL_Log("gpu_backend: shot %dx%d (%d shapes, %d glyphs, %d lineV, %d geomV, %d runs) -> %s",W,H,g_nsh,g_ngl,g_nlv,g_ngv,g_nrun,path);
}
/* Render the current batch to an offscreen texture + download → a new surface
   (caller frees). Used by the recorder/screenshots. Call before gpu_frame_end. */
struct SDL_Surface *gpu_grab_surface(void){
    if(!g_ready) return NULL;
    int W=0,H=0; SDL_GetWindowSizeInPixels(g_win,&W,&H); if(W<=0||H<=0) return NULL;
    SDL_GPUTextureFormat fmt=SDL_GetGPUSwapchainTextureFormat(g_dev,g_win);
    SDL_GPUTexture*tex=SDL_CreateGPUTexture(g_dev,&(SDL_GPUTextureCreateInfo){.type=SDL_GPU_TEXTURETYPE_2D,.format=fmt,.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=(Uint32)W,.height=(Uint32)H,.layer_count_or_depth=1,.num_levels=1,.sample_count=SDL_GPU_SAMPLECOUNT_1});
    SDL_GPUTransferBuffer*down=SDL_CreateGPUTransferBuffer(g_dev,&(SDL_GPUTransferBufferCreateInfo){.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=(Uint32)(W*H*4)});
    if(!tex||!down){ if(tex)SDL_ReleaseGPUTexture(g_dev,tex); if(down)SDL_ReleaseGPUTransferBuffer(g_dev,down); return NULL; }
    SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(g_dev);
    upload_batch(cmd);
    SDL_GPUColorTargetInfo cti={.texture=tex,.clear_color={FR(BG_HEX),FG(BG_HEX),FB(BG_HEX),1},.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass*rp=SDL_BeginGPURenderPass(cmd,&cti,1,NULL);
    replay(cmd,rp,W,H); SDL_EndGPURenderPass(rp);
    SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
    SDL_DownloadFromGPUTexture(cp,&(SDL_GPUTextureRegion){.texture=tex,.w=(Uint32)W,.h=(Uint32)H,.d=1},&(SDL_GPUTextureTransferInfo){.transfer_buffer=down,.pixels_per_row=(Uint32)W,.rows_per_layer=(Uint32)H});
    SDL_EndGPUCopyPass(cp);
    SDL_GPUFence*fe=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_WaitForGPUFences(g_dev,true,&fe,1); SDL_ReleaseGPUFence(g_dev,fe);
    void*px=SDL_MapGPUTransferBuffer(g_dev,down,false);
    SDL_PixelFormat pf=(fmt==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)?SDL_PIXELFORMAT_BGRA32:SDL_PIXELFORMAT_RGBA32;
    SDL_Surface*s=SDL_CreateSurface(W,H,pf);
    if(s&&px){ for(int y=0;y<H;y++) memcpy((Uint8*)s->pixels+(size_t)y*s->pitch,(Uint8*)px+(size_t)y*W*4,(size_t)W*4); }
    SDL_UnmapGPUTransferBuffer(g_dev,down);
    SDL_ReleaseGPUTransferBuffer(g_dev,down); SDL_ReleaseGPUTexture(g_dev,tex);
    return s;
}

void gpu_frame_end(void){
    if(!g_ready) return;
    const char*sp=SDL_getenv("MUSE_GPU_SHOT");
    if(sp&&*sp){ shot(sp); exit(0); }
    SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(g_dev);
    if(!cmd) return;
    SDL_GPUTexture*swap=NULL; Uint32 w=0,h=0;
    if(!SDL_WaitAndAcquireGPUSwapchainTexture(cmd,g_win,&swap,&w,&h)||!swap||!w||!h){ SDL_SubmitGPUCommandBuffer(cmd); return; }
    upload_batch(cmd);
    SDL_GPUColorTargetInfo cti={.texture=swap,.clear_color={FR(BG_HEX),FG(BG_HEX),FB(BG_HEX),1},.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass*rp=SDL_BeginGPURenderPass(cmd,&cti,1,NULL);
    replay(cmd,rp,(int)w,(int)h); SDL_EndGPURenderPass(rp);
    SDL_SubmitGPUCommandBuffer(cmd);
}

/* Phase-0 compat: a self-contained test scene via the emit API. */
void gpu_test_frame(void){
    gpu_frame_begin();
    gpu_emit_rrect(40,40,360,220,22,0x23,0x26,0x2E,255);
    gpu_emit_rrect_outline(40,40,360,220,22,1.6f,0x3A,0x3F,0x49,255);
    gpu_emit_rrect(150,105,140,52,13,0xE3,0xA6,0x4C,255);
    gpu_frame_end();
}

void gpu_shutdown(void){
    g_ready=false;
    if(g_dev){
        SDL_WaitForGPUIdle(g_dev);
        if(g_shbuf)SDL_ReleaseGPUBuffer(g_dev,g_shbuf); if(g_glbuf)SDL_ReleaseGPUBuffer(g_dev,g_glbuf); if(g_lvbuf)SDL_ReleaseGPUBuffer(g_dev,g_lvbuf); if(g_gvbuf)SDL_ReleaseGPUBuffer(g_dev,g_gvbuf);
        if(g_shtb)SDL_ReleaseGPUTransferBuffer(g_dev,g_shtb); if(g_gltb)SDL_ReleaseGPUTransferBuffer(g_dev,g_gltb); if(g_lvtb)SDL_ReleaseGPUTransferBuffer(g_dev,g_lvtb); if(g_gvtb)SDL_ReleaseGPUTransferBuffer(g_dev,g_gvtb);
        if(g_samp)SDL_ReleaseGPUSampler(g_dev,g_samp); if(g_samp_lin)SDL_ReleaseGPUSampler(g_dev,g_samp_lin); if(g_atlas)SDL_ReleaseGPUTexture(g_dev,g_atlas);
        for(int i=0;i<g_nimg;i++){ if(g_imgs[i]){ if(g_imgs[i]->tex)SDL_ReleaseGPUTexture(g_dev,g_imgs[i]->tex); free(g_imgs[i]); } }
        free(g_imgs); g_imgs=NULL; g_nimg=g_capimg=0;
        if(g_sdf)SDL_ReleaseGPUGraphicsPipeline(g_dev,g_sdf); if(g_txt)SDL_ReleaseGPUGraphicsPipeline(g_dev,g_txt); if(g_line)SDL_ReleaseGPUGraphicsPipeline(g_dev,g_line); if(g_geom)SDL_ReleaseGPUGraphicsPipeline(g_dev,g_geom);
        if(g_svs)SDL_ReleaseGPUShader(g_dev,g_svs); if(g_sfs)SDL_ReleaseGPUShader(g_dev,g_sfs);
        if(g_tvs)SDL_ReleaseGPUShader(g_dev,g_tvs); if(g_tfs)SDL_ReleaseGPUShader(g_dev,g_tfs);
        if(g_lvs)SDL_ReleaseGPUShader(g_dev,g_lvs); if(g_lfs)SDL_ReleaseGPUShader(g_dev,g_lfs);
        if(g_gvs)SDL_ReleaseGPUShader(g_dev,g_gvs); if(g_gfs)SDL_ReleaseGPUShader(g_dev,g_gfs);
        if(g_win)SDL_ReleaseWindowFromGPUDevice(g_dev,g_win);
        SDL_DestroyGPUDevice(g_dev); g_dev=NULL;
    }
    g_shbuf=g_glbuf=g_lvbuf=g_gvbuf=NULL; g_shcap=g_glcap=g_lvcap=g_gvcap=0; g_shtb=g_gltb=g_lvtb=g_gvtb=NULL; g_shtbcap=g_gltbcap=g_lvtbcap=g_gvtbcap=0;
    g_sdf=g_txt=g_line=g_geom=NULL; g_lvs=g_lfs=g_gvs=g_gfs=NULL; g_atlas=NULL; g_samp=NULL; g_samp_lin=NULL; g_win=NULL;
    free(g_sh);g_sh=NULL;g_capsh=0; free(g_gl);g_gl=NULL;g_capgl=0; free(g_run);g_run=NULL;g_caprun=0; free(g_lv);g_lv=NULL;g_caplv=0; free(g_gv);g_gv=NULL;g_capgv=0;
}

#endif /* MUSE_GPU */
