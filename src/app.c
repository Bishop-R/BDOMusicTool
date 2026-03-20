#include "app.h"
#include "audio.h"
#include "bdo_format.h"
#include "ice.h"
#include "muse_format.h"
#include "midi_import.h"
#include "sample_extract.h"
#include "ui.h"
#include "ui_piano_roll.h"
#include "ui_toolbar.h"
#include "ui_render.h"
#include "undo.h"
#include "instruments.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* system cursors - created once, destroyed on shutdown */
static SDL_Cursor *g_cursors[SDL_SYSTEM_CURSOR_COUNT];

/* font search paths */
static const char *FONT_PATHS[] = {
    NULL, /* placeholder for $HOME/.fonts/Roboto-Regular.ttf */
    "./fonts/Roboto-Medium.ttf",     /* dist/release layout */
    "/usr/share/fonts/truetype/roboto/Roboto-Regular.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "C:\\Windows\\Fonts\\segoeui.ttf",
};
#define NUM_FONT_PATHS (sizeof(FONT_PATHS) / sizeof(FONT_PATHS[0]))

static void on_playback_finished(void *userdata) {
    MuseApp *app = (MuseApp *)userdata;
    app->playing = false;
    app->playhead_ms = 0;
}

void set_status_msg(MuseApp *app, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status_msg, sizeof(app->status_msg), fmt, ap);
    va_end(ap);
    app->status_msg_ticks = SDL_GetTicks();
}

/* ---- Sample extraction helpers (used during init) ---- */

typedef struct {
    char path[4096];
    bool done;
    bool cancelled;
} FolderPickState;

static void SDLCALL folder_pick_callback(void *userdata,
                                          const char * const *filelist,
                                          int filter)
{
    (void)filter;
    FolderPickState *s = (FolderPickState *)userdata;
    if (filelist && filelist[0]) {
        snprintf(s->path, sizeof(s->path), "%s", filelist[0]);
    } else {
        s->cancelled = true;
    }
    s->done = true;
}

typedef struct {
    SDL_Renderer *renderer;
    SDL_Window   *window;
    int    win_w, win_h;
} ExtractCtx;

static void extract_progress_render(int current, int total,
                                     const char *name, void *ctx_arg)
{
    ExtractCtx *ec = (ExtractCtx *)ctx_arg;

    /* pump events so window stays responsive */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
            SDL_GetWindowSize(ec->window, &ec->win_w, &ec->win_h);
        }
    }

    float w = (float)ec->win_w;
    float h = (float)ec->win_h;

    /* dark background */
    SDL_SetRenderDrawColor(ec->renderer, 24, 26, 30, 255);
    SDL_RenderClear(ec->renderer);

    /* title */
    draw_text_centered(ec->renderer, "Extracting instrument samples...",
                       w / 2.0f, h / 2.0f - 60.0f, 22.0f,
                       220, 220, 220);

    /* current instrument name */
    draw_text_centered(ec->renderer, name,
                       w / 2.0f, h / 2.0f - 25.0f, 16.0f,
                       160, 170, 180);

    /* progress text */
    char prog_text[64];
    snprintf(prog_text, sizeof(prog_text), "%d / %d", current, total);
    draw_text_centered(ec->renderer, prog_text,
                       w / 2.0f, h / 2.0f + 10.0f, 18.0f,
                       200, 200, 200);

    /* progress bar */
    float bar_w = 400.0f;
    if (bar_w > w - 80.0f) bar_w = w - 80.0f;
    float bar_h = 16.0f;
    float bar_x = (w - bar_w) / 2.0f;
    float bar_y = h / 2.0f + 40.0f;

    /* bar background */
    draw_rounded_rect(ec->renderer, bar_x, bar_y, bar_w, bar_h,
                      4.0f, 50, 52, 58, 255);

    /* bar fill */
    float frac = (float)current / (float)total;
    float fill_w = bar_w * frac;
    if (fill_w > 0) {
        draw_rounded_rect(ec->renderer, bar_x, bar_y,
                          fill_w, bar_h, 4.0f,
                          80, 140, 220, 255);
    }

    SDL_RenderPresent(ec->renderer);
}

bool muse_app_init(MuseApp *app) {
    /* create save dirs on first launch */
    SDL_CreateDirectory("Save");
    SDL_CreateDirectory("Save/Autosave");

    memset(app, 0, sizeof(*app));
    app->running         = true;
    app->key_height      = KEY_HEIGHT_DEFAULT;
    app->zoom_x          = 1.0;
    app->left_panel_open = true;
    app->win_w           = DEFAULT_WIN_W;
    app->win_h           = DEFAULT_WIN_H;
    app->grid_snap       = GRID_1_16;
    app->tool            = TOOL_DRAW;
    app->measures        = 4;
    app->hover_layer     = -1;
    app->hover_btn       = -1;
    app->resize_note_idx = -1;
    app->midi_dlg_dropdown_ch = -1;
    app->midi_dlg_dropdown_hover = -1;
    app->cur_vel         = 100;
    app->cur_ntype       = 0;
    app->key_highlight_pitch = -1;
    for (int i = 0; i < 128; i++) app->retrigger_start[i] = -1.0;
    snprintf(app->filename, sizeof(app->filename), "untitled");
    note_array_init(&app->clipboard);

    /* start scrolled to a sensible octave range */
    app->scroll_y = 20;

    muse_project_init(&app->project);
    muse_project_add_layer(&app->project, 0x11); /* Flor. Piano */

    SDL_SetHint("SDL_APP_ID", "composer");
    app->window = SDL_CreateWindow("Composer", DEFAULT_WIN_W, DEFAULT_WIN_H,
                                   SDL_WINDOW_RESIZABLE);
    if (!app->window) return false;
    SDL_SetWindowMinimumSize(app->window, MIN_WIN_W, MIN_WIN_H);

    /* window icon (embedded PNG) */
    {
        #include "app_icon.h"
        SDL_Surface *icon = load_png_surface_mem(app_icon_png, (int)app_icon_png_len);
        if (icon) {
            SDL_SetWindowIcon(app->window, icon);
            SDL_DestroySurface(icon);
        }
    }

    app->renderer = SDL_CreateRenderer(app->window, NULL);
    if (!app->renderer) return false;
    SDL_SetRenderVSync(app->renderer, 1);

    for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++)
        g_cursors[i] = SDL_CreateSystemCursor((SDL_SystemCursor)i);

    app->focused = true;

    /* find and load font */
    char home_font[512];
    const char *home = SDL_getenv("HOME");
    if (home) {
        snprintf(home_font, sizeof(home_font), "%s/.fonts/Roboto-Medium.ttf", home);
        FONT_PATHS[0] = home_font;
    }
    bool font_ok = false;
    for (int i = 0; i < (int)NUM_FONT_PATHS; i++) {
        if (!FONT_PATHS[i]) continue;
        if (text_init(app->renderer, FONT_PATHS[i])) {
            font_ok = true;
            break;
        }
    }
    if (!font_ok) SDL_Log("Warning: no font loaded - text will not render");

    /* load instrument icons */
    {
        const char *icon_tries[] = {
            "./Data/Icons",       /* dist/release layout */
            "./Icons",            /* dev cwd */
            "../Icons",           /* build/ -> Icons/ */
            "../src/Icons",       /* build/ -> src/Icons/ */
            NULL
        };
        bool icons_loaded = false;
        /* try relative paths first (probe for a known file) */
        for (int i = 0; icon_tries[i]; i++) {
            char test_path[512];
            snprintf(test_path, sizeof(test_path), "%s/Guitar_gray.png", icon_tries[i]);
            FILE *test = fopen(test_path, "r");
            if (test) {
                fclose(test);
                init_instrument_icons(app->renderer, icon_tries[i]);
                icons_loaded = true;
                break;
            }
        }
        /* fallback: absolute path for my dev machine */
        if (!icons_loaded) {
            const char *h2 = SDL_getenv("HOME");
            if (h2) {
                char icons_path[512];
                snprintf(icons_path, sizeof(icons_path), "%s/Dokumente/Muse/src/Icons", h2);
                init_instrument_icons(app->renderer, icons_path);
            }
        }
    }

    /* find samples dir and extract from BDO if needed */
    {
        const char *samples_dir = NULL;
        const char *tries[] = {
            "./Data/samples",       /* dist/release layout */
            "./samples",            /* dev cwd */
            "../samples",           /* build/ -> samples/ */
            "../../samples",        /* build/src/ -> samples/ */
            NULL
        };
        /* probe for key_zones.json to find the right dir */
        for (int i = 0; tries[i]; i++) {
            char test_path[512];
            snprintf(test_path, sizeof(test_path), "%s/key_zones.json", tries[i]);
            FILE *test = fopen(test_path, "r");
            if (test) {
                fclose(test);
                samples_dir = tries[i];
                break;
            }
        }
        /* fallback: dev machine layout */
        static char dev_samples[512];
        if (!samples_dir) {
            const char *h = SDL_getenv("HOME");
            if (h) {
                snprintf(dev_samples, sizeof(dev_samples), "%s/Dokumente/Muse/samples", h);
                char test_path[512];
                snprintf(test_path, sizeof(test_path), "%s/key_zones.json", dev_samples);
                FILE *test = fopen(test_path, "r");
                if (test) {
                    fclose(test);
                    samples_dir = dev_samples;
                }
            }
        }

        /* Check if actual sample .ogg files exist (not just key_zones.json) */
        bool need_extraction = false;
        if (samples_dir) {
            char probe[512];
            snprintf(probe, sizeof(probe),
                     "%s/midi_instrument_07_piano/285036551.ogg", samples_dir);
            FILE *test = fopen(probe, "r");
            if (test) {
                fclose(test);
            } else {
                need_extraction = true;
            }
        }

        if (need_extraction && samples_dir) {
            bool extraction_done = false;
            const char *paz_dir = NULL;

            {
                static char dropped_paz_dir[4096];
                bool waiting = true;
                bool got_path = false;

                while (waiting) {
                    SDL_Event ev;
                    while (SDL_PollEvent(&ev)) {
                        if (ev.type == SDL_EVENT_QUIT) {
                            waiting = false;
                        } else if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
                            /* user dropped a file - derive Paz dir from it */
                            snprintf(dropped_paz_dir, sizeof(dropped_paz_dir), "%s", ev.drop.data);
                            /* strip filename to get directory */
                            char *last_sep = strrchr(dropped_paz_dir, '/');
                            if (!last_sep) last_sep = strrchr(dropped_paz_dir, '\\');
                            if (last_sep) *last_sep = '\0';
                            /* check if this dir or its parent has PAZ files */
                            char test_path[4096];
                            snprintf(test_path, sizeof(test_path), "%s/PAD10132.PAZ", dropped_paz_dir);
                            FILE *pf = fopen(test_path, "rb");
                            if (pf) {
                                fclose(pf);
                                paz_dir = dropped_paz_dir;
                                got_path = true;
                                waiting = false;
                            } else {
                                /* maybe they dropped a file from the parent dir */
                                char *parent_sep = strrchr(dropped_paz_dir, '/');
                                if (!parent_sep) parent_sep = strrchr(dropped_paz_dir, '\\');
                                if (parent_sep) {
                                    char parent[4096];
                                    snprintf(parent, sizeof(parent), "%.*s/Paz",
                                             (int)(parent_sep - dropped_paz_dir), dropped_paz_dir);
                                    snprintf(test_path, sizeof(test_path), "%s/PAD10132.PAZ", parent);
                                    pf = fopen(test_path, "rb");
                                    if (pf) {
                                        fclose(pf);
                                        snprintf(dropped_paz_dir, sizeof(dropped_paz_dir), "%s", parent);
                                        paz_dir = dropped_paz_dir;
                                        got_path = true;
                                        waiting = false;
                                    }
                                }
                            }
                        }
                    }

                    /* render the instruction screen */
                    SDL_SetRenderDrawColor(app->renderer, 0x1A, 0x1A, 0x1E, 0xFF);
                    SDL_RenderClear(app->renderer);
                    float cx = (float)app->win_w / 2.0f;
                    float cy = (float)app->win_h / 2.0f;
                    draw_text_centered(app->renderer, "Instrument Samples Required",
                                       cx, cy - 80, 16, 0xD4, 0xBC, 0x98);
                    draw_text_centered(app->renderer,
                        "Drag any file from your BDO 'Paz' folder onto this window.",
                        cx, cy - 40, 12, 0xC0, 0xC0, 0xC0);
                    draw_text_centered(app->renderer,
                        "Look for a 'Paz' folder in your BDO installation directory.",
                        cx, cy + 5, 10, 0x80, 0x80, 0x80);
                    draw_text_centered(app->renderer,
                        "Press ESC to skip (instruments will use fallback sounds)",
                        cx, cy + 60, 10, 0x60, 0x60, 0x60);
                    SDL_RenderPresent(app->renderer);
                    SDL_Delay(16);

                    /* allow ESC to skip */
                    const bool *keys = SDL_GetKeyboardState(NULL);
                    if (keys[SDL_SCANCODE_ESCAPE]) waiting = false;
                }
            }

            if (paz_dir) {
                /* render extraction progress screen */
                ExtractCtx ectx;
                ectx.renderer = app->renderer;
                ectx.window   = app->window;
                ectx.win_w    = app->win_w;
                ectx.win_h    = app->win_h;

                int extracted = extract_all_samples(paz_dir, samples_dir,
                                                     extract_progress_render, &ectx);

                if (extracted > 0) {
                    extraction_done = true;
                    SDL_Log("Sample extraction complete: %d samples", extracted);
                } else {
                    SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_WARNING,
                        "Extraction Failed",
                        "Could not extract instrument samples from the BDO archives.\n"
                        "The PAZ files may be from an incompatible game version.\n\n"
                        "Audio playback will not be available.",
                        app->window);
                }
            }

            (void)extraction_done;
        }

        if (samples_dir)
            muse_audio_set_samples_dir(samples_dir);
    }
    if (!muse_audio_init())
        SDL_Log("Audio init failed - continuing without audio");

    muse_audio_set_on_finished(on_playback_finished, app);
    muse_audio_set_fx_params(&app->fx_reverb, &app->fx_delay, &app->fx_chorus_fb,
                              &app->fx_chorus_depth, &app->fx_chorus_freq);

    undo_init();

    #define LINK_TOKEN_SIZE 72
    {
        FILE *cfg = fopen("Save/account.cfg", "r");
        if (cfg) {
            char hex[LINK_TOKEN_SIZE * 2 + 4];
            memset(hex, 0, sizeof(hex));
            if (fgets(hex, sizeof(hex), cfg)) {
                hex[strcspn(hex, "\n")] = 0;
                if (strlen(hex) == LINK_TOKEN_SIZE * 2) {
                    uint8_t token[LINK_TOKEN_SIZE];
                    for (int i = 0; i < LINK_TOKEN_SIZE; i++) {
                        unsigned int b;
                        sscanf(hex + i * 2, "%2x", &b);
                        token[i] = (uint8_t)b;
                    }
                    ice_init();
                    ice_decrypt(token, LINK_TOKEN_SIZE);
                    uint32_t oid = token[0]|(token[1]<<8)|(token[2]<<16)|(token[3]<<24);
                    app->linked_owner_id = oid;
                    int cn_i = 0;
                    for (int i = 0; i < 62 && 8 + i + 1 < LINK_TOKEN_SIZE; i += 2) {
                        uint16_t ch = token[8 + i] | (token[8 + i + 1] << 8);
                        if (ch == 0) break;
                        if (ch < 128 && cn_i < (int)sizeof(app->char_name) - 1)
                            app->char_name[cn_i++] = (char)ch;
                    }
                    app->char_name[cn_i] = '\0';
                }
            }
            fclose(cfg);
        }
    }
    #undef LINK_TOKEN_SIZE

    /* set initial overlay state based on what's needed */
    if (app->char_name[0] == '\0') {
        app->overlay_state = 2; /* link account */
    } else {
        FILE *seen = fopen("Save/.welcome_shown", "r");
        if (!seen) {
            SDL_CreateDirectory("Save");
            FILE *mark = fopen("Save/.welcome_shown", "w");
            if (mark) { fprintf(mark, "1\n"); fclose(mark); }
            app->overlay_state = 3; /* disclaimer */
        } else {
            fclose(seen);
            app->overlay_state = 0; /* no overlay */
        }
    }
    app->overlay_fade = 0.0f;
    app->overlay_dismiss = false;

    /* check for autosave recovery */
    {
        const char *as_path = "Save/Autosave/autosave.composer";
        FILE *test = fopen(as_path, "rb");
        if (test) {
            fclose(test);
            SDL_MessageBoxButtonData buttons[] = {
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Discard" },
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Recover" },
            };
            SDL_MessageBoxData mbd = {
                .flags = SDL_MESSAGEBOX_INFORMATION,
                .window = app->window,
                .title = "Autosave Recovery",
                .message = "An autosave file was found from a previous session.\n"
                           "Would you like to recover it?",
                .numbuttons = 2,
                .buttons = buttons,
            };
            int btn = 0;
            if (SDL_ShowMessageBox(&mbd, &btn) && btn == 1) {
                if (muse_load(as_path, app) == 0) {
                    snprintf(app->filename, sizeof(app->filename), "recovered");
                    app->project.dirty = true;
                    app->playhead_ms = 0;
                    app->scroll_x = 0;
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
                    app->fx_reverb       = app->project.effector_reverb;
                    app->fx_delay        = app->project.effector_delay;
                    app->fx_chorus_fb    = app->project.effector_chorus_fb;
                    app->fx_chorus_depth = app->project.effector_chorus_depth;
                    app->fx_chorus_freq  = app->project.effector_chorus_freq;
                }
            }
            remove(as_path);
        }
    }

    return true;
}

void muse_app_shutdown(MuseApp *app) {
    /* clean up autosave if project was saved */
    if (!app->project.dirty) {
        remove("Save/Autosave/autosave.composer");
    }
    undo_free();
    note_array_free(&app->clipboard);
    muse_audio_shutdown();
    cleanup_instrument_icons();
    text_shutdown();
    muse_project_free(&app->project);
    free(app->lasso_points); app->lasso_points = NULL;
    free(app->move_orig_starts);  app->move_orig_starts  = NULL;
    free(app->move_orig_pitches); app->move_orig_pitches = NULL;
    if (app->midi_dlg_data) {
        midi_import_data_free((MidiImportData *)app->midi_dlg_data);
        app->midi_dlg_data = NULL;
    }
    for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++)
        if (g_cursors[i]) SDL_DestroyCursor(g_cursors[i]);
    if (app->renderer) SDL_DestroyRenderer(app->renderer);
    if (app->window)   SDL_DestroyWindow(app->window);
}

/* snap to nearest valid drum key */

