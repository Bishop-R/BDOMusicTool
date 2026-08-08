/*
 * ui_menu.c — popup menu with single-level submenus.
 *
 * Layout: rows of fixed height. Width auto-sized to fit the longest
 * label + (shortcut|chevron) with padding. A submenu opens to the right
 * of its parent item (left-flips if clipped, shifts up if too tall).
 * Keyboard: Up/Down within the active pane, Right enters a submenu,
 * Left leaves it, Esc unwinds one level.
 */
#include "ui_menu.h"
#include "ui_render.h"
#include "ui.h"

#include <SDL3/SDL.h>
#include <math.h>

#define MENU_ROW_H_ITEM    30.0f
#define MENU_ROW_H_HEADER  24.0f
#define MENU_ROW_H_SEP     8.0f
#define MENU_PAD_X         14.0f
#define MENU_PAD_Y         8.0f
#define MENU_SHORTCUT_GAP  28.0f
#define MENU_CHEVRON_GAP   18.0f
#define MENU_FONT_SIZE     11.0f
#define MENU_HEADER_SIZE   9.0f
#define MENU_MIN_W         140.0f

typedef struct {
    const UiMenuItem *items;
    int               n;
    float             x, y;
    float             w, h;
    float             content_h;
    float             scroll;
    int               hover;     /* index of hovered item, -1 if none */
} MenuPane;

typedef struct {
    bool            open;
    UiMenuCallback  cb;
    void           *user;
    int             win_w, win_h;

    MenuPane root;
    /* Submenu state. sub_parent < 0 means no submenu open. */
    MenuPane sub;
    int      sub_parent;
    bool     kb_in_sub;   /* keyboard focus is inside the submenu */
} MenuState;

static MenuState g_menu;

/* ── per-pane helpers ──────────────────────────────────────── */

static bool is_separator(const UiMenuItem *it) { return it->label == NULL; }
static bool is_header   (const UiMenuItem *it) { return it->label != NULL && it->id == 0 && it->submenu == NULL && !it->disabled; }
static bool is_clickable(const UiMenuItem *it) {
    if (it->label == NULL || it->disabled) return false;
    return it->id != 0 || it->submenu != NULL;
}
static bool has_submenu (const UiMenuItem *it) { return it->submenu != NULL && it->submenu_n > 0; }

static float row_height(const UiMenuItem *it) {
    if (is_separator(it)) return MENU_ROW_H_SEP;
    if (is_header(it))    return MENU_ROW_H_HEADER;
    return MENU_ROW_H_ITEM;
}

static void measure_pane(MenuPane *p) {
    float max_w = MENU_MIN_W;
    float total_h = MENU_PAD_Y * 2;
    for (int i = 0; i < p->n; i++) {
        const UiMenuItem *it = &p->items[i];
        total_h += row_height(it);
        if (is_separator(it)) continue;
        float lw = text_width(it->label, is_header(it) ? MENU_HEADER_SIZE : MENU_FONT_SIZE);
        float rw = 0;
        if (it->shortcut) rw = MENU_SHORTCUT_GAP + text_width(it->shortcut, MENU_FONT_SIZE);
        else if (has_submenu(it)) rw = MENU_CHEVRON_GAP;
        float row_w = lw + rw + MENU_PAD_X * 2;
        if (row_w > max_w) max_w = row_w;
    }
    p->w = max_w;
    p->content_h = total_h;
    p->h = total_h;
    if (p->scroll < 0) p->scroll = 0;
}

static void edge_flip_pane(MenuPane *p) {
    float available_h = g_menu.win_h > 16 ? g_menu.win_h - 8.0f : 200.0f;
    p->h = p->content_h < available_h ? p->content_h : available_h;
    float max_scroll = p->content_h - p->h;
    if (max_scroll < 0) max_scroll = 0;
    if (p->scroll > max_scroll) p->scroll = max_scroll;
    if (p->x + p->w > g_menu.win_w) p->x = g_menu.win_w - p->w - 4;
    if (p->y + p->h > g_menu.win_h) p->y = g_menu.win_h - p->h - 4;
    if (p->x < 4) p->x = 4;
    if (p->y < 4) p->y = 4;
}

