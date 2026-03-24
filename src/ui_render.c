#include "ui_render.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Pre-baked font atlas - glyphs in a PNG with binary metrics.
   No FreeType at runtime, just blitting from the atlas. */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static SDL_Renderer *g_renderer;
static SDL_Texture  *g_atlas_tex;    /* the single atlas texture */

/* Forward declarations for texture-cached circles and rings */
#define CIRCLE_CACHE_MAX 16
static struct {
    SDL_Texture *tex;
    int radius;
    int size;
} circle_cache[CIRCLE_CACHE_MAX];
static int circle_cache_count = 0;

#define RING_CACHE_MAX 16
static struct {
    SDL_Texture *tex;
    int radius;
    int size;
} ring_cache[RING_CACHE_MAX];
static int ring_cache_count = 0;

#define GRAD_CACHE_MAX 8
#define GRAD_TEX_W 64
static struct {
    SDL_Texture *tex;
    uint8_t r0, g0, b0, r1, g1, b1;
} grad_cache[GRAD_CACHE_MAX];
static int grad_cache_count = 0;

/* Aurora blob texture */
#define BLOB_TEX_SIZE 64
static SDL_Texture *g_blob_tex = NULL;

#define GLYPH_FIRST   32
#define GLYPH_COUNT   95
#define MAX_SIZES     16

typedef struct {
    uint16_t atlas_x, atlas_y;  /* position in atlas */
    uint16_t w, h;              /* glyph bitmap size */
    int16_t  bearing_x;         /* offset from pen to left edge */
    int16_t  bearing_y;         /* offset from top of line to top of glyph */
    uint16_t advance;           /* horizontal advance */
} AtlasGlyph;

typedef struct {
    int       px_size;
    int16_t   ascent, descent;
    AtlasGlyph glyphs[GLYPH_COUNT];
} AtlasSize;

static AtlasSize g_sizes[MAX_SIZES];
static int       g_num_sizes;
static float     g_ascent_ratio;

/* find atlas entry for a pixel size, or nearest match if exact isn't baked */
static AtlasSize *get_size(int px) {
    AtlasSize *best = NULL;
    int best_diff = 9999;
    for (int i = 0; i < g_num_sizes; i++) {
        int diff = abs(g_sizes[i].px_size - px);
        if (diff < best_diff) { best_diff = diff; best = &g_sizes[i]; }
    }
    return best;
}

