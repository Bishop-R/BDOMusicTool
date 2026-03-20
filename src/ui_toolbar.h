#ifndef MUSE_UI_TOOLBAR_H
#define MUSE_UI_TOOLBAR_H

#include <stdbool.h>

typedef struct MuseApp MuseApp;
void ui_toolbar_render(MuseApp *app);
bool ui_toolbar_click(MuseApp *app, float mx, float my);
void ui_toolbar_open(MuseApp *app);
void ui_toolbar_open_path(MuseApp *app, const char *path);
void ui_toolbar_save_as(MuseApp *app);
void ui_toolbar_link_account(MuseApp *app);

#endif /* MUSE_UI_TOOLBAR_H */