static int snap_to_drum_pitch(uint8_t inst_id, int pitch) {
    if (!inst_is_drum(inst_id)) return pitch;
    int best = pitch, best_dist = 999;
    for (int p = PITCH_MIN; p <= PITCH_MAX; p++) {
        if (drum_key_name(inst_id, p)) {
            int d = abs(p - pitch);
            if (d < best_dist) { best_dist = d; best = p; }
        }
    }
    return best;
}

/* coord helpers */

float app_roll_x(const MuseApp *app) {
    return (float)(app->left_panel_open ? LEFT_PANEL_W : 0) + KEYS_WIDTH;
}
float app_roll_y(const MuseApp *app) {
    return (float)(TRANSPORT_H + 2 + HEADER_HEIGHT);
}
float app_roll_w(const MuseApp *app) {
    return (float)app->win_w - app_roll_x(app);
}
static bool has_selected_notes(const MuseApp *app) {
    return app->_sel_cache;
}
float app_roll_h(const MuseApp *app) {
    float vel_h = has_selected_notes(app) ? VEL_PANE_H : 0;
    return (float)(app->win_h - TRANSPORT_H - 2 - HEADER_HEIGHT - STATUS_BAR_H) - vel_h;
}

static double beat_px(const MuseApp *app) {
    return BEAT_WIDTH_DEFAULT * app->zoom_x;
}
static double ms_per_beat(const MuseApp *app) {
    int bpm = app->project.bpm ? app->project.bpm : 120;
    return 60000.0 / bpm;
}

double app_x_to_ms(const MuseApp *app, float x) {
    float rx = app_roll_x(app);
    return app->scroll_x + (x - rx) / beat_px(app) * ms_per_beat(app);
}

float app_ms_to_x(const MuseApp *app, double ms) {
    return app_roll_x(app) + (float)((ms - app->scroll_x) / ms_per_beat(app) * beat_px(app));
}

int app_y_to_pitch(const MuseApp *app, float y) {
    float ry = app_roll_y(app);
    int row = (int)((y - ry) / app->key_height) + app->scroll_y;
    return PITCH_MAX - row;
}

float app_pitch_to_y(const MuseApp *app, int pitch) {
    int row = PITCH_MAX - pitch;
    return app_roll_y(app) + (float)(row - app->scroll_y) * app->key_height;
}

/* grid subdivisions per beat */
static int grid_div(const MuseApp *app) {
    switch (app->grid_snap) {
    case GRID_1_4:  return 1;
    case GRID_1_8:  return 2;
    case GRID_1_16: return 4;
    case GRID_1_32: return 8;
    case GRID_1_64: return 16;
    default:        return 1;
    }
}

double app_snap_ms(const MuseApp *app, double ms) {
    if (app->grid_snap == GRID_FREE) return ms;
    double mpb = ms_per_beat(app);
    double div = mpb / grid_div(app);
    return round(ms / div) * div;
}

static double app_snap_ms_floor(const MuseApp *app, double ms) {
    if (app->grid_snap == GRID_FREE) return ms;
    double mpb = ms_per_beat(app);
    double div = mpb / grid_div(app);
    return floor(ms / div) * div;
}

/* snap to 1/64 grid (finest) - used during note resize drag */
static double app_snap_ms_64(const MuseApp *app, double ms) {
    double div = ms_per_beat(app) / 16.0;
    return round(ms / div) * div;
}

NoteArray *app_active_notes(MuseApp *app) {
    if (app->project.num_layers == 0) return NULL;
    MuseLayer *ly = &app->project.layers[app->project.active_layer];
    if (ly->active_sub < 0 || ly->active_sub >= ly->num_sublayers) return NULL;
    return &ly->sublayers[ly->active_sub];
}

/* note hit testing */

static int find_note_at(MuseApp *app, float mx, float my, bool *on_right_edge) {
    NoteArray *na = app_active_notes(app);
    if (!na) return -1;
    double click_ms = app_x_to_ms(app, mx);
    int click_pitch = app_y_to_pitch(app, my);

    for (int i = na->count - 1; i >= 0; i--) {
        MuseNote *n = &na->notes[i];
        if (n->pitch != click_pitch) continue;
        if (click_ms >= n->start && click_ms <= n->start + n->dur) {
            if (on_right_edge) {
                float nx_end = app_ms_to_x(app, n->start + n->dur);
                *on_right_edge = (mx >= nx_end - 6);
            }
            return i;
        }
    }
    return -1;
}

static void deselect_all_layers(MuseApp *app) {
    for (int li = 0; li < app->project.num_layers; li++) {
        MuseLayer *ly = &app->project.layers[li];
        for (int s = 0; s < ly->num_sublayers; s++) {
            NoteArray *na = &ly->sublayers[s];
            for (int i = 0; i < na->count; i++)
                na->notes[i].selected = 0;
        }
    }
}

static void deselect_layer(MuseApp *app) {
    NoteArray *na = app_active_notes(app);
    if (!na) return;
    for (int i = 0; i < na->count; i++)
        na->notes[i].selected = 0;
}

/* auto-fit measures to the furthest note (min 4) */
static void auto_expand_measures(MuseApp *app) {
    int bpm = app->project.bpm ? app->project.bpm : 120;
    int ts = app->project.time_sig ? app->project.time_sig : 4;
    double mpb = 60000.0 / bpm;
    double max_end = 0;
    for (int li = 0; li < app->project.num_layers; li++) {
        MuseLayer *ly = &app->project.layers[li];
        for (int si = 0; si < ly->num_sublayers; si++) {
            NoteArray *na = &ly->sublayers[si];
            for (int i = 0; i < na->count; i++) {
                double end = na->notes[i].start + na->notes[i].dur;
                if (end > max_end) max_end = end;
            }
        }
    }
    int needed = (int)(max_end / (ts * mpb)) + 2;
    if (needed < 1) needed = 1;
    app->measures = needed;
}

static void restart_playback_if_playing(MuseApp *app) {
    /* no-op: audio engine live-scans the project, picks up edits immediately */
    (void)app;
}

static void delete_selected(MuseApp *app) {
    NoteArray *na = app_active_notes(app);
    if (!na) return;
    int del_count = 0;
    for (int i = 0; i < na->count; i++)
        if (na->notes[i].selected) del_count++;
    undo_push(&app->project);
    for (int i = na->count - 1; i >= 0; i--) {
        if (na->notes[i].selected)
            note_array_remove(na, i);
    }
    app->project.dirty = true;
    if (del_count > 0) set_status_msg(app, "Deleted %d notes", del_count);
    auto_expand_measures(app);
    restart_playback_if_playing(app);
}

/* event handling */

static void handle_left_panel_click(MuseApp *app, float mx, float my) {
    /* layout must match ui_piano_roll.c rendering exactly -
       cy tracks vertical position same as the render code */
    float lp_y = (float)(TRANSPORT_H + 2);
    float cy = lp_y - app->left_panel_scroll;

    /* instruments section - matches render: cy += 10, header, cy += 28 + 5 */
    cy += 10;   /* header top offset */
    cy += 28 + 5; /* header h=28 + gap to first instrument row */

    for (int i = 0; i < app->project.num_layers; i++) {
        MuseLayer *ly = &app->project.layers[i];

        /* separator: 2px above + 1px line + 2px below = 5px */
        if (i > 0) {
            cy += 2;
            cy += 3;
        }

        float row_top = cy;

        /* Top row: h=28, buttons at cy+2 are 24px tall.
           color(4-16), mute(20-44), solo(46-70), name(74-...), remove(RIGHT-30), note count */
        if (my >= cy && my < cy + 28) {
            if (mx >= 20 && mx < 44) {
                /* Mute toggle */
                ly->muted = !ly->muted;
                return;
            }
            if (mx >= 46 && mx < 70) {
                /* Solo toggle */
                ly->solo = !ly->solo;
                return;
            }
            /* Remove button (x) on far right */
            if (app->project.num_layers > 1 && mx >= LEFT_PANEL_W - 24 - 12 && mx < LEFT_PANEL_W - 6 - 12) {
                undo_push(&app->project);
                muse_project_remove_layer(&app->project, i);
                return;
            }
            /* Name area  - select layer */
            if (mx >= 74) {
                app->project.active_layer = i;
                return;
            }
        }
        cy += 28; /* top row h=28 */

        /* bottom row: aux btn + vol slider, h=28 */
        if (my >= cy && my < cy + 28) {
            app->project.active_layer = i;
            /* Aux button: x=24, w=32 */
            if (mx >= 24 && mx < 56) {
                app->aux_open = true;
                app->aux_layer = i;
                app->aux_drag_slider = 0;
                return;
            }
            /* Volume slider: x=77, w=LEFT_PANEL_W-77-36 (matches render) */
            float sl_x = 77, sl_w = LEFT_PANEL_W - 77 - 36 - 12;
            if (mx >= sl_x && mx < sl_x + sl_w) {
                float frac = (mx - sl_x) / sl_w;
                if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                ly->volume = (uint8_t)(frac * 100);
                app->lp_drag_slider = 1;
                app->lp_slider_x = sl_x;
                app->lp_slider_w = sl_w;
                app->lp_drag_layer = i;
            }
            return;
        }
        cy += 28; /* aux/vol row h=28 */

        /* sublayer tabs */
        cy += 2;
        if (my >= cy && my < cy + 18) {
            float tx = 24;
            for (int si = 0; si < ly->num_sublayers; si++) {
                if (mx >= tx && mx < tx + 22) {
                    /* Select this sublayer */
                    ly->active_sub = si;
                    app->project.active_layer = i;
                    return;
                }
                tx += 24;
            }
            /* + button */
            if (mx >= tx && mx < tx + 22) {
                muse_layer_add_sublayer(ly);
                app->project.active_layer = i;
                return;
            }
            tx += 24;
            /* - button (if num_sublayers > 1) */
            if (ly->num_sublayers > 1 && mx >= tx && mx < tx + 22) {
                undo_push(&app->project);
                muse_layer_remove_sublayer(ly, ly->active_sub);
                app->project.active_layer = i;
                return;
            }
        }
        cy += 20; /* sublayer tabs h=18+2 */

        /* Marnian synth profile selector (0x14, 0x18, 0x1C, 0x20) */
        if (ly->inst_id == 0x14 || ly->inst_id == 0x18 ||
            ly->inst_id == 0x1C || ly->inst_id == 0x20) {
            cy += 2; /* pady=(2,0) */
            if (my >= cy && my < cy + 20 && mx >= 80 && mx < 170) {
                app->project.active_layer = i;
                app->dropdown_open = 4;
                app->dropdown_anchor = (UiRect){ 80, cy, 90, 20 };
                app->dropdown_hover = -1;
                return;
            }
            cy += 22;
        }
    }

    /* Use cached button rects for remaining buttons */
    if (ui_rect_contains(app->_lp_add_inst, mx, my)) {
        app->picker_open = true;
        app->picker_mode = 0; /* add instrument */
        app->picker_scroll = 0;
        app->picker_hover = -1;
        return;
    }
    /* technique dropdown */
    if (ui_rect_contains(app->_lp_technique, mx, my) && app->project.num_layers > 0) {
        MuseLayer *aly = &app->project.layers[app->project.active_layer];
        const MuseInstrument *ains = inst_by_id(aly->inst_id);
        if (ains && ains->num_techniques > 1) {
            app->dropdown_open = 3;
            app->dropdown_anchor = app->_lp_technique;
            app->dropdown_hover = -1;
        }
        return;
    }

    /* velocity slider - sets default vel for new notes only */
    if (ui_rect_contains(app->_lp_vel_slider, mx, my) && app->project.num_layers > 0) {
        float frac = (mx - app->_lp_vel_slider.x) / app->_lp_vel_slider_w;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        app->cur_vel = (uint8_t)(1 + frac * 126);
        app->lp_drag_slider = 2;
        app->lp_slider_x = app->_lp_vel_slider.x;
        app->lp_slider_w = app->_lp_vel_slider_w;
        return;
    }

    /* per-note velocity slider - changes selected notes' vel */
    if (app->_lp_note_vel_visible &&
        ui_rect_contains(app->_lp_note_vel_slider, mx, my) && app->project.num_layers > 0) {
        float frac = (mx - app->_lp_note_vel_slider.x) / app->_lp_note_vel_slider_w;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        uint8_t new_vel = (uint8_t)(1 + frac * 126);
        NoteArray *na = app_active_notes(app);
        if (na) {
            undo_push(&app->project);
            for (int ni = 0; ni < na->count; ni++) {
                if (na->notes[ni].selected & 1)
                    na->notes[ni].vel = new_vel;
            }
            app->project.dirty = true;
        }
        app->lp_drag_slider = 8;
        app->lp_slider_x = app->_lp_note_vel_slider.x;
        app->lp_slider_w = app->_lp_note_vel_slider_w;
        return;
    }

    /* FX sliders: reverb(3), delay(4), chorus params(5-7) */
    {
        int *fx_ptrs[] = {&app->fx_reverb, &app->fx_delay, &app->fx_chorus_fb,
                          &app->fx_chorus_depth, &app->fx_chorus_freq};
        for (int f = 0; f < 5; f++) {
            if (ui_rect_contains(app->_lp_fx_sliders[f], mx, my)) {
                float frac = (mx - app->_lp_fx_slider_x) / app->_lp_fx_slider_w;
                if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                *fx_ptrs[f] = (int)(frac * 100);
                app->project.effector_reverb       = (uint8_t)app->fx_reverb;
                app->project.effector_delay        = (uint8_t)app->fx_delay;
                app->project.effector_chorus_fb    = (uint8_t)app->fx_chorus_fb;
                app->project.effector_chorus_depth = (uint8_t)app->fx_chorus_depth;
                app->project.effector_chorus_freq  = (uint8_t)app->fx_chorus_freq;
                app->lp_drag_slider = 3 + f;
                app->lp_slider_x = app->_lp_fx_slider_x;
                app->lp_slider_w = app->_lp_fx_slider_w;
                return;
            }
        }
    }

    /* filename entry - click to edit or reposition cursor */
    if (ui_rect_contains(app->_lp_fn_entry, mx, my)) {
        if (app->edit_field != 3) {
            app->edit_field = 3;
            snprintf(app->edit_buf, sizeof(app->edit_buf), "%s", app->filename);
            SDL_StartTextInput(app->window);
        }
        /* Find cursor position closest to click x */
        float click_x = mx - app->_lp_fn_entry.x - 8;
        int len = (int)strlen(app->edit_buf);
        int best = len;
        float best_dist = 9999;
        char tmp[256];
        for (int i = 0; i <= len && i < (int)sizeof(tmp) - 1; i++) {
            memcpy(tmp, app->edit_buf, i);
            tmp[i] = '\0';
            float w = text_width(tmp, 13);
            float d = (click_x - w < 0) ? w - click_x : click_x - w;
            if (d < best_dist) { best_dist = d; best = i; }
        }
        app->edit_cursor = best;
        return;
    }

    if (ui_rect_contains(app->_lp_import_midi, mx, my)) {
        ui_toolbar_open(app);
        return;
    }
    if (ui_rect_contains(app->_lp_link_btn, mx, my)) {
        ui_toolbar_link_account(app);
        return;
    }
    if (ui_rect_contains(app->_lp_export_bdo, mx, my)) {
        if (app->char_name[0] == '\0') {
            set_status_msg(app, "Link your account first before exporting BDO");
            return;
        }
        /* always use the linked account info for BDO export */
        snprintf(app->project.char_name, sizeof(app->project.char_name), "%s", app->char_name);
        app->project.owner_id = app->linked_owner_id;
        /* sync global FX values */
        app->project.effector_reverb       = (uint8_t)app->fx_reverb;
        app->project.effector_delay        = (uint8_t)app->fx_delay;
        app->project.effector_chorus_fb    = (uint8_t)app->fx_chorus_fb;
        app->project.effector_chorus_depth = (uint8_t)app->fx_chorus_depth;
        app->project.effector_chorus_freq  = (uint8_t)app->fx_chorus_freq;
        SDL_CreateDirectory("converted");
        char bdo_path[512];
        /* BDO game files are extensionless */
        char clean_name[256];
        snprintf(clean_name, sizeof(clean_name), "%s", app->filename);
        size_t nl = strlen(clean_name);
        if (nl > 4 && strcmp(clean_name + nl - 4, ".bdo") == 0)
            clean_name[nl - 4] = '\0';
        if (nl > 9 && strcmp(clean_name + nl - 9, ".composer") == 0)
            clean_name[nl - 9] = '\0';
        snprintf(bdo_path, sizeof(bdo_path), "converted/%s", clean_name);
        if (bdo_save(bdo_path, &app->project) == 0) {
            app->project.dirty = false;
            set_status_msg(app, "Exported: %s", bdo_path);
        }
        return;
    }
    if (ui_rect_contains(app->_lp_export_wav, mx, my)) {
        /* pause playback during export to avoid race conditions */
        bool was_playing = app->playing;
        if (was_playing) {
            app->playing = false;
            app->paused = true;
            muse_audio_stop();
        }
        SDL_CreateDirectory("converted");
        char wav_path[512];
        snprintf(wav_path, sizeof(wav_path), "converted/%s.wav", app->filename);
        muse_audio_export_wav(wav_path, &app->project, app->measures);
        set_status_msg(app, "Exported: %s", wav_path);
        if (was_playing) {
            app->paused = false;
            app->playing = true;
            app->play_start_ticks = SDL_GetTicks();
            app->play_start_ms = app->playhead_ms;
            muse_audio_play(&app->project, app->playhead_ms);
        }
        return;
    }
}