bool text_init(SDL_Renderer *r, const char *font_path) {
    g_renderer = r;
    (void)font_path;  /* unused, we load from atlas files instead */

    /* Derive atlas paths from executable location:
       The atlas files are in the source dir, but we search relative paths */
    const char *atlas_dirs[] = {
        "./Data",  /* dist/release layout */
        ".",       /* cwd (dev) */
        "..",      /* build/ → src/ */
        "../src",  /* build/ → src/ */
    };

    char png_path[512], bin_path[512];
    FILE *mf = NULL;

    for (int i = 0; i < 4; i++) {
        snprintf(png_path, sizeof(png_path), "%s/font_atlas.png", atlas_dirs[i]);
        snprintf(bin_path, sizeof(bin_path), "%s/font_atlas.bin", atlas_dirs[i]);
        mf = fopen(bin_path, "rb");
        if (mf) break;
    }
    if (!mf) {
        SDL_Log("font_atlas.bin not found");
        return false;
    }

    /* Load metrics */
    uint8_t num_sizes;
    fread(&num_sizes, 1, 1, mf);
    if (num_sizes > MAX_SIZES) num_sizes = MAX_SIZES;
    g_num_sizes = num_sizes;

    for (int s = 0; s < num_sizes; s++) {
        uint8_t px_size, num_glyphs;
        int16_t ascent, descent;
        fread(&px_size, 1, 1, mf);
        fread(&ascent, 2, 1, mf);
        fread(&descent, 2, 1, mf);
        fread(&num_glyphs, 1, 1, mf);

        g_sizes[s].px_size = px_size;
        g_sizes[s].ascent = ascent;
        g_sizes[s].descent = descent;

        int count = (num_glyphs < GLYPH_COUNT) ? num_glyphs : GLYPH_COUNT;
        for (int g = 0; g < count; g++) {
            uint16_t ax, ay, w, h, adv;
            int16_t bx, by;
            fread(&ax, 2, 1, mf);
            fread(&ay, 2, 1, mf);
            fread(&w,  2, 1, mf);
            fread(&h,  2, 1, mf);
            fread(&bx, 2, 1, mf);
            fread(&by, 2, 1, mf);
            fread(&adv, 2, 1, mf);
            g_sizes[s].glyphs[g] = (AtlasGlyph){ ax, ay, w, h, bx, by, adv };
        }
        /* Skip any extra glyphs in file */
        for (int g = count; g < num_glyphs; g++)
            fseek(mf, 14, SEEK_CUR);
    }
    fclose(mf);

    /* Compute ascent ratio from first size for centering */
    if (g_num_sizes > 0) {
        AtlasSize *s0 = &g_sizes[0];
        g_ascent_ratio = (float)s0->ascent / (float)(s0->ascent + s0->descent);
    } else {
        g_ascent_ratio = 0.8f;
    }

    /* Load atlas PNG */
    int img_w, img_h, img_ch;
    uint8_t *pixels = stbi_load(png_path, &img_w, &img_h, &img_ch, 4);
    if (!pixels) {
        SDL_Log("Failed to load %s", png_path);
        return false;
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(img_w, img_h,
                            SDL_PIXELFORMAT_RGBA32, pixels, img_w * 4);
    g_atlas_tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(pixels);

    if (!g_atlas_tex) {
        SDL_Log("Failed to create atlas texture");
        return false;
    }
    SDL_SetTextureBlendMode(g_atlas_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_atlas_tex, SDL_SCALEMODE_NEAREST);

    /* Generate aurora blob texture */
    {
        uint8_t *bpx = (uint8_t *)calloc(BLOB_TEX_SIZE * BLOB_TEX_SIZE * 4, 1);
        if (bpx) {
            float ctr = BLOB_TEX_SIZE / 2.0f;
            for (int by = 0; by < BLOB_TEX_SIZE; by++) {
                for (int bx = 0; bx < BLOB_TEX_SIZE; bx++) {
                    float dx = (float)bx - ctr + 0.5f;
                    float dy = (float)by - ctr + 0.5f;
                    float d = sqrtf(dx*dx + dy*dy) / ctr;
                    float a = 1.0f - d;
                    if (a < 0) a = 0;
                    a = a * a * a;
                    int idx = (by * BLOB_TEX_SIZE + bx) * 4;
                    bpx[idx] = bpx[idx+1] = bpx[idx+2] = 255;
                    bpx[idx+3] = (uint8_t)(a * 255);
                }
            }
            SDL_Surface *bs = SDL_CreateSurfaceFrom(BLOB_TEX_SIZE, BLOB_TEX_SIZE,
                                    SDL_PIXELFORMAT_RGBA32, bpx, BLOB_TEX_SIZE * 4);
            if (bs) {
                g_blob_tex = SDL_CreateTextureFromSurface(r, bs);
                SDL_DestroySurface(bs);
                if (g_blob_tex) {
                    SDL_SetTextureBlendMode(g_blob_tex, SDL_BLENDMODE_ADD);
                    SDL_SetTextureScaleMode(g_blob_tex, SDL_SCALEMODE_LINEAR);
                }
            }
            free(bpx);
        }
    }

    return true;
}

void text_shutdown(void) {
    if (g_atlas_tex) { SDL_DestroyTexture(g_atlas_tex); g_atlas_tex = NULL; }
    g_num_sizes = 0;
    g_renderer = NULL;
    /* Free cached circle/ring textures */
    for (int i = 0; i < circle_cache_count; i++) {
        if (circle_cache[i].tex) SDL_DestroyTexture(circle_cache[i].tex);
    }
    circle_cache_count = 0;
    for (int i = 0; i < ring_cache_count; i++) {
        if (ring_cache[i].tex) SDL_DestroyTexture(ring_cache[i].tex);
    }
    ring_cache_count = 0;
    for (int i = 0; i < grad_cache_count; i++) {
        if (grad_cache[i].tex) SDL_DestroyTexture(grad_cache[i].tex);
    }
    grad_cache_count = 0;
    if (g_blob_tex) { SDL_DestroyTexture(g_blob_tex); g_blob_tex = NULL; }
}

/* --- text drawing --- */

static void draw_text_px(SDL_Renderer *r, const char *str,
                         float x, float y, int px_size,
                         uint8_t cr, uint8_t cg, uint8_t cb) {
    if (!str || !*str || !g_atlas_tex) return;
    AtlasSize *as = get_size(px_size);
    if (!as) return;

    SDL_SetTextureColorMod(g_atlas_tex, cr, cg, cb);

    int pen_x = (int)(x + 0.5f);
    int pen_y = (int)(y + 0.5f);

    const char *s = str;
    while (*s) {
        int ch = (unsigned char)*s++;
        if (ch < GLYPH_FIRST || ch >= GLYPH_FIRST + GLYPH_COUNT) {
            if ((ch & 0xC0) == 0xC0) {
                if (ch >= 0xF0)      { if (*s) s++; if (*s) s++; if (*s) s++; }
                else if (ch >= 0xE0) { if (*s) s++; if (*s) s++; }
                else                 { if (*s) s++; }
            }
            continue;
        }
        AtlasGlyph *g = &as->glyphs[ch - GLYPH_FIRST];
        if (g->w > 0 && g->h > 0) {
            SDL_FRect src = { (float)g->atlas_x, (float)g->atlas_y,
                              (float)g->w, (float)g->h };
            SDL_FRect dst = { (float)(pen_x + g->bearing_x),
                              (float)(pen_y + g->bearing_y),
                              (float)g->w, (float)g->h };
            SDL_RenderTexture(r, g_atlas_tex, &src, &dst);
        }
        pen_x += g->advance;
    }
}

static float text_width_px(const char *str, int px_size) {
    if (!str) return 0;
    AtlasSize *as = get_size(px_size);
    if (!as) return 0;
    int w = 0;
    while (*str) {
        int ch = (unsigned char)*str++;
        if (ch < GLYPH_FIRST || ch >= GLYPH_FIRST + GLYPH_COUNT) {
            if ((ch & 0xC0) == 0xC0) {
                if (ch >= 0xF0)      { if (*str) str++; if (*str) str++; if (*str) str++; }
                else if (ch >= 0xE0) { if (*str) str++; if (*str) str++; }
                else                 { if (*str) str++; }
            }
            continue;
        }
        w += as->glyphs[ch - GLYPH_FIRST].advance;
    }
    return (float)w;
}

/* --- public text API --- */

float text_width(const char *str, float size) {
    return text_width_px(str, (int)(size + 0.5f));
}

float text_line_height(float size) {
    AtlasSize *as = get_size((int)(size + 0.5f));
    return as ? (float)(as->ascent + as->descent) : size;
}

void draw_text(SDL_Renderer *r, const char *str, float x, float y,
               float size, uint8_t cr, uint8_t cg, uint8_t cb) {
    draw_text_px(r, str, x, y, (int)(size + 0.5f), cr, cg, cb);
}

void draw_text_bold(SDL_Renderer *r, const char *str, float x, float y,
                    float size, uint8_t cr, uint8_t cg, uint8_t cb) {
    int px = (int)(size + 0.5f);
    draw_text_px(r, str, x, y, px, cr, cg, cb);
    draw_text_px(r, str, x + 1.0f, y, px, cr, cg, cb);
}

void draw_text_right(SDL_Renderer *r, const char *str, float x, float y,
                     float size, uint8_t cr, uint8_t cg, uint8_t cb) {
    int px = (int)(size + 0.5f);
    float w = text_width_px(str, px);
    draw_text_px(r, str, x - w, y, px, cr, cg, cb);
}

void draw_text_centered(SDL_Renderer *r, const char *str,
                        float cx, float cy, float size,
                        uint8_t cr, uint8_t cg, uint8_t cb) {
    int px = (int)(size + 0.5f);
    float w = text_width_px(str, px);
    AtlasSize *as = get_size(px);
    float line_h = as ? (float)(as->ascent + as->descent) : (float)px;
    float y = cy - line_h * 0.5f;
    draw_text_px(r, str, cx - w * 0.5f, y, px, cr, cg, cb);
}

/* --- drawing primitives --- */

void draw_filled_rect(SDL_Renderer *r, float x, float y, float w, float h,
                      uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    if (ca < 0xFF) SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_FRect rc = { x, y, w, h };
    SDL_RenderFillRect(r, &rc);
}

/* Cached gradient textures - generate once, reuse forever */

static SDL_Texture *get_gradient_texture(SDL_Renderer *r,
        uint8_t r0, uint8_t g0, uint8_t b0,
        uint8_t r1, uint8_t g1, uint8_t b1) {
    for (int i = 0; i < grad_cache_count; i++) {
        if (grad_cache[i].r0 == r0 && grad_cache[i].g0 == g0 &&
            grad_cache[i].b0 == b0 && grad_cache[i].r1 == r1 &&
            grad_cache[i].g1 == g1 && grad_cache[i].b1 == b1)
            return grad_cache[i].tex;
    }

    uint8_t pixels[GRAD_TEX_W * 4];
    for (int i = 0; i < GRAD_TEX_W; i++) {
        float t = (float)i / (float)(GRAD_TEX_W - 1);
        pixels[i * 4 + 0] = (uint8_t)(r0 + (r1 - r0) * t);
        pixels[i * 4 + 1] = (uint8_t)(g0 + (g1 - g0) * t);
        pixels[i * 4 + 2] = (uint8_t)(b0 + (b1 - b0) * t);
        pixels[i * 4 + 3] = 255;
    }

    SDL_Surface *surf = SDL_CreateSurfaceFrom(GRAD_TEX_W, 1, SDL_PIXELFORMAT_RGBA32,
                                               pixels, GRAD_TEX_W * 4);
    SDL_Texture *tex = NULL;
    if (surf) {
        tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_DestroySurface(surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }

    if (tex) {
        if (grad_cache_count >= GRAD_CACHE_MAX) {
            /* Evict oldest entry to make room */
            SDL_DestroyTexture(grad_cache[0].tex);
            memmove(&grad_cache[0], &grad_cache[1],
                    (GRAD_CACHE_MAX - 1) * sizeof(grad_cache[0]));
            grad_cache_count = GRAD_CACHE_MAX - 1;
        }
        grad_cache[grad_cache_count].tex = tex;
        grad_cache[grad_cache_count].r0 = r0;
        grad_cache[grad_cache_count].g0 = g0;
        grad_cache[grad_cache_count].b0 = b0;
        grad_cache[grad_cache_count].r1 = r1;
        grad_cache[grad_cache_count].g1 = g1;
        grad_cache[grad_cache_count].b1 = b1;
        grad_cache_count++;
    }
    return tex;
}

void draw_hgradient_rect(SDL_Renderer *r, float x, float y, float w, float h,
                         uint8_t r0, uint8_t g0, uint8_t b0,
                         uint8_t r1, uint8_t g1, uint8_t b1) {
    SDL_Texture *tex = get_gradient_texture(r, r0, g0, b0, r1, g1, b1);
    if (tex) {
        SDL_FRect dst = { x, y, w, h };
        SDL_RenderTexture(r, tex, NULL, &dst);
    } else {
        /* Fallback: per-strip */
        int iw = (int)(w + 0.5f);
        if (iw < 1) iw = 1;
        for (int i = 0; i < iw; i++) {
            float t = (iw > 1) ? (float)i / (float)(iw - 1) : 0.0f;
            uint8_t cr = (uint8_t)(r0 + (r1 - r0) * t);
            uint8_t cg = (uint8_t)(g0 + (g1 - g0) * t);
            uint8_t cb = (uint8_t)(b0 + (b1 - b0) * t);
            SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
            SDL_FRect strip = { x + (float)i, y, 1.0f, h };
            SDL_RenderFillRect(r, &strip);
        }
    }
}

void draw_rect_outline(SDL_Renderer *r, float x, float y, float w, float h,
                       uint8_t cr, uint8_t cg, uint8_t cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    SDL_FRect rc = { x, y, w, h };
    SDL_RenderRect(r, &rc);
}

void draw_hline(SDL_Renderer *r, float x1, float x2, float y,
                uint8_t cr, uint8_t cg, uint8_t cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    SDL_RenderLine(r, x1, y, x2, y);
}

void draw_vline(SDL_Renderer *r, float x, float y1, float y2,
                uint8_t cr, uint8_t cg, uint8_t cb) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    SDL_RenderLine(r, x, y1, x, y2);
}

/* AA rounded rect - scanline coverage for smooth corners.
   This was annoying to get right but the result looks great. */

static void aa_rounded_rect_fill(SDL_Renderer *r, float x, float y, float w, float h,
                                 float rad, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    if (rad < 0.5f) rad = 0.5f;
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    float cx_l = x + rad;
    float cx_r = x + w - rad;
    float cy_t = y + rad;
    float cy_b = y + h - rad;

    int iy_top_start = (int)floorf(y);
    int iy_top_end   = (int)ceilf(cy_t);   /* first row past top corners */
    int iy_bot_start = (int)floorf(cy_b);   /* first bottom corner row */
    int iy_bot_end   = (int)ceilf(y + h);

    /* top corners */
    for (int iy = iy_top_start; iy < iy_top_end; iy++) {
        float fy = (float)iy + 0.5f;
        float dy = fy - cy_t;
        float d2 = rad * rad - dy * dy;
        if (d2 <= 0) continue;
        float dx = sqrtf(d2);
        float row_x0 = cx_l - dx;
        float row_x1 = cx_r + dx;

        float row_top_f = (float)iy;
        float row_bot_f = (float)(iy + 1);
        float v_cov = 1.0f;
        if (row_top_f < y) v_cov = row_bot_f - y;
        if (v_cov < 0) v_cov = 0;
        if (v_cov > 1) v_cov = 1;
        uint8_t row_a = (uint8_t)(ca * v_cov);

        float frac_left = row_x0 - floorf(row_x0);
        float frac_right = ceilf(row_x1) - row_x1;

        if (frac_left > 0.01f) {
            uint8_t edge_a = (uint8_t)(row_a * (1.0f - frac_left));
            SDL_SetRenderDrawColor(r, cr, cg, cb, edge_a);
            SDL_FRect px = { floorf(row_x0), (float)iy, 1, 1 };
            SDL_RenderFillRect(r, &px);
            row_x0 = floorf(row_x0) + 1;
        }
        if (frac_right > 0.01f) {
            uint8_t edge_a = (uint8_t)(row_a * (1.0f - frac_right));
            SDL_SetRenderDrawColor(r, cr, cg, cb, edge_a);
            SDL_FRect px = { floorf(row_x1), (float)iy, 1, 1 };
            SDL_RenderFillRect(r, &px);
            row_x1 = floorf(row_x1);
        }
        float interior_w = row_x1 - row_x0;
        if (interior_w > 0) {
            SDL_SetRenderDrawColor(r, cr, cg, cb, row_a);
            SDL_FRect row = { row_x0, (float)iy, interior_w, 1 };
            SDL_RenderFillRect(r, &row);
        }
    }

    /* middle - just one big rect, easy */
    {
        float mid_top = cy_t;
        float mid_bot = cy_b;
        if (mid_top < y) mid_top = y;
        if (mid_bot > y + h) mid_bot = y + h;
        float mid_h = mid_bot - mid_top;
        if (mid_h > 0) {
            SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
            SDL_FRect mid = { x, mid_top, w, mid_h };
            SDL_RenderFillRect(r, &mid);
        }
    }

    /* bottom corners */
    for (int iy = iy_bot_start; iy < iy_bot_end; iy++) {
        float fy = (float)iy + 0.5f;
        float dy = fy - cy_b;
        float d2 = rad * rad - dy * dy;
        if (d2 <= 0) continue;
        float dx = sqrtf(d2);
        float row_x0 = cx_l - dx;
        float row_x1 = cx_r + dx;

        float row_top_f = (float)iy;
        float row_bot_f = (float)(iy + 1);
        float v_cov = 1.0f;
        if (row_bot_f > y + h) v_cov = (y + h) - row_top_f;
        if (v_cov < 0) v_cov = 0;
        if (v_cov > 1) v_cov = 1;
        uint8_t row_a = (uint8_t)(ca * v_cov);

        float frac_left = row_x0 - floorf(row_x0);
        float frac_right = ceilf(row_x1) - row_x1;

        if (frac_left > 0.01f) {
            uint8_t edge_a = (uint8_t)(row_a * (1.0f - frac_left));
            SDL_SetRenderDrawColor(r, cr, cg, cb, edge_a);
            SDL_FRect px = { floorf(row_x0), (float)iy, 1, 1 };
            SDL_RenderFillRect(r, &px);
            row_x0 = floorf(row_x0) + 1;
        }
        if (frac_right > 0.01f) {
            uint8_t edge_a = (uint8_t)(row_a * (1.0f - frac_right));
            SDL_SetRenderDrawColor(r, cr, cg, cb, edge_a);
            SDL_FRect px = { floorf(row_x1), (float)iy, 1, 1 };
            SDL_RenderFillRect(r, &px);
            row_x1 = floorf(row_x1);
        }
        float interior_w = row_x1 - row_x0;
        if (interior_w > 0) {
            SDL_SetRenderDrawColor(r, cr, cg, cb, row_a);
            SDL_FRect row = { row_x0, (float)iy, interior_w, 1 };
            SDL_RenderFillRect(r, &row);
        }
    }
}

void draw_rounded_rect(SDL_Renderer *r, float x, float y, float w, float h,
                       float radius, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    if (radius < 1.0f) {
        /* No rounding, just a normal rect */
        draw_filled_rect(r, x, y, w, h, cr, cg, cb, ca);
        return;
    }
    aa_rounded_rect_fill(r, x, y, w, h, radius, cr, cg, cb, ca);
}

void draw_rounded_rect_outline(SDL_Renderer *r, float x, float y, float w, float h,
                               float radius, uint8_t cr, uint8_t cg, uint8_t cb) {
    if (radius < 1.0f) {
        draw_rect_outline(r, x, y, w, h, cr, cg, cb);
        return;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    float rad = radius;
    if (rad > w * 0.5f) rad = w * 0.5f;
    if (rad > h * 0.5f) rad = h * 0.5f;

    /* Straight edges */
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    SDL_RenderLine(r, x + rad, y, x + w - rad, y);                   /* top */
    SDL_RenderLine(r, x + rad, y + h - 1, x + w - rad, y + h - 1);  /* bottom */
    SDL_RenderLine(r, x, y + rad, x, y + h - 1 - rad);              /* left */
    SDL_RenderLine(r, x + w - 1, y + rad, x + w - 1, y + h - 1 - rad); /* right */

    /* corner arcs - oversampled for smooth AA */
    float cx_l = x + rad;
    float cx_r = x + w - 1 - rad;
    float cy_t = y + rad;
    float cy_b = y + h - 1 - rad;

    int steps = (int)(rad * 6);
    if (steps < 24) steps = 24;
    float step = (3.14159265f * 0.5f) / steps;

    for (int i = 0; i <= steps; i++) {
        float a = (float)i * step;
        float dx = cosf(a) * rad;
        float dy = sinf(a) * rad;

        float corners[4][2] = {
            { cx_l - dx, cy_t - dy },  /* top-left */
            { cx_r + dx, cy_t - dy },  /* top-right */
            { cx_l - dx, cy_b + dy },  /* bottom-left */
            { cx_r + dx, cy_b + dy },  /* bottom-right */
        };
        for (int c = 0; c < 4; c++) {
            float px = corners[c][0], py = corners[c][1];
            /* Coverage-based AA using fractional position */
            float fx = px - floorf(px);
            float fy = py - floorf(py);
            float coverage = (1.0f - fx) * (1.0f - fy);
            if (coverage < 0.1f) coverage = 0.1f;
            SDL_SetRenderDrawColor(r, cr, cg, cb, (uint8_t)(255.0f * coverage));
            SDL_RenderPoint(r, floorf(px), floorf(py));
            /* Adjacent pixel for AA */
            if (fx > 0.05f) {
                SDL_SetRenderDrawColor(r, cr, cg, cb, (uint8_t)(255.0f * fx * (1.0f - fy)));
                SDL_RenderPoint(r, floorf(px) + 1, floorf(py));
            }
            if (fy > 0.05f) {
                SDL_SetRenderDrawColor(r, cr, cg, cb, (uint8_t)(255.0f * (1.0f - fx) * fy));
                SDL_RenderPoint(r, floorf(px), floorf(py) + 1);
            }
        }
    }
}

/* Cached circle textures - 8x8 supersampled, then tinted at draw time.
   Way faster than drawing circles every frame. */

static SDL_Texture *get_circle_texture(SDL_Renderer *r, int irad) {
    /* Check cache */
    for (int i = 0; i < circle_cache_count; i++)
        if (circle_cache[i].radius == irad)
            return circle_cache[i].tex;

    /* Generate with 8x8 supersampling */
    int size = irad * 2 + 2;
    float center = size / 2.0f;
    float rad = (float)irad;
    uint8_t *pixels = (uint8_t *)calloc(size * size * 4, 1);
    if (!pixels) return NULL;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int count = 0;
            for (int sy = 0; sy < 8; sy++) {
                float fy = (float)y + (sy + 0.5f) / 8.0f - center;
                float fy2 = fy * fy;
                for (int sx = 0; sx < 8; sx++) {
                    float fx = (float)x + (sx + 0.5f) / 8.0f - center;
                    if (fx * fx + fy2 <= rad * rad) count++;
                }
            }
            if (count > 0) {
                int idx = (y * size + x) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = (uint8_t)(255 * count / 64);
            }
        }
    }

    SDL_Surface *surf = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32,
                                               pixels, size * 4);
    SDL_Texture *tex = NULL;
    if (surf) {
        tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_DestroySurface(surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }
    free(pixels);

    if (tex) {
        if (circle_cache_count >= CIRCLE_CACHE_MAX) {
            /* Evict oldest entry to make room */
            SDL_DestroyTexture(circle_cache[0].tex);
            memmove(&circle_cache[0], &circle_cache[1],
                    (CIRCLE_CACHE_MAX - 1) * sizeof(circle_cache[0]));
            circle_cache_count = CIRCLE_CACHE_MAX - 1;
        }
        circle_cache[circle_cache_count].tex = tex;
        circle_cache[circle_cache_count].radius = irad;
        circle_cache[circle_cache_count].size = size;
        circle_cache_count++;
    }
    return tex;
}

