/*
 * ui_menu.h — popup contextual menu.
 *
 * Single-instance system (only one menu can be open at a time, which is
 * how menus work everywhere). Opens at an anchor point — typically the
 * cursor — and flips to stay on-screen. Items support labels, ids,
 * right-aligned shortcut hints, separators, and non-clickable section
 * headers. Dismisses on Esc, outside-click, or item invoke.
 *
 * Items are passed as a `const UiMenuItem *items` array. The system
 * holds the pointer for the lifetime the menu is open, so the caller
 * must keep the array alive (static const is the typical choice).
 */
#ifndef MUSE_UI_MENU_H
#define MUSE_UI_MENU_H

#include <SDL3/SDL.h>
#include <stdbool.h>

struct UiMenuItem;

typedef struct UiMenuItem {
    const char              *label;     /* NULL = separator (thin line);
                                           non-NULL with id == 0 and no submenu =
                                             section header (greyed, bold);
                                           otherwise clickable. */
    int                      id;        /* passed to the callback when chosen.
                                           Ignored for parent-of-submenu items. */
    const char              *shortcut;  /* optional, right-aligned hint */
    bool                     disabled;  /* clickable item but greyed and inert */
    const struct UiMenuItem *submenu;   /* NULL = leaf item; non-NULL means
                                           hovering this item pops a submenu */
    int                      submenu_n;
} UiMenuItem;

typedef void (*UiMenuCallback)(int item_id, void *user);

/* Open at anchor (typically cursor coords). Replaces any open menu. */
void ui_menu_open(const UiMenuItem *items, int n,
                  float anchor_x, float anchor_y,
                  UiMenuCallback cb, void *user);

void ui_menu_close(void);
bool ui_menu_is_open(void);

/* Per-frame hooks — call after panels_render_all and before app event
   dispatch (so the menu sits topmost and consumes input first). */
void ui_menu_render(SDL_Renderer *r, float mx, float my);
bool ui_menu_handle_event(const SDL_Event *ev); /* true = consumed */

#endif /* MUSE_UI_MENU_H */