static void handle_mouse_down_roll(MuseApp *app, float mx, float my, int button) {
    if (button == SDL_BUTTON_MIDDLE) {
        /* middle-click = chordify selected notes */
        NoteArray *na = app_active_notes(app);
        bool has_sel = false;
        if (na) {
            for (int i = 0; i < na->count; i++)
                if (na->notes[i].selected) { has_sel = true; break; }
        }
        if (has_sel) {
            app->chord_open = true;
            app->chord_scroll = 0;
            app->chord_hover = -1;
        } else {
            /* No selection: pan as fallback */
            app->panning = true;
            app->drag_start_x = mx;
            app->drag_start_y = my;
            app->pan_start_scroll_x = app->scroll_x;
            app->pan_start_scroll_y = app->scroll_y;
        }
        return;
    }

    if (button == SDL_BUTTON_RIGHT) {
        /* Right-click erase sweep */
        undo_push(&app->project);
        app->erase_sweeping = true;
        bool edge = false;
        int idx = find_note_at(app, mx, my, &edge);
        if (idx >= 0) {
            NoteArray *na = app_active_notes(app);
            if (na) {
                note_array_remove(na, idx);
                app->project.dirty = true;
                auto_expand_measures(app);
            }
        }
        return;
    }

    /* Left button */
    bool on_edge = false;
    int idx = find_note_at(app, mx, my, &on_edge);

    if (app->tool == TOOL_ERASE) {
        undo_push(&app->project);
        app->erase_sweeping = true;
        if (idx >= 0) {
            NoteArray *na = app_active_notes(app);
            if (na) {
                note_array_remove(na, idx);
                app->project.dirty = true;
                auto_expand_measures(app);
            }
        }
        return;
    }

    if (app->tool == TOOL_SELECT || (app->tool == TOOL_DRAW && idx >= 0)) {
        if (idx >= 0) {
            NoteArray *na = app_active_notes(app);
            if (!na) return;
            bool shift = SDL_GetModState() & SDL_KMOD_SHIFT;
            if (!shift && !na->notes[idx].selected)
                deselect_layer(app);
            na->notes[idx].selected = 1;

            if (on_edge) {
                /* Start resize */
                app->resizing_note = true;
                app->resize_note_idx = idx;
                app->resize_note_layer = app->project.active_layer;
                app->drag_start_x = mx;
            } else {
                /* start move - capture originals to avoid float drift */
                app->moving_notes = true;
                app->move_anchor_x = mx;
                app->move_anchor_y = my;
                app->move_time_offset = 0;
                app->move_pitch_offset = 0;
                /* Store originals for all selected notes */
                {
                    int sel_count = 0;
                    for (int i = 0; i < na->count; i++)
                        if (na->notes[i].selected) sel_count++;
                    free(app->move_orig_starts);
                    free(app->move_orig_pitches);
                    app->move_orig_starts  = (double *)malloc(sel_count * sizeof(double));
                    app->move_orig_pitches = (int *)malloc(sel_count * sizeof(int));
                    app->move_orig_count   = sel_count;
                    int idx2 = 0;
                    for (int i = 0; i < na->count; i++) {
                        if (!na->notes[i].selected) continue;
                        app->move_orig_starts[idx2]  = na->notes[i].start;
                        app->move_orig_pitches[idx2] = na->notes[i].pitch;
                        idx2++;
                    }
                }
            }
        } else {
            bool shift = SDL_GetModState() & SDL_KMOD_SHIFT;
            if (shift) {
                /* shift+click empty space: brush select */
                app->brush_selecting = true;
            } else {
                bool alt_held = SDL_GetModState() & SDL_KMOD_ALT;
                deselect_layer(app);
                if (alt_held) {
                    /* alt+drag: lasso selection */
                    app->lasso_selecting = true;
                    app->lasso_count = 0;
                    if (!app->lasso_points) {
                        app->lasso_capacity = 256;
                        app->lasso_points = malloc(app->lasso_capacity * 2 * sizeof(float));
                    }
                    if (app->lasso_points) {
                        app->lasso_points[0] = mx;
                        app->lasso_points[1] = my;
                        app->lasso_count = 1;
                    }
                } else {
                    /* Start box selection */
                    app->selecting = true;
                    app->sel_x0 = app->sel_x1 = mx;
                    app->sel_y0 = app->sel_y1 = my;
                }
            }
        }
        return;
    }

    if (app->tool == TOOL_DRAW && idx < 0) {
        /* Place new note */
        NoteArray *na = app_active_notes(app);
        if (!na) return;
        int pitch = app_y_to_pitch(app, my);
        if (pitch < PITCH_MIN || pitch > PITCH_MAX) return;

        /* Clamp to instrument range */
        if (app->project.num_layers > 0) {
            const MuseInstrument *cinst = inst_by_id(
                app->project.layers[app->project.active_layer].inst_id);
            if (cinst && (pitch < cinst->pitch_lo || pitch > cinst->pitch_hi))
                return;
            /* Snap to valid drum key if drum instrument */
            pitch = snap_to_drum_pitch(
                app->project.layers[app->project.active_layer].inst_id, pitch);
        }

        double ms = app_snap_ms_floor(app, app_x_to_ms(app, mx));
        if (ms < 0) ms = 0;
        double dur = ms_per_beat(app) / grid_div(app);
        undo_push(&app->project);
        deselect_layer(app);
        /* Use app->cur_vel (from slider) and app->cur_ntype (persisted across draws) */
        {
            /* Use the first technique of the instrument if no note is selected */
            if (app->project.num_layers > 0) {
                MuseLayer *lyr = &app->project.layers[app->project.active_layer];
                const MuseInstrument *instr = inst_by_id(lyr->inst_id);
                if (instr && instr->num_techniques > 0) {
                    bool valid = false;
                    for (int t = 0; t < instr->num_techniques; t++) {
                        if (instr->techniques[t] == app->cur_ntype) { valid = true; break; }
                    }
                    if (!valid) app->cur_ntype = instr->techniques[0];
                }
            }
        }
        /* Skip spacer keys on drum instruments */
        if (app->project.num_layers > 0) {
            MuseLayer *ly = &app->project.layers[app->project.active_layer];
            if (inst_is_spacer_key(ly->inst_id, (uint8_t)pitch)) goto skip_note_place;
        }
        MuseNote note = {
            .pitch = (uint8_t)pitch,
            .vel   = app->cur_vel,
            .ntype = app->cur_ntype,
            .selected = 1,
            .start = ms,
            .dur   = dur,
        };
        note_array_push(na, note);
        app->project.dirty = true;

        /* Auto-expand measures to fit the note */
        {
            int bpm = app->project.bpm ? app->project.bpm : 120;
            int ts = app->project.time_sig ? app->project.time_sig : 4;
            double mpb = 60000.0 / bpm;
            double note_end_ms = ms + dur;
            int needed = (int)(note_end_ms / (ts * mpb)) + 2;
            if (needed > app->measures) app->measures = needed;
        }

        /* Play audio preview of the placed note */
        if (app->project.num_layers > 0) {
            MuseLayer *ly = &app->project.layers[app->project.active_layer];
            muse_audio_preview(pitch, app->cur_vel, 500, ly->inst_id, app->cur_ntype, ly->synth_profile);
        }

        /* Immediately start resizing the new note */
        app->resizing_note = true;
        app->resize_note_idx = na->count - 1;
        app->resize_note_layer = app->project.active_layer;
        app->drag_start_x = mx;
    skip_note_place: ;
    }
}

static void handle_mouse_motion(MuseApp *app, float mx, float my) {
    app->mouse_x = mx;
    app->mouse_y = my;

    /* Scrollbar drag */
    if (app->sb_dragging > 0) {
        if (app->sb_dragging == 1) {
            /* Vertical scrollbar */
            float ry = app_roll_y(app), rh = app_roll_h(app);
            int max_scroll = NUM_PITCHES - (int)(rh / app->key_height);
            if (max_scroll < 0) max_scroll = 0;
            float delta = my - app->sb_drag_start;
            float frac_delta = delta / rh;
            app->scroll_y = (int)(app->sb_drag_orig + frac_delta * max_scroll);
            if (app->scroll_y < 0) app->scroll_y = 0;
            if (app->scroll_y > max_scroll) app->scroll_y = max_scroll;
        } else if (app->sb_dragging == 2) {
            /* Horizontal scrollbar */
            int bpm = app->project.bpm ? app->project.bpm : 120;
            double mpb = 60000.0 / bpm;
            int beats_per_measure = app->project.time_sig ? app->project.time_sig : 4;
            double total_ms = app->measures * beats_per_measure * mpb;
            float track_w = app_roll_w(app) - 14;
            float delta = mx - app->sb_drag_start;
            float frac_delta = delta / track_w;
            app->scroll_x = app->sb_drag_orig + frac_delta * total_ms;
            if (app->scroll_x < 0) app->scroll_x = 0;
        } else if (app->sb_dragging == 3) {
            /* Left panel scrollbar */
            float lp_y = (float)(TRANSPORT_H + 2);
            float lp_h = (float)(app->win_h - TRANSPORT_H - 2 - STATUS_BAR_H);
            float track_h = lp_h - 8;
            float content_h = app->left_panel_scroll_max;
            float max_scroll = content_h - lp_h;
            if (max_scroll < 0) max_scroll = 0;
            float visible_frac = lp_h / content_h;
            if (visible_frac > 1.0f) visible_frac = 1.0f;
            float thumb_h = track_h * visible_frac;
            if (thumb_h < 24) thumb_h = 24;
            float delta = my - app->sb_drag_start;
            float frac_delta = delta / (track_h - thumb_h);
            app->left_panel_scroll = (float)(app->sb_drag_orig + frac_delta * max_scroll);
            if (app->left_panel_scroll < 0) app->left_panel_scroll = 0;
            if (app->left_panel_scroll > max_scroll) app->left_panel_scroll = max_scroll;
        }
        return;
    }

    /* Velocity pane brush-select (shift+drag)  - selects stem lines */
    if (app->vel_brush_selecting) {
        NoteArray *na = app_active_notes(app);
        if (na) {
            float ry = app_roll_y(app), rh = app_roll_h(app);
            float vel_y_bs = ry + rh;
            float pad = 4;
            float y_base = vel_y_bs + VEL_PANE_H - pad;
            /* Select all stems whose x is near the cursor, regardless of y */
            for (int i = 0; i < na->count; i++) {
                if (!(na->notes[i].selected & 1)) continue;
                float nx = app_ms_to_x(app, na->notes[i].start);
                if (fabsf(mx - nx) < 6 &&
                    my >= vel_y_bs && my <= y_base) {
                    na->notes[i].selected |= 2;
                }
            }
        }
        return;
    }

    /* Shift+right-drag: set stems to mouse height as cursor passes them */
    if (app->vel_set_dragging) {
        NoteArray *na = app_active_notes(app);
        if (na) {
            float ry = app_roll_y(app), rh = app_roll_h(app);
            float vel_y = ry + rh;
            float pad = 4;
            float bar_h = VEL_PANE_H - 2 * pad;
            float y_base = vel_y + VEL_PANE_H - pad;
            float new_vel = (y_base - my) / bar_h * 127.0f;
            if (new_vel < 1) new_vel = 1;
            if (new_vel > 127) new_vel = 127;
            for (int i = 0; i < na->count; i++) {
                if (!(na->notes[i].selected & 1)) continue;
                float nx = app_ms_to_x(app, na->notes[i].start);
                if (fabsf(mx - nx) < 6)
                    na->notes[i].vel = (uint8_t)new_vel;
            }
            app->project.dirty = true;
        }
        return;
    }

    /* Velocity pane drag */
    if (app->vel_dragging) {
        NoteArray *na = app_active_notes(app);
        if (na && app->vel_drag_note_idx >= 0 && app->vel_drag_note_idx < na->count) {
            float ry = app_roll_y(app), rh = app_roll_h(app);
            float vel_y = ry + rh;
            float pad = 4;
            float bar_h = VEL_PANE_H - 2 * pad;
            float y_base = vel_y + VEL_PANE_H - pad;
            float new_vel = (y_base - my) / bar_h * 127.0f;
            if (new_vel < 1) new_vel = 1;
            if (new_vel > 127) new_vel = 127;
            if (app->vel_group_dragging) {
                /* Group drag: apply delta to all vel-selected stems */
                int delta = (int)new_vel - app->vel_group_drag_base;
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected & 2) {
                        int nv = na->notes[i].vel + delta;
                        if (nv < 1) nv = 1;
                        if (nv > 127) nv = 127;
                        na->notes[i].vel = (uint8_t)nv;
                    }
                }
                app->vel_group_drag_base = (int)new_vel;
            } else {
                na->notes[app->vel_drag_note_idx].vel = (uint8_t)new_vel;
            }
            app->project.dirty = true;
        }
        return;
    }

    /* Piano key drag preview */
    if (app->key_dragging) {
        int pitch = app_y_to_pitch(app, my);
        if (pitch >= PITCH_MIN && pitch <= PITCH_MAX &&
            pitch != app->key_last_preview &&
            app->project.num_layers > 0 &&
            !inst_is_spacer_key(app->project.layers[app->project.active_layer].inst_id, (uint8_t)pitch)) {
            MuseLayer *ly = &app->project.layers[app->project.active_layer];
            muse_audio_preview(pitch, 100, 500, ly->inst_id, app->cur_ntype, ly->synth_profile);
            app->key_last_preview = pitch;
        }
        app->key_highlight_pitch = app_y_to_pitch(app, my);
        return;
    }

    /* Left panel slider drag */
    if (app->lp_drag_slider > 0) {
        float frac = (mx - app->lp_slider_x) / app->lp_slider_w;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        if (app->lp_drag_slider == 1) {
            /* Volume slider */
            int idx = app->lp_drag_layer;
            if (idx >= 0 && idx < app->project.num_layers)
                app->project.layers[idx].volume = (uint8_t)(frac * 100);
        } else if (app->lp_drag_slider == 2) {
            /* velocity slider - new note default */
            app->cur_vel = (uint8_t)(1 + frac * 126);
        } else if (app->lp_drag_slider == 8) {
            /* per-note vel slider - update selected notes */
            uint8_t new_vel = (uint8_t)(1 + frac * 126);
            NoteArray *na = app_active_notes(app);
            if (na) {
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected & 1)
                        na->notes[i].vel = new_vel;
                }
                app->project.dirty = true;
            }
        } else if (app->lp_drag_slider >= 3 && app->lp_drag_slider <= 7) {
            /* FX sliders: 3=reverb, 4=delay, 5=cho_fb, 6=cho_depth, 7=cho_freq */
            int *fx_ptrs[] = {&app->fx_reverb, &app->fx_delay, &app->fx_chorus_fb,
                              &app->fx_chorus_depth, &app->fx_chorus_freq};
            *fx_ptrs[app->lp_drag_slider - 3] = (int)(frac * 100);
            app->project.effector_reverb       = (uint8_t)app->fx_reverb;
            app->project.effector_delay        = (uint8_t)app->fx_delay;
            app->project.effector_chorus_fb    = (uint8_t)app->fx_chorus_fb;
            app->project.effector_chorus_depth = (uint8_t)app->fx_chorus_depth;
            app->project.effector_chorus_freq  = (uint8_t)app->fx_chorus_freq;
        }
        return;
    }

    if (app->erase_sweeping) {
        /* Erase notes under cursor during sweep */
        float roll_rx = app_roll_x(app), roll_ry = app_roll_y(app);
        float roll_rw = app_roll_w(app), roll_rh = app_roll_h(app);
        if (mx >= roll_rx && mx < roll_rx + roll_rw &&
            my >= roll_ry && my < roll_ry + roll_rh) {
            bool edge = false;
            int idx = find_note_at(app, mx, my, &edge);
            if (idx >= 0) {
                NoteArray *na = app_active_notes(app);
                if (na) {
                    note_array_remove(na, idx);
                    app->project.dirty = true;
                    auto_expand_measures(app);
                }
            }
        }
        return;
    }

    /* Loop region drag (shift+drag on header) */
    if (app->dragging && app->looping && my < app_roll_y(app)) {
        double ms = app_x_to_ms(app, mx);
        if (ms < 0) ms = 0;
        double start_ms_hdr = app_x_to_ms(app, app->drag_start_x);
        if (ms < start_ms_hdr) {
            app->loop_start_ms = ms;
            app->loop_end_ms = start_ms_hdr;
        } else {
            app->loop_start_ms = start_ms_hdr;
            app->loop_end_ms = ms;
        }
        return;
    }

    if (app->panning) {
        double dx = mx - app->drag_start_x;
        double dy = my - app->drag_start_y;
        app->scroll_x = app->pan_start_scroll_x - dx / beat_px(app) * ms_per_beat(app);
        if (app->scroll_x < 0) app->scroll_x = 0;
        app->scroll_y = app->pan_start_scroll_y - (int)(dy / app->key_height);
        if (app->scroll_y < 0) app->scroll_y = 0;
        return;
    }

    if (app->brush_selecting) {
        /* Brush select: additively select note under cursor */
        bool dummy_edge = false;
        int idx = find_note_at(app, mx, my, &dummy_edge);
        if (idx >= 0) {
            NoteArray *na = app_active_notes(app);
            if (na && !na->notes[idx].selected)
                na->notes[idx].selected = 1;
        }
        return;
    }

    if (app->lasso_selecting && app->lasso_points) {
        /* Append point to lasso polygon */
        if (app->lasso_count >= app->lasso_capacity) {
            app->lasso_capacity *= 2;
            app->lasso_points = realloc(app->lasso_points,
                                        app->lasso_capacity * 2 * sizeof(float));
        }
        if (app->lasso_points) {
            app->lasso_points[app->lasso_count * 2] = mx;
            app->lasso_points[app->lasso_count * 2 + 1] = my;
            app->lasso_count++;
        }
        return;
    }

    if (app->selecting) {
        app->sel_x1 = mx;
        app->sel_y1 = my;
        /* live preview: highlight notes inside box while dragging */
        NoteArray *na = app_active_notes(app);
        if (na) {
            float x0 = app->sel_x0 < mx ? app->sel_x0 : mx;
            float x1 = app->sel_x0 < mx ? mx : app->sel_x0;
            float y0 = app->sel_y0 < my ? app->sel_y0 : my;
            float y1 = app->sel_y0 < my ? my : app->sel_y0;
            double ms0 = app_x_to_ms(app, x0);
            double ms1 = app_x_to_ms(app, x1);
            int p_hi = app_y_to_pitch(app, y0);
            int p_lo = app_y_to_pitch(app, y1);
            for (int i = 0; i < na->count; i++) {
                MuseNote *n = &na->notes[i];
                bool in_box = (n->start + n->dur >= ms0 && n->start <= ms1 &&
                               n->pitch >= p_lo && n->pitch <= p_hi);
                n->selected = in_box ? 1 : 0;
            }
        }
        return;
    }

    if (app->resizing_note) {
        NoteArray *na = app_active_notes(app);
        if (na && app->resize_note_idx >= 0 && app->resize_note_idx < na->count) {
            MuseNote *n = &na->notes[app->resize_note_idx];
            double end_ms = app_snap_ms_64(app, app_x_to_ms(app, mx));
            double min_dur = ms_per_beat(app) / 16;
            double new_dur = end_ms - n->start;
            if (new_dur < min_dur) new_dur = min_dur;
            /* apply duration delta to all selected notes */
            double dur_delta = new_dur - n->dur;
            n->dur = new_dur;
            for (int i = 0; i < na->count; i++) {
                if (i == app->resize_note_idx) continue;
                if (!na->notes[i].selected) continue;
                na->notes[i].dur += dur_delta;
                if (na->notes[i].dur < min_dur) na->notes[i].dur = min_dur;
            }
            /* Auto-expand measures */
            {
                int bpm = app->project.bpm ? app->project.bpm : 120;
                int ts = app->project.time_sig ? app->project.time_sig : 4;
                double end_ms = n->start + n->dur;
                int needed = (int)(end_ms / (ts * 60000.0 / bpm)) + 2;
                if (needed > app->measures) app->measures = needed;
            }
        }
        return;
    }

    if (app->moving_notes) {
        NoteArray *na = app_active_notes(app);
        if (!na) return;
        double dt = app_x_to_ms(app, mx) - app_x_to_ms(app, app->move_anchor_x);
        /* Snap time delta to 1/64 grid */
        dt = app_snap_ms_64(app, dt);
        int dp = app_y_to_pitch(app, my) - app_y_to_pitch(app, app->move_anchor_y);

        /* Apply delta from originals (no float drift from incremental updates) */
        int oi = 0;
        for (int i = 0; i < na->count; i++) {
            if (!na->notes[i].selected) continue;
            if (oi < app->move_orig_count) {
                na->notes[i].start = app->move_orig_starts[oi] + dt;
                na->notes[i].pitch = (uint8_t)(app->move_orig_pitches[oi] + dp);
                oi++;
            }
        }
        app->move_time_offset = dt;
        app->move_pitch_offset = dp;
        return;
    }

    /* Update mouse cursor based on tool and hover state */
    {
        static SDL_SystemCursor last_cid = SDL_SYSTEM_CURSOR_DEFAULT;
        float roll_rx = app_roll_x(app), roll_ry = app_roll_y(app);
        float roll_rw = app_roll_w(app), roll_rh = app_roll_h(app);
        bool in_roll = (mx >= roll_rx && mx < roll_rx + roll_rw &&
                        my >= roll_ry && my < roll_ry + roll_rh);
        SDL_SystemCursor cid = SDL_SYSTEM_CURSOR_DEFAULT;
        if (in_roll) {
            if (app->tool == TOOL_DRAW) {
                cid = SDL_SYSTEM_CURSOR_CROSSHAIR;
            } else if (app->tool == TOOL_ERASE) {
                cid = SDL_SYSTEM_CURSOR_NOT_ALLOWED;
            } else { /* TOOL_SELECT */
                bool edge = false;
                int idx = find_note_at(app, mx, my, &edge);
                if (idx >= 0 && edge)
                    cid = SDL_SYSTEM_CURSOR_EW_RESIZE;
                else if (idx >= 0)
                    cid = SDL_SYSTEM_CURSOR_MOVE;
            }
        }
        if (cid != last_cid) {
            if (g_cursors[cid]) SDL_SetCursor(g_cursors[cid]);
            last_cid = cid;
        }
    }
}