void draw_circle_filled(SDL_Renderer *r, float cx, float cy, float radius,
                        uint8_t cr, uint8_t cg, uint8_t cb) {
    int irad = (int)(radius + 0.5f);
    if (irad < 1) irad = 1;
    SDL_Texture *tex = get_circle_texture(r, irad);
    if (!tex) return;

    int size = irad * 2 + 2;
    SDL_SetTextureColorMod(tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(tex, 255);
    SDL_FRect dst = { cx - size / 2.0f, cy - size / 2.0f, (float)size, (float)size };
    SDL_RenderTexture(r, tex, NULL, &dst);
}

/* Same idea but for ring/outline circles */

static SDL_Texture *get_ring_texture(SDL_Renderer *r, int irad) {
    for (int i = 0; i < ring_cache_count; i++)
        if (ring_cache[i].radius == irad)
            return ring_cache[i].tex;

    int size = irad * 2 + 2;
    float center = size / 2.0f;
    float rad = (float)irad;
    float r2_outer = (rad + 0.5f) * (rad + 0.5f);
    float r2_inner = (rad - 0.5f) * (rad - 0.5f);
    uint8_t *pixels = (uint8_t *)calloc(size * size * 4, 1);
    if (!pixels) return NULL;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int count = 0;
            for (int sy = 0; sy < 8; sy++) {
                float fy = (float)y + (sy + 0.5f) / 8.0f - center;
                float fy2 = fy * fy;
                for (int sx = 0; sx < 8; sx++) {
                    float fx = (float)x + (sx + 0.5f) / 8.0f - center;
                    float d2 = fx * fx + fy2;
                    if (d2 <= r2_outer && d2 >= r2_inner) count++;
                }
            }
            if (count > 0) {
                int idx = (y * size + x) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = (uint8_t)(255 * count / 64);
            }
        }
    }

    SDL_Surface *surf = SDL_CreateSurfaceFrom(size, size, SDL_PIXELFORMAT_RGBA32,
                                               pixels, size * 4);
    SDL_Texture *tex = NULL;
    if (surf) {
        tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_DestroySurface(surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }
    free(pixels);

    if (tex) {
        if (ring_cache_count >= RING_CACHE_MAX) {
            /* Evict oldest entry to make room */
            SDL_DestroyTexture(ring_cache[0].tex);
            memmove(&ring_cache[0], &ring_cache[1],
                    (RING_CACHE_MAX - 1) * sizeof(ring_cache[0]));
            ring_cache_count = RING_CACHE_MAX - 1;
        }
        ring_cache[ring_cache_count].tex = tex;
        ring_cache[ring_cache_count].radius = irad;
        ring_cache[ring_cache_count].size = size;
        ring_cache_count++;
    }
    return tex;
}

