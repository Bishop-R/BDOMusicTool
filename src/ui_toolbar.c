#include "ui_toolbar.h"
#include "platform.h"
#include "app.h"
#include "audio.h"
#include "bdo_format.h"
#include "muse_format.h"
#include "midi_import.h"
#include "ui.h"
#include "ui_render.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* File dialog callbacks */
static void open_callback(void *userdata, const char *const *filelist, int filter) {
    (void)filter;
    MuseApp *app = (MuseApp *)userdata;
    if (!filelist || !filelist[0]) return;
    const char *path = filelist[0];
    size_t len = strlen(path);
    /* Check if MIDI file */
    bool is_midi = (len > 4 && strcmp(path + len - 4, ".mid") == 0) ||
                   (len > 5 && strcmp(path + len - 5, ".midi") == 0);
    if (is_midi) {
        /* parse first, then show import dialog to let user pick instruments */
        MidiImportData *mid = midi_parse(path);
        if (mid && mid->num_channels > 0) {
            const char *f = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') f = p + 1;
            snprintf(app->filename, sizeof(app->filename), "%s", f);
            /* strip .mid/.midi extension */
            size_t fl = strlen(app->filename);
            if (fl > 4 && strcmp(app->filename + fl - 4, ".mid") == 0)
                app->filename[fl - 4] = '\0';
            else if (fl > 5 && strcmp(app->filename + fl - 5, ".midi") == 0)
                app->filename[fl - 5] = '\0';
            app->midi_dlg_data = mid;
            app->midi_dlg_open = true;
            app->midi_dlg_scroll = 0;
            app->midi_dlg_hover = -1;
            app->midi_dlg_hover_btn = 0;
            app->midi_dlg_dropdown_ch = -1;
            app->midi_dlg_dropdown_hover = -1;
            app->midi_dlg_dropdown_scroll = 0;
            app->midi_dlg_tech_dropdown_ch = -1;
            app->midi_dlg_tech_dropdown_hover = -1;
            app->midi_dlg_tech_dropdown_scroll = 0;
        } else {
            midi_import_data_free(mid);
        }
    } else if (len > 9 && strcmp(path + len - 9, ".composer") == 0) {
        /* .composer project file */
        int rc = muse_load(path, app);
        if (rc == 0) {
            int total_notes = 0;
            for (int li = 0; li < app->project.num_layers; li++)
                for (int si = 0; si < app->project.layers[li].num_sublayers; si++)
                    total_notes += app->project.layers[li].sublayers[si].count;
            set_status_msg(app, "Opened: %s (%d notes)", app->filename, total_notes);
        } else {
            SDL_Log("Muse load failed: rc=%d (path=%s)", rc, path);
        }
    } else {
        /* Treat any non-MIDI file as BDO (game files often have no extension) */
        if (app->char_name[0] == '\0') {
            set_status_msg(app, "Link your account first (use a one-note BDO file)");
            return;
        }
        int rc = bdo_load(path, app->char_name, &app->project);
        if (rc == 0) {
            const char *f = path;
            for (const char *p = path; *p; p++)
                if (*p == '/' || *p == '\\') f = p + 1;
            /* Strip .bdo extension if present, otherwise use full filename */
            size_t fl = strlen(f);
            if (fl > 4 && strcmp(f + fl - 4, ".bdo") == 0)
                snprintf(app->filename, sizeof(app->filename), "%.*s", (int)(fl - 4), f);
            else
                snprintf(app->filename, sizeof(app->filename), "%s", f);
            snprintf(app->char_name, sizeof(app->char_name), "%s", app->project.char_name);
            app->project.dirty = false;
            app->playhead_ms = 0; app->scroll_x = 0;
            /* Calculate measures from note data */
            double max_end = 0;
            for (int li = 0; li < app->project.num_layers; li++) {
                for (int si = 0; si < app->project.layers[li].num_sublayers; si++) {
                    NoteArray *na = &app->project.layers[li].sublayers[si];
                    for (int ni = 0; ni < na->count; ni++) {
                        double end = na->notes[ni].start + na->notes[ni].dur;
                        if (end > max_end) max_end = end;
                    }
                }
            }
            double beat_ms = 60000.0 / (app->project.bpm ? app->project.bpm : 120);
            int beats = (int)(max_end / beat_ms) + 4;
            int ts = app->project.time_sig ? app->project.time_sig : 4;
            app->measures = (beats + ts - 1) / ts;
            if (app->measures < 4) app->measures = 4;
            /* Copy effector settings from loaded project to app */
            app->fx_reverb       = app->project.effector_reverb;
            app->fx_delay        = app->project.effector_delay;
            app->fx_chorus_fb    = app->project.effector_chorus_fb;
            app->fx_chorus_depth = app->project.effector_chorus_depth;
            app->fx_chorus_freq  = app->project.effector_chorus_freq;
            {
                int total_notes = 0;
                for (int li = 0; li < app->project.num_layers; li++)
                    for (int si = 0; si < app->project.layers[li].num_sublayers; si++)
                        total_notes += app->project.layers[li].sublayers[si].count;
                set_status_msg(app, "Imported: %s (%d notes)", app->filename, total_notes);
            }
        } else if (rc == -5) {
            set_status_msg(app, "Cannot open: file belongs to a different account");
        } else {
            SDL_Log("BDO load failed: rc=%d (path=%s)", rc, path);
        }
    }
}