static void handle_mouse_up(MuseApp *app, int button) {
    if (app->sb_dragging > 0 && button == SDL_BUTTON_LEFT) {
        app->sb_dragging = 0;
        return;
    }

    if (app->vel_brush_selecting && button == SDL_BUTTON_LEFT) {
        app->vel_brush_selecting = false;
        return;
    }

    if (app->vel_set_dragging && button == SDL_BUTTON_RIGHT) {
        app->vel_set_dragging = false;
        return;
    }

    if (app->vel_dragging && button == SDL_BUTTON_LEFT) {
        app->vel_dragging = false;
        app->vel_group_dragging = false;
        return;
    }

    if (app->key_dragging && button == SDL_BUTTON_LEFT) {
        app->key_dragging = false;
        app->key_last_preview = -1;
        app->key_highlight_pitch = -1;
        return;
    }

    if (app->lp_drag_slider > 0 && button == SDL_BUTTON_LEFT) {
        app->lp_drag_slider = 0;
        return;
    }

    if (app->erase_sweeping && (button == SDL_BUTTON_RIGHT || button == SDL_BUTTON_LEFT)) {
        app->erase_sweeping = false;
        return;
    }

    if (button == SDL_BUTTON_MIDDLE) {
        app->panning = false;
        return;
    }

    /* Loop drag release */
    if (app->dragging && app->looping) {
        app->dragging = false;
        if (app->loop_end_ms - app->loop_start_ms < 1) {
            /* Trivial drag  - clear loop */
            app->looping = false;
        }
        return;
    }

    if (app->brush_selecting) {
        app->brush_selecting = false;
        return;
    }

    if (app->lasso_selecting) {
        /* Finish lasso: select notes whose center is inside the polygon */
        app->lasso_selecting = false;
        if (app->lasso_count >= 3 && app->lasso_points) {
            NoteArray *na = app_active_notes(app);
            if (na) {
                for (int i = 0; i < na->count; i++) {
                    MuseNote *n = &na->notes[i];
                    float cx = app_ms_to_x(app, n->start + n->dur * 0.5);
                    float cy = app_pitch_to_y(app, n->pitch) + app->key_height * 0.5f;
                    /* Ray-casting point-in-polygon test */
                    bool inside = false;
                    int npts = app->lasso_count;
                    int j = npts - 1;
                    for (int k = 0; k < npts; k++) {
                        float xi = app->lasso_points[k * 2], yi = app->lasso_points[k * 2 + 1];
                        float xj = app->lasso_points[j * 2], yj = app->lasso_points[j * 2 + 1];
                        if (((yi > cy) != (yj > cy)) &&
                            (cx < (xj - xi) * (cy - yi) / (yj - yi) + xi))
                            inside = !inside;
                        j = k;
                    }
                    n->selected = inside ? 1 : 0;
                }
            }
        }
        return;
    }

    if (app->selecting) {
        /* Select notes within box */
        NoteArray *na = app_active_notes(app);
        if (na) {
            float x0 = app->sel_x0 < app->sel_x1 ? app->sel_x0 : app->sel_x1;
            float x1 = app->sel_x0 < app->sel_x1 ? app->sel_x1 : app->sel_x0;
            float y0 = app->sel_y0 < app->sel_y1 ? app->sel_y0 : app->sel_y1;
            float y1 = app->sel_y0 < app->sel_y1 ? app->sel_y1 : app->sel_y0;
            double ms0 = app_x_to_ms(app, x0);
            double ms1 = app_x_to_ms(app, x1);
            int p_hi = app_y_to_pitch(app, y0);
            int p_lo = app_y_to_pitch(app, y1);
            for (int i = 0; i < na->count; i++) {
                MuseNote *n = &na->notes[i];
                if (n->start + n->dur >= ms0 && n->start <= ms1 &&
                    n->pitch >= p_lo && n->pitch <= p_hi)
                    n->selected = 1;
            }
        }
        app->selecting = false;
    }

    if (app->resizing_note) {
        /* Snap final duration */
        NoteArray *na = app_active_notes(app);
        if (na && app->resize_note_idx >= 0 && app->resize_note_idx < na->count) {
            MuseNote *n = &na->notes[app->resize_note_idx];
            n->dur = app_snap_ms_64(app, n->start + n->dur) - n->start;
            double min_dur = ms_per_beat(app) / 16;
            if (n->dur < min_dur) n->dur = min_dur;
        }
        app->resizing_note = false;
        app->resize_note_idx = -1;
        restart_playback_if_playing(app);
    }

    if (app->moving_notes) {
        /* Snap positions */
        NoteArray *na = app_active_notes(app);
        if (na) {
            int lo = PITCH_MIN, hi = PITCH_MAX;
            if (app->project.num_layers > 0)
                inst_pitch_range(app->project.layers[app->project.active_layer].inst_id, &lo, &hi);
            for (int i = 0; i < na->count; i++) {
                if (!na->notes[i].selected) continue;
                na->notes[i].start = app_snap_ms(app, na->notes[i].start);
                if (na->notes[i].start < 0) na->notes[i].start = 0;
                if (na->notes[i].pitch < lo) na->notes[i].pitch = (uint8_t)lo;
                if (na->notes[i].pitch > hi) na->notes[i].pitch = (uint8_t)hi;
                /* Snap to valid drum key if drum instrument */
                if (app->project.num_layers > 0)
                    na->notes[i].pitch = (uint8_t)snap_to_drum_pitch(
                        app->project.layers[app->project.active_layer].inst_id,
                        na->notes[i].pitch);
            }
        }
        app->moving_notes = false;
        app->move_time_offset = 0;
        app->move_pitch_offset = 0;
        free(app->move_orig_starts);  app->move_orig_starts  = NULL;
        free(app->move_orig_pitches); app->move_orig_pitches = NULL;
        app->move_orig_count = 0;
        auto_expand_measures(app);
        restart_playback_if_playing(app);
    }
}

static void handle_scroll(MuseApp *app, float dx, float dy) {
    (void)dx;
    /* Left panel scroll */
    if (app->left_panel_open && app->mouse_x < LEFT_PANEL_W &&
        app->mouse_y > TRANSPORT_H) {
        app->left_panel_scroll -= dy * 20;
        if (app->left_panel_scroll < 0) app->left_panel_scroll = 0;
        float max_s = app->left_panel_scroll_max -
                      (float)(app->win_h - TRANSPORT_H - STATUS_BAR_H);
        if (max_s < 0) max_s = 0;
        if (app->left_panel_scroll > max_s) app->left_panel_scroll = max_s;
        return;
    }

    SDL_Keymod mod = SDL_GetModState();
    bool shift = mod & SDL_KMOD_SHIFT;
    bool ctrl  = mod & SDL_KMOD_CTRL;
    bool alt   = mod & SDL_KMOD_ALT;

    /* Shift = h-zoom, Alt = v-zoom, Ctrl = h-scroll, plain = v-scroll */
    if (shift && !ctrl && !alt) {
        /* h-zoom anchored at mouse */
        double factor = (dy > 0) ? 1.25 : (1.0 / 1.25);
        double ms_at_mouse = app_x_to_ms(app, app->mouse_x);
        app->zoom_x *= factor;
        if (app->zoom_x < 0.1) app->zoom_x = 0.1;
        if (app->zoom_x > 8.0) app->zoom_x = 8.0;
        double new_ms = app_x_to_ms(app, app->mouse_x);
        app->scroll_x += ms_at_mouse - new_ms;
        if (app->scroll_x < 0) app->scroll_x = 0;
    } else if (alt && !ctrl) {
        /* v-zoom anchored at mouse */
        double factor = (dy > 0) ? 1.25 : (1.0 / 1.25);
        int old_h = app->key_height;
        int new_h = (int)(old_h * factor + 0.5);
        if (new_h < KEY_HEIGHT_MIN) new_h = KEY_HEIGHT_MIN;
        if (new_h > KEY_HEIGHT_MAX) new_h = KEY_HEIGHT_MAX;
        if (new_h != old_h) {
            /* Adjust scroll_y to keep pitch under cursor stable */
            int pitch_at_mouse = app_y_to_pitch(app, app->mouse_y);
            app->key_height = new_h;
            /* Recalculate what scroll_y should be to keep pitch_at_mouse at same screen position */
            float ry = app_roll_y(app);
            int target_row = PITCH_MAX - pitch_at_mouse;
            float screen_offset = app->mouse_y - ry;
            int new_scroll = target_row - (int)(screen_offset / new_h);
            if (new_scroll < 0) new_scroll = 0;
            app->scroll_y = new_scroll;
        }
    } else if (ctrl && !shift) {
        /* Horizontal scroll */
        app->scroll_x += (dy > 0 ? -1 : 1) * ms_per_beat(app) * 2;
        if (app->scroll_x < 0) app->scroll_x = 0;
    } else {
        /* Vertical scroll */
        app->scroll_y += (dy > 0) ? -2 : 2;
        if (app->scroll_y < 0) app->scroll_y = 0;
        int max_scroll = NUM_PITCHES - (int)(app_roll_h(app) / app->key_height);
        if (max_scroll < 0) max_scroll = 0;
        if (app->scroll_y > max_scroll) app->scroll_y = max_scroll;
    }
}

/* total items in instrument picker */
static int picker_total_items(void) {
    /* Must match INST_GROUPS in ui_piano_roll.c */
    static const int counts[] = {8, 10, 4, 4};
    int total = 0;
    for (int i = 0; i < 4; i++) total += counts[i];
    return total;
}

/* Get instrument ID by flat index in picker */
static uint8_t picker_id_at(int idx) {
    static const uint8_t groups[][16] = {
        {0x00,0x01,0x02,0x04,0x05,0x06,0x07,0x08},
        {0x0A,0x0B,0x0D,0x0F,0x10,0x11,0x12,0x13,0x27,0x28},
        {0x14,0x18,0x1C,0x20},
        {0x0E,0x24,0x25,0x26},
    };
    static const int counts[] = {8, 10, 4, 4};
    int off = 0;
    for (int g = 0; g < 4; g++) {
        if (idx < off + counts[g]) return groups[g][idx - off];
        off += counts[g];
    }
    return 0;
}

/* check if instrument already in use (exclude_layer for "change inst" mode) */
bool inst_already_used(const MuseProject *p, uint8_t inst_id, int exclude_layer) {
    for (int i = 0; i < p->num_layers; i++) {
        if (i == exclude_layer) continue;
        if (p->layers[i].inst_id == inst_id) return true;
    }
    return false;
}

/* instrument picker event handler, returns true if consumed */
static bool handle_picker_event(MuseApp *app, const SDL_Event *ev) {
    if (!app->picker_open) return false;

    float dw = 300, dh = 420;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;
    float hdr_h = 32;
    float content_y = dy + hdr_h + 4;
    float content_h = dh - hdr_h - 12;

    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
        app->picker_open = false;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        /* Calculate total content height */
        static const int grp_counts[] = {8, 10, 4, 4};
        float total_h = 0;
        for (int g = 0; g < 4; g++) {
            total_h += 20; /* group header */
            total_h += grp_counts[g] * 28; /* items (26 + 2 spacing) */
            total_h += 6; /* group spacing */
        }
        float max_scroll = total_h - content_h;
        if (max_scroll < 0) max_scroll = 0;
        app->picker_scroll -= ev->wheel.y * 20;
        if (app->picker_scroll < 0) app->picker_scroll = 0;
        if (app->picker_scroll > max_scroll) app->picker_scroll = max_scroll;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        float mx = ev->motion.x, my = ev->motion.y;
        app->mouse_x = mx; app->mouse_y = my;
        /* Hit test picker items */
        app->picker_hover = -1;
        if (mx >= dx + 10 && mx < dx + dw - 10 &&
            my >= content_y && my < content_y + content_h) {
            /* Calculate which item we're hovering */
            float cy = content_y - app->picker_scroll;
            int item = 0;
            static const int grp_counts[] = {8, 10, 4, 4};
            for (int g = 0; g < 4; g++) {
                cy += 20; /* group header */
                for (int i = 0; i < grp_counts[g]; i++) {
                    float btn_y = cy;
                    float btn_h = 26;
                    if (my >= btn_y && my < btn_y + btn_h) {
                        app->picker_hover = item;
                        return true;
                    }
                    cy += btn_h + 2;
                    item++;
                }
                cy += 6;
            }
        }
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        float mx = ev->button.x, my = ev->button.y;

        /* Close button */
        if (mx >= dx + dw - 28 && mx < dx + dw && my >= dy && my < dy + hdr_h) {
            app->picker_open = false;
            return true;
        }

        /* Click outside dialog = close */
        if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
            app->picker_open = false;
            return true;
        }

        /* Click on a hovered instrument */
        if (app->picker_hover >= 0 && app->picker_hover < picker_total_items()) {
            uint8_t inst_id = picker_id_at(app->picker_hover);
            /* block duplicates - each inst can only appear once */
            int exclude = (app->picker_mode == 2) ? app->project.active_layer : -1;
            if (inst_already_used(&app->project, inst_id, exclude)) {
                return true; /* already in use, ignore click */
            }
            if (app->picker_mode == 0) {
                /* Add new instrument layer */
                muse_project_add_layer(&app->project, inst_id);
                app->project.active_layer = app->project.num_layers - 1;
            } else if (app->picker_mode == 2) {
                /* change active layer's instrument */
                if (app->project.num_layers > 0) {
                    MuseLayer *ly = &app->project.layers[app->project.active_layer];
                    ly->inst_id = inst_id;
                    app->project.dirty = true;
                }
            } else {
                /* Move selected notes to this instrument */
                NoteArray *na = app_active_notes(app);
                int src_layer = app->project.active_layer;
                if (na) {
                    /* Find or create target layer */
                    int target = -1;
                    for (int i = 0; i < app->project.num_layers; i++) {
                        if (app->project.layers[i].inst_id == inst_id) { target = i; break; }
                    }
                    if (target < 0) {
                        /* Save sends before realloc invalidates pointers */
                        uint8_t sv_vol   = app->project.layers[src_layer].volume;
                        uint8_t sv_rev   = app->project.layers[src_layer].reverb_send;
                        uint8_t sv_dly   = app->project.layers[src_layer].delay_send;
                        uint8_t sv_cho   = app->project.layers[src_layer].chorus_send;
                        muse_project_add_layer(&app->project, inst_id);
                        target = app->project.num_layers - 1;
                        /* Copy per-instrument effect sends from source */
                        app->project.layers[target].volume      = sv_vol;
                        app->project.layers[target].reverb_send = sv_rev;
                        app->project.layers[target].delay_send  = sv_dly;
                        app->project.layers[target].chorus_send = sv_cho;
                    }
                    /* Move selected notes */
                    MuseLayer *dst = &app->project.layers[target];
                    NoteArray *dst_na = &dst->sublayers[dst->active_sub];
                    int mv_lo, mv_hi;
                    inst_pitch_range(inst_id, &mv_lo, &mv_hi);
                    for (int i = na->count - 1; i >= 0; i--) {
                        if (na->notes[i].selected) {
                            MuseNote n = na->notes[i];
                            n.selected = 0;
                            /* Validate ntype for target instrument */
                            if (!inst_has_technique(inst_id, n.ntype))
                                n.ntype = 0;
                            /* Keep original pitch  - don't clamp to range */
                            note_array_push(dst_na, n);
                            /* Remove from source */
                            memmove(&na->notes[i], &na->notes[i + 1],
                                    (na->count - i - 1) * sizeof(MuseNote));
                            na->count--;
                        }
                    }
                    /* Remove source layer if it's now empty */
                    if (src_layer != target && muse_layer_note_count(&app->project.layers[src_layer]) == 0) {
                        muse_project_remove_layer(&app->project, src_layer);
                        /* Adjust target index if it shifted */
                        if (target > src_layer) target--;
                    }
                    app->project.active_layer = target;
                }
            }
            app->picker_open = false;
            return true;
        }
        return true; /* consume click even if not on item */
    }

    /* Consume all other events when picker is open */
    return (ev->type == SDL_EVENT_MOUSE_BUTTON_UP);
}