void draw_circle_outline(SDL_Renderer *r, float cx, float cy, float radius,
                         uint8_t cr, uint8_t cg, uint8_t cb) {
    int irad = (int)(radius + 0.5f);
    if (irad < 1) irad = 1;
    SDL_Texture *tex = get_ring_texture(r, irad);
    if (!tex) return;

    int size = irad * 2 + 2;
    SDL_SetTextureColorMod(tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(tex, 255);
    SDL_FRect dst = { cx - size / 2.0f, cy - size / 2.0f, (float)size, (float)size };
    SDL_RenderTexture(r, tex, NULL, &dst);
}

bool ui_rect_contains(UiRect rc, float px, float py) {
    return px >= rc.x && px < rc.x + rc.w && py >= rc.y && py < rc.y + rc.h;
}

/* --- widgets --- */

void draw_dropdown_arrow(SDL_Renderer *r, float cx, float cy, float size,
                         uint8_t cr, uint8_t cg, uint8_t cb) {
    /* little down arrow, scanline-rendered for smoothness */
    float half_w = size * 0.3f;
    float half_h = size * 0.2f;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, cr, cg, cb, 0xFF);
    int steps = (int)(half_h * 8) + 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float dy = -half_h + t * (half_h * 2);
        float w = half_w * (1.0f - t);
        if (w < 0.3f) w = 0.3f;
        SDL_FRect strip = { cx - w, cy + dy, w * 2, 0.25f };
        SDL_RenderFillRect(r, &strip);
    }
}