static float row_y_of(const MenuPane *p, int idx) {
    float cy = p->y + MENU_PAD_Y - p->scroll;
    for (int i = 0; i < idx && i < p->n; i++) cy += row_height(&p->items[i]);
    return cy;
}

static int hit_test_pane(const MenuPane *p, float mx, float my) {
    if (mx < p->x || mx >= p->x + p->w ||
        my < p->y || my >= p->y + p->h) return -1;
    float cy = p->y + MENU_PAD_Y - p->scroll;
    for (int i = 0; i < p->n; i++) {
        float h = row_height(&p->items[i]);
        if (my >= cy && my < cy + h) {
            return is_clickable(&p->items[i]) ? i : -1;
        }
        cy += h;
    }
    return -1;
}

static int next_clickable_in(const MenuPane *p, int from, int dir) {
    if (p->n == 0) return -1;
    int i = from;
    for (int step = 0; step < p->n; step++) {
        i += dir;
        if (i < 0) i = p->n - 1;
        if (i >= p->n) i = 0;
        if (is_clickable(&p->items[i])) return i;
    }
    return -1;
}

static void reveal_hover(MenuPane *p) {
    if (!p || p->hover < 0 || p->hover >= p->n) return;
    float top = MENU_PAD_Y;
    for (int i = 0; i < p->hover; i++) top += row_height(&p->items[i]);
    float bottom = top + row_height(&p->items[p->hover]);
    if (top < p->scroll + MENU_PAD_Y)
        p->scroll = top - MENU_PAD_Y;
    else if (bottom > p->scroll + p->h - MENU_PAD_Y)
        p->scroll = bottom - p->h + MENU_PAD_Y;
    float max_scroll = p->content_h - p->h;
    if (max_scroll < 0) max_scroll = 0;
    if (p->scroll < 0) p->scroll = 0;
    if (p->scroll > max_scroll) p->scroll = max_scroll;
}

/* ── submenu open/close ────────────────────────────────────── */

static void close_sub(void) {
    g_menu.sub_parent = -1;
    g_menu.sub.items  = NULL;
    g_menu.sub.n      = 0;
    g_menu.sub.hover  = -1;
    g_menu.kb_in_sub  = false;
}

static void open_sub_for(int parent_idx, bool from_keyboard) {
    if (parent_idx < 0 || parent_idx >= g_menu.root.n) { close_sub(); return; }
    const UiMenuItem *parent = &g_menu.root.items[parent_idx];
    if (!has_submenu(parent)) { close_sub(); return; }

    g_menu.sub_parent = parent_idx;
    g_menu.sub.items  = parent->submenu;
    g_menu.sub.n      = parent->submenu_n;
    g_menu.sub.scroll = 0;
    measure_pane(&g_menu.sub);
    g_menu.sub.x = g_menu.root.x + g_menu.root.w - 4;
    g_menu.sub.y = row_y_of(&g_menu.root, parent_idx) - MENU_PAD_Y;
    /* Left-flip if would clip the right edge */
    if (g_menu.sub.x + g_menu.sub.w > g_menu.win_w) {
        g_menu.sub.x = g_menu.root.x - g_menu.sub.w + 4;
    }
    edge_flip_pane(&g_menu.sub);

    g_menu.sub.hover = from_keyboard ? next_clickable_in(&g_menu.sub, -1, +1) : -1;
    g_menu.kb_in_sub = from_keyboard;
}

/* ── public ──────────────────────────────────────────────────── */