/* aux send dialog */
static bool handle_aux_event(MuseApp *app, const SDL_Event *ev) {
    if (!app->aux_open) return false;
    if (app->aux_layer < 0 || app->aux_layer >= app->project.num_layers) {
        app->aux_open = false;
        return true;
    }
    MuseLayer *ly = &app->project.layers[app->aux_layer];

    float dw = 300, dh = 200;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;
    float hdr_h = 28;

    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
        app->aux_open = false;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        float mx = ev->motion.x, my = ev->motion.y;
        app->mouse_x = mx; app->mouse_y = my;
        /* Drag slider */
        if (app->aux_drag_slider > 0) {
            float frac = (mx - app->_aux_slider_x - 10) / (app->_aux_slider_w - 20);
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int val = (int)(frac * 100 + 0.5f);
            uint8_t *vals[] = {&ly->reverb_send, &ly->delay_send, &ly->chorus_send};
            *vals[app->aux_drag_slider - 1] = (uint8_t)val;
        }
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        float mx = ev->button.x, my = ev->button.y;

        /* Close button */
        if (mx >= dx + dw - 28 && mx < dx + dw && my >= dy && my < dy + hdr_h) {
            app->aux_open = false;
            return true;
        }

        /* Click outside = close */
        if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
            app->aux_open = false;
            return true;
        }

        /* OK button */
        float ok_y = dy + hdr_h + 12 + 3 * 40 + 4;
        UiRect ok_btn = { dx + dw / 2 - 40, ok_y, 80, 28 };
        if (ui_rect_contains(ok_btn, mx, my)) {
            app->aux_open = false;
            return true;
        }

        /* Slider clicks */
        for (int i = 0; i < 3; i++) {
            if (ui_rect_contains(app->_aux_sliders[i], mx, my)) {
                app->aux_drag_slider = i + 1;
                float frac = (mx - app->_aux_slider_x - 10) / (app->_aux_slider_w - 20);
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                uint8_t *vals[] = {&ly->reverb_send, &ly->delay_send, &ly->chorus_send};
                *vals[i] = (uint8_t)(frac * 100 + 0.5f);
                return true;
            }
        }
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        app->aux_drag_slider = 0;
        return true;
    }

    return false;
}

/* dropdown event handler */
static bool handle_dropdown_event(MuseApp *app, const SDL_Event *ev) {
    if (app->dropdown_open == 0) return false;

    float item_h = 28;
    float menu_w = app->dropdown_anchor.w;
    if (menu_w < 80) menu_w = 80;

    int n_items = 0;
    if (app->dropdown_open == 1) n_items = 5;
    else if (app->dropdown_open == 2) n_items = 6;
    else if (app->dropdown_open == 4) n_items = 4;
    else if (app->dropdown_open == 3) {
        if (app->project.num_layers > 0) {
            const MuseInstrument *ains = inst_by_id(
                app->project.layers[app->project.active_layer].inst_id);
            if (ains) n_items = ains->num_techniques;
        }
        if (n_items == 0) n_items = 1;
    }

    float menu_h = n_items * item_h + 4;
    float dmx = app->dropdown_anchor.x;
    float dmy = app->dropdown_anchor.y + app->dropdown_anchor.h + 2;
    if (dmy + menu_h > app->win_h) dmy = app->dropdown_anchor.y - menu_h - 2;

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        float mx = ev->motion.x, my = ev->motion.y;
        app->dropdown_hover = -1;
        if (mx >= dmx && mx < dmx + menu_w && my >= dmy && my < dmy + menu_h) {
            int idx = (int)((my - dmy - 2) / item_h);
            if (idx >= 0 && idx < n_items) app->dropdown_hover = idx;
        }
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        float mx = ev->button.x, my = ev->button.y;
        if (mx >= dmx && mx < dmx + menu_w && my >= dmy && my < dmy + menu_h) {
            int idx = (int)((my - dmy - 2) / item_h);
            if (idx >= 0 && idx < n_items) {
                if (app->dropdown_open == 1) {
                    const int ts_vals[] = {2, 3, 4, 6, 8};
                    app->project.time_sig = ts_vals[idx];
                } else if (app->dropdown_open == 2) {
                    app->grid_snap = idx; /* GRID_1_4..GRID_FREE */
                } else if (app->dropdown_open == 3) {
                    /* Apply technique to selected notes */
                    if (app->project.num_layers > 0) {
                        const MuseInstrument *ains = inst_by_id(
                            app->project.layers[app->project.active_layer].inst_id);
                        if (ains && idx < ains->num_techniques) {
                            uint8_t ntype = ains->techniques[idx];
                            app->cur_ntype = ntype; /* persist for new notes + preview */
                            NoteArray *na = app_active_notes(app);
                            if (na) {
                                for (int i = 0; i < na->count; i++)
                                    if (na->notes[i].selected)
                                        na->notes[i].ntype = ntype;
                                app->project.dirty = true;
                            }
                        }
                    }
                } else if (app->dropdown_open == 4) {
                    /* Marnian synth profile */
                    if (app->project.num_layers > 0) {
                        app->project.layers[app->project.active_layer].synth_profile = (uint8_t)idx;
                    }
                }
            }
        }
        app->dropdown_open = 0;
        return true;
    }

    /* Escape or click outside to close */
    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
        app->dropdown_open = 0;
        return true;
    }

    return true; /* consume all events while dropdown is open */
}

/* Must match INST_GROUPS in ui_piano_roll.c */
typedef struct { const char *group; int ids[16]; int count; } MidiInstGroup;
static const MidiInstGroup INST_GROUPS[] = {
    {"Beginner", {0x00,0x01,0x02,0x04,0x05,0x06,0x07,0x08}, 8},
    {"Florchestra", {0x0A,0x0B,0x0D,0x0F,0x10,0x11,0x12,0x13,0x27,0x28}, 10},
    {"Marnian", {0x14,0x18,0x1C,0x20}, 4},
    {"Electric Guitar & Bass", {0x0E,0x24,0x25,0x26}, 4},
};
#define INST_GROUP_COUNT 4

static bool handle_midi_dlg_event(MuseApp *app, const SDL_Event *ev) {
    if (!app->midi_dlg_open || !app->midi_dlg_data) return false;
    MidiImportData *mid = (MidiImportData *)app->midi_dlg_data;

    float dw = 500, dh = 430;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;

    float content_y = dy + 50;
    if (mid->tempo_changes > 1) {
        content_y += 16;
    }
    float combine_cb_y = content_y;
    content_y += 22;
    content_y += 16; /* column headers */
    float list_h = dh - (content_y - dy) - 50;
    float row_h = 32;

    float btn_y = dy + dh - 42;
    UiRect import_btn = { dx + dw - 190, btn_y, 80, 30 };
    UiRect cancel_btn = { dx + dw - 100, btn_y, 80, 30 };

    /* MIDI instrument dropdown overlay handling (when open, consumes events first) */
    /* dropdown_ch: -2 = combine instrument, >= 0 = per-channel, -1 = closed */
    if (app->midi_dlg_dropdown_ch >= -2 && app->midi_dlg_dropdown_ch != -1) {
        float dd_x;
        float dd_anchor_y;
        if (app->midi_dlg_dropdown_ch == -2) {
            dd_x = dx + 180;
            dd_anchor_y = dy + 50 + (mid->tempo_changes > 1 ? 36 : 0) + 22;
        } else {
            float dd_row_y = content_y + app->midi_dlg_dropdown_ch * row_h - app->midi_dlg_scroll;
            dd_x = dx + 260;
            dd_anchor_y = dd_row_y + row_h + 2;
        }
        float dd_w = 220;
        float dd_item_h = 24;
        float dd_hdr_h = 18;
        /* compute total height matching render */
        float dd_total_h = 4;
        for (int g = 0; g < INST_GROUP_COUNT; g++) {
            dd_total_h += dd_hdr_h;
            dd_total_h += INST_GROUPS[g].count * dd_item_h;
            if (g < INST_GROUP_COUNT - 1) dd_total_h += 4;
        }
        float dd_y = dd_anchor_y;
        if (dd_y + dd_total_h > dy + dh) dd_y = dd_anchor_y - dd_total_h - row_h - 4;
        if (dd_y < 0) dd_y = 0;
        float dd_max_h = (float)app->win_h - dd_y - 10;
        float dd_h = dd_total_h > dd_max_h ? dd_max_h : dd_total_h;

        if (ev->type == SDL_EVENT_MOUSE_MOTION) {
            float mx = ev->motion.x, my = ev->motion.y;
            app->midi_dlg_dropdown_hover = -1;
            if (mx >= dd_x && mx < dd_x + dd_w && my >= dd_y && my < dd_y + dd_h) {
                /* walk through groups to find which item is hovered */
                float cy2 = dd_y + 2 - app->midi_dlg_dropdown_scroll;
                int idx = 0;
                for (int g = 0; g < INST_GROUP_COUNT; g++) {
                    cy2 += dd_hdr_h; /* skip header */
                    for (int i = 0; i < INST_GROUPS[g].count; i++) {
                        if (my >= cy2 && my < cy2 + dd_item_h) {
                            app->midi_dlg_dropdown_hover = idx;
                        }
                        cy2 += dd_item_h;
                        idx++;
                    }
                    if (g < INST_GROUP_COUNT - 1) cy2 += 4;
                }
            }
            return true;
        }
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
            if (app->midi_dlg_dropdown_hover >= 0) {
                /* map flat index back to instrument id */
                int idx = 0;
                uint8_t sel_id = 0;
                bool found = false;
                for (int g = 0; g < INST_GROUP_COUNT && !found; g++) {
                    for (int i = 0; i < INST_GROUPS[g].count && !found; i++) {
                        if (idx == app->midi_dlg_dropdown_hover) {
                            sel_id = (uint8_t)INST_GROUPS[g].ids[i];
                            found = true;
                        }
                        idx++;
                    }
                }
                if (found) {
                    if (app->midi_dlg_dropdown_ch == -2) {
                        mid->combine_inst_id = sel_id;
                    } else {
                        mid->channels[app->midi_dlg_dropdown_ch].user_inst_id = sel_id;
                    }
                }
            }
            app->midi_dlg_dropdown_ch = -1;
            app->midi_dlg_dropdown_scroll = 0;
            return true;
        }
        if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
            app->midi_dlg_dropdown_scroll -= ev->wheel.y * 20;
            float max_dd_scroll = dd_total_h - dd_h;
            if (max_dd_scroll < 0) max_dd_scroll = 0;
            if (app->midi_dlg_dropdown_scroll < 0) app->midi_dlg_dropdown_scroll = 0;
            if (app->midi_dlg_dropdown_scroll > max_dd_scroll) app->midi_dlg_dropdown_scroll = max_dd_scroll;
            return true;
        }
        if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
            app->midi_dlg_dropdown_ch = -1;
            app->midi_dlg_dropdown_scroll = 0;
            return true;
        }
        return true; /* consume all events while inst dropdown is open */
    }

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        float mx = ev->motion.x, my = ev->motion.y;
        app->midi_dlg_hover = -1;
        app->midi_dlg_hover_btn = 0;

        /* Check channel rows */
        if (mx >= dx + 4 && mx < dx + dw - 4 && my >= content_y && my < content_y + list_h) {
            int idx = (int)((my - content_y + app->midi_dlg_scroll) / row_h);
            if (idx >= 0 && idx < mid->num_channels) app->midi_dlg_hover = idx;
        }

        /* Check buttons */
        if (ui_rect_contains(import_btn, mx, my)) app->midi_dlg_hover_btn = 1;
        if (ui_rect_contains(cancel_btn, mx, my)) app->midi_dlg_hover_btn = 2;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        (ev->button.button == SDL_BUTTON_LEFT || ev->button.button == SDL_BUTTON_RIGHT)) {
        float mx = ev->button.x, my = ev->button.y;

        /* Combine all checkbox */
        if (mx >= dx + 12 && mx < dx + 170 &&
            my >= combine_cb_y - 2 && my < combine_cb_y + 20) {
            mid->combine_all = !mid->combine_all;
            return true;
        }

        /* Combine instrument selector (only when combine_all is on) */
        if (mid->combine_all &&
            mx >= dx + 180 && mx < dx + 180 + 160 &&
            my >= combine_cb_y - 4 && my < combine_cb_y + 20) {
            app->midi_dlg_dropdown_ch = -2;
            app->midi_dlg_dropdown_hover = -1;
            app->midi_dlg_dropdown_scroll = 0;
            return true;
        }

        /* Click on instrument selector -> open dropdown for that channel */
        if (app->midi_dlg_hover >= 0 && app->midi_dlg_hover < mid->num_channels) {
            MidiChannel *hch = &mid->channels[app->midi_dlg_hover];
            bool ch_greyed = mid->combine_all && !hch->is_percussion;
            float ry = content_y + app->midi_dlg_hover * row_h - app->midi_dlg_scroll;
            if (!ch_greyed && mx >= dx + 260 && mx < dx + 260 + 180 && my >= ry && my < ry + row_h) {
                app->midi_dlg_dropdown_ch = app->midi_dlg_hover;
                app->midi_dlg_dropdown_hover = -1;
                app->midi_dlg_dropdown_scroll = 0;
                return true;
            }
        }

        /* Import button */
        if (ui_rect_contains(import_btn, mx, my)) {
            midi_apply(mid, &app->project);
            app->playhead_ms = 0;
            app->scroll_x = 0;
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

            {
                int total_notes = 0;
                for (int li = 0; li < app->project.num_layers; li++)
                    for (int si = 0; si < app->project.layers[li].num_sublayers; si++)
                        total_notes += app->project.layers[li].sublayers[si].count;
                set_status_msg(app, "Imported: %s (%d notes)", app->filename, total_notes);
            }
            midi_import_data_free(mid);
            app->midi_dlg_data = NULL;
            app->midi_dlg_open = false;
            return true;
        }

        /* Cancel button */
        if (ui_rect_contains(cancel_btn, mx, my)) {
            midi_import_data_free(mid);
            app->midi_dlg_data = NULL;
            app->midi_dlg_open = false;
            return true;
        }
        return true;
    }

    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
        midi_import_data_free(mid);
        app->midi_dlg_data = NULL;
        app->midi_dlg_open = false;
        return true;
    }

    /* Scroll the channel list */
    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        app->midi_dlg_scroll -= ev->wheel.y * 20;
        float max_scroll = mid->num_channels * row_h - list_h;
        if (max_scroll < 0) max_scroll = 0;
        if (app->midi_dlg_scroll < 0) app->midi_dlg_scroll = 0;
        if (app->midi_dlg_scroll > max_scroll) app->midi_dlg_scroll = max_scroll;
        return true;
    }

    return true; /* consume all events while dialog is open */
}

static bool handle_chord_event(MuseApp *app, const SDL_Event *ev) {
    if (!app->chord_open) return false;

    float dw = 320, dh = 480;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;
    float hdr_h = 32;
    float content_y = dy + hdr_h + 4;
    float content_h = dh - hdr_h - 12;

    if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
        app->chord_open = false;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
        app->chord_scroll -= ev->wheel.y * 20;
        if (app->chord_scroll < 0) app->chord_scroll = 0;
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_MOTION) {
        float mx = ev->motion.x, my = ev->motion.y;
        app->mouse_x = mx; app->mouse_y = my;
        app->chord_hover = -1;
        if (mx >= dx + 10 && mx < dx + dw - 10 &&
            my >= content_y && my < content_y + content_h) {
            float cy = content_y - app->chord_scroll;
            int item = 0;
            static const int grp_counts[] = {6,10,2,6,3,3,4,3};
            for (int g = 0; g < 8; g++) {
                cy += 20; /* group header */
                for (int i = 0; i < grp_counts[g]; i++) {
                    float btn_h = 24;
                    if (my >= cy && my < cy + btn_h) {
                        app->chord_hover = item;
                        return true;
                    }
                    cy += btn_h + 2;
                    item++;
                }
                cy += 6;
            }
        }
        return true;
    }

    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT) {
        float mx = ev->button.x, my = ev->button.y;

        /* Close button */
        if (mx >= dx + dw - 28 && mx < dx + dw && my >= dy && my < dy + hdr_h) {
            app->chord_open = false;
            return true;
        }
        /* Click outside */
        if (mx < dx || mx > dx + dw || my < dy || my > dy + dh) {
            app->chord_open = false;
            return true;
        }

        /* Click on chord */
        if (app->chord_hover >= 0 && app->chord_hover < chord_total_items()) {
            const ChordDef *cd = chord_at(app->chord_hover);
            if (cd) {
                /* apply chord: group notes by start time (~1ms tolerance),
                   find root (lowest pitch) per group, replace with chord intervals */
                NoteArray *na = app_active_notes(app);
                if (na) {
                    undo_push(&app->project);

                    /* Collect selected note indices */
                    int *sel_idx = NULL;
                    int sel_count = 0;
                    for (int ni = 0; ni < na->count; ni++) {
                        if (na->notes[ni].selected) {
                            sel_idx = realloc(sel_idx, (size_t)(sel_count + 1) * sizeof(int));
                            sel_idx[sel_count++] = ni;
                        }
                    }

                    /* Mark which selected notes have been processed */
                    bool *used = calloc((size_t)sel_count, sizeof(bool));

                    /* Group by start time, find root, build chord notes */
                    NoteArray new_notes;
                    note_array_init(&new_notes);

                    for (int gi = 0; gi < sel_count; gi++) {
                        if (used[gi]) continue;
                        double grp_start = na->notes[sel_idx[gi]].start;
                        /* Find all notes in this time group and the root */
                        int root_si = gi;
                        uint8_t root_pitch = na->notes[sel_idx[gi]].pitch;
                        for (int gj = gi + 1; gj < sel_count; gj++) {
                            if (used[gj]) continue;
                            double dt = na->notes[sel_idx[gj]].start - grp_start;
                            if (dt < -1.0 || dt > 1.0) continue;
                            used[gj] = true;
                            if (na->notes[sel_idx[gj]].pitch < root_pitch) {
                                root_pitch = na->notes[sel_idx[gj]].pitch;
                                root_si = gj;
                            }
                        }
                        used[gi] = true;

                        /* Use the root note's properties */
                        MuseNote root = na->notes[sel_idx[root_si]];
                        /* Create chord notes: root + each interval */
                        for (int k = 0; k < cd->count; k++) {
                            int new_pitch = root_pitch + cd->intervals[k];
                            if (new_pitch >= PITCH_MIN && new_pitch <= PITCH_MAX) {
                                MuseNote cn = root;
                                cn.pitch = (uint8_t)new_pitch;
                                cn.selected = 1;
                                note_array_push(&new_notes, cn);
                            }
                        }
                    }

                    /* Delete all originally selected notes (reverse order) */
                    for (int ni = na->count - 1; ni >= 0; ni--) {
                        if (na->notes[ni].selected)
                            note_array_remove(na, ni);
                    }

                    /* Add new chord notes */
                    for (int ni = 0; ni < new_notes.count; ni++)
                        note_array_push(na, new_notes.notes[ni]);

                    note_array_free(&new_notes);
                    free(sel_idx);
                    free(used);
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                }
            }
            app->chord_open = false;
            return true;
        }
        return true;
    }

    return (ev->type == SDL_EVENT_MOUSE_BUTTON_UP);
}