void draw_ctk_button(SDL_Renderer *r, UiRect rc, const char *label,
                     float font_sz, bool hovered, bool active) {
    uint8_t br, bg, bb;
    if (active)       { br = 0xD8; bg = 0xAD; bb = 0x70; }
    else if (hovered) { br = 0x44; bg = 0x43; bb = 0x48; }
    else              { br = 0x31; bg = 0x32; bb = 0x39; }
    draw_rounded_rect(r, rc.x, rc.y, rc.w, rc.h, 6, br, bg, bb, 0xFF);
    uint8_t tr, tg, tb;
    if (active) { tr = 0x16; tg = 0x16; tb = 0x18; }
    else        { tr = 0xDD; tg = 0xC3; tb = 0x9E; }
    draw_text_centered(r, label, rc.x + rc.w / 2, rc.y + rc.h / 2,
                       font_sz, tr, tg, tb);
}

void draw_ctk_entry(SDL_Renderer *r, UiRect rc, const char *text,
                    float font_sz, bool focused) {
    /* Interior fill */
    draw_rounded_rect(r, rc.x, rc.y, rc.w, rc.h, 6, 0x31, 0x32, 0x39, 0xFF);
    /* 2px border, gold when focused */
    uint8_t br = focused ? 0xD8 : 0x44;
    uint8_t bg_c = focused ? 0xAD : 0x43;
    uint8_t bb = focused ? 0x70 : 0x48;
    draw_rounded_rect_outline(r, rc.x, rc.y, rc.w, rc.h, 6, br, bg_c, bb);
    draw_rounded_rect_outline(r, rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2, 5, br, bg_c, bb);
    if (text) {
        push_clip(r, rc.x + 6, rc.y, rc.w - 12, rc.h);
        /* Vertically center text in the entry */
        int px = (int)(font_sz + 0.5f);
        AtlasSize *as = get_size(px);
        float line_h = as ? (float)(as->ascent + as->descent) : (float)px;
        float ty = rc.y + (rc.h - line_h) * 0.5f;
        draw_text_px(r, text, rc.x + 8, ty, px, 0xE0, 0xE0, 0xE0);
        pop_clip(r);
    }
}

