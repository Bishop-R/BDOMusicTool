#ifndef MUSE_APP_H
#define MUSE_APP_H

#include "model.h"
#include "ui_render.h"
#include <SDL3/SDL.h>
#include <stdbool.h>

typedef enum { TOOL_SELECT, TOOL_DRAW, TOOL_ERASE } MuseTool;
typedef enum { GRID_1_4, GRID_1_8, GRID_1_16, GRID_1_32, GRID_1_64, GRID_FREE } GridSnap;

typedef struct MuseApp {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    MuseProject   project;
    MuseTool      tool;
    GridSnap      grid_snap;

    /* view */
    int    key_height;
    double zoom_x;
    double scroll_x;         /* ms */
    int    scroll_y;         /* pitch offset (rows from top) */
    bool   left_panel_open;

    /* mouse */
    float  mouse_x, mouse_y;
    bool   mouse_down_l, mouse_down_r, mouse_down_m;
    float  drag_start_x, drag_start_y;
    bool   dragging;
    bool   selecting;         /* box selection active */
    float  sel_x0, sel_y0, sel_x1, sel_y1;
    bool   brush_selecting;   /* shift+drag brush sel */
    bool   lasso_selecting;   /* alt+drag lasso sel */
    float  *lasso_points;     /* x,y pairs */
    int    lasso_count;
    int    lasso_capacity;
    bool   resizing_note;     /* dragging right edge of note */
    int    resize_note_idx;
    int    resize_note_layer;

    /* note drag */
    bool   moving_notes;
    double move_pitch_offset;
    double move_time_offset;
    float  move_anchor_x, move_anchor_y;

    /* piano key drag preview */
    bool   key_dragging;
    int    key_last_preview;   /* last previewed pitch, -1=none */
    int    key_highlight_pitch; /* pitch to highlight on piano, -1=none */

    /* retrigger flash (visual feedback when a note gets retriggered) */
    double retrigger_start[128]; /* last start per pitch, -1=inactive */
    uint64_t retrigger_flash_tick[128]; /* SDL tick on retrigger */

    /* middle-click pan */
    bool   panning;
    double pan_start_scroll_x;
    int    pan_start_scroll_y;

    /* erase sweep (right-click drag or erase tool drag) */
    bool   erase_sweeping;

    /* velocity pane */
    float  vel_pane_h;           /* current height (resizable) */
    bool   vel_resize_dragging;  /* dragging the top edge to resize */
    float  vel_resize_start_y;   /* mouse Y at drag start */
    float  vel_resize_start_h;   /* pane height at drag start */
    bool   vel_dragging;
    int    vel_drag_note_idx;
    float  vel_drag_start_y;
    int    vel_drag_orig_vel;
    bool   vel_brush_selecting; /* shift+drag brush sel in vel pane */
    bool   vel_set_dragging;    /* shift+right-drag: set stems to mouse y */
    float  vel_set_mouse_x;     /* current mouse pos during set-drag */
    float  vel_set_mouse_y;
    bool   vel_paint_dragging;  /* alt+right-drag: paint velocity on notes */
    float  vel_paint_mouse_x;   /* current mouse pos for readout */
    float  vel_paint_mouse_y;
    float  vel_paint_anchor_x;  /* X where drag started */
    int    vel_paint_orig_vel;   /* velocity at drag start */
    int    vel_paint_note_idx;   /* note being edited */
    int    vel_paint_last_vel;   /* current velocity (for readout) */
    bool   vel_group_dragging;  /* dragging vel-selected group */
    int    vel_group_drag_base; /* base vel at drag start */

    /* scrollbar drag: 0=none, 1=vert, 2=horiz */
    int    sb_dragging;
    float  sb_drag_start;       /* mouse start pos */
    float  sb_drag_orig;        /* original scroll value */

    /* left panel slider drag: 0=none, 1=vol, 2=vel, 3-7=fx, 8=note_vel */
    int    lp_drag_slider;
    float  lp_slider_x;
    float  lp_slider_w;
    int    lp_drag_layer;  /* which layer index for volume slider */

    /* left panel button rects (cached during render) */
    UiRect _lp_import_midi;
    UiRect _lp_export_bdo;
    UiRect _lp_export_wav;
    UiRect _lp_add_inst;
    UiRect _lp_technique;
    UiRect _lp_vel_slider;
    float  _lp_vel_slider_w;
    UiRect _lp_note_vel_slider;
    float  _lp_note_vel_slider_w;
    bool   _lp_note_vel_visible;
    UiRect _lp_fx_sliders[5];
    float  _lp_fx_slider_x;
    float  _lp_fx_slider_w;
    UiRect _lp_link_btn;
    UiRect _lp_fn_entry;

    /* transport / playback */
    double playhead_ms;
    bool   playing;
    bool   paused;
    double loop_start_ms, loop_end_ms;
    bool   looping;
    uint64_t play_start_ticks;
    double   play_start_ms;

    /* hover tracking */
    int    hover_btn;
    int    hover_layer;
    float  left_panel_scroll;
    float  left_panel_scroll_max;

    /* current draw vel/technique (persists between note placements) */
    uint8_t cur_vel;      /* default 100 */
    uint8_t cur_ntype;    /* default 0 */

    /* note move originals (avoids float drift from incremental deltas) */
    double *move_orig_starts;
    int    *move_orig_pitches;
    int     move_orig_count;

    /* inline text editing */
    int    edit_field;    /* 0=none, 1=bpm, 2=measures, 3=filename, 4-8=vel params */
    char   edit_buf[64];
    int    edit_cursor;

    /* toolbar entry rects for click-to-edit */
    UiRect _tb_bpm_entry;
    UiRect _tb_meas_entry;
    UiRect _tb_ts_entry;

    /* midi dialog velocity param entry rects (edit_field 4-8) */
    UiRect _vel_param_rects[5]; /* 0=min, 1=max, 2=floor, 3=step_base, 4=step_size */

    /* dropdown overlay */
    int    dropdown_open;       /* 0=none, 1=time_sig, 2=grid, 3=technique */
    UiRect dropdown_anchor;
    int    dropdown_hover;      /* -1=none */

    /* instrument picker dialog */
    bool   picker_open;
    int    picker_mode;       /* 0=add, 1=move notes, 2=change inst */
    float  picker_scroll;
    int    picker_hover;      /* -1=none */

    /* aux send dialog */
    bool   aux_open;
    int    aux_layer;
    int    aux_drag_slider;   /* 0=none, 1=reverb, 2=delay, 3=chorus */
    UiRect _aux_sliders[3];
    float  _aux_slider_x;
    float  _aux_slider_w;

    /* chord picker */
    bool   chord_open;
    float  chord_scroll;
    int    chord_hover;       /* -1=none */

    /* MIDI import dialog */
    bool   midi_dlg_open;
    float  midi_dlg_scroll;
    int    midi_dlg_hover;       /* -1=none */
    int    midi_dlg_hover_btn;   /* 0=none, 1=import, 2=cancel */
    void  *midi_dlg_data;        /* MidiImportData* */
    int    midi_dlg_dropdown_ch; /* -1=none */
    int    midi_dlg_dropdown_hover;
    float  midi_dlg_dropdown_scroll;
    int    midi_dlg_tech_dropdown_ch;
    int    midi_dlg_tech_dropdown_hover;
    float  midi_dlg_tech_dropdown_scroll;

    /* clipboard */
    NoteArray clipboard;
    double clipboard_cursor_ms;

    /* file */
    char   filename[256];
    char   char_name[64];      /* BDO character name from account link */
    uint32_t linked_owner_id;  /* persistent owner_id from account link */

    /* measures */
    int    measures;

    /* global FX params (maps to BDO's effector values) */
    int    fx_reverb;       /* 0-100 */
    int    fx_delay;        /* 0-100 */
    int    fx_chorus_fb;    /* 0-100 */
    int    fx_chorus_depth; /* 0-100 */
    int    fx_chorus_freq;  /* 0-100 */

    /* toolbar rects (set by ui_toolbar_render) */
    UiRect _tb_new, _tb_open, _tb_save, _tb_play, _tb_stop, _tb_grid;
    float  _tb_tool_x;
    float  _tb_tool_widths[3];

    bool   running;
    bool   focused;
    int    win_w, win_h;

    /* living canvas - fancy visual effects */
    float  anim_time;             /* seconds since start */
    float  dt;                    /* frame delta */

    /* springy piano keys */
    float  key_spring[128];       /* depression offset */
    float  key_spring_vel[128];   /* spring velocity */

    /* aurora nebula (fades in during playback) */
    #define NUM_AURORA 6
    struct { float cx, cy, phase_x, phase_y, speed, radius;
             uint8_t r, g, b; } aurora[6];
    bool   aurora_ready;
    float  continuous_play_secs;  /* how long uninterrupted */

    /* 3D perspective transition (F11) */
    bool   perspective_enabled;
    float  perspective_t;         /* 0=2D, 1=full 3D */
    float  perspective_target;

    /* firefly particles at playhead intersection */
    #define MAX_PARTICLES 200
    struct { float x, y, vx, vy, life, max_life; uint8_t r, g, b; } particles[200];
    int    particle_count;

    /* autosave */
    uint64_t last_autosave_ticks;

    /* status bar messages */
    char     status_msg[128];
    uint64_t status_msg_ticks;

    /* avoid calling SDL_SetWindowTitle every frame */
    char     last_title[320];

    /* cached per-frame to avoid O(N) scan on every call */
    bool     _sel_cache;

    /* startup overlay: 0=none, 1=extracting samples, 2=link account, 3=disclaimer */
    int      overlay_state;
    float    overlay_fade;
    bool     overlay_dismiss;

    /* shortcut cheatsheet window */
    bool     shortcuts_open;
    float    shortcuts_x, shortcuts_y;
    bool     shortcuts_dragging;
    float    shortcuts_drag_ox, shortcuts_drag_oy;
    float    shortcuts_scroll;
    float    _shortcuts_content_h;
} MuseApp;

bool muse_app_init(MuseApp *app);
void muse_app_shutdown(MuseApp *app);
void muse_app_handle_event(MuseApp *app, const SDL_Event *ev);
void muse_app_render(MuseApp *app);

/* coordinate helpers */
double app_x_to_ms(const MuseApp *app, float x);
float  app_ms_to_x(const MuseApp *app, double ms);
int    app_y_to_pitch(const MuseApp *app, float y);
float  app_pitch_to_y(const MuseApp *app, int pitch);
double app_snap_ms(const MuseApp *app, double ms);
float  app_roll_x(const MuseApp *app);
float  app_roll_y(const MuseApp *app);
float  app_roll_w(const MuseApp *app);
float  app_roll_h(const MuseApp *app);

/* active note array shortcut */
NoteArray *app_active_notes(MuseApp *app);

/* check if instrument already used by a layer (exclude_layer=-1 to check all) */
bool inst_already_used(const MuseProject *p, uint8_t inst_id, int exclude_layer);

/* status message (printf-style) */
void set_status_msg(MuseApp *app, const char *fmt, ...);

#endif /* MUSE_APP_H */