void muse_app_handle_event(MuseApp *app, const SDL_Event *ev) {
    /* startup overlay eats events when active */
    if (app->overlay_state > 0 && !app->overlay_dismiss) {
        if (ev->type == SDL_EVENT_QUIT) {
            app->running = false;
            return;
        }
        if (app->overlay_state == 2) {
            /* link account: handle drops and ESC */
            if (ev->type == SDL_EVENT_DROP_FILE && ev->drop.data) {
                const char *path = ev->drop.data;
                #define _LT 72
                FILE *df = fopen(path, "rb");
                if (df) {
                    fseek(df, 0, SEEK_END);
                    long fsz = ftell(df);
                    fseek(df, 0, SEEK_SET);
                    if (fsz >= 4 + _LT) {
                        uint8_t hdr[4 + _LT];
                        fread(hdr, 1, sizeof(hdr), df);
                        fclose(df);
                        uint32_t ver = hdr[0]|(hdr[1]<<8)|(hdr[2]<<16)|(hdr[3]<<24);
                        if (ver == 9 || ver == 6) {
                            uint8_t tok[_LT];
                            memcpy(tok, hdr + 4, _LT);
                            uint32_t oid = 0;
                            char cname[64] = {0};
                            int rc = bdo_extract_owner(path, &oid, cname, sizeof(cname));
                            if (rc == 0 && oid != 0) {
                                app->linked_owner_id = oid;
                                snprintf(app->char_name, sizeof(app->char_name), "%s", cname);
                                SDL_CreateDirectory("Save");
                                FILE *cfg = fopen("Save/account.cfg", "w");
                                if (cfg) {
                                    for (int ti = 0; ti < _LT; ti++)
                                        fprintf(cfg, "%02x", tok[ti]);
                                    fprintf(cfg, "\n");
                                    fclose(cfg);
                                }
                                app->overlay_dismiss = true;
                            }
                        }
                    } else {
                        fclose(df);
                    }
                }
                #undef _LT
            }
            if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
                app->overlay_dismiss = true;
            }
        } else if (app->overlay_state == 3) {
            /* disclaimer: any click/key dismisses */
            if (ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                app->overlay_dismiss = true;
            }
        }
        /* let window resize through */
        if (ev->type == SDL_EVENT_WINDOW_RESIZED) {
            app->win_w = ev->window.data1;
            app->win_h = ev->window.data2;
        }
        return;
    }

    /* shortcuts cheatsheet events (non-modal, doesn't block other interaction) */
    if (app->shortcuts_open) {
        float sw = 280, sh = 340;
        float sx = app->shortcuts_x, sy = app->shortcuts_y;
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            float mx = ev->button.x, my = ev->button.y;
            /* close button (top-right of title bar) */
            if (mx >= sx + sw - 24 && mx <= sx + sw && my >= sy && my <= sy + 26) {
                app->shortcuts_open = false;
                return;
            }
            /* start drag on title bar */
            if (mx >= sx && mx <= sx + sw && my >= sy && my <= sy + 26) {
                app->shortcuts_dragging = true;
                app->shortcuts_drag_ox = mx - sx;
                app->shortcuts_drag_oy = my - sy;
                return;
            }
        }
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && app->shortcuts_dragging) {
            app->shortcuts_dragging = false;
        }
        if (ev->type == SDL_EVENT_MOUSE_MOTION && app->shortcuts_dragging) {
            app->shortcuts_x = ev->motion.x - app->shortcuts_drag_ox;
            app->shortcuts_y = ev->motion.y - app->shortcuts_drag_oy;
            return;
        }
        if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE) {
            app->shortcuts_open = false;
            return;
        }
        /* scroll inside the cheatsheet - clamp immediately using cached max */
        if (ev->type == SDL_EVENT_MOUSE_WHEEL) {
            float mx = app->mouse_x, my = app->mouse_y;
            if (mx >= sx && mx <= sx + sw && my >= sy + 26 && my <= sy + sh) {
                app->shortcuts_scroll -= ev->wheel.y * 20;
                if (app->shortcuts_scroll < 0) app->shortcuts_scroll = 0;
                return;
            }
        }
    }

    /* shortcuts button click (in status bar) */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        float bw = text_width("Shortcuts", 10) + 16;
        float bh = STATUS_BAR_H - 4;
        float bx = (float)app->win_w - bw - 6;
        float by = (float)app->win_h - STATUS_BAR_H + 2;
        float mx = ev->button.x, my = ev->button.y;
        if (mx >= bx && mx <= bx + bw && my >= by && my <= by + bh) {
            app->shortcuts_open = !app->shortcuts_open;
            if (app->shortcuts_open) {
                /* position above the button */
                app->shortcuts_x = (float)app->win_w - 290;
                app->shortcuts_y = (float)app->win_h - STATUS_BAR_H - 350;
            }
            return;
        }
    }

    /* modal dialogs eat events when open */
    if (handle_dropdown_event(app, ev)) return;
    if (handle_midi_dlg_event(app, ev)) return;
    if (handle_picker_event(app, ev)) return;
    if (handle_aux_event(app, ev)) return;
    if (handle_chord_event(app, ev)) return;

    switch (ev->type) {
    case SDL_EVENT_QUIT:
        if (app->project.dirty) {
            SDL_MessageBoxButtonData btns[] = {
                { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
                { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Quit" },
            };
            SDL_MessageBoxData mbd = {
                .flags = SDL_MESSAGEBOX_WARNING,
                .window = app->window,
                .title = "Unsaved Changes",
                .message = "You have unsaved changes. Quit anyway?",
                .numbuttons = 2,
                .buttons = btns,
            };
            int btn = 0;
            if (!SDL_ShowMessageBox(&mbd, &btn) || btn != 1) break;
        }
        app->running = false;
        break;

    case SDL_EVENT_WINDOW_RESIZED:
        app->win_w = ev->window.data1;
        app->win_h = ev->window.data2;
        break;

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        app->focused = true;
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        app->focused = false;
        break;

    case SDL_EVENT_DROP_FILE:
        if (ev->drop.data) {
            ui_toolbar_open_path(app, ev->drop.data);
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        handle_mouse_motion(app, ev->motion.x, ev->motion.y);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        float mx = ev->button.x, my = ev->button.y;
        app->mouse_x = mx; app->mouse_y = my;

        /* Dismiss text entry if clicking outside the entry field */
        if (app->edit_field != 0 && ev->button.button == SDL_BUTTON_LEFT) {
            UiRect *er;
            if (app->edit_field == 1) er = &app->_tb_bpm_entry;
            else if (app->edit_field == 2) er = &app->_tb_meas_entry;
            else er = &app->_lp_fn_entry;
            if (!ui_rect_contains(*er, mx, my)) {
                /* Commit */
                if (app->edit_field == 1) {
                    int val = atoi(app->edit_buf);
                    if (val < 1) val = 1; if (val > 200) val = 200;
                    app->project.bpm = (uint16_t)val;
                } else if (app->edit_field == 2) {
                    int val = atoi(app->edit_buf);
                    if (val < 1) val = 1; if (val > 999) val = 999;
                    app->measures = val;
                } else if (app->edit_field == 3) {
                    if (app->edit_buf[0])
                        snprintf(app->filename, sizeof(app->filename), "%s", app->edit_buf);
                }
                app->edit_field = 0;
                app->edit_buf[0] = '\0';
                SDL_StopTextInput(app->window);
            }
        }

        /* Toolbar first */
        if (ev->button.button == SDL_BUTTON_LEFT && ui_toolbar_click(app, mx, my))
            break;

        /* Corner collapse lip */
        if (ev->button.button == SDL_BUTTON_LEFT) {
            float keys_x = (float)(app->left_panel_open ? LEFT_PANEL_W : 0);
            float lip_x = keys_x + KEYS_WIDTH - CORNER_TAB_W;
            float lip_y = (float)(TRANSPORT_H + 2);
            if (mx >= lip_x && mx < lip_x + CORNER_TAB_W &&
                my >= lip_y && my < lip_y + HEADER_HEIGHT) {
                app->left_panel_open = !app->left_panel_open;
                break;
            }
        }

        /* Left panel layer selection */
        /* Left panel scrollbar drag */
        if (ev->button.button == SDL_BUTTON_LEFT && app->left_panel_open &&
            mx >= LEFT_PANEL_W - 12 && mx < LEFT_PANEL_W &&
            my > TRANSPORT_H + 2 && my < app->win_h - STATUS_BAR_H) {
            float lp_y = (float)(TRANSPORT_H + 2);
            float lp_h = (float)(app->win_h - TRANSPORT_H - 2 - STATUS_BAR_H);
            float content_h = app->left_panel_scroll_max;
            if (content_h > lp_h) {
                float track_h = lp_h - 8;
                float visible_frac = lp_h / content_h;
                if (visible_frac > 1.0f) visible_frac = 1.0f;
                float thumb_h = track_h * visible_frac;
                if (thumb_h < 24) thumb_h = 24;
                float max_scroll = content_h - lp_h;
                float scroll_frac = (max_scroll > 0) ? app->left_panel_scroll / max_scroll : 0;
                float thumb_y = lp_y + 4 + scroll_frac * (track_h - thumb_h);
                if (my >= thumb_y && my < thumb_y + thumb_h) {
                    app->sb_dragging = 3;
                    app->sb_drag_start = my;
                    app->sb_drag_orig = app->left_panel_scroll;
                    break;
                }
                /* Click above/below thumb  - jump scroll */
                float click_frac = (my - lp_y - 4) / (track_h - thumb_h);
                if (click_frac < 0) click_frac = 0;
                if (click_frac > 1) click_frac = 1;
                app->left_panel_scroll = click_frac * max_scroll;
                app->sb_dragging = 3;
                app->sb_drag_start = my;
                app->sb_drag_orig = app->left_panel_scroll;
                break;
            }
        }

        if (ev->button.button == SDL_BUTTON_LEFT && app->left_panel_open &&
            mx < LEFT_PANEL_W && my > TRANSPORT_H) {
            handle_left_panel_click(app, mx, my);
            break;
        }

        /* Right-click on left panel instrument name -> change instrument */
        if (ev->button.button == SDL_BUTTON_RIGHT && app->left_panel_open &&
            mx < LEFT_PANEL_W && my > TRANSPORT_H && mx >= 74) {
            /* Layout must match handle_left_panel_click / ui_piano_roll.c render */
            float lp_y = (float)(TRANSPORT_H + 2);
            float cy_chk = lp_y - app->left_panel_scroll;
            cy_chk += 10;       /* header top offset */
            cy_chk += 28 + 5;   /* header h=28 + gap to first row */
            for (int i = 0; i < app->project.num_layers; i++) {
                /* separator: 5px total */
                if (i > 0) {
                    cy_chk += 2;
                    cy_chk += 3;
                }
                if (my >= cy_chk && my < cy_chk + 28 && mx >= 74) {
                    app->project.active_layer = i;
                    app->picker_open = true;
                    app->picker_mode = 2; /* change this layer's instrument */
                    app->picker_scroll = 0;
                    app->picker_hover = -1;
                    break;
                }
                cy_chk += 28;       /* top row h=28 */
                cy_chk += 28;       /* aux/vol row h=28 */
                cy_chk += 2 + 20;   /* sublayer gap + tabs */
                /* Marnian synth profile row adds extra height */
                MuseLayer *rly = &app->project.layers[i];
                if (rly->inst_id == 0x14 || rly->inst_id == 0x18 ||
                    rly->inst_id == 0x1C || rly->inst_id == 0x20)
                    cy_chk += 2 + 22;
            }
            break;
        }

        /* Piano keys click  - preview note + start drag */
        {
            float keys_x = (float)(app->left_panel_open ? LEFT_PANEL_W : 0);
            float ry_k = app_roll_y(app);
            float rh_k = app_roll_h(app);
            if (ev->button.button == SDL_BUTTON_LEFT &&
                mx >= keys_x && mx < keys_x + KEYS_WIDTH &&
                my >= ry_k && my < ry_k + rh_k) {
                int pitch = app_y_to_pitch(app, my);
                if (pitch >= PITCH_MIN && pitch <= PITCH_MAX &&
                    app->project.num_layers > 0) {
                    MuseLayer *ly = &app->project.layers[app->project.active_layer];
                    if (inst_is_spacer_key(ly->inst_id, (uint8_t)pitch)) goto skip_key_preview;
                    muse_audio_preview(pitch, 100, 500, ly->inst_id, app->cur_ntype, ly->synth_profile);
                    app->key_dragging = true;
                    app->key_last_preview = pitch;
                    app->key_highlight_pitch = pitch;
                skip_key_preview: ;
                }
            }
        }

        /* Scrollbar clicks */
        float rx = app_roll_x(app), ry = app_roll_y(app);
        float rw = app_roll_w(app), rh = app_roll_h(app);
        {
            float sb_w = 14;
            float sb_x = (float)app->win_w - sb_w;
            float sb_h = 14;
            float sb_y = (float)app->win_h - STATUS_BAR_H - sb_h;

            /* Vertical scrollbar */
            if (ev->button.button == SDL_BUTTON_LEFT &&
                mx >= sb_x && mx < sb_x + sb_w &&
                my >= ry && my < ry + rh) {
                /* Calculate scroll from click position */
                float frac = (my - ry) / rh;
                int max_scroll = NUM_PITCHES - (int)(rh / app->key_height);
                if (max_scroll < 0) max_scroll = 0;
                app->scroll_y = (int)(frac * max_scroll);
                app->sb_dragging = 1;
                app->sb_drag_start = my;
                app->sb_drag_orig = (float)app->scroll_y;
                break;
            }

            /* Horizontal scrollbar */
            if (ev->button.button == SDL_BUTTON_LEFT &&
                mx >= rx && mx < rx + rw - sb_w &&
                my >= sb_y && my < sb_y + sb_h) {
                int bpm = app->project.bpm ? app->project.bpm : 120;
                double mpb = 60000.0 / bpm;
                int beats_per_measure = app->project.time_sig ? app->project.time_sig : 4;
                double total_ms = app->measures * beats_per_measure * mpb;
                float track_w = rw - sb_w;
                float frac = (mx - rx) / track_w;
                app->scroll_x = frac * total_ms;
                if (app->scroll_x < 0) app->scroll_x = 0;
                app->sb_dragging = 2;
                app->sb_drag_start = mx;
                app->sb_drag_orig = (float)app->scroll_x;
                break;
            }
        }

        /* Roll area */
        if (mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
            handle_mouse_down_roll(app, mx, my, ev->button.button);
        }

        /* Velocity pane click  - find knob, start drag or brush-select */
        float vel_y = ry + rh;
        if (ev->button.button == SDL_BUTTON_LEFT &&
            mx >= rx && mx < rx + rw && my >= vel_y && my < vel_y + VEL_PANE_H) {
            NoteArray *na = app_active_notes(app);
            bool shift_held = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            if (na) {
                float pad = 4;
                float bar_h = VEL_PANE_H - 2 * pad;
                float y_base = vel_y + VEL_PANE_H - pad;
                /* Find nearest selected note stem */
                int best = -1;
                float best_dist = 999999;
                for (int i = 0; i < na->count; i++) {
                    if (!(na->notes[i].selected & 1)) continue;
                    float nx = app_ms_to_x(app, na->notes[i].start);
                    float ddx = fabsf(mx - nx);
                    if (ddx > 10) continue;
                    float vhh = (na->notes[i].vel / 127.0f) * bar_h;
                    float knob_y = y_base - vhh;
                    float ddy = fabsf(my - knob_y);
                    float d = ddy + ddx * 0.5f;
                    if (d < best_dist) { best_dist = d; best = i; }
                }
                if (shift_held) {
                    /* Shift+click: brush-select mode  - toggle bit 2 on all stems at this x */
                    for (int i = 0; i < na->count; i++) {
                        if (!(na->notes[i].selected & 1)) continue;
                        float nx = app_ms_to_x(app, na->notes[i].start);
                        if (fabsf(mx - nx) < 6)
                            na->notes[i].selected ^= 2;
                    }
                    app->vel_brush_selecting = true;
                } else if (best >= 0 && best_dist < 30) {
                    undo_push(&app->project);
                    /* Check if this stem is vel-selected (bit 2) for group drag */
                    if (na->notes[best].selected & 2) {
                        /* Group drag: drag all vel-selected stems */
                        app->vel_group_dragging = true;
                        app->vel_dragging = true;
                        app->vel_drag_note_idx = best;
                        app->vel_drag_start_y = my;
                        app->vel_drag_orig_vel = na->notes[best].vel;
                        app->vel_group_drag_base = na->notes[best].vel;
                        /* Set velocity of dragged stem based on click position */
                        float new_vel = (y_base - my) / bar_h * 127.0f;
                        if (new_vel < 1) new_vel = 1;
                        if (new_vel > 127) new_vel = 127;
                        int delta = (int)new_vel - app->vel_group_drag_base;
                        for (int i = 0; i < na->count; i++) {
                            if (na->notes[i].selected & 2) {
                                int nv = na->notes[i].vel + delta;
                                if (nv < 1) nv = 1;
                                if (nv > 127) nv = 127;
                                na->notes[i].vel = (uint8_t)nv;
                            }
                        }
                        app->vel_group_drag_base = (int)new_vel;
                        app->project.dirty = true;
                    } else {
                        /* Normal single-stem drag */
                        app->vel_dragging = true;
                        app->vel_group_dragging = false;
                        app->vel_drag_note_idx = best;
                        app->vel_drag_start_y = my;
                        app->vel_drag_orig_vel = na->notes[best].vel;
                        float new_vel = (y_base - my) / bar_h * 127.0f;
                        if (new_vel < 1) new_vel = 1;
                        if (new_vel > 127) new_vel = 127;
                        na->notes[best].vel = (uint8_t)new_vel;
                        app->project.dirty = true;
                    }
                } else {
                    /* Clicked empty area in velocity pane  - clear vel-selection */
                    for (int i = 0; i < na->count; i++)
                        na->notes[i].selected &= ~2;
                }
            }
        }

        /* Shift+right-click+drag in velocity pane: set stems to mouse height */
        if (ev->button.button == SDL_BUTTON_RIGHT &&
            (SDL_GetModState() & SDL_KMOD_SHIFT) &&
            mx >= rx && mx < rx + rw && my >= vel_y && my < vel_y + VEL_PANE_H) {
            NoteArray *na = app_active_notes(app);
            if (na) {
                float pad = 4;
                float bar_h = VEL_PANE_H - 2 * pad;
                float y_base = vel_y + VEL_PANE_H - pad;
                undo_push(&app->project);
                app->vel_set_dragging = true;
                /* Set any stems near the cursor to the current y velocity */
                float new_vel = (y_base - my) / bar_h * 127.0f;
                if (new_vel < 1) new_vel = 1;
                if (new_vel > 127) new_vel = 127;
                for (int i = 0; i < na->count; i++) {
                    if (!(na->notes[i].selected & 1)) continue;
                    float nx = app_ms_to_x(app, na->notes[i].start);
                    if (fabsf(mx - nx) < 6) {
                        na->notes[i].vel = (uint8_t)new_vel;
                    }
                }
                app->project.dirty = true;
            }
        }

        /* Timeline header click  - set playhead or start loop drag */
        float hdr_y = (float)(TRANSPORT_H + 2);
        if (mx >= rx && mx < rx + rw && my >= hdr_y && my < hdr_y + HEADER_HEIGHT) {
            bool shift_hdr = SDL_GetModState() & SDL_KMOD_SHIFT;
            if (shift_hdr) {
                /* Start loop region drag */
                double ms = app_x_to_ms(app, mx);
                if (ms < 0) ms = 0;
                app->loop_start_ms = ms;
                app->loop_end_ms = ms;
                app->looping = true;
                app->dragging = true; /* reuse dragging flag */
                app->drag_start_x = mx;
            } else {
                app->playhead_ms = app_x_to_ms(app, mx);
                if (app->playhead_ms < 0) app->playhead_ms = 0;
                if (app->playing) {
                    app->play_start_ticks = SDL_GetTicks();
                    app->play_start_ms = app->playhead_ms - 5.0;
                    muse_audio_stop();
                    muse_audio_play(&app->project, app->playhead_ms - 5.0);
                }
            }
        }
        break;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
        handle_mouse_up(app, ev->button.button);
        app->mouse_down_l = (ev->button.button == SDL_BUTTON_LEFT) ? false : app->mouse_down_l;
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        handle_scroll(app, ev->wheel.x, ev->wheel.y);
        break;

    case SDL_EVENT_KEY_DOWN: {
        bool ctrl = ev->key.mod & SDL_KMOD_CTRL;
        bool shift = ev->key.mod & SDL_KMOD_SHIFT;
        bool alt = ev->key.mod & SDL_KMOD_ALT;

        /* Text entry mode for BPM/measures/filename */
        if (app->edit_field != 0) {
            SDL_Keycode k = ev->key.key;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE || k == SDLK_TAB) {
                /* Commit edit */
                if (k != SDLK_ESCAPE) {
                    if (app->edit_field == 1) {
                        int val = atoi(app->edit_buf);
                        if (val < 1) val = 1;
                        if (val > 200) val = 200;
                        app->project.bpm = (uint16_t)val;
                    } else if (app->edit_field == 2) {
                        int val = atoi(app->edit_buf);
                        if (val < 1) val = 1;
                        if (val > 999) val = 999;
                        app->measures = val;
                    } else if (app->edit_field == 3) {
                        if (app->edit_buf[0])
                            snprintf(app->filename, sizeof(app->filename), "%s", app->edit_buf);
                    }
                }
                app->edit_field = 0;
                app->edit_buf[0] = '\0';
                SDL_StopTextInput(app->window);
            } else if (k == SDLK_BACKSPACE) {
                if (app->edit_field == 3) {
                    int len = (int)strlen(app->edit_buf);
                    if (app->edit_cursor > 0 && app->edit_cursor <= len) {
                        memmove(app->edit_buf + app->edit_cursor - 1,
                                app->edit_buf + app->edit_cursor,
                                len - app->edit_cursor + 1);
                        app->edit_cursor--;
                    }
                } else {
                    int len = (int)strlen(app->edit_buf);
                    if (len > 0) app->edit_buf[len - 1] = '\0';
                }
            } else if (k == SDLK_DELETE && app->edit_field == 3) {
                int len = (int)strlen(app->edit_buf);
                if (app->edit_cursor < len) {
                    memmove(app->edit_buf + app->edit_cursor,
                            app->edit_buf + app->edit_cursor + 1,
                            len - app->edit_cursor);
                }
            } else if (k == SDLK_LEFT && app->edit_field == 3) {
                if (app->edit_cursor > 0) app->edit_cursor--;
            } else if (k == SDLK_RIGHT && app->edit_field == 3) {
                int len = (int)strlen(app->edit_buf);
                if (app->edit_cursor < len) app->edit_cursor++;
            } else if (k == SDLK_HOME && app->edit_field == 3) {
                app->edit_cursor = 0;
            } else if (k == SDLK_END && app->edit_field == 3) {
                app->edit_cursor = (int)strlen(app->edit_buf);
            } else if (app->edit_field == 3) {
                /* Filename: handled via SDL_EVENT_TEXT_INPUT below */
                break;
            } else if (k >= SDLK_0 && k <= SDLK_9) {
                int len = (int)strlen(app->edit_buf);
                if (len < 5) {
                    app->edit_buf[len] = (char)('0' + (k - SDLK_0));
                    app->edit_buf[len + 1] = '\0';
                }
            } else if (k >= SDLK_KP_0 && k <= SDLK_KP_9) {
                int len = (int)strlen(app->edit_buf);
                if (len < 5) {
                    app->edit_buf[len] = (char)('0' + (k - SDLK_KP_0));
                    app->edit_buf[len + 1] = '\0';
                }
            }
            break;
        }

        switch (ev->key.key) {
        case SDLK_SPACE:
            if (app->playing) {
                app->paused = true;
                app->playing = false;
                muse_audio_pause();
            } else if (app->paused) {
                /* Resume from pause  - continue voices where they were */
                app->paused = false;
                app->playing = true;
                app->play_start_ticks = SDL_GetTicks();
                app->play_start_ms = app->playhead_ms;
                muse_audio_resume();
            } else {
                /* Fresh start */
                double start_ms = app->playhead_ms;
                if (app->looping && app->loop_end_ms > app->loop_start_ms) {
                    if (start_ms < app->loop_start_ms || start_ms >= app->loop_end_ms)
                        start_ms = app->loop_start_ms;
                }
                app->playhead_ms = start_ms;
                app->playing = true;
                app->play_start_ticks = SDL_GetTicks();
                app->play_start_ms = start_ms - 5.0;
                muse_audio_play(&app->project, start_ms - 5.0);
            }
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:
            delete_selected(app);
            break;
        case SDLK_A:
            if (ctrl) {
                NoteArray *na = app_active_notes(app);
                if (na) for (int i = 0; i < na->count; i++) na->notes[i].selected = 1;
            }
            break;
        case SDLK_Z:
            if (ctrl) {
                if (shift) {
                    redo_pop(&app->project);
                    set_status_msg(app, "Redo");
                } else {
                    undo_pop(&app->project);
                    set_status_msg(app, "Undo");
                }
            }
            break;
        case SDLK_Y:
            if (ctrl) { redo_pop(&app->project); set_status_msg(app, "Redo"); }
            break;
        case SDLK_S:
            if (ctrl) {
                if (strcmp(app->filename, "untitled") != 0) {
                    size_t fnlen = strlen(app->filename);
                    bool fn_is_composer = (fnlen > 9 && strcmp(app->filename + fnlen - 9, ".composer") == 0);
                    char save_path[512];
                    if (fn_is_composer)
                        snprintf(save_path, sizeof(save_path), "Save/%s", app->filename);
                    else
                        snprintf(save_path, sizeof(save_path), "Save/%s", app->filename);
                    int rc;
                    if (fn_is_composer)
                        rc = muse_save(save_path, app);
                    else
                        rc = bdo_save(save_path, &app->project);
                    if (rc == 0) {
                        app->project.dirty = false;
                        SDL_Log("Saved: %s", save_path);
                        set_status_msg(app, "Saved: %s", save_path);
                    }
                } else {
                    ui_toolbar_save_as(app);
                }
            } else {
                /* S = select tool */
                app->tool = TOOL_SELECT;
            }
            break;
        case SDLK_O:
            if (ctrl) ui_toolbar_open(app);
            break;
        case SDLK_N:
            if (ctrl) {
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
                    if (!SDL_ShowMessageBox(&mbd, &btn) || btn != 1) break;
                }
                muse_project_free(&app->project);
                muse_project_init(&app->project);
                muse_project_add_layer(&app->project, 0x11);
                snprintf(app->filename, sizeof(app->filename), "untitled");
                app->playhead_ms = 0;
                app->scroll_x = 0;
                /* Reset effector values */
                app->fx_reverb = 0;
                app->fx_delay = 0;
                app->fx_chorus_fb = 0;
                app->fx_chorus_depth = 0;
                app->fx_chorus_freq = 0;
                app->project.effector_reverb = 0;
                app->project.effector_delay = 0;
                app->project.effector_chorus_fb = 0;
                app->project.effector_chorus_depth = 0;
                app->project.effector_chorus_freq = 0;
                /* Reset grid, zoom, undo */
                app->grid_snap = GRID_1_16;
                app->zoom_x = 1.0;
                app->key_height = KEY_HEIGHT_DEFAULT;
                undo_clear();
                app->measures = 4;
                set_status_msg(app, "New project");
            }
            break;
        /* tool shortcuts: D/S/E */
        case SDLK_D:
            if (ctrl) {
                /* Ctrl+D: Duplicate selected notes */
                NoteArray *na = app_active_notes(app);
                if (na) {
                    undo_push(&app->project);
                    int orig_count = na->count;
                    for (int i = 0; i < orig_count; i++) {
                        if (na->notes[i].selected) {
                            MuseNote n = na->notes[i];
                            na->notes[i].selected = 0;
                            n.selected = 1;
                            note_array_push(na, n);
                        }
                    }
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                }
            } else {
                app->tool = TOOL_DRAW;
            }
            break;
        case SDLK_E:
            if (!ctrl) app->tool = TOOL_ERASE;
            break;
        case SDLK_L:
            if (ctrl) {
                /* Toggle loop from selected notes */
                if (app->looping) {
                    app->looping = false;
                } else {
                    NoteArray *na = app_active_notes(app);
                    if (na) {
                        double lo = 1e18, hi = 0;
                        int nsel = 0;
                        for (int i = 0; i < na->count; i++) {
                            if (na->notes[i].selected) {
                                if (na->notes[i].start < lo) lo = na->notes[i].start;
                                double e = na->notes[i].start + na->notes[i].dur;
                                if (e > hi) hi = e;
                                nsel++;
                            }
                        }
                        if (nsel > 0 && hi > lo) {
                            app->loop_start_ms = lo;
                            app->loop_end_ms = hi;
                            app->looping = true;
                        }
                    }
                }
            }
            break;
        case SDLK_HOME:
            app->playhead_ms = 0;
            app->scroll_x = 0;
            break;
        case SDLK_I:
            if (ctrl && shift) {
                /* Move selected notes to different instrument */
                NoteArray *na = app_active_notes(app);
                if (na) {
                    bool has_sel = false;
                    for (int i = 0; i < na->count; i++)
                        if (na->notes[i].selected) { has_sel = true; break; }
                    if (has_sel) {
                        app->picker_open = true;
                        app->picker_mode = 1;
                        app->picker_scroll = 0;
                        app->picker_hover = -1;
                    }
                }
            }
            break;
        case SDLK_TAB:
            app->left_panel_open = !app->left_panel_open;
            break;
        /* Shift+Up/Down: transpose; plain Up/Down: adjust velocity */
        case SDLK_UP: {
            NoteArray *na = app_active_notes(app);
            if (!na) break;
            if (shift && alt) {
                /* Shift+Alt+Up: transpose ALL instruments (except drums) */
                int semi = ctrl ? 12 : 1;
                bool ok = true;
                for (int li = 0; li < app->project.num_layers && ok; li++) {
                    if (inst_is_drum(app->project.layers[li].inst_id)) continue;
                    for (int si = 0; si < app->project.layers[li].num_sublayers && ok; si++)
                        for (int ni = 0; ni < app->project.layers[li].sublayers[si].count; ni++)
                            if (app->project.layers[li].sublayers[si].notes[ni].pitch + semi > PITCH_MAX)
                                { ok = false; break; }
                }
                if (ok) {
                    undo_push(&app->project);
                    for (int li = 0; li < app->project.num_layers; li++) {
                        if (inst_is_drum(app->project.layers[li].inst_id)) continue;
                        for (int si = 0; si < app->project.layers[li].num_sublayers; si++)
                            for (int ni = 0; ni < app->project.layers[li].sublayers[si].count; ni++)
                                app->project.layers[li].sublayers[si].notes[ni].pitch += semi;
                    }
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                    set_status_msg(app, "Transposed all +%d", semi);
                }
            } else if (shift) {
                /* Shift+Up = 1 semitone, Ctrl+Shift+Up = 1 octave */
                int semi = ctrl ? 12 : 1;
                bool ok = true;
                for (int i = 0; i < na->count; i++) {
                    if (!na->notes[i].selected) continue;
                    if (na->notes[i].pitch + semi > PITCH_MAX) { ok = false; break; }
                }
                if (ok) {
                    undo_push(&app->project);
                    for (int i = 0; i < na->count; i++)
                        if (na->notes[i].selected) na->notes[i].pitch += semi;
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                }
            } else {
                undo_push(&app->project);
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected && na->notes[i].vel < 127)
                        na->notes[i].vel += (ctrl ? 10 : 1);
                    if (na->notes[i].vel > 127) na->notes[i].vel = 127;
                }
            }
            break;
        }
        case SDLK_DOWN: {
            NoteArray *na = app_active_notes(app);
            if (!na) break;
            if (shift && alt) {
                /* Shift+Alt+Down: transpose ALL instruments (except drums) */
                int semi = ctrl ? 12 : 1;
                bool ok = true;
                for (int li = 0; li < app->project.num_layers && ok; li++) {
                    if (inst_is_drum(app->project.layers[li].inst_id)) continue;
                    for (int si = 0; si < app->project.layers[li].num_sublayers && ok; si++)
                        for (int ni = 0; ni < app->project.layers[li].sublayers[si].count; ni++)
                            if (app->project.layers[li].sublayers[si].notes[ni].pitch - semi < PITCH_MIN)
                                { ok = false; break; }
                }
                if (ok) {
                    undo_push(&app->project);
                    for (int li = 0; li < app->project.num_layers; li++) {
                        if (inst_is_drum(app->project.layers[li].inst_id)) continue;
                        for (int si = 0; si < app->project.layers[li].num_sublayers; si++)
                            for (int ni = 0; ni < app->project.layers[li].sublayers[si].count; ni++)
                                app->project.layers[li].sublayers[si].notes[ni].pitch -= semi;
                    }
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                    set_status_msg(app, "Transposed all -%d", semi);
                }
            } else if (shift) {
                int semi = ctrl ? 12 : 1;
                bool ok = true;
                for (int i = 0; i < na->count; i++) {
                    if (!na->notes[i].selected) continue;
                    if (na->notes[i].pitch - semi < PITCH_MIN) { ok = false; break; }
                }
                if (ok) {
                    undo_push(&app->project);
                    for (int i = 0; i < na->count; i++)
                        if (na->notes[i].selected) na->notes[i].pitch -= semi;
                    app->project.dirty = true;
                    restart_playback_if_playing(app);
                }
            } else {
                undo_push(&app->project);
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected && na->notes[i].vel > 1) {
                        int dec = ctrl ? 10 : 1;
                        na->notes[i].vel = (uint8_t)(na->notes[i].vel > dec ? na->notes[i].vel - dec : 1);
                    }
                }
            }
            break;
        }
        case SDLK_LEFT: {
            NoteArray *na = app_active_notes(app);
            if (na) {
                double nudge = ms_per_beat(app) / grid_div(app);
                undo_push(&app->project);
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected) {
                        na->notes[i].start -= nudge;
                        if (na->notes[i].start < 0) na->notes[i].start = 0;
                    }
                }
                app->project.dirty = true;
            }
            break;
        }
        case SDLK_RIGHT: {
            NoteArray *na = app_active_notes(app);
            if (na) {
                double nudge = ms_per_beat(app) / grid_div(app);
                undo_push(&app->project);
                for (int i = 0; i < na->count; i++) {
                    if (na->notes[i].selected)
                        na->notes[i].start += nudge;
                }
                app->project.dirty = true;
            }
            break;
        }
        case SDLK_C:
            if (ctrl) {
                NoteArray *na = app_active_notes(app);
                if (na) {
                    note_array_clear(&app->clipboard);
                    for (int i = 0; i < na->count; i++)
                        if (na->notes[i].selected)
                            note_array_push(&app->clipboard, na->notes[i]);
                    app->clipboard_cursor_ms = app->playhead_ms;
                    set_status_msg(app, "Copied %d notes", app->clipboard.count);
                }
            }
            break;
        case SDLK_X:
            if (ctrl) {
                NoteArray *na = app_active_notes(app);
                if (na) {
                    note_array_clear(&app->clipboard);
                    app->clipboard_cursor_ms = app->playhead_ms;
                    undo_push(&app->project);
                    for (int i = na->count - 1; i >= 0; i--) {
                        if (na->notes[i].selected) {
                            note_array_push(&app->clipboard, na->notes[i]);
                            note_array_remove(na, i);
                        }
                    }
                    app->project.dirty = true;
                    auto_expand_measures(app);
                    restart_playback_if_playing(app);
                }
            }
            break;
        case SDLK_V:
            if (ctrl) {
                NoteArray *na = app_active_notes(app);
                if (na && app->clipboard.count > 0) {
                    undo_push(&app->project);
                    deselect_all_layers(app);
                    double offset;
                    if (fabs(app->playhead_ms - app->clipboard_cursor_ms) > 1.0) {
                        /* Cursor moved since copy -- align to new cursor position */
                        double min_start = 1e18;
                        for (int i = 0; i < app->clipboard.count; i++)
                            if (app->clipboard.notes[i].start < min_start)
                                min_start = app->clipboard.notes[i].start;
                        offset = app->playhead_ms - min_start;
                    } else {
                        offset = 0; /* paste in-place */
                    }
                    for (int i = 0; i < app->clipboard.count; i++) {
                        MuseNote n = app->clipboard.notes[i];
                        n.start += offset;
                        n.selected = 1;
                        note_array_push(na, n);
                    }
                    app->project.dirty = true;
                    auto_expand_measures(app);
                    set_status_msg(app, "Pasted %d notes", app->clipboard.count);
                    restart_playback_if_playing(app);
                }
            }
            break;
        case SDLK_F11:
            app->perspective_enabled = !app->perspective_enabled;
            set_status_msg(app, "3D mode: %s", app->perspective_enabled ? "ON" : "OFF");
            break;
        default: break;
        }
        break;
    }

    case SDL_EVENT_TEXT_INPUT:
        /* Handle text input for filename editing */
        if (app->edit_field == 3) {
            const char *text = ev->text.text;
            int len = (int)strlen(app->edit_buf);
            int tlen = (int)strlen(text);
            if (len + tlen < (int)sizeof(app->edit_buf) - 1) {
                /* Insert at cursor position */
                memmove(app->edit_buf + app->edit_cursor + tlen,
                        app->edit_buf + app->edit_cursor,
                        len - app->edit_cursor + 1);
                memcpy(app->edit_buf + app->edit_cursor, text, tlen);
                app->edit_cursor += tlen;
            }
        }
        break;

    default:
        break;
    }
}