void draw_ctk_slider(SDL_Renderer *r, float x, float y, float w,
                     float value, float min_val, float max_val, bool active) {
    float track_h = 6;
    float thumb_r = 8;
    float cy = y + thumb_r;
    float pad = thumb_r + 2;
    float track_y = cy - track_h / 2;

    /* Track background */
    draw_rounded_rect(r, x + pad, track_y, w - 2 * pad, track_h, 3,
                      0x31, 0x32, 0x39, 0xFF);
    /* Filled portion */
    float frac = (value - min_val) / (max_val - min_val);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float fill_w = (w - 2 * pad) * frac;
    if (fill_w > 3)
        draw_rounded_rect(r, x + pad, track_y, fill_w, track_h, 3,
                          0x44, 0x43, 0x48, 0xFF);
    /* gold thumb, darker shade when being dragged */
    float tx = x + pad + (w - 2 * pad) * frac;
    if (active)
        draw_circle_filled(r, tx, cy, thumb_r, 0xB0, 0x90, 0x46);
    else
        draw_circle_filled(r, tx, cy, thumb_r, 0xD8, 0xAD, 0x70);
}

void draw_ctk_radio(SDL_Renderer *r, float x, float y, const char *label,
                    float font_sz, bool selected) {
    float circle_r = 8;
    float cx = x + circle_r;
    float cy = y + circle_r;
    /* 2px AA border ring: draw outer circle in border color, inner in fill color */
    draw_circle_filled(r, cx, cy, circle_r, 0x44, 0x43, 0x48);
    if (selected) {
        draw_circle_filled(r, cx, cy, circle_r - 2.0f, 0xD8, 0xAD, 0x70);
    } else {
        draw_circle_filled(r, cx, cy, circle_r - 2.0f, 0x31, 0x32, 0x39);
    }
    /* label text, vcenter with circle */
    int px = (int)(font_sz + 0.5f);
    AtlasSize *as = get_size(px);
    float line_h = as ? (float)(as->ascent + as->descent) : (float)px;
    float ty = cy - line_h * 0.5f;
    draw_text_px(r, label, x + circle_r * 2 + 4, ty, px,
                  0xE0, 0xE0, 0xE0);
}