static void save_callback(void *userdata, const char *const *filelist, int filter) {
    (void)filter;
    MuseApp *app = (MuseApp *)userdata;
    if (!filelist || !filelist[0]) return;
    const char *spath = filelist[0];
    size_t slen = strlen(spath);

    /* push effector state into project before writing */
    app->project.effector_reverb       = (uint8_t)app->fx_reverb;
    app->project.effector_delay        = (uint8_t)app->fx_delay;
    app->project.effector_chorus_fb    = (uint8_t)app->fx_chorus_fb;
    app->project.effector_chorus_depth = (uint8_t)app->fx_chorus_depth;
    app->project.effector_chorus_freq  = (uint8_t)app->fx_chorus_freq;

    bool is_comp = (slen > 9 && strcmp(spath + slen - 9, ".composer") == 0);
    int rc;
    if (is_comp)
        rc = muse_save(spath, app);
    else
        rc = bdo_save(spath, &app->project);

    if (rc == 0) {
        const char *f = spath;
        for (const char *p = spath; *p; p++)
            if (*p == '/' || *p == '\\') f = p + 1;
        size_t fl = strlen(f);
        if (is_comp && fl > 9 && strcmp(f + fl - 9, ".composer") == 0)
            snprintf(app->filename, sizeof(app->filename), "%.*s", (int)(fl - 9), f);
        else if (!is_comp && fl > 4 && strcmp(f + fl - 4, ".bdo") == 0)
            snprintf(app->filename, sizeof(app->filename), "%.*s", (int)(fl - 4), f);
        else
            snprintf(app->filename, sizeof(app->filename), "%s", f);
        app->project.dirty = false;
        set_status_msg(app, "Saved: %s", app->filename);
    }
}

static const SDL_DialogFileFilter open_filters[] = {
    { "All Files",   "*" },
    { "Composer Project", "composer" },
    { "BDO Files",   "bdo" },
    { "MIDI Files",  "mid;midi" },
};
static const SDL_DialogFileFilter save_filters[] = {
    { "Composer Project", "composer" },
    { "BDO Files",    "bdo" },
};