static void fx_tick(MuseApp *app) {
    float now = (float)SDL_GetTicks() / 1000.0f;
    app->dt = now - app->anim_time;
    if (app->dt > 0.1f) app->dt = 0.1f;
    if (app->dt < 0.001f) app->dt = 0.001f;
    app->anim_time = now;

    /* Track continuous playback time */
    if (app->playing) {
        app->continuous_play_secs += app->dt;
    } else {
        app->continuous_play_secs = 0;
    }

    /* 3D perspective transition  - one-shot per F11 press */
    if (app->perspective_enabled && app->playing) {
        app->perspective_target = 1.0f;
    } else {
        app->perspective_target = 0.0f;
        /* Auto-disable when we leave 3D */
        if (app->perspective_t < 0.01f)
            app->perspective_enabled = false;
    }
    /* Fast transition (~0.5 seconds) */
    float persp_speed = 2.0f * app->dt;
    if (app->perspective_t < app->perspective_target)
        app->perspective_t = fminf(app->perspective_t + persp_speed, app->perspective_target);
    else if (app->perspective_t > app->perspective_target)
        app->perspective_t = fmaxf(app->perspective_t - persp_speed, app->perspective_target);

    /* Initialize aurora blobs if needed */
    if (!app->aurora_ready) {
        app->aurora_ready = true;
        const uint8_t colors[][3] = {
            {0x30, 0x18, 0x60}, {0x60, 0x48, 0x20},
            {0x18, 0x40, 0x38}, {0x48, 0x20, 0x30},
            {0x58, 0x40, 0x18}, {0x20, 0x30, 0x58},
        };
        for (int i = 0; i < NUM_AURORA; i++) {
            app->aurora[i].cx = 0.3f + (i % 3) * 0.2f;
            app->aurora[i].cy = 0.3f + (i / 3) * 0.3f;
            app->aurora[i].phase_x = i * 1.37f;
            app->aurora[i].phase_y = i * 2.13f;
            app->aurora[i].speed = 0.15f + i * 0.04f;
            app->aurora[i].radius = 180.0f + i * 30.0f;
            app->aurora[i].r = colors[i][0];
            app->aurora[i].g = colors[i][1];
            app->aurora[i].b = colors[i][2];
        }
    }

    /* Update firefly particles */
    for (int i = 0; i < app->particle_count; ) {
        app->particles[i].x += app->particles[i].vx * app->dt;
        app->particles[i].y += app->particles[i].vy * app->dt;
        app->particles[i].vx *= 0.96f;
        app->particles[i].vy *= 0.96f;
        app->particles[i].vy -= 15.0f * app->dt; /* float upward */
        app->particles[i].life -= app->dt;
        if (app->particles[i].life <= 0) {
            app->particles[i] = app->particles[--app->particle_count];
        } else {
            i++;
        }
    }
}