void ui_menu_open(const UiMenuItem *items, int n,
                  float anchor_x, float anchor_y,
                  UiMenuCallback cb, void *user) {
    g_menu.open       = true;
    g_menu.cb         = cb;
    g_menu.user       = user;
    g_menu.root.items = items;
    g_menu.root.n     = n;
    g_menu.root.x     = anchor_x;
    g_menu.root.y     = anchor_y;
    g_menu.root.hover = -1;
    g_menu.root.scroll = 0;
    if (g_menu.win_w <= 0) g_menu.win_w = 1400;
    if (g_menu.win_h <= 0) g_menu.win_h = 800;
    measure_pane(&g_menu.root);
    edge_flip_pane(&g_menu.root);
    close_sub();
}

void ui_menu_close(void) {
    g_menu.open       = false;
    g_menu.root.items = NULL;
    g_menu.root.n     = 0;
    g_menu.root.hover = -1;
    close_sub();
}

bool ui_menu_is_open(void) { return g_menu.open; }

/* ── render ──────────────────────────────────────────────────── */

static void draw_chevron(SDL_Renderer *r, float cx, float cy, uint8_t alpha) {
    /* Right-pointing triangle, 4 px tall */
    for (int i = 0; i < 4; i++) {
        draw_filled_rect(r, cx + i, cy - (3 - i), 1, (3 - i) * 2 + 1,
                         COL_TEXT_DIM, alpha);
    }
}

static void render_pane(SDL_Renderer *r, const MenuPane *p, int parent_hover_lock) {
    /* parent_hover_lock: if >= 0, that root item is forced highlighted
       (so the parent of an open submenu stays visually selected). */
    /* 2-layer drop shadow */
    draw_rounded_rect(r, p->x + 4, p->y + 5, p->w, p->h, 8,
                      0x00, 0x00, 0x00, 0x40);
    draw_rounded_rect(r, p->x + 2, p->y + 3, p->w, p->h, 8,
                      0x00, 0x00, 0x00, 0x28);

    /* Background gradient (themed popup surface) */
    draw_rounded_rect_vgradient(r, p->x, p->y, p->w, p->h, 8,
        COL_BG_LIGHT, 0xFF,
        COL_BG_DARK, 0xFF);
    draw_rounded_rect_outline(r, p->x, p->y, p->w, p->h, 8, COL_BORDER);
    draw_filled_rect(r, p->x + 8, p->y + 1, p->w - 16, 1,
                     0xFF, 0xFF, 0xFF, 0x10);

    SDL_Rect clip = {(int)p->x, (int)p->y, (int)p->w, (int)p->h};
    SDL_SetRenderClipRect(r, &clip);
    float cy = p->y + MENU_PAD_Y - p->scroll;
    for (int i = 0; i < p->n; i++) {
        const UiMenuItem *it = &p->items[i];
        float h = row_height(it);

        if (cy + h <= p->y || cy >= p->y + p->h) {
            cy += h;
            continue;
        }
        if (is_separator(it)) {
            float sy = cy + h * 0.5f;
            draw_filled_rect(r, p->x + 10, sy,     p->w - 20, 1, 0xFF, 0xFF, 0xFF, 0x10);
            draw_filled_rect(r, p->x + 10, sy + 1, p->w - 20, 1, 0x00, 0x00, 0x00, 0x20);
        } else if (is_header(it)) {
            draw_text_bold(r, it->label, p->x + MENU_PAD_X, cy + 4,
                           MENU_HEADER_SIZE, COL_TEXT_DIM);
        } else {
            bool hov = (i == p->hover) || (i == parent_hover_lock);
            if (hov && !it->disabled) {
                draw_rounded_rect(r, p->x + 4, cy + 1, p->w - 8, h - 2, 4,
                                  COL_GOLD, 0x22);
                draw_filled_rect(r, p->x + 4, cy + 1, 2, h - 2,
                                 COL_GOLD_BRIGHT, 0xFF);
            }
            if (it->disabled)
                draw_text(r, it->label, p->x + MENU_PAD_X, cy + (h - 11) * 0.5f,
                          MENU_FONT_SIZE, COL_TEXT_DIM);
            else if (hov)
                draw_text(r, it->label, p->x + MENU_PAD_X, cy + (h - 11) * 0.5f,
                          MENU_FONT_SIZE, COL_TEXT_GOLD);
            else
                draw_text(r, it->label, p->x + MENU_PAD_X, cy + (h - 11) * 0.5f,
                          MENU_FONT_SIZE, COL_TEXT);

            if (it->shortcut) {
                draw_text_right(r, it->shortcut,
                                p->x + p->w - MENU_PAD_X,
                                cy + (h - 11) * 0.5f,
                                MENU_FONT_SIZE, COL_TEXT_DIM);
            } else if (has_submenu(it)) {
                draw_chevron(r,
                             p->x + p->w - MENU_PAD_X - 4,
                             cy + h * 0.5f,
                             it->disabled ? 0x60 : 0xC0);
            }
        }
        cy += h;
    }
    SDL_SetRenderClipRect(r, NULL);
    if (p->content_h > p->h + .5f) {
        float track_h = p->h - 12.0f;
        float thumb_h = fmaxf(22.0f, track_h * p->h / p->content_h);
        float max_scroll = p->content_h - p->h;
        float thumb_y = p->y + 6.0f +
            (track_h - thumb_h) * (max_scroll > 0 ? p->scroll / max_scroll : 0);
        draw_rounded_rect(r, p->x + p->w - 5.0f, p->y + 6.0f,
                          2.0f, track_h, 1, COL_BORDER, 0x70);
        draw_rounded_rect(r, p->x + p->w - 6.0f, thumb_y,
                          4.0f, thumb_h, 2, COL_TEXT_DIM, 0xC0);
    }
}