static void link_callback(void *userdata, const char *const *filelist, int filter) {
    (void)filter;
    MuseApp *app = (MuseApp *)userdata;
    if (!filelist || !filelist[0]) return;

    #define LINK_TOKEN_SIZE 72
    FILE *f = fopen_utf8(filelist[0], "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 4 + LINK_TOKEN_SIZE) { fclose(f); return; }
    uint8_t hdr[4 + LINK_TOKEN_SIZE];
    fread(hdr, 1, sizeof(hdr), f);
    fclose(f);

    uint32_t ver = hdr[0]|(hdr[1]<<8)|(hdr[2]<<16)|(hdr[3]<<24);
    if (ver != 9 && ver != 6) return;

    uint8_t token[LINK_TOKEN_SIZE];
    memcpy(token, hdr + 4, LINK_TOKEN_SIZE);

    uint32_t oid = 0;
    char cname[64] = {0};
    int rc = bdo_extract_owner(filelist[0], &oid, cname, sizeof(cname));
    if (rc != 0 || oid == 0) return;

    app->linked_owner_id = oid;
    snprintf(app->char_name, sizeof(app->char_name), "%s", cname);
    set_status_msg(app, "Account linked: %s", cname);

    {
        FILE *cfg = fopen("Save/account.cfg", "w");
        if (cfg) {
            for (int i = 0; i < LINK_TOKEN_SIZE; i++)
                fprintf(cfg, "%02x", token[i]);
            fprintf(cfg, "\n");
            fclose(cfg);
        }
    }
    #undef LINK_TOKEN_SIZE
}

static const SDL_DialogFileFilter link_filters[] = {
    { "All Files", "*" },
    { "BDO Files", "bdo" },
};

void ui_toolbar_link_account(MuseApp *app) {
    SDL_ShowOpenFileDialog(link_callback, app, app->window, link_filters, 2, NULL, false);
}

void ui_toolbar_open(MuseApp *app) {
    SDL_ShowOpenFileDialog(open_callback, app, app->window, open_filters, 4, "Save", false);
}
void ui_toolbar_open_path(MuseApp *app, const char *path) {
    const char *filelist[] = { path, NULL };
    open_callback(app, filelist, 0);
}
void ui_toolbar_save_as(MuseApp *app) {
    SDL_ShowSaveFileDialog(save_callback, app, app->window, save_filters, 2, "Save");
}

/* --- Toolbar rendering ---
   Layout: New Open Save | BPM Time Grid Measures | Play Stop Time | Tools */

void ui_toolbar_render(MuseApp *app) {
    SDL_Renderer *r = app->renderer;
    float mx = app->mouse_x, my = app->mouse_y;
    bool in_bar = (my >= 0 && my < TRANSPORT_H);

    /* Toolbar background — subtle top-to-bottom gradient for depth */
    draw_rounded_rect(r, 0, 0, (float)app->win_w, TRANSPORT_H, 6, COL_SURFACE, 0xFF);
    /* lighter top strip for subtle gradient feel */
    draw_filled_rect(r, 0, 0, (float)app->win_w, 1, 0xFF, 0xFF, 0xFF, 0x08);
    draw_filled_rect(r, 0, 1, (float)app->win_w, 1, 0xFF, 0xFF, 0xFF, 0x04);
    /* tiny gap below the toolbar */
    draw_filled_rect(r, 0, TRANSPORT_H, (float)app->win_w, 2, COL_BG_DARK, 0xFF);

    float btn_h = 28;
    float cy = (TRANSPORT_H - btn_h) / 2.0f;  /* vertically center buttons */
    float x = 8;

    /* New */
    UiRect btn_new = { x, cy, 40, btn_h };
    draw_ctk_button(r, btn_new, "New", 13, in_bar && ui_rect_contains(btn_new, mx, my), false);
    x += 42;

    /* Open */
    UiRect btn_open = { x, cy, 44, btn_h };
    draw_ctk_button(r, btn_open, "Open", 13, in_bar && ui_rect_contains(btn_open, mx, my), false);
    x += 46;

    /* Save */
    UiRect btn_save = { x, cy, 44, btn_h };
    draw_ctk_button(r, btn_save, "Save", 13, in_bar && ui_rect_contains(btn_save, mx, my), false);
    x += 46;

    /* Separator — thin styled vertical line */
    x += 6;
    draw_vline(r, x, cy + 4, cy + btn_h - 4, COL_BORDER);
    draw_filled_rect(r, x + 1, cy + 4, 1, btn_h - 8, 0xFF, 0xFF, 0xFF, 0x06);
    x += 8;

    /* BPM label + entry */
    float label_y = (TRANSPORT_H - text_line_height(13)) / 2.0f;
    draw_text(r, "BPM:", x, label_y, 13, COL_TEXT_DIM);
    x += text_width("BPM:", 13) + 4;
    char bpm_str[16];
    if (app->edit_field == 1)
        snprintf(bpm_str, sizeof(bpm_str), "%s|", app->edit_buf);
    else
        snprintf(bpm_str, sizeof(bpm_str), "%d", app->project.bpm);
    UiRect bpm_entry = { x, cy, 50, btn_h };
    draw_ctk_entry(r, bpm_entry, bpm_str, 13, app->edit_field == 1);
    x += 52;

    /* Time: */
    x += 12;
    draw_text(r, "Time:", x, label_y, 13, COL_TEXT_DIM);
    x += text_width("Time:", 13) + 4;
    char ts_str[8];
    snprintf(ts_str, sizeof(ts_str), "%d", app->project.time_sig);
    UiRect ts_entry = { x, cy, 50, btn_h };
    draw_ctk_entry(r, ts_entry, ts_str, 13, false);
    draw_dropdown_arrow(r, x + 50 - 10, cy + btn_h / 2, 8, COL_TEXT_DIM);
    x += 52;

    /* Grid: */
    x += 12;
    draw_text(r, "Grid:", x, label_y, 13, COL_TEXT_DIM);
    x += text_width("Grid:", 13) + 4;
    const char *grid_labels[] = {"1/4","1/8","1/16","1/32","1/64","Free"};
    UiRect grid_btn = { x, cy, 60, btn_h };
    draw_ctk_entry(r, grid_btn, grid_labels[app->grid_snap], 13, false);
    draw_dropdown_arrow(r, x + 60 - 10, cy + btn_h / 2, 8, COL_TEXT_DIM);
    x += 62;

    /* Measures: */
    x += 12;
    draw_text(r, "Measures:", x, label_y, 13, COL_TEXT_DIM);
    x += text_width("Measures:", 13) + 4;
    char meas_str[16];
    if (app->edit_field == 2)
        snprintf(meas_str, sizeof(meas_str), "%s|", app->edit_buf);
    else
        snprintf(meas_str, sizeof(meas_str), "%d", app->measures);
    UiRect meas_entry = { x, cy, 50, btn_h };
    draw_ctk_entry(r, meas_entry, meas_str, 13, app->edit_field == 2);
    x += 52;

    /* Separator */
    x += 4;

    /* Playback: ▶ ■ time */
    x += 16;
    UiRect btn_play = { x, cy, 36, btn_h };
    /* Play button - draw as shape because our font atlas doesn't have these glyphs */
    {
        bool ph = in_bar && ui_rect_contains(btn_play, mx, my);
        if (ph)
            draw_rounded_rect(r, btn_play.x, btn_play.y, btn_play.w, btn_play.h, 6, COL_BORDER, 0xFF);
        else
            draw_rounded_rect(r, btn_play.x, btn_play.y, btn_play.w, btn_play.h, 6, COL_SURFACE, 0xFF);
        float pcx = btn_play.x + btn_play.w / 2;
        float pcy = btn_play.y + btn_play.h / 2;
        if (app->playing) {
            /* Pause: two vertical bars */
            draw_filled_rect(r, pcx - 5, pcy - 5, 3, 10, COL_GOLD_LIGHT, 0xFF);
            draw_filled_rect(r, pcx + 2, pcy - 5, 3, 10, COL_GOLD_LIGHT, 0xFF);
        } else {
            /* Play triangle - scanline rendered for smooth AA */
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, COL_GOLD_LIGHT, 0xFF);
            for (float dy = -6; dy <= 6; dy += 0.25f) {
                float hw = 5.0f * (1.0f - fabsf(dy) / 6.0f);
                SDL_FRect strip = { pcx - 3, pcy + dy, hw, 0.25f };
                SDL_RenderFillRect(r, &strip);
            }
        }
    }
    x += 38;

    UiRect btn_stop = { x, cy, 36, btn_h };
    /* Stop button */
    {
        bool sh = in_bar && ui_rect_contains(btn_stop, mx, my);
        if (sh)
            draw_rounded_rect(r, btn_stop.x, btn_stop.y, btn_stop.w, btn_stop.h, 6, COL_BORDER, 0xFF);
        else
            draw_rounded_rect(r, btn_stop.x, btn_stop.y, btn_stop.w, btn_stop.h, 6, COL_SURFACE, 0xFF);
        float scx = btn_stop.x + btn_stop.w / 2;
        float scy = btn_stop.y + btn_stop.h / 2;
        draw_filled_rect(r, scx - 5, scy - 5, 10, 10, COL_GOLD_LIGHT, 0xFF);
    }
    x += 38;

    /* Time display - mm:ss.ms / total */
    x += 8;
    {
        int total_ms = (int)app->playhead_ms;
        int mins = total_ms / 60000;
        int secs = (total_ms / 1000) % 60;
        int ms = total_ms % 1000;
        /* Calculate total duration */
        double total_dur_ms = app->measures * app->project.time_sig *
                              (60000.0 / (app->project.bpm ? app->project.bpm : 120));
        int dur_mins = (int)total_dur_ms / 60000;
        int dur_secs = ((int)total_dur_ms / 1000) % 60;
        int dur_ms = (int)total_dur_ms % 1000;
        char time_str[48];
        snprintf(time_str, sizeof(time_str), "%d:%02d.%03d / %d:%02d.%03d",
                 mins, secs, ms, dur_mins, dur_secs, dur_ms);
        float time_y = (TRANSPORT_H - text_line_height(11)) / 2.0f;
        draw_text_bold(r, time_str, x, time_y, 11, COL_TEXT);
    }

    /* tool radio buttons (right side) */
    float tool_x = (float)app->win_w - 8;
    /* Draw right-to-left */
    const char *tool_labels[] = { "Select (S)", "Draw (D)", "Erase (E)" };
    MuseTool tool_vals[] = { TOOL_SELECT, TOOL_DRAW, TOOL_ERASE };
    float tool_widths[3];
    float total_tools_w = 0;
    for (int i = 0; i < 3; i++) {
        tool_widths[i] = 16 + 4 + text_width(tool_labels[i], 11) + 8;
        total_tools_w += tool_widths[i];
    }
    /* Separator before tools */
    float sep_x = (float)app->win_w - 8 - total_tools_w - 12;
    draw_vline(r, sep_x, cy + 4, cy + btn_h - 4, COL_BORDER);
    draw_filled_rect(r, sep_x + 1, cy + 4, 1, btn_h - 8, 0xFF, 0xFF, 0xFF, 0x06);

    float radio_h = 16;  /* circle diameter */
    float radio_y = (TRANSPORT_H - radio_h) / 2.0f;
    float tx = sep_x + 8;
    for (int i = 0; i < 3; i++) {
        draw_ctk_radio(r, tx, radio_y, tool_labels[i], 11, app->tool == tool_vals[i]);
        tx += tool_widths[i];
    }

    /* Store button rects for click handling */
    app->_tb_new = btn_new;
    app->_tb_open = btn_open;
    app->_tb_save = btn_save;
    app->_tb_play = btn_play;
    app->_tb_stop = btn_stop;
    app->_tb_grid = grid_btn;
    app->_tb_bpm_entry = bpm_entry;
    app->_tb_meas_entry = meas_entry;
    app->_tb_ts_entry = ts_entry;
    app->_tb_tool_x = sep_x + 8;
    app->_tb_tool_widths[0] = tool_widths[0];
    app->_tb_tool_widths[1] = tool_widths[1];
    app->_tb_tool_widths[2] = tool_widths[2];
}