void muse_app_render(MuseApp *app) {
    fx_tick(app);

    /* Cache selection state for this frame (avoids O(N) scan on every app_roll_h call) */
    app->_sel_cache = false;
    {
        NoteArray *na = app_active_notes(app);
        if (na) {
            for (int i = 0; i < na->count; i++) {
                if (na->notes[i].selected) { app->_sel_cache = true; break; }
            }
        }
    }

    /* Advance playhead and trigger audio */
    if (app->playing) {
        uint64_t now = SDL_GetTicks();
        app->playhead_ms = app->play_start_ms + (double)(now - app->play_start_ticks);
        if (app->looping && app->playhead_ms >= app->loop_end_ms) {
            app->playhead_ms = app->loop_start_ms;
            app->play_start_ms = app->loop_start_ms;
            app->play_start_ticks = now;
            muse_audio_seek(app->loop_start_ms);
        }
        muse_audio_tick(app->playhead_ms);

        /* Auto-scroll to keep playhead visible (scroll when cursor reaches right edge) */
        float ph_x = app_ms_to_x(app, app->playhead_ms);
        float roll_rx = app_roll_x(app);
        float roll_rw = app_roll_w(app);
        if (ph_x > roll_rx + roll_rw - 10) {
            double ms_visible = (double)roll_rw / beat_px(app) * ms_per_beat(app);
            app->scroll_x = app->playhead_ms - ms_visible * 0.15;
        }

        /* Check if playback ended naturally */
        double total_dur_ms = app->measures * app->project.time_sig *
                              (60000.0 / (app->project.bpm ? app->project.bpm : 120));
        if (app->playhead_ms >= total_dur_ms) {
            app->playing = false;
            app->playhead_ms = 0;
        }
    }

    /* Autosave every 2 minutes */
    {
        uint64_t now = SDL_GetTicks();
        if (app->project.dirty && now - app->last_autosave_ticks > 120000) {
            muse_save("Save/Autosave/autosave.composer", app);
            app->last_autosave_ticks = now;
        }
    }

    /* Update window title only when it changes */
    {
        char title[320];
        snprintf(title, sizeof(title), "Composer - %s%s",
                 app->filename, app->project.dirty ? " *" : "");
        if (strcmp(title, app->last_title) != 0) {
            SDL_SetWindowTitle(app->window, title);
            memcpy(app->last_title, title, sizeof(title));
        }
    }

    SDL_Renderer *r = app->renderer;

    SDL_SetRenderDrawColor(r, COL_BG_DARK, 0xFF);
    SDL_RenderClear(r);

    ui_toolbar_render(app);
    /* Fade toolbar out during 3D transition */
    if (app->perspective_t > 0.01f) {
        uint8_t fa = (uint8_t)(fminf(app->perspective_t * 3.0f, 1.0f) * 255);
        draw_filled_rect(app->renderer, 0, 0, (float)app->win_w, TRANSPORT_H + 2,
                         0x16, 0x16, 0x18, fa);
    }
    ui_piano_roll_render(app);

    /* Status bar */
    {
        float sy = (float)(app->win_h - STATUS_BAR_H);
        draw_filled_rect(r, 0, sy, (float)app->win_w, STATUS_BAR_H,
                         COL_BG, 0xFF);

        /* status text (shows dynamic message if recent, otherwise note count etc) */
        char status[256];
        uint64_t now_ticks = SDL_GetTicks();
        if (app->status_msg[0] && now_ticks - app->status_msg_ticks < 3000) {
            snprintf(status, sizeof(status), "%s", app->status_msg);
        } else {
            NoteArray *na = app_active_notes(app);
            int sel = 0, total = 0;
            if (na) {
                total = na->count;
                for (int i = 0; i < na->count; i++)
                    if (na->notes[i].selected) sel++;
            }
            const char *tool_names[] = { "Select", "Draw", "Erase" };
            if (app->looping) {
                snprintf(status, sizeof(status), "Tool: %s  |  Notes: %d  |  Sel: %d  |  Loop: %.1fs-%.1fs",
                         tool_names[app->tool], total, sel,
                         app->loop_start_ms / 1000.0, app->loop_end_ms / 1000.0);
            } else {
                snprintf(status, sizeof(status), "Tool: %s  |  Notes: %d  |  Selected: %d",
                         tool_names[app->tool], total, sel);
            }
        }
        float status_ty = sy + (STATUS_BAR_H - text_line_height(10)) / 2.0f;
        draw_text(r, status, 8, status_ty, 10, COL_GOLD_LIGHT);

        /* shortcuts button - right-aligned in status bar */
        {
            const char *btn_text = "Shortcuts";
            float bw = text_width(btn_text, 10) + 16;
            float bh = STATUS_BAR_H - 4;
            float bx = (float)app->win_w - bw - 6;
            float by = sy + 2;
            bool hovered = (app->mouse_x >= bx && app->mouse_x <= bx + bw &&
                            app->mouse_y >= by && app->mouse_y <= by + bh);
            draw_rounded_rect(r, bx, by, bw, bh, 3,
                              hovered ? 0x3A : 0x2A, hovered ? 0x3A : 0x2A, hovered ? 0x40 : 0x30, 0xFF);
            draw_text_centered(r, btn_text, bx + bw/2, by + bh/2, 10, COL_TEXT_DIM);
        }
    }
    /* Fade status bar during 3D transition */
    if (app->perspective_t > 0.01f) {
        float sy = (float)(app->win_h - STATUS_BAR_H);
        uint8_t fa = (uint8_t)(fminf(app->perspective_t * 3.0f, 1.0f) * 255);
        draw_filled_rect(r, 0, sy, (float)app->win_w, STATUS_BAR_H,
                         0x16, 0x16, 0x18, fa);
    }

    /* Modal overlays (drawn on top of everything) */
    ui_instrument_picker_render(app);
    ui_aux_send_render(app);
    ui_chord_picker_render(app);
    ui_midi_import_render(app);
    ui_dropdown_render(app);

    /* shortcut cheatsheet (draggable floating window) */
    if (app->shortcuts_open) {
        float sw = 280, sh = 340;
        float sx = app->shortcuts_x, sy2 = app->shortcuts_y;
        /* clamp to screen */
        if (sx < 0) sx = 0;
        if (sy2 < 0) sy2 = 0;
        if (sx + sw > (float)app->win_w) sx = (float)app->win_w - sw;
        if (sy2 + sh > (float)app->win_h) sy2 = (float)app->win_h - sh;
        app->shortcuts_x = sx; app->shortcuts_y = sy2;

        /* shadow */
        draw_rounded_rect(r, sx + 3, sy2 + 3, sw, sh, 6, 0x00, 0x00, 0x00, 0x60);
        /* background */
        draw_rounded_rect(r, sx, sy2, sw, sh, 6, COL_SURFACE, 0xFF);
        /* title bar */
        draw_filled_rect(r, sx, sy2, sw, 26, COL_BG, 0xFF);
        draw_text_bold(r, "Keyboard Shortcuts", sx + 8, sy2 + 7, 11, COL_GOLD);
        draw_text(r, "x", sx + sw - 18, sy2 + 6, 13, COL_TEXT_DIM);

        /* content area with clipping and scroll */
        float content_y = sy2 + 28;
        float content_h = sh - 30;
        float total_content_h = 700; /* approximate total height of all rows */
        float max_scroll = total_content_h - content_h;
        if (max_scroll < 0) max_scroll = 0;
        if (app->shortcuts_scroll > max_scroll) app->shortcuts_scroll = max_scroll;
        push_clip(r, sx, content_y, sw, content_h);

        float ly = content_y - app->shortcuts_scroll;
        float lx = sx + 10;
        float rx = sx + 150;
        float lh = 16;

        #define SC_ROW(key, desc) do { \
            draw_text(r, key, lx, ly, 10, COL_GOLD_LIGHT); \
            draw_text(r, desc, rx, ly, 10, COL_TEXT_DIM); \
            ly += lh; \
        } while(0)

        draw_text_bold(r, "General", lx, ly, 10, COL_TEXT); ly += lh;
        SC_ROW("Space",         "Play / Pause");
        SC_ROW("Home",          "Go to start");
        SC_ROW("Ctrl+N",       "New project");
        SC_ROW("Ctrl+O",       "Open");
        SC_ROW("Ctrl+S",       "Save");
        SC_ROW("Ctrl+Z",       "Undo");
        SC_ROW("Ctrl+Y",       "Redo");
        SC_ROW("Ctrl+Shift+Z", "Redo (alt)");
        ly += 6;

        draw_text_bold(r, "Tools", lx, ly, 10, COL_TEXT); ly += lh;
        SC_ROW("D",     "Draw tool");
        SC_ROW("S",     "Select tool");
        SC_ROW("E",     "Erase tool");
        SC_ROW("Del",   "Delete selected");
        SC_ROW("Tab",   "Toggle side panel");
        ly += 6;

        draw_text_bold(r, "Editing", lx, ly, 10, COL_TEXT); ly += lh;
        SC_ROW("Ctrl+A",         "Select all");
        SC_ROW("Ctrl+C",         "Copy");
        SC_ROW("Ctrl+X",         "Cut");
        SC_ROW("Ctrl+V",         "Paste");
        SC_ROW("Ctrl+D",         "Duplicate");
        SC_ROW("Left / Right",   "Nudge in time");
        SC_ROW("Up / Down",      "Adjust velocity");
        SC_ROW("Shift+Up/Down",  "Transpose semitone");
        SC_ROW("Ctrl+Shift+Up",  "Transpose octave");
        SC_ROW("Shift+Alt+Up/Dn","Transpose all (no drums)");
        SC_ROW("Ctrl+Shift+I",   "Move to instrument");
        ly += 6;

        draw_text_bold(r, "Selection", lx, ly, 10, COL_TEXT); ly += lh;
        SC_ROW("Click+Drag",      "Box select");
        SC_ROW("Shift+Drag",      "Brush select");
        SC_ROW("Alt+Drag",        "Lasso select");
        SC_ROW("Right-Drag",      "Erase sweep");
        SC_ROW("Middle-Click",    "Chordify (with selection)");
        ly += 6;

        draw_text_bold(r, "View", lx, ly, 10, COL_TEXT); ly += lh;
        SC_ROW("Scroll",         "Vertical scroll");
        SC_ROW("Ctrl+Scroll",    "Horizontal scroll");
        SC_ROW("Shift+Scroll",   "Horizontal zoom");
        SC_ROW("Alt+Scroll",     "Vertical zoom");
        SC_ROW("Middle-Drag",    "Pan view");
        SC_ROW("Ctrl+L",         "Loop selection");
        SC_ROW("Shift+Drag bar", "Set loop region");
        SC_ROW("F11",            "3D mode (playback)");

        #undef SC_ROW

        /* scrollbar if needed */
        if (max_scroll > 0) {
            float sb_x = sx + sw - 8;
            float sb_track_h = content_h - 4;
            float sb_thumb_h = (content_h / total_content_h) * sb_track_h;
            if (sb_thumb_h < 20) sb_thumb_h = 20;
            float sb_thumb_y = content_y + 2 + (app->shortcuts_scroll / max_scroll) * (sb_track_h - sb_thumb_h);
            draw_filled_rect(r, sb_x, content_y + 2, 4, sb_track_h, 0x30, 0x30, 0x35, 0xFF);
            draw_rounded_rect(r, sb_x, sb_thumb_y, 4, sb_thumb_h, 2, 0x60, 0x60, 0x68, 0xFF);
        }

        pop_clip(r);
    }

    /* startup overlay (renders on top of compositor) */
    if (app->overlay_state > 0) {
        float ww = (float)app->win_w;
        float wh = (float)app->win_h;
        float cx = ww / 2.0f;
        float cy = wh / 2.0f;

        if (app->overlay_dismiss) {
            if (app->overlay_state == 2) {
                /* link done - jump straight to disclaimer (no fade) */
                FILE *seen = fopen("Save/.welcome_shown", "r");
                if (!seen) {
                    SDL_CreateDirectory("Save");
                    FILE *mark = fopen("Save/.welcome_shown", "w");
                    if (mark) { fprintf(mark, "1\n"); fclose(mark); }
                    app->overlay_state = 3;
                    app->overlay_fade = 0.0f;
                    app->overlay_dismiss = false;
                } else {
                    fclose(seen);
                    app->overlay_state = 0;
                }
            } else {
                /* disclaimer fade out */
                app->overlay_fade += 0.015f;
                if (app->overlay_fade >= 1.0f) {
                    app->overlay_fade = 1.0f;
                    app->overlay_state = 0;
                }
            }
        }

        float t = app->overlay_fade;
        float alpha_f = 1.0f - t;
        uint8_t oa = (uint8_t)(alpha_f * 255);

        if (oa > 0) {
            /* render overlay + text to a temp texture so they fade as one */
            SDL_Texture *ov_tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_TARGET, (int)ww, (int)wh);
            if (ov_tex) {
                SDL_SetTextureBlendMode(ov_tex, SDL_BLENDMODE_BLEND);
                SDL_SetRenderTarget(r, ov_tex);
                SDL_SetRenderDrawColor(r, 0x1A, 0x1A, 0x1E, 0xFF);
                SDL_RenderClear(r);

                if (app->overlay_state == 2) {
                    draw_text_centered(r, "Setup Required",
                                       cx, cy - 90, 16, 0xD4, 0xBC, 0x98);
                    draw_text_centered(r,
                        "To open and export your BDO music files, we need to identify",
                        cx, cy - 50, 11, 0xC0, 0xC0, 0xC0);
                    draw_text_centered(r,
                        "your in-game character. This only uses your character name.",
                        cx, cy - 32, 11, 0xC0, 0xC0, 0xC0);
                    {
                        /* split line to highlight "single note" in gold */
                        const char *pre = "1. In game, save a composition with a ";
                        const char *hi = "single note";
                        float pw = text_width(pre, 11);
                        float hw = text_width(hi, 11);
                        float total = pw + hw;
                        float lx = cx - total / 2.0f;
                        draw_text(r, pre, lx, cy + 0, 11, 0xA0, 0xA0, 0xA0);
                        draw_text(r, hi, lx + pw, cy + 0, 11, 0xD4, 0xBC, 0x98);
                    }
                    draw_text_centered(r,
                        "2. Drag that file onto this window",
                        cx, cy + 22, 11, 0xA0, 0xA0, 0xA0);
                    draw_text_centered(r,
                        "BDO music files are in: Documents/Black Desert/Music/",
                        cx, cy + 55, 10, 0x80, 0x80, 0x80);
                    if (!app->overlay_dismiss)
                        draw_text_centered(r,
                            "Press ESC to skip (you won't be able to open or export BDO files)",
                            cx, cy + 80, 10, 0x60, 0x60, 0x60);
                } else if (app->overlay_state == 3) {
                    draw_text_centered(r, "Composer - BDO Music Tool",
                                       cx, cy - 80, 18, 0xD4, 0xBC, 0x98);
                    draw_text_centered(r, "Early Access Build",
                                       cx, cy - 50, 13, 0xC0, 0xA0, 0x60);
                    draw_text_centered(r,
                        "This software is still a work in progress.",
                        cx, cy - 15, 12, 0xC0, 0xC0, 0xC0);
                    draw_text_centered(r,
                        "There will be bugs and missing features.",
                        cx, cy + 5, 12, 0xC0, 0xC0, 0xC0);
                    draw_text_centered(r,
                        "Report issues: github.com/Bishop-R/BDOMusicTool",
                        cx, cy + 40, 11, 0x80, 0xB0, 0xD0);
                    draw_text_centered(r,
                        "Discord DM: bishof.",
                        cx, cy + 60, 11, 0x80, 0xA0, 0xD0);
                    if (!app->overlay_dismiss)
                        draw_text_centered(r,
                            "Click or press any key to continue",
                            cx, cy + 100, 10, 0x60, 0x60, 0x60);
                }

                SDL_SetRenderTarget(r, NULL);
                SDL_SetTextureAlphaMod(ov_tex, oa);
                SDL_RenderTexture(r, ov_tex, NULL, NULL);
                SDL_DestroyTexture(ov_tex);
            }
        }
    }

    SDL_RenderPresent(r);
}