void ui_menu_render(SDL_Renderer *r, float mx, float my) {
    if (!g_menu.open) return;

    int ww = 0, wh = 0;
    SDL_GetCurrentRenderOutputSize(r, &ww, &wh);
    if (ww > 0 && wh > 0) {
        g_menu.win_w = ww;
        g_menu.win_h = wh;
        edge_flip_pane(&g_menu.root);
        if (g_menu.sub_parent >= 0) edge_flip_pane(&g_menu.sub);
    }

    /* Update hover from cursor. Submenu takes priority when cursor is
       inside it (so sweeping back to parent doesn't kill the sub). */
    int sub_hit = (g_menu.sub_parent >= 0) ? hit_test_pane(&g_menu.sub, mx, my) : -1;
    if (sub_hit >= 0) {
        g_menu.sub.hover = sub_hit;
        g_menu.kb_in_sub = true;
    } else {
        int root_hit = hit_test_pane(&g_menu.root, mx, my);
        if (root_hit >= 0) {
            g_menu.root.hover = root_hit;
            g_menu.kb_in_sub = false;
            /* Auto-open / switch submenus on hover. */
            const UiMenuItem *it = &g_menu.root.items[root_hit];
            if (has_submenu(it) && g_menu.sub_parent != root_hit) {
                open_sub_for(root_hit, false);
            } else if (!has_submenu(it) && g_menu.sub_parent >= 0) {
                close_sub();
            }
        }
        /* If cursor over neither, keep current submenu open (so the user
           can sweep the mouse outside the menu rectangles slightly
           without losing the submenu — common UX). */
    }

    render_pane(r, &g_menu.root, g_menu.sub_parent);
    if (g_menu.sub_parent >= 0) render_pane(r, &g_menu.sub, -1);
}

/* ── event handling ───────────────────────────────────────────── */

static MenuPane *active_pane(void) {
    return (g_menu.kb_in_sub && g_menu.sub_parent >= 0) ? &g_menu.sub : &g_menu.root;
}

static void invoke_current(void) {
    MenuPane *p = active_pane();
    if (p->hover < 0 || p->hover >= p->n) return;
    const UiMenuItem *it = &p->items[p->hover];
    if (!is_clickable(it)) return;
    if (has_submenu(it) && p == &g_menu.root) {
        /* Pressing Enter on a parent: enter the submenu via keyboard. */
        open_sub_for(p->hover, true);
        return;
    }
    int id = it->id;
    UiMenuCallback cb = g_menu.cb;
    void *user = g_menu.user;
    ui_menu_close();
    if (cb && id != 0) cb(id, user);
}