void draw_scrollbar_v(SDL_Renderer *r, float x, float y, float h,
                      float view_frac, float scroll_frac, bool hovered) {
    if (view_frac >= 1.0f) return;
    float track_w = 12;
    /* no visible track, just the thumb */
    float thumb_h = h * view_frac;
    if (thumb_h < 24) thumb_h = 24;
    float thumb_y = y + (h - thumb_h) * scroll_frac;
    uint8_t tc = hovered ? 0xCC : 0x59;
    uint8_t tg = hovered ? 0xA4 : 0x5A;
    uint8_t tb = hovered ? 0x71 : 0x62;
    draw_rounded_rect(r, x + 2, thumb_y + 2, track_w - 4, thumb_h - 4, 6, tc, tg, tb, 0xFF);
}

void draw_scrollbar_h(SDL_Renderer *r, float x, float y, float w,
                      float view_frac, float scroll_frac, bool hovered) {
    if (view_frac >= 1.0f) return;
    float track_h = 12;
    /* no visible track, just the thumb */
    float thumb_w = w * view_frac;
    if (thumb_w < 24) thumb_w = 24;
    float thumb_x = x + (w - thumb_w) * scroll_frac;
    uint8_t tc = hovered ? 0xCC : 0x59;
    uint8_t tg = hovered ? 0xA4 : 0x5A;
    uint8_t tb = hovered ? 0x71 : 0x62;
    draw_rounded_rect(r, thumb_x + 2, y + 2, thumb_w - 4, track_h - 4, 6, tc, tg, tb, 0xFF);
}