bool ui_toolbar_click(MuseApp *app, float mx, float my) {
    if (my < 0 || my >= TRANSPORT_H) return false;

    if (ui_rect_contains(app->_tb_new, mx, my)) {
        if (app->project.dirty) {
            SDL_MessageBoxButtonData btns[] = {
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Discard" },
            };
            SDL_MessageBoxData mbd = {
                .flags = SDL_MESSAGEBOX_WARNING,
                .window = app->window,
                .title = "Unsaved Changes",
                .message = "Unsaved changes will be lost. Continue?",
                .numbuttons = 2,
                .buttons = btns,
            };
            int btn = 0;
            if (!SDL_ShowMessageBox(&mbd, &btn) || btn != 1) return true;
        }
        muse_project_free(&app->project);
        muse_project_init(&app->project);
        muse_project_add_layer(&app->project, 0x11);
        snprintf(app->filename, sizeof(app->filename), "untitled");
        app->playhead_ms = 0; app->scroll_x = 0;
        set_status_msg(app, "New project");
        return true;
    }
    if (ui_rect_contains(app->_tb_open, mx, my)) {
        ui_toolbar_open(app);
        return true;
    }
    if (ui_rect_contains(app->_tb_save, mx, my)) {
        if (strcmp(app->filename, "untitled") != 0) {
            size_t fnlen = strlen(app->filename);
            bool fn_is_comp = (fnlen > 9 && strcmp(app->filename + fnlen - 9, ".composer") == 0);
            char path[512];
            if (fn_is_comp)
                snprintf(path, sizeof(path), "%s", app->filename);
            else
                snprintf(path, sizeof(path), "%s.bdo", app->filename);
            int rc;
            if (fn_is_comp)
                rc = muse_save(path, app);
            else
                rc = bdo_save(path, &app->project);
            if (rc == 0) {
                app->project.dirty = false;
                set_status_msg(app, "Saved: %s", path);
            }
        } else {
            ui_toolbar_save_as(app);
        }
        return true;
    }
    if (ui_rect_contains(app->_tb_play, mx, my)) {
        if (app->playing) {
            /* Pause */
            app->paused = true;
            app->playing = false;
            muse_audio_pause();
        } else if (app->paused) {
            /* Resume from pause */
            app->paused = false;
            app->playing = true;
            app->play_start_ticks = SDL_GetTicks();
            app->play_start_ms = app->playhead_ms;
            muse_audio_resume();
        } else {
            /* Fresh start */
            app->playing = true;
            app->play_start_ticks = SDL_GetTicks();
            app->play_start_ms = app->playhead_ms - 5.0;
            muse_audio_play(&app->project, app->playhead_ms - 5.0);
        }
        return true;
    }
    if (ui_rect_contains(app->_tb_stop, mx, my)) {
        app->playing = false;
        app->paused = false;
        app->playhead_ms = 0;
        muse_audio_stop();
        return true;
    }
    if (ui_rect_contains(app->_tb_grid, mx, my)) {
        app->dropdown_open = 2;
        app->dropdown_anchor = app->_tb_grid;
        app->dropdown_hover = -1;
        return true;
    }
    /* BPM entry */
    if (ui_rect_contains(app->_tb_bpm_entry, mx, my)) {
        app->edit_field = 1;
        snprintf(app->edit_buf, sizeof(app->edit_buf), "%d", app->project.bpm);
        app->edit_cursor = (int)strlen(app->edit_buf);
        return true;
    }
    /* Measures entry */
    if (ui_rect_contains(app->_tb_meas_entry, mx, my)) {
        app->edit_field = 2;
        snprintf(app->edit_buf, sizeof(app->edit_buf), "%d", app->measures);
        app->edit_cursor = (int)strlen(app->edit_buf);
        return true;
    }
    /* Time sig dropdown */
    if (ui_rect_contains(app->_tb_ts_entry, mx, my)) {
        app->dropdown_open = 1;
        app->dropdown_anchor = app->_tb_ts_entry;
        app->dropdown_hover = -1;
        return true;
    }

    /* Tool radio buttons */
    float tx = app->_tb_tool_x;
    MuseTool tools[] = { TOOL_SELECT, TOOL_DRAW, TOOL_ERASE };
    for (int i = 0; i < 3; i++) {
        if (mx >= tx && mx < tx + app->_tb_tool_widths[i] && my >= 6 && my < 34) {
            app->tool = tools[i];
            return true;
        }
        tx += app->_tb_tool_widths[i];
    }

    return false;
}