bool ui_menu_handle_event(const SDL_Event *ev) {
    if (!g_menu.open) return false;

    if (ev->type == SDL_EVENT_KEY_DOWN) {
        MenuPane *p = active_pane();
        switch (ev->key.key) {
        case SDLK_ESCAPE:
            if (g_menu.sub_parent >= 0 && g_menu.kb_in_sub) close_sub();
            else ui_menu_close();
            return true;
        case SDLK_DOWN:
            p->hover = next_clickable_in(p, p->hover < 0 ? -1 : p->hover, +1);
            reveal_hover(p);
            return true;
        case SDLK_UP:
            p->hover = next_clickable_in(p, p->hover < 0 ? p->n : p->hover, -1);
            reveal_hover(p);
            return true;
        case SDLK_RIGHT: {
            if (!g_menu.kb_in_sub && p->hover >= 0 && has_submenu(&p->items[p->hover])) {
                open_sub_for(p->hover, true);
            }
            return true;
        }
        case SDLK_LEFT:
            if (g_menu.kb_in_sub) close_sub();
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            invoke_current();
            return true;
        }
        return true; /* eat all keys while open */
    }

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        /* Hover updates happen in render. Don't consume motion so background
           UI doesn't get stuck-hovered after the menu closes. */
        return false;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        float mx = ev->button.x, my = ev->button.y;

        int sub_hit  = (g_menu.sub_parent >= 0) ? hit_test_pane(&g_menu.sub,  mx, my) : -1;
        int root_hit = hit_test_pane(&g_menu.root, mx, my);

        if (sub_hit >= 0) {
            g_menu.sub.hover = sub_hit;
            g_menu.kb_in_sub = true;
            invoke_current();
            return true;
        }
        if (root_hit >= 0) {
            g_menu.root.hover = root_hit;
            g_menu.kb_in_sub  = false;
            const UiMenuItem *it = &g_menu.root.items[root_hit];
            if (has_submenu(it)) {
                /* Click on parent of submenu: ensure submenu is open
                   (hover already opens it, but click solidifies). */
                if (g_menu.sub_parent != root_hit) open_sub_for(root_hit, false);
                return true;
            }
            invoke_current();
            return true;
        }

        bool inside_root = (mx >= g_menu.root.x && mx < g_menu.root.x + g_menu.root.w &&
                            my >= g_menu.root.y && my < g_menu.root.y + g_menu.root.h);
        bool inside_sub  = (g_menu.sub_parent >= 0 &&
                            mx >= g_menu.sub.x && mx < g_menu.sub.x + g_menu.sub.w &&
                            my >= g_menu.sub.y && my < g_menu.sub.y + g_menu.sub.h);
        /* Click on a dead row (header/separator/disabled item) inside a
           pane: consume it and keep the menu open, like a disabled item. */
        if (inside_root || inside_sub) return true;
        /* Click outside both panes: dismiss and let the click propagate
           to the UI underneath. */
        ui_menu_close();
        return false;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP)  return true;
    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        MenuPane *p = &g_menu.root;
        if (g_menu.sub_parent >= 0 &&
            mx >= g_menu.sub.x && mx < g_menu.sub.x + g_menu.sub.w &&
            my >= g_menu.sub.y && my < g_menu.sub.y + g_menu.sub.h)
            p = &g_menu.sub;
        float max_scroll = p->content_h - p->h;
        if (max_scroll > 0) {
            p->scroll -= ev->wheel.y * MENU_ROW_H_ITEM * 3.0f;
            if (p->scroll < 0) p->scroll = 0;
            if (p->scroll > max_scroll) p->scroll = max_scroll;
            p->hover = hit_test_pane(p, mx, my);
        }
        return true;
    }

    return false;
}