/* --- clip stack --- */

#define MAX_CLIPS 16
static SDL_Rect clip_stack[MAX_CLIPS];
static int clip_depth = 0;

void push_clip(SDL_Renderer *r, float x, float y, float w, float h) {
    SDL_Rect rc = { (int)x, (int)y, (int)ceilf(w), (int)ceilf(h) };
    if (clip_depth < MAX_CLIPS) clip_stack[clip_depth++] = rc;
    SDL_SetRenderClipRect(r, &rc);
}

void pop_clip(SDL_Renderer *r) {
    if (clip_depth > 0) clip_depth--;
    if (clip_depth > 0)
        SDL_SetRenderClipRect(r, &clip_stack[clip_depth - 1]);
    else
        SDL_SetRenderClipRect(r, NULL);
}

/* --- instrument icons --- */

#define MAX_INST_ID 0x30
static SDL_Texture *g_inst_icons[MAX_INST_ID];

static SDL_Texture *load_icon(SDL_Renderer *r, const char *path) {
    int w, h, ch;
    uint8_t *pixels = stbi_load(path, &w, &h, &ch, 4);
    if (!pixels) {
        SDL_Log("Failed to load icon: %s", path);
        return NULL;
    }
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    SDL_Texture *tex = NULL;
    if (surf) {
        tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_DestroySurface(surf);
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
    }
    stbi_image_free(pixels);
    return tex;
}

void init_instrument_icons(SDL_Renderer *r, const char *icons_dir) {
    memset(g_inst_icons, 0, sizeof(g_inst_icons));

    /* Mapping: inst_id -> filename */
    static const struct { uint8_t id; const char *file; } icon_map[] = {
        { 0x00, "Guitar_gray.png" },
        { 0x01, "Flute_gray.png" },
        { 0x02, "Recorder.png" },
        { 0x04, "HandDrum.png" },
        { 0x05, "Cymbals.png" },
        { 0x06, "Harp_gray.png" },
        { 0x07, "Piano_gray.png" },
        { 0x08, "Violin_gray.png" },
        { 0x0A, "Guitar_gold.png" },
        { 0x0B, "Flute_gold.png" },
        { 0x0D, "Drumset_gold.png" },
        { 0x0F, "Contrabass_gold.png" },
        { 0x10, "Harp_gold.png" },
        { 0x11, "Piano_gold.png" },
        { 0x12, "Violin_gold.png" },
        { 0x13, "HandPan_gold.png" },
        { 0x14, "WavyPlanet_green.png" },
        { 0x18, "IllusionTree_green.png" },
        { 0x1C, "SecretNote_green.png" },
        { 0x0E, "Marnibass_blue.png" },
        { 0x20, "Sandwich_green.png" },
        { 0x24, "SilverWave_blue.png" },
        { 0x25, "Highway_blue.png" },
        { 0x26, "Hexe_blue.png" },
        { 0x27, "Clarinet_gold.png" },
        { 0x28, "Horn_gold.png" },
    };
    int count = (int)(sizeof(icon_map) / sizeof(icon_map[0]));

    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", icons_dir, icon_map[i].file);
        SDL_Texture *tex = load_icon(r, path);
        if (tex && icon_map[i].id < MAX_INST_ID) {
            g_inst_icons[icon_map[i].id] = tex;
        }
    }
    SDL_Log("Loaded %d instrument icons from %s", count, icons_dir);
}

SDL_Texture *get_instrument_icon(uint8_t inst_id) {
    if (inst_id >= MAX_INST_ID) return NULL;
    return g_inst_icons[inst_id];
}

void cleanup_instrument_icons(void) {
    for (int i = 0; i < MAX_INST_ID; i++) {
        if (g_inst_icons[i]) {
            SDL_DestroyTexture(g_inst_icons[i]);
            g_inst_icons[i] = NULL;
        }
    }
}

SDL_Surface *load_png_surface(const char *path) {
    int w, h, ch;
    uint8_t *pixels = stbi_load(path, &w, &h, &ch, 4);
    if (!pixels) return NULL;
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (surf) memcpy(surf->pixels, pixels, (size_t)(w * h * 4));
    stbi_image_free(pixels);
    return surf;
}

SDL_Surface *load_png_surface_mem(const void *data, int len) {
    int w, h, ch;
    uint8_t *pixels = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!pixels) return NULL;
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (surf) memcpy(surf->pixels, pixels, (size_t)(w * h * 4));
    stbi_image_free(pixels);
    return surf;
}

/* aurora blob - the glowy nebula things during playback */
void draw_aurora_blob(SDL_Renderer *r, float cx, float cy, float radius,
                      uint8_t cr, uint8_t cg, uint8_t cb, uint8_t alpha) {
    if (!g_blob_tex) return;
    SDL_SetTextureColorMod(g_blob_tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(g_blob_tex, alpha);
    SDL_FRect dst = { cx - radius, cy - radius, radius * 2, radius * 2 };
    SDL_RenderTexture(r, g_blob_tex, NULL, &dst);
}
