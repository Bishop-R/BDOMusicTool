#include "ui_piano_roll.h"
#include "app.h"
#include "audio.h"
#include "instruments.h"
#include "midi_import.h"
#include "ui.h"
#include "ui_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *NOTE_NAMES[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };

static const uint8_t BLACK_KEY_LUT[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
static int is_black_key(int pitch) {
    return BLACK_KEY_LUT[pitch % 12];
}

/* Build a bitmask of pitches currently sounding at playhead_ms.
   active_pitches[pitch] = 1 if sounding, 2 if retriggered with flash,
   3 if retrigger flash fading out. */
static void get_active_pitches(MuseApp *app, uint8_t active[128]) {
    memset(active, 0, 128);
    if (!app->playing) {
        /* Clear retrigger tracking when not playing */
        for (int i = 0; i < 128; i++) app->retrigger_start[i] = -1.0;
        return;
    }
    double t = app->playhead_ms;
    /* Track latest start time per pitch for current frame */
    double starts[128];
    for (int i = 0; i < 128; i++) starts[i] = -1.0;

    bool has_solo = false;
    for (int li = 0; li < app->project.num_layers; li++)
        if (app->project.layers[li].solo) { has_solo = true; break; }

    for (int li = 0; li < app->project.num_layers; li++) {
        const MuseLayer *ly = &app->project.layers[li];
        if (ly->muted) continue;
        if (has_solo && !ly->solo) continue;
        for (int si = 0; si < ly->num_sublayers; si++) {
            const NoteArray *na = &ly->sublayers[si];
            for (int ni = 0; ni < na->count; ni++) {
                const MuseNote *n = &na->notes[ni];
                if (n->start <= t && t < n->start + n->dur) {
                    int p = n->pitch;
                    if (p < 0 || p > 127) continue;
                    if (!active[p]) {
                        active[p] = 1;
                        starts[p] = n->start;
                    } else if (starts[p] != n->start) {
                        active[p] = 1; /* multiple notes, will check retrigger below */
                        if (n->start > starts[p]) starts[p] = n->start;
                    }
                }
            }
        }
    }

    /* Detect retriggers: start time changed from previous frame */
    uint64_t now = SDL_GetTicks();
    for (int p = 0; p < 128; p++) {
        if (active[p] && starts[p] >= 0) {
            if (app->retrigger_start[p] >= 0 && starts[p] != app->retrigger_start[p]) {
                /* new note on this pitch - trigger flash */
                app->retrigger_flash_tick[p] = now;
            }
            app->retrigger_start[p] = starts[p];
        } else {
            app->retrigger_start[p] = -1.0;
        }

        /* Apply flash state: 80ms bright flash, then 120ms brighter-than-normal */
        if (active[p] && app->retrigger_flash_tick[p] > 0) {
            uint64_t elapsed = now - app->retrigger_flash_tick[p];
            if (elapsed < 80)
                active[p] = 3; /* bright flash */
            else if (elapsed < 200)
                active[p] = 2; /* brighter retrigger gradient */
            /* After 200ms, stays at normal level 1 */
        }
    }
}

void ui_piano_roll_render(MuseApp *app) {
    SDL_Renderer *r = app->renderer;
    int kh = app->key_height;
    float rx = app_roll_x(app);
    float ry = app_roll_y(app);
    float rw = app_roll_w(app);
    float rh = app_roll_h(app);
    float keys_x = (float)(app->left_panel_open ? LEFT_PANEL_W : 0);

    /* During 3D transition: expand roll to fill screen, covering UI panels */
    /* Smoothstep for clean eased transition */
    float persp_raw = app->perspective_t;
    float persp_t = persp_raw * persp_raw * (3.0f - 2.0f * persp_raw);
    if (persp_t > 0.001f) {
        float full_w = (float)app->win_w;
        float full_h = (float)app->win_h;
        rx = rx + (0.0f - rx) * persp_t;
        ry = ry + (0.0f - ry) * persp_t;
        rw = rw + (full_w - rw) * persp_t;
        rh = rh + (full_h - rh) * persp_t;
        keys_x = keys_x * (1.0f - persp_t);
    }

    const MuseInstrument *inst = NULL;
    if (app->project.num_layers > 0) {
        MuseLayer *ly = &app->project.layers[app->project.active_layer];
        inst = inst_by_id(ly->inst_id);
    }

    double bpx = BEAT_WIDTH_DEFAULT * app->zoom_x;
    int bpm = app->project.bpm ? app->project.bpm : 120;
    double mpb = 60000.0 / bpm;
    int beats_per_measure = app->project.time_sig ? app->project.time_sig : 4;
    double start_ms = app->scroll_x;
    int first_beat = (int)(start_ms / mpb);
    if (first_beat < 0) first_beat = 0;

    /* Fill expanded area with roll background during perspective transition */
    if (persp_t > 0.01f) {
        draw_filled_rect(r, rx, ry, rw, rh, COL_BG_DARK, 0xFF);
    }

    /* Clip to roll area for grid + notes */
    push_clip(r, rx, ry, rw, rh);

    /* Compute visible pitch range to avoid iterating all 108 pitches */
    int vis_pitch_hi = app_y_to_pitch(app, ry);         /* top of view = highest visible pitch */
    int vis_pitch_lo = app_y_to_pitch(app, ry + rh);    /* bottom of view = lowest visible pitch */
    if (vis_pitch_hi > PITCH_MAX) vis_pitch_hi = PITCH_MAX;
    if (vis_pitch_lo < PITCH_MIN) vis_pitch_lo = PITCH_MIN;
    /* Add 1-pitch margin for partial rows */
    if (vis_pitch_hi < PITCH_MAX) vis_pitch_hi++;
    if (vis_pitch_lo > PITCH_MIN) vis_pitch_lo--;

    /* row backgrounds - fade out when entering 3D mode */
    {
        uint8_t row_a = (uint8_t)(0xFF * fmaxf(1.0f - persp_t * 3.0f, 0.0f));
        if (row_a > 0) {
            for (int pitch = vis_pitch_lo; pitch <= vis_pitch_hi; pitch++) {
                float y = app_pitch_to_y(app, pitch);

                float y0 = y < ry ? ry : y;
                float y1 = (y + kh) > (ry + rh) ? (ry + rh) : (y + kh);
                float h  = y1 - y0;
                int oor = inst && (pitch < inst->pitch_lo || pitch > inst->pitch_hi ||
                          inst_is_spacer_key(inst->id, (uint8_t)pitch));

                if (oor)
                    draw_filled_rect(r, rx, y0, rw, h, COL_OOR_DARK, row_a);
                else if (!is_black_key(pitch))
                    draw_filled_rect(r, rx, y0, rw, h, 0x1A, 0x1A, 0x1D, row_a);

                /* Horizontal grid lines */
                int sem = pitch % 12;
                if (sem == 0)
                    draw_hline(r, rx, rx + rw, y0, COL_BORDER);
                else
                    draw_hline(r, rx, rx + rw, y0, COL_GRID_SUB);
            }
        }
    }

    /* subtle gold band showing the playable range for the current technique
       (only visible when it differs from the default technique) */
    if (inst && inst->num_techniques > 0 && app->cur_ntype != inst->techniques[0]) {
        int tech_lo, tech_hi, def_lo, def_hi;
        bool has_tech = muse_audio_technique_range(inst->id, app->cur_ntype, &tech_lo, &tech_hi);
        bool has_def  = muse_audio_technique_range(inst->id, inst->techniques[0], &def_lo, &def_hi);
        if (has_tech && has_def &&
            (tech_lo > def_lo + 2 || tech_hi < def_hi - 2)) {
            uint8_t band_a = 0x18;
            int tvis_lo = tech_lo > vis_pitch_lo ? tech_lo : vis_pitch_lo;
            int tvis_hi = tech_hi < vis_pitch_hi ? tech_hi : vis_pitch_hi;
            for (int pitch = tvis_lo; pitch <= tvis_hi; pitch++) {
                float y = app_pitch_to_y(app, pitch);
                float y0 = y < ry ? ry : y;
                float y1 = (y + kh) > (ry + rh) ? (ry + rh) : (y + kh);
                draw_filled_rect(r, rx, y0, rw, y1 - y0, COL_GOLD_DARK, band_a);
            }
        }
    }

    /* red danger zone - BDO caps notes at 10k, show where you went over */
    if (inst) {
        MuseLayer *ely = &app->project.layers[app->project.active_layer];
        double exceed_ms = muse_layer_exceed_ms(ely);
        if (exceed_ms >= 0) {
            float ex_x = rx + (float)((exceed_ms - app->scroll_x) / mpb * bpx);
            if (ex_x < rx + rw) {
                float ex_w = rx + rw - ex_x;
                if (ex_x < rx) { ex_w -= (rx - ex_x); ex_x = rx; }
                if (ex_w > 0)
                    draw_filled_rect(r, ex_x, ry, ex_w, rh, 0xE0, 0x30, 0x30, 0x20);
            }
        }
    }

    /* fades in during sustained playback */
    if (app->continuous_play_secs > 20.0f) {
        float aurora_t = fminf((app->continuous_play_secs - 20.0f) / 10.0f, 1.0f);
        float t = app->anim_time;
        for (int i = 0; i < NUM_AURORA; i++) {
            float bx = rx + rw * (0.5f + 0.35f * sinf(t * app->aurora[i].speed + app->aurora[i].phase_x));
            float by = ry + rh * (0.5f + 0.30f * cosf(t * app->aurora[i].speed * 0.7f + app->aurora[i].phase_y));
            uint8_t ba = (uint8_t)(22.0f * aurora_t);
            draw_aurora_blob(r, bx, by, app->aurora[i].radius,
                             app->aurora[i].r, app->aurora[i].g, app->aurora[i].b, ba);
        }
    }

    /* vertical grid lines (fade out during 3D transition) */
    if (app->perspective_t > 0.4f) goto skip_2d_grid;
    /* Subdivision lines first (lightest) */
    {
        int subdiv = 1;
        switch (app->grid_snap) {
        case GRID_1_4:  subdiv = 1; break;
        case GRID_1_8:  subdiv = 2; break;
        case GRID_1_16: subdiv = 4; break;
        case GRID_1_32: subdiv = 8; break;
        case GRID_1_64: subdiv = 16; break;
        default: break;
        }
        if (subdiv > 1) {
            int first_sub = (int)(start_ms / (mpb / subdiv));
            if (first_sub < 0) first_sub = 0;
            for (int s = first_sub; ; s++) {
                double ms = s * (mpb / subdiv);
                float x = rx + (float)((ms - start_ms) / mpb * bpx);
                if (x > rx + rw) break;
                if (x < rx) continue;
                if (s % subdiv != 0)
                    draw_vline(r, x, ry, ry + rh, COL_GRID_SUB);
            }
        }
    }

    for (int b = first_beat; ; b++) {
        double ms = b * mpb;
        float x = rx + (float)((ms - start_ms) / mpb * bpx);
        if (x > rx + rw) break;
        if (x < rx) continue;
        if (b % beats_per_measure == 0)
            draw_vline(r, x, ry, ry + rh, COL_GRID_MEASURE);
        else
            draw_vline(r, x, ry, ry + rh, COL_GRID_BEAT);
    }
    skip_2d_grid:;

    /* 3D perspective grid - camera looks right, notes fly toward you */
    if (app->perspective_t > 0.01f) {
        float pt = persp_t; /* smoothstepped */
        float vp_x = rx + rw;           /* vanishing point: RIGHT edge */
        float vp_y = ry + rh * 0.5f;    /* vertically centered */
        float focal = 400.0f;

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

        /* pitch lines converging toward vanishing point on the right */
        for (int pitch = PITCH_MIN; pitch <= PITCH_MAX; pitch += 12) {
            float py_full = app_pitch_to_y(app, pitch);
            uint8_t la = (uint8_t)(35 * pt);
            SDL_SetRenderDrawColor(r, COL_BORDER, la);
            SDL_RenderLine(r, rx, py_full, vp_x, vp_y);
        }

        /* depth markers - beat lines receding into the distance */
        int bpm_3d = app->project.bpm ? app->project.bpm : 120;
        double mpb_3d = 60000.0 / bpm_3d;
        int bpm_3d_ts = app->project.time_sig ? app->project.time_sig : 4;
        for (int b = 0; b < 60; b++) {
            float time_ms = (float)(b * mpb_3d);
            float z = time_ms * 0.06f;
            float persp = focal / (z + focal);
            float x_3d = vp_x - rw * 0.95f * persp;
            float half_h = rh * 0.48f * persp;
            uint8_t la = (uint8_t)(25 * pt * persp);
            if (la < 2) continue;
            if (b % bpm_3d_ts == 0) {
                SDL_SetRenderDrawColor(r, COL_GRID_MEASURE, la);
            } else {
                SDL_SetRenderDrawColor(r, COL_GRID_BEAT, la);
            }
            SDL_RenderLine(r, x_3d, vp_y - half_h, x_3d, vp_y + half_h);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    }

    /* draw all notes across all layers, dim inactive ones */
    double vis_start_ms = app->scroll_x;
    double vis_end_ms = app->scroll_x + (double)rw / bpx * mpb;

    /* Pre-compute perspective constants (loop-invariant) */
    float vp_x = rx + rw;
    float vp_y = ry + rh * 0.5f;
    const float focal = 400.0f;
    float note_lh = text_line_height(6.0f);

    for (int li = 0; li < app->project.num_layers; li++) {
        MuseLayer *ly = &app->project.layers[li];
        if (ly->muted) continue;
        bool is_active_layer = (li == app->project.active_layer);

        for (int si = 0; si < ly->num_sublayers; si++) {
            NoteArray *na = &ly->sublayers[si];
            bool is_active_sub = is_active_layer && (si == ly->active_sub);
            /* dim_mode: 0=active sublayer, 1=inactive sub, 2=inactive layer */
            int dim_mode = is_active_sub ? 0 : (is_active_layer ? 1 : 2);

            for (int n = 0; n < na->count; n++) {
                MuseNote *note = &na->notes[n];
                /* Cull notes outside visible time range (skip 3D mode which shows different range) */
                if (app->perspective_t < 0.5f) {
                    if (note->start > vis_end_ms) continue;
                    if (note->start + note->dur < vis_start_ms) continue;
                }
                float ny = app_pitch_to_y(app, note->pitch);

                float nx = rx + (float)((note->start - start_ms) / mpb * bpx);
                float nw = (float)(note->dur / mpb * bpx);
                if (nw < 4) nw = 4;

                /* 3D perspective interpolation */
                float nh_2d = (float)kh;
                if (app->perspective_t > 0.001f) {
                    float pt = persp_t; /* smoothstepped */

                    /* Time ahead of playhead → depth */
                    float time_ahead_ms = (float)(note->start - app->playhead_ms);
                    float z = time_ahead_ms * 0.06f;

                    /* cull by end time, keep notes that are still sounding */
                    float end_ahead_ms = (float)(note->start + note->dur - app->playhead_ms);
                    if (end_ahead_ms * 0.06f < -40.0f && pt > 0.8f) continue;

                    float persp = focal / (fmaxf(z, 0.5f) + focal);

                    /* Notes approach from RIGHT: close=left, far=right */
                    float nx_3d = vp_x - rw * 0.95f * persp;

                    /* Pitch spread narrows toward vanishing point */
                    float pitch_y = app_pitch_to_y(app, note->pitch) + (float)kh * 0.5f;
                    float ny_3d = vp_y + (pitch_y - vp_y) * persp - (float)kh * persp * 0.5f;

                    /* Note size scales with perspective */
                    float nw_3d = fmaxf((float)(note->dur * 0.06f) * persp, 2.0f);
                    float nh_3d = (float)kh * persp;

                    /* Interpolate between 2D and 3D */
                    nx = nx + (nx_3d - nx) * pt;
                    ny = ny + (ny_3d - ny) * pt;
                    nw = nw + (nw_3d - nw) * pt;
                    nh_2d = nh_2d + (nh_3d - nh_2d) * pt;

                    /* In 3D, show notes further ahead that 2D would cull */
                    if (pt > 0.5f && nx + nw >= rx && nx <= rx + rw) {
                        /* Keep this note even if 2D would skip it */
                    } else if (nx + nw < rx || nx > rx + rw) {
                        continue;
                    }
                }

                if (ny + nh_2d < ry || ny > ry + rh) continue;
                if (app->perspective_t < 0.5f && (nx + nw < rx || nx > rx + rw)) continue;

                const MuseTechnique *tech = technique_by_id(note->ntype);
                int tcr = tech ? tech->r : 120;
                int tcg = tech ? tech->g : 140;
                int tcb = tech ? tech->b : 170;

                /* brightness based on velocity and whether this is the active sublayer */
                int dr, dg, db;
                if (dim_mode == 2) {
                    /* Inactive layer: divide by 3 */
                    dr = tcr / 3; dg = tcg / 3; db = tcb / 3;
                } else if (dim_mode == 1) {
                    /* Inactive sublayer */
                    float f = 0.25f + 0.2f * (note->vel / 127.0f);
                    dr = (int)(tcr * f); if (dr > 255) dr = 255;
                    dg = (int)(tcg * f); if (dg > 255) dg = 255;
                    db = (int)(tcb * f); if (db > 255) db = 255;
                } else {
                    /* Active sublayer */
                    float f = 0.5f + 0.5f * (note->vel / 127.0f);
                    dr = (int)(tcr * f); if (dr > 255) dr = 255;
                    dg = (int)(tcg * f); if (dg > 255) dg = 255;
                    db = (int)(tcb * f); if (db > 255) db = 255;
                }

                /* note quad - does proper perspective projection in 3D mode */
                float bar_h = 3;

                /* Compute 2D corner positions */
                float x2_l = nx, x2_r = nx + nw;
                float y2_t = ny + 1, y2_b = ny + 1 + nh_2d - 2;

                /* Compute 3D trapezoid corners */
                float tl_x, tl_y, tr_x, tr_y, br_x, br_y, bl_x, bl_y;
                float near_bright = 1.0f, far_bright = 1.0f;

                if (persp_t > 0.01f) {

                    /* Near edge = note start, Far edge = note end */
                    float t_near = (float)(note->start - app->playhead_ms);
                    float t_far  = (float)(note->start + note->dur - app->playhead_ms);
                    float z_near = t_near * 0.06f;
                    float z_far  = t_far  * 0.06f;
                    /* clamp at playhead so notes don't clip through the camera */
                    float z_near_clamped = fmaxf(z_near, 0.0f);
                    float z_far_clamped  = fmaxf(z_far,  0.0f);
                    float p_near = focal / (z_near_clamped + focal);
                    float p_far  = focal / (z_far_clamped  + focal);

                    float pitch_top = app_pitch_to_y(app, note->pitch);
                    float pitch_bot = pitch_top + (float)kh;

                    /* 3D projected corners */
                    float xn = vp_x - rw * 0.95f * p_near;
                    float xf = vp_x - rw * 0.95f * p_far;
                    float yt_n = vp_y + (pitch_top - vp_y) * p_near;
                    float yb_n = vp_y + (pitch_bot - vp_y) * p_near;
                    float yt_f = vp_y + (pitch_top - vp_y) * p_far;
                    float yb_f = vp_y + (pitch_bot - vp_y) * p_far;

                    /* lerp 2D to 3D corners */
                    float pt = persp_t;
                    tl_x = x2_l + (xn   - x2_l) * pt;
                    tl_y = y2_t + (yt_n  - y2_t) * pt;
                    tr_x = x2_r + (xf   - x2_r) * pt;
                    tr_y = y2_t + (yt_f  - y2_t) * pt;
                    br_x = x2_r + (xf   - x2_r) * pt;
                    br_y = y2_b + (yb_f  - y2_b) * pt;
                    bl_x = x2_l + (xn   - x2_l) * pt;
                    bl_y = y2_b + (yb_n  - y2_b) * pt;

                    /* Depth brightness: near=full, far=dimmer */
                    near_bright = 1.0f - fminf(fmaxf(t_near, 0) * 0.00005f, 0.5f);
                    far_bright  = 1.0f - fminf(fmaxf(t_far,  0) * 0.00005f, 0.7f);
                    near_bright = 1.0f - persp_t + persp_t * near_bright;
                    far_bright  = 1.0f - persp_t + persp_t * far_bright;
                } else {
                    tl_x = x2_l; tl_y = y2_t;
                    tr_x = x2_r; tr_y = y2_t;
                    br_x = x2_r; br_y = y2_b;
                    bl_x = x2_l; bl_y = y2_b;
                }

                /* Draw perspective quad with vertex colors for depth shading */
                {
                    float rn = dr / 255.0f * near_bright;
                    float gn = dg / 255.0f * near_bright;
                    float bn = db / 255.0f * near_bright;
                    float rf = dr / 255.0f * far_bright;
                    float gf = dg / 255.0f * far_bright;
                    float bf = db / 255.0f * far_bright;
                    SDL_FColor cn = { rn, gn, bn, 1.0f };
                    SDL_FColor cf = { rf, gf, bf, 1.0f };

                    if (persp_t > 0.1f) {
                        /* per-edge normal AA fringe */
                        float aa = 0.7f;
                        float px[4] = {tl_x, tr_x, br_x, bl_x};
                        float py[4] = {tl_y, tr_y, br_y, bl_y};
                        float ox[4], oy[4];
                        for (int ei = 0; ei < 4; ei++) {
                            int n2 = (ei + 1) % 4;
                            float edx = px[n2] - px[ei], edy = py[n2] - py[ei];
                            float el = sqrtf(edx*edx + edy*edy);
                            if (el < 0.001f) el = 1.0f;
                            float enx = edy / el, eny = -edx / el;
                            int prev = (ei + 3) % 4;
                            float pedx = px[ei] - px[prev], pedy = py[ei] - py[prev];
                            float pl = sqrtf(pedx*pedx + pedy*pedy);
                            if (pl < 0.001f) pl = 1.0f;
                            float pnx = pedy / pl, pny = -pedx / pl;
                            ox[ei] = (pnx + enx) * 0.5f * aa;
                            oy[ei] = (pny + eny) * 0.5f * aa;
                        }
                        SDL_FColor c[4] = {cn, cf, cf, cn};
                        SDL_FColor c0[4] = {{rn,gn,bn,0},{rf,gf,bf,0},{rf,gf,bf,0},{rn,gn,bn,0}};
                        /* inner solid */
                        SDL_Vertex iv[4];
                        for (int j=0;j<4;j++) { iv[j].position.x=px[j]-ox[j]; iv[j].position.y=py[j]-oy[j]; iv[j].color=c[j]; }
                        int ii[6]={0,1,2,0,2,3};
                        SDL_RenderGeometry(r,NULL,iv,4,ii,6);
                        /* fringe strips */
                        for (int ei=0;ei<4;ei++) {
                            int n2=(ei+1)%4;
                            SDL_Vertex fv[4]={
                                {.position={px[ei]-ox[ei],py[ei]-oy[ei]},.color=c[ei]},
                                {.position={px[n2]-ox[n2],py[n2]-oy[n2]},.color=c[n2]},
                                {.position={px[n2]+ox[n2],py[n2]+oy[n2]},.color=c0[n2]},
                                {.position={px[ei]+ox[ei],py[ei]+oy[ei]},.color=c0[ei]},
                            };
                            int fi[6]={0,1,2,0,2,3};
                            SDL_RenderGeometry(r,NULL,fv,4,fi,6);
                        }
                    } else {
                        SDL_Vertex verts[4] = {
                            { .position={tl_x, tl_y}, .color=cn },
                            { .position={tr_x, tr_y}, .color=cf },
                            { .position={br_x, br_y}, .color=cf },
                            { .position={bl_x, bl_y}, .color=cn },
                        };
                        int idx[6] = {0, 1, 2, 0, 2, 3};
                        SDL_RenderGeometry(r, NULL, verts, 4, idx, 6);
                    }
                }

                /* Top edge highlight for 3D depth */
                if (persp_t > 0.3f) {
                    float ha = 0.15f * persp_t;
                    SDL_FColor hn = {1, 1, 1, ha};
                    SDL_FColor hf = {1, 1, 1, ha * far_bright};
                    SDL_Vertex hv[4] = {
                        { .position={tl_x, tl_y},     .color=hn },
                        { .position={tr_x, tr_y},     .color=hf },
                        { .position={tr_x, tr_y + 1}, .color=hf },
                        { .position={tl_x, tl_y + 1}, .color=hn },
                    };
                    int hi[6] = {0, 1, 2, 0, 2, 3};
                    SDL_RenderGeometry(r, NULL, hv, 4, hi, 6);
                }

                /* outline */
                if (note->selected) {
                    SDL_SetRenderDrawColor(r, COL_GOLD_BRIGHT, 0xFF);
                } else {
                    uint8_t oa = (persp_t > 0.5f) ? (uint8_t)(0x44 * far_bright) : 0x44;
                    SDL_SetRenderDrawColor(r, 0x44, 0x43, 0x48, oa);
                }
                SDL_RenderLine(r, tl_x, tl_y, tr_x, tr_y);   /* top */
                SDL_RenderLine(r, tr_x, tr_y, br_x, br_y);   /* right (far) */
                SDL_RenderLine(r, br_x, br_y, bl_x, bl_y);   /* bottom */
                SDL_RenderLine(r, bl_x, bl_y, tl_x, tl_y);   /* left (near) */

                /* spawn firefly particles where the playhead crosses notes */
                if (persp_t > 0.3f && app->playing &&
                    note->start <= app->playhead_ms &&
                    note->start + note->dur > app->playhead_ms &&
                    app->particle_count < MAX_PARTICLES) {
                    /* Spawn ~1 particle per note per few frames */
                    uint32_t seed = (uint32_t)(note->pitch * 17 + (int)(app->anim_time * 60));
                    if ((seed % 4) == 0) {
                        int pi = app->particle_count++;
                        /* Spawn at the near (left) edge of the note = playhead position */
                        app->particles[pi].x = tl_x;
                        app->particles[pi].y = tl_y + (bl_y - tl_y) * 0.5f;
                        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                        float angle = ((seed & 0xFF) / 255.0f) * 6.28f;
                        float speed = 15.0f + (((seed >> 8) & 0xFF) / 255.0f) * 30.0f;
                        app->particles[pi].vx = cosf(angle) * speed;
                        app->particles[pi].vy = sinf(angle) * speed - 10.0f;
                        app->particles[pi].life = 0.6f + ((seed >> 16) & 0xFF) / 255.0f * 0.8f;
                        app->particles[pi].max_life = app->particles[pi].life;
                        app->particles[pi].r = (uint8_t)dr;
                        app->particles[pi].g = (uint8_t)dg;
                        app->particles[pi].b = (uint8_t)db;
                    }
                }

                /* velocity bar at bottom of note */
                if (kh > 6) {
                    float vel_alpha = 1.0f - fminf(persp_t * 8.0f, 1.0f);
                    if (vel_alpha > 0.01f) {
                        float vw = (note->vel / 127.0f) * nw;
                        draw_filled_rect(r, nx, ny + nh_2d - 1 - bar_h, vw, bar_h,
                                         0xFF, 0xFF, 0xFF, (uint8_t)(0xFF * vel_alpha));
                    }
                }

                /* note label (fades out early in 3D to reduce clutter) */
                if (nw >= 20 && nh_2d >= 10 && persp_t < 0.125f) {
                    float label_alpha = 1.0f - fminf(persp_t * 8.0f, 1.0f);
                    char lbl[8];
                    int sem = note->pitch % 12;
                    int oct = note->pitch / 12 - 1 + (inst ? inst->oct_offset : 0);
                    const char *dname = NULL;
                    if (inst && inst->is_drum)
                        dname = drum_key_name(inst->id, note->pitch);
                    if (dname)
                        snprintf(lbl, sizeof(lbl), "%s", dname);
                    else
                        snprintf(lbl, sizeof(lbl), "%s%d", NOTE_NAMES[sem], oct);
                    float fsz = 6.0f;
                    float label_y = ny + (nh_2d - note_lh) / 2.0f;
                    uint8_t lc_base = (dim_mode == 0) ? 0x00 : 0x33;
                    uint8_t lc = (uint8_t)(lc_base + (0xFF - lc_base) * (1.0f - label_alpha));
                    draw_text(r, lbl, nx + 3, label_y, fsz, lc, lc, lc);
                }
            }
        }
    }

    /* box selection overlay - dashed gold rectangle */
    if (app->selecting) {
        float sx0 = app->sel_x0 < app->sel_x1 ? app->sel_x0 : app->sel_x1;
        float sy0 = app->sel_y0 < app->sel_y1 ? app->sel_y0 : app->sel_y1;
        float sw = fabsf(app->sel_x1 - app->sel_x0);
        float sh = fabsf(app->sel_y1 - app->sel_y0);
        /* batch all dashes into one draw call for perf */
        SDL_FRect dashes[1024]; /* plenty for any reasonable selection */
        int nd = 0;
        /* Horizontal dashes (top + bottom, 1px thick rects) */
        for (float dx = 0; dx < sw && nd < 1020; dx += 8) {
            float seg = (dx + 4 < sw) ? 4 : sw - dx;
            dashes[nd++] = (SDL_FRect){ sx0 + dx, sy0, seg, 1 };
            dashes[nd++] = (SDL_FRect){ sx0 + dx, sy0 + sh, seg, 1 };
        }
        /* Vertical dashes (left + right, 1px wide rects) */
        for (float dy = 0; dy < sh && nd < 1020; dy += 8) {
            float seg = (dy + 4 < sh) ? 4 : sh - dy;
            dashes[nd++] = (SDL_FRect){ sx0, sy0 + dy, 1, seg };
            dashes[nd++] = (SDL_FRect){ sx0 + sw, sy0 + dy, 1, seg };
        }
        SDL_SetRenderDrawColor(r, COL_GOLD, 0xFF);
        SDL_RenderFillRects(r, dashes, nd);
    }

    /* lasso selection */
    if (app->lasso_selecting && app->lasso_count >= 2 && app->lasso_points) {
        SDL_SetRenderDrawColor(r, COL_GOLD, 0xFF);
        for (int i = 1; i < app->lasso_count; i++) {
            SDL_RenderLine(r,
                app->lasso_points[(i-1)*2], app->lasso_points[(i-1)*2+1],
                app->lasso_points[i*2], app->lasso_points[i*2+1]);
        }
    }

    /* loop region - dim everything outside, gold boundaries */
    if (app->looping) {
        float lx0 = rx + (float)((app->loop_start_ms - start_ms) / mpb * bpx);
        float lx1 = rx + (float)((app->loop_end_ms - start_ms) / mpb * bpx);
        /* dim outside the loop */
        if (lx0 > rx)
            draw_filled_rect(r, rx, ry, lx0 - rx, rh, 0x00, 0x00, 0x00, 0x40);
        if (lx1 < rx + rw)
            draw_filled_rect(r, lx1, ry, rx + rw - lx1, rh, 0x00, 0x00, 0x00, 0x40);
        /* Loop boundary lines (gold, 2px wide) */
        float cx0 = lx0 < rx ? rx : lx0;
        float cx1 = lx1 > rx + rw ? rx + rw : lx1;
        if (cx0 >= rx && cx0 <= rx + rw) {
            draw_vline(r, cx0, ry, ry + rh, COL_GOLD);
            draw_vline(r, cx0 + 1, ry, ry + rh, COL_GOLD);
        }
        if (cx1 >= rx && cx1 <= rx + rw) {
            draw_vline(r, cx1, ry, ry + rh, COL_GOLD);
            draw_vline(r, cx1 - 1, ry, ry + rh, COL_GOLD);
        }
    }

    /* firefly particles */
    if (persp_t > 0.3f && app->particle_count > 0) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_ADD);
        for (int i = 0; i < app->particle_count; i++) {
            float lf = app->particles[i].life / app->particles[i].max_life;
            /* Fade in quickly, fade out slowly */
            float alpha = lf < 0.8f ? lf / 0.8f : (1.0f - lf) / 0.2f;
            if (alpha < 0) alpha = 0;
            float sz = 1.5f + lf * 2.5f;
            uint8_t a = (uint8_t)(220 * alpha * persp_t);
            SDL_SetRenderDrawColor(r, app->particles[i].r, app->particles[i].g,
                                      app->particles[i].b, a);
            SDL_FRect pr = { app->particles[i].x - sz/2, app->particles[i].y - sz/2, sz, sz };
            SDL_RenderFillRect(r, &pr);
            /* Bright core */
            if (sz > 2) {
                SDL_SetRenderDrawColor(r, 0xFF, 0xFF, 0xFF, (uint8_t)(a * 0.5f));
                SDL_FRect pc = { app->particles[i].x - 0.5f, app->particles[i].y - 0.5f, 1, 1 };
                SDL_RenderFillRect(r, &pc);
            }
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    }

    /* playhead line */
    {
        float px_2d = rx + (float)((app->playhead_ms - start_ms) / mpb * bpx);
        /* In 3D: playhead is at z=0 (camera position) → projects to left edge */
        float focal_ph = 400.0f;
        float px_3d = (rx + rw) - rw * 0.95f * (focal_ph / (0.5f + focal_ph));
        float px = px_2d + (px_3d - px_2d) * persp_t;

        if (px >= rx - 10 && px <= rx + rw + 10) {
            if (persp_t < 0.2f) {
                /* 2D glow */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                uint8_t ga = (uint8_t)(0x28 * (1.0f - persp_t));
                SDL_SetRenderDrawColor(r, COL_PLAYHEAD, ga);
                SDL_FRect g1 = { px - 2, ry, 1, rh }; SDL_RenderFillRect(r, &g1);
                SDL_FRect g2 = { px + 3, ry, 1, rh }; SDL_RenderFillRect(r, &g2);
                ga = (uint8_t)(0x50 * (1.0f - persp_t));
                SDL_SetRenderDrawColor(r, COL_PLAYHEAD, ga);
                SDL_FRect g3 = { px - 1, ry, 1, rh }; SDL_RenderFillRect(r, &g3);
                SDL_FRect g4 = { px + 2, ry, 1, rh }; SDL_RenderFillRect(r, &g4);
            }
            /* Core line */
            draw_vline(r, px, ry, ry + rh, COL_PLAYHEAD);
            draw_vline(r, px + 1, ry, ry + rh, COL_PLAYHEAD);
        }
    }

    pop_clip(r);

    /* Fade piano keys during perspective transition */
    if (app->perspective_t > 0.99f) goto skip_piano_keys;

    /* piano keys sidebar */
    uint8_t active_pitches[128];
    get_active_pitches(app, active_pitches);

    push_clip(r, keys_x, ry, KEYS_WIDTH, rh);
    float key_lh = text_line_height(7.0f);
    for (int pitch = vis_pitch_lo; pitch <= vis_pitch_hi; pitch++) {
        float y = app_pitch_to_y(app, pitch);

        int oor = inst && (pitch < inst->pitch_lo || pitch > inst->pitch_hi ||
                          inst_is_spacer_key(inst->id, (uint8_t)pitch));
        int sem = pitch % 12;
        int oct = pitch / 12 - 1 + (inst ? inst->oct_offset : 0);
        int ap = (pitch >= 0 && pitch <= 127) ? active_pitches[pitch] : 0;
        if (!ap && pitch == app->key_highlight_pitch) ap = 1;

        /* spring physics - update before computing y0/y1 */
        if (pitch >= 0 && pitch < 128) {
            float target = (ap && !oor) ? 3.0f : 0.0f;
            float force = (target - app->key_spring[pitch]) * 350.0f;
            app->key_spring_vel[pitch] += force * app->dt;
            app->key_spring_vel[pitch] *= 0.86f;
            app->key_spring[pitch] += app->key_spring_vel[pitch] * app->dt;
            y += app->key_spring[pitch];
        }

        float y0 = y < ry ? ry : y;
        float y1 = (y + kh) > (ry + rh) ? (ry + rh) : (y + kh);
        float h = y1 - y0;

        if (ap && !oor) {
            /* Playback highlight: horizontal gradient (muted→warm gold) */
            uint8_t r0, g0, b0, r1_c, g1_c, b1_c;
            if (ap == 3) {
                /* Retrigger flash: bright white-gold burst */
                if (is_black_key(pitch))
                    { r0=0x80; g0=0x70; b0=0x50; r1_c=0xE0; g1_c=0xD0; b1_c=0x90; }
                else
                    { r0=0xA0; g0=0x90; b0=0x70; r1_c=0xFF; g1_c=0xF0; b1_c=0xC0; }
            } else if (ap == 2) {
                /* Retrigger: brighter gradient */
                if (is_black_key(pitch))
                    { r0=0x40; g0=0x35; b0=0x28; r1_c=0xa0; g1_c=0x80; b1_c=0x48; }
                else
                    { r0=0x55; g0=0x48; b0=0x35; r1_c=0xc0; g1_c=0xa8; b1_c=0x70; }
            } else {
                if (is_black_key(pitch))
                    { r0=0x2e; g0=0x28; b0=0x20; r1_c=0x6a; g1_c=0x58; b1_c=0x38; }
                else
                    { r0=0x40; g0=0x38; b0=0x2a; r1_c=0x8a; g1_c=0x78; b1_c=0x50; }
            }
            draw_hgradient_rect(r, keys_x, y0, KEYS_WIDTH, h,
                                r0, g0, b0, r1_c, g1_c, b1_c);
        } else if (oor)
            draw_filled_rect(r, keys_x, y0, KEYS_WIDTH, h, COL_OOR_BG, 0xFF);
        else if (is_black_key(pitch))
            draw_filled_rect(r, keys_x, y0, KEYS_WIDTH, h, COL_BLACK_KEY, 0xFF);
        else
            draw_filled_rect(r, keys_x, y0, KEYS_WIDTH, h, COL_WHITE_KEY, 0xFF);

        /* thin outline around each key */
        draw_rect_outline(r, keys_x, y0, (float)KEYS_WIDTH, (float)kh, COL_BG_DARK);

        /* Key label */
        if (kh >= 8) {
            char lbl[8];
            const char *dname = NULL;
            if (inst && inst->is_drum)
                dname = drum_key_name(inst->id, pitch);
            if (dname)
                snprintf(lbl, sizeof(lbl), "%s", dname);
            else
                snprintf(lbl, sizeof(lbl), "%s%d", NOTE_NAMES[sem], oct);

            float fsz = 7.0f;
            uint8_t tr, tg, tb;
            if (ap && !oor) {
                /* Highlighted key: dark label for contrast (bg_dark) */
                tr = 0x16; tg = 0x16; tb = 0x18;
            } else if (oor)           { tr = 0x33; tg = 0x33; tb = 0x35; }
            else if (sem == 0) { tr = 0xD8; tg = 0xAD; tb = 0x70; }
            else if (is_black_key(pitch)) { tr = 0x88; tg = 0x88; tb = 0x88; }
            else               { tr = 0xCC; tg = 0xCC; tb = 0xCC; }

            if (!ap && inst && inst->is_drum && dname) {
                tr = 0xDD; tg = 0xC3; tb = 0x9E;
            }

            draw_text_right(r, lbl, keys_x + KEYS_WIDTH - 4,
                           y + (kh - key_lh) / 2.0f,
                           fsz, tr, tg, tb);
        }
    }
    /* Right border on keys */
    draw_vline(r, keys_x + KEYS_WIDTH - 1, ry, ry + rh, COL_BORDER);
    /* Fade piano keys during transition */
    if (persp_t > 0.01f) {
        uint8_t fa = (uint8_t)(fminf(persp_t * 3.0f, 1.0f) * 255);
        draw_filled_rect(r, keys_x, ry, (float)KEYS_WIDTH, rh, 0x16, 0x16, 0x18, fa);
    }
    pop_clip(r);
    skip_piano_keys:;

    /* timeline header (hidden in 3D) */
    if (persp_t > 0.99f) goto skip_timeline_header;
    {
        float hdr_y = (float)(TRANSPORT_H + 2);
        /* Corner canvas with collapse lip */
        draw_filled_rect(r, keys_x, hdr_y, (float)KEYS_WIDTH, HEADER_HEIGHT,
                         COL_BG_DARK, 0xFF);
        /* Collapse lip tab */
        {
            float lip_w = CORNER_TAB_W;
            float lip_x = keys_x + KEYS_WIDTH - lip_w;
            draw_rounded_rect(r, lip_x, hdr_y, lip_w, HEADER_HEIGHT, 4,
                             COL_BG, 0xFF);
            /* Arrow: ◀ when open, ▶ when closed */
            const char *arrow = app->left_panel_open ? "<" : ">";
            draw_text_centered(r, arrow, lip_x + lip_w / 2, hdr_y + HEADER_HEIGHT / 2,
                              6, COL_TEXT_DIM);
        }
        draw_filled_rect(r, rx, hdr_y, rw, HEADER_HEIGHT, COL_BG_DARK, 0xFF);

        push_clip(r, rx, hdr_y, rw, HEADER_HEIGHT);
        for (int b = first_beat; ; b++) {
            double ms = b * mpb;
            float x = rx + (float)((ms - start_ms) / mpb * bpx);
            if (x > rx + rw) break;
            if (x < rx - 60) continue;

            if (b % beats_per_measure == 0) {
                int measure = b / beats_per_measure + 1;
                draw_vline(r, x, hdr_y, hdr_y + HEADER_HEIGHT, COL_GOLD_DARK);
                char lbl[16];
                snprintf(lbl, sizeof(lbl), "%d", measure);
                draw_text_bold(r, lbl, x + 4, hdr_y + HEADER_HEIGHT / 2 - 5, 9, COL_GOLD);
            } else {
                draw_vline(r, x, hdr_y + HEADER_HEIGHT / 2,
                           hdr_y + HEADER_HEIGHT, COL_TEXT_DIM);
                int measure = b / beats_per_measure + 1;
                int beat_in_m = b % beats_per_measure + 1;
                char lbl[16];
                snprintf(lbl, sizeof(lbl), "%d.%d", measure, beat_in_m);
                draw_text(r, lbl, x + 3, hdr_y + HEADER_HEIGHT / 2 + 1,
                          7, COL_BORDER_LIGHT);
            }
        }

        /* Loop region in header */
        if (app->looping) {
            float lx0 = rx + (float)((app->loop_start_ms - start_ms) / mpb * bpx);
            float lx1 = rx + (float)((app->loop_end_ms - start_ms) / mpb * bpx);
            float lx0c = lx0 < rx ? rx : lx0;
            float lx1c = lx1 > rx + rw ? rx + rw : lx1;
            if (lx1c > lx0c) {
                draw_filled_rect(r, lx0c, hdr_y, lx1c - lx0c, HEADER_HEIGHT, COL_GOLD, 0x30);
            }
            /* Triangular handles at loop boundaries */
            float th = 8; /* triangle height */
            SDL_FColor gc = { 0xD8/255.0f, 0xAD/255.0f, 0x70/255.0f, 1.0f };
            if (lx0 >= rx && lx0 <= rx + rw) {
                SDL_Vertex lt[3] = {
                    { .position={lx0,      hdr_y},      .color=gc },
                    { .position={lx0 + th, hdr_y},      .color=gc },
                    { .position={lx0,      hdr_y + th}, .color=gc },
                };
                SDL_RenderGeometry(r, NULL, lt, 3, NULL, 0);
            }
            if (lx1 >= rx && lx1 <= rx + rw) {
                SDL_Vertex rt[3] = {
                    { .position={lx1,      hdr_y},      .color=gc },
                    { .position={lx1 - th, hdr_y},      .color=gc },
                    { .position={lx1,      hdr_y + th}, .color=gc },
                };
                SDL_RenderGeometry(r, NULL, rt, 3, NULL, 0);
            }
        }

        /* Playhead in header */
        {
            float px = rx + (float)((app->playhead_ms - start_ms) / mpb * bpx);
            if (px >= rx && px <= rx + rw) {
                draw_vline(r, px, hdr_y, hdr_y + HEADER_HEIGHT, COL_PLAYHEAD);
                draw_vline(r, px + 1, hdr_y, hdr_y + HEADER_HEIGHT, COL_PLAYHEAD);
            }
        }
        pop_clip(r);

        /* Bottom border of header */
        draw_hline(r, keys_x, (float)app->win_w, hdr_y + HEADER_HEIGHT, COL_BORDER);
        /* Fade header during transition */
        if (persp_t > 0.01f) {
            uint8_t fa = (uint8_t)(fminf(persp_t * 3.0f, 1.0f) * 255);
            draw_filled_rect(r, keys_x, hdr_y, (float)app->win_w - keys_x, HEADER_HEIGHT + 1,
                             0x16, 0x16, 0x18, fa);
        }
    }
    skip_timeline_header:;

    /* scrollbars (hidden in 3D) */
    if (persp_t > 0.99f) goto skip_scrollbars;
    {
        float sb_w = 14;
        float sb_x = (float)app->win_w - sb_w;
        float view_frac_v = rh / (float)(NUM_PITCHES * kh);
        if (view_frac_v > 1.0f) view_frac_v = 1.0f;
        float scroll_frac_v = (float)app->scroll_y / (float)(NUM_PITCHES - (int)(rh / kh));
        if (scroll_frac_v < 0) scroll_frac_v = 0;
        if (scroll_frac_v > 1) scroll_frac_v = 1;
        draw_scrollbar_v(r, sb_x, ry, rh, view_frac_v, scroll_frac_v, false);
    }

    /* velocity pane - only shows when notes are selected, FL Studio style */
    {
        /* Collect selected notes */
        NoteArray *na = app_active_notes(app);
        int sel_count = 0;
        if (na) {
            for (int i = 0; i < na->count; i++)
                if (na->notes[i].selected) sel_count++;
        }

        if (sel_count > 0) {
            float vy = ry + rh;
            float vpad = 4;
            float vbar_h = VEL_PANE_H - 2 * vpad;
            float bottom_y = vy + VEL_PANE_H - vpad;

            /* Separator line */
            draw_hline(r, keys_x, (float)app->win_w, vy, COL_BORDER);

            /* Vel label area (left side, same width as keys) */
            draw_filled_rect(r, keys_x, vy + 1, KEYS_WIDTH, VEL_PANE_H - 1,
                             COL_BG_DARK, 0xFF);
            /* Scale labels and tick marks */
            const int scale_vals[] = {1, 25, 50, 75, 100, 127};
            for (int s = 0; s < 6; s++) {
                float sy = bottom_y - (scale_vals[s] / 127.0f) * vbar_h;
                char sv[8]; snprintf(sv, sizeof(sv), "%d", scale_vals[s]);
                draw_text_right(r, sv, keys_x + KEYS_WIDTH - 4, sy - 3, 7, COL_TEXT_DIM);
                draw_hline(r, keys_x + KEYS_WIDTH - 2, keys_x + KEYS_WIDTH, sy,
                           COL_BORDER_LIGHT);
            }

            /* Vel pane background */
            draw_filled_rect(r, rx, vy + 1, rw, VEL_PANE_H - 1, COL_BG_DARK, 0xFF);

            push_clip(r, rx, vy + 1, rw, VEL_PANE_H - 2);

            /* Build sorted list of selected notes for trapezoid graph */
            typedef struct { float x; float y_top; int idx; } VelPoint;
            VelPoint pts_buf[sel_count > 0 ? sel_count : 1];
            VelPoint *pts = pts_buf;
            int npts = 0;
            for (int i = 0; i < na->count && pts; i++) {
                if (!na->notes[i].selected) continue;
                MuseNote *note = &na->notes[i];
                float nx = rx + (float)((note->start - start_ms) / mpb * bpx);
                float vel_h = (note->vel / 127.0f) * vbar_h;
                pts[npts].x = nx;
                pts[npts].y_top = bottom_y - vel_h;
                pts[npts].idx = i;
                npts++;
            }
            /* Sort by x position */
            for (int i = 1; i < npts; i++) {
                VelPoint tmp = pts[i];
                int j = i - 1;
                while (j >= 0 && pts[j].x > tmp.x) { pts[j + 1] = pts[j]; j--; }
                pts[j + 1] = tmp;
            }

            /* Draw trapezoid fill polygons between consecutive stems
               using SDL_RenderGeometry for efficient GPU-rendered quads */
            for (int i = 0; i < npts - 1; i++) {
                float x1 = pts[i].x, y1 = pts[i].y_top;
                float x2 = pts[i + 1].x, y2 = pts[i + 1].y_top;
                if (x2 < rx || x1 > rx + rw) continue;
                const MuseTechnique *t1 = technique_by_id(na->notes[pts[i].idx].ntype);
                const MuseTechnique *t2 = technique_by_id(na->notes[pts[i+1].idx].ntype);
                uint8_t fc_r = (uint8_t)(((t1 ? t1->r : 120) + (t2 ? t2->r : 120)) * 0.5f * 0.30f);
                uint8_t fc_g = (uint8_t)(((t1 ? t1->g : 140) + (t2 ? t2->g : 140)) * 0.5f * 0.30f);
                uint8_t fc_b = (uint8_t)(((t1 ? t1->b : 170) + (t2 ? t2->b : 170)) * 0.5f * 0.30f);
                /* Trapezoid as 2 triangles: (x1,y1) (x2,y2) (x2,bottom) (x1,bottom) */
                SDL_FColor fc = { fc_r / 255.0f, fc_g / 255.0f, fc_b / 255.0f, 1.0f };
                SDL_Vertex verts[4] = {
                    { .position={x1, y1},       .color=fc },
                    { .position={x2, y2},       .color=fc },
                    { .position={x2, bottom_y}, .color=fc },
                    { .position={x1, bottom_y}, .color=fc },
                };
                int indices[6] = { 0, 1, 2, 0, 2, 3 };
                SDL_RenderGeometry(r, NULL, verts, 4, indices, 6);
                /* Anti-alias the diagonal edge with a translucent strip */
                SDL_FColor fc_aa = { fc_r / 255.0f, fc_g / 255.0f, fc_b / 255.0f, 0.35f };
                SDL_FColor fc_zero = { fc_r / 255.0f, fc_g / 255.0f, fc_b / 255.0f, 0.0f };
                /* Perpendicular offset for 1px AA fringe above the diagonal */
                float dx_e = x2 - x1, dy_e = y2 - y1;
                float len = sqrtf(dx_e * dx_e + dy_e * dy_e);
                if (len > 0.5f) {
                    float nx_e = -dy_e / len, ny_e = dx_e / len;
                    SDL_Vertex aa[4] = {
                        { .position={x1,           y1},            .color=fc_aa },
                        { .position={x2,           y2},            .color=fc_aa },
                        { .position={x2 + nx_e,    y2 + ny_e},    .color=fc_zero },
                        { .position={x1 + nx_e,    y1 + ny_e},    .color=fc_zero },
                    };
                    int aa_idx[6] = { 0, 1, 2, 0, 2, 3 };
                    SDL_RenderGeometry(r, NULL, aa, 4, aa_idx, 6);
                }
                /* Edge line from (x1,y1) to (x2,y2) */
                SDL_SetRenderDrawColor(r, COL_BORDER_LIGHT, 0xFF);
                SDL_RenderLine(r, x1, y1, x2, y2);
            }

            /* Draw stems and knobs (sort by velocity so high ones draw on top) */
            /* Re-sort by velocity ascending */
            for (int i = 1; i < npts; i++) {
                VelPoint tmp = pts[i];
                int j = i - 1;
                while (j >= 0 && na->notes[pts[j].idx].vel > na->notes[tmp.idx].vel) {
                    pts[j + 1] = pts[j]; j--;
                }
                pts[j + 1] = tmp;
            }

            for (int p = 0; p < npts; p++) {
                MuseNote *note = &na->notes[pts[p].idx];
                float nx = pts[p].x;
                float y_top = pts[p].y_top;

                /* stem color: technique tint shifted by pitch for some visual variety */
                const MuseTechnique *tech = technique_by_id(note->ntype);
                int tcr = tech ? tech->r : 120;
                int tcg = tech ? tech->g : 140;
                int tcb = tech ? tech->b : 170;
                float pitch_hue = ((note->pitch * 37) % 360) / 360.0f;
                int sr = (int)(tcr * 0.5f + pitch_hue * 130); if (sr > 255) sr = 255;
                int sg = (int)(tcg * 0.5f + (1.0f - pitch_hue) * 100); if (sg > 255) sg = 255;
                int sb = (int)(tcb * 0.5f + pitch_hue * 80); if (sb > 255) sb = 255;

                /* Vel-selected stems (bit 2) get white line, others get colored line */
                bool is_drag_target = (app->vel_dragging && pts[p].idx == app->vel_drag_note_idx);
                bool is_vel_selected = (note->selected & 2) != 0;
                uint8_t line_r, line_g, line_b;
                if (is_vel_selected || is_drag_target) {
                    line_r = 0xFF; line_g = 0xFF; line_b = 0xFF;
                } else {
                    line_r = (uint8_t)sr; line_g = (uint8_t)sg; line_b = (uint8_t)sb;
                }

                /* Stem (2px wide, starts below knob) */
                float stem_cx = nx + 0.5f;
                draw_vline(r, nx, y_top + 5, bottom_y, line_r, line_g, line_b);
                draw_vline(r, nx + 1, y_top + 5, bottom_y, line_r, line_g, line_b);

                /* Knob */
                if (is_drag_target)
                    draw_circle_filled(r, stem_cx, y_top, 5, COL_GOLD_BRIGHT);
                else
                    draw_circle_filled(r, stem_cx, y_top, 5, COL_TEXT);
            }

            /* Velocity readout while dragging */
            if (app->vel_dragging && app->vel_drag_note_idx >= 0 &&
                app->vel_drag_note_idx < na->count) {
                MuseNote *dn = &na->notes[app->vel_drag_note_idx];
                float dnx = rx + (float)((dn->start - start_ms) / mpb * bpx);
                float dn_vel_h = (dn->vel / 127.0f) * vbar_h;
                float dn_y = bottom_y - dn_vel_h;
                char vstr[8];
                snprintf(vstr, sizeof(vstr), "%d", dn->vel);
                draw_text_bold(r, vstr, dnx + 10, dn_y - 4, 9, COL_GOLD_BRIGHT);
            }

            pop_clip(r);
        }
    }

    /* horizontal scrollbar */
    {
        float sb_w = 14;
        float sb_h = 14;
        float total_ms_w = app->measures * beats_per_measure * mpb;
        float total_px = (float)(total_ms_w / mpb * bpx);
        /* Ensure minimum scrollable width so scrollbar is always usable */
        if (total_px < rw * 2.0f) total_px = rw * 2.0f;
        float sb_y = (float)app->win_h - STATUS_BAR_H - sb_h;
        float view_frac_h = rw / total_px;
        if (view_frac_h > 1.0f) view_frac_h = 1.0f;
        float scroll_frac_h = (float)(app->scroll_x / total_ms_w);
        if (scroll_frac_h < 0) scroll_frac_h = 0;
        if (scroll_frac_h > 1) scroll_frac_h = 1;
        draw_scrollbar_h(r, rx, sb_y, rw - sb_w, view_frac_h, scroll_frac_h, false);
    }
    skip_scrollbars:;

    /* Skip left panel in full 3D mode */
    if (app->perspective_t > 0.99f) goto skip_left_panel;

    /* left panel - instruments, note props, effectors, file stuff */
    if (app->left_panel_open) {
        float lp_y = (float)(TRANSPORT_H + 2);
        float lp_h = (float)(app->win_h - TRANSPORT_H - 2 - STATUS_BAR_H);
        draw_filled_rect(r, 0, lp_y, LEFT_PANEL_W, lp_h, COL_BG, 0xFF);
        draw_vline(r, (float)LEFT_PANEL_W - 1, lp_y,
                   (float)(app->win_h - STATUS_BAR_H), COL_BORDER);

        push_clip(r, 0, lp_y, LEFT_PANEL_W, lp_h);
        /* leave some margin on the right so the scrollbar doesn't overlap content */
        float sb_margin = 12;
        float lp_content_w = LEFT_PANEL_W - sb_margin;
        float cy = lp_y - app->left_panel_scroll;
        float lp_mx = app->mouse_x, lp_my = app->mouse_y;
        bool lp_hover = (lp_mx >= 0 && lp_mx < LEFT_PANEL_W &&
                         lp_my >= lp_y && lp_my < lp_y + lp_h);

        /* --- INSTRUMENTS --- */
        cy += 10;
        draw_text_bold(r, "INSTRUMENTS", 4, cy, 11, COL_GOLD);
        cy += 28 + 5; /* header h=28, plus small gap to first instrument row */

        for (int i = 0; i < app->project.num_layers; i++) {
            MuseLayer *ly = &app->project.layers[i];
            const MuseInstrument *ins = inst_by_id(ly->inst_id);
            bool sel = (i == app->project.active_layer);

            /* separator line between instruments */
            if (i > 0) {
                cy += 2;
                draw_hline(r, 4, LEFT_PANEL_W - 4, cy, COL_BORDER);
                cy += 3; /* 2px above + 1px line + 2px below = pady=2 */
            }

            /* top row: icon, mute, solo, name */
            float row_top = cy;

            /* instrument icon, or color swatch if icon missing */
            {
                SDL_Texture *icon = get_instrument_icon(ly->inst_id);
                if (icon) {
                    SDL_FRect icon_dst = { 2, cy + 4, 16, 16 };
                    SDL_RenderTexture(r, icon, NULL, &icon_dst);
                } else if (ins) {
                    draw_filled_rect(r, 4, cy + 4, 12, 20,
                                     ins->color_r, ins->color_g, ins->color_b, 0xFF);
                }
            }

            /* Mute */
            {
                UiRect mute_rc = { 20, cy + 2, 24, 24 };
                if (ly->muted)
                    draw_rounded_rect(r, mute_rc.x, mute_rc.y, mute_rc.w, mute_rc.h, 4, COL_ERROR, 0xFF);
                else
                    draw_rounded_rect(r, mute_rc.x, mute_rc.y, mute_rc.w, mute_rc.h, 4, COL_SURFACE, 0xFF);
                draw_text_centered(r, "M", mute_rc.x + 12, mute_rc.y + 12, 9, COL_TEXT);
            }

            /* Solo */
            {
                UiRect solo_rc = { 46, cy + 2, 24, 24 };
                if (ly->solo)
                    draw_rounded_rect(r, solo_rc.x, solo_rc.y, solo_rc.w, solo_rc.h, 4, COL_GOLD, 0xFF);
                else
                    draw_rounded_rect(r, solo_rc.x, solo_rc.y, solo_rc.w, solo_rc.h, 4, COL_SURFACE, 0xFF);
                draw_text_centered(r, "S", solo_rc.x + 12, solo_rc.y + 12, 9,
                               ly->solo ? COL_BG_DARK : COL_TEXT);
            }

            /* instrument name */
            const char *name = ins ? ins->name : "Unknown";
            float name_lh = text_line_height(11);
            float name_y = cy + 2 + (24 - name_lh) / 2.0f;
            if (sel && !ly->muted)
                draw_text_bold(r, name, 74, name_y, 11, COL_GOLD);
            else if (sel)
                draw_text_bold(r, name, 74, name_y, 11, COL_TEXT_DIM);
            else
                draw_text(r, name, 74, name_y, 11, COL_TEXT_DIM);

            /* Remove button (×) on far right, if multiple layers */
            if (app->project.num_layers > 1) {
                UiRect rm_rc = { lp_content_w - 22, cy + 6, 16, 16 };
                bool rm_hov = lp_hover && ui_rect_contains(rm_rc, lp_mx, lp_my);
                draw_rounded_rect(r, rm_rc.x, rm_rc.y, rm_rc.w, rm_rc.h, 3,
                                  rm_hov ? COL_BG_LIGHT : COL_SURFACE, 0xFF);
                /* Draw × centered, 3px half-size, integer coords for symmetry */
                int xi = (int)(rm_rc.x + rm_rc.w / 2);
                int yi = (int)(rm_rc.y + rm_rc.h / 2);
                uint8_t xr = rm_hov ? 0xE0 : 0x9A, xg = rm_hov ? 0x55 : 0x9A, xb = rm_hov ? 0x55 : 0x9E;
                SDL_SetRenderDrawColor(r, xr, xg, xb, 0xFF);
                SDL_RenderLine(r, xi - 3, yi - 3, xi + 3, yi + 3);
                SDL_RenderLine(r, xi + 3, yi - 3, xi - 3, yi + 3);
            }

            /* note count */
            int nc = 0;
            for (int s = 0; s < ly->num_sublayers; s++)
                nc += ly->sublayers[s].count;
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "%d", nc);
            float cnt_lh = text_line_height(13);
            float cnt_y = cy + (28 - cnt_lh) / 2.0f;
            float cnt_x = (app->project.num_layers > 1) ? lp_content_w - 34 : lp_content_w - 8;
            draw_text_right(r, cnt, cnt_x, cnt_y, 13, COL_TEXT_DIM);

            cy += 28; /* top row h=28 */

            /* bottom row: aux send + volume slider */
            {
                /* Aux send button */
                UiRect aux_rc = { 24, cy + 4, 32, 20 };
                draw_rounded_rect(r, aux_rc.x, aux_rc.y, aux_rc.w, aux_rc.h, 4,
                                  COL_SURFACE, 0xFF);
                draw_text_centered(r, "Aux", aux_rc.x + 16, aux_rc.y + 10, 9, COL_TEXT_DIM);

                /* volume */
                float vol_lh = text_line_height(9);
                draw_text(r, "Vol:", 60, cy + (28 - vol_lh) / 2.0f, 9, COL_TEXT_DIM);
                /* Slider: aligned to text center (cy+14 - thumb_r=8 = cy+6) */
                draw_ctk_slider(r, 77, cy + 6, lp_content_w - 77 - 36,
                                (float)ly->volume, 0, 100,
                                app->lp_drag_slider == 1);
                char vol_str[8];
                snprintf(vol_str, sizeof(vol_str), "%d", ly->volume);
                draw_text(r, vol_str, lp_content_w - 32, cy + (28 - vol_lh) / 2.0f, 9, COL_TEXT);
            }
            cy += 28; /* aux/vol row h=28 */

            /* sublayer tabs */
            cy += 2;
            {
                float tx = 24;
                for (int si = 0; si < ly->num_sublayers; si++) {
                    bool active_sub = (si == ly->active_sub);
                    UiRect sub_rc = { tx, cy, 22, 18 };
                    draw_rounded_rect(r, sub_rc.x, sub_rc.y, sub_rc.w, sub_rc.h, 3,
                                      active_sub ? 0xD8 : 0x31,
                                      active_sub ? 0xAD : 0x32,
                                      active_sub ? 0x70 : 0x39, 0xFF);
                    char sn[4]; snprintf(sn, sizeof(sn), "%d", si + 1);
                    if (active_sub)
                        draw_text_bold(r, sn, tx + 11 - text_width(sn, 9) / 2, cy + 9.0f - text_line_height(9) / 2, 9, 0x00, 0x00, 0x00);
                    else
                        draw_text_centered(r, sn, tx + 11, cy + 9.0f, 9, COL_TEXT_DIM);
                    tx += 24;
                }
                /* + button */
                UiRect plus_rc = { tx, cy, 22, 18 };
                draw_rounded_rect(r, plus_rc.x, plus_rc.y, plus_rc.w, plus_rc.h, 3,
                                  COL_SURFACE, 0xFF);
                draw_text_centered(r, "+", tx + 11, cy + 9, 9, COL_TEXT_DIM);
                tx += 24;
                /* - button (only if multiple sublayers) */
                if (ly->num_sublayers > 1) {
                    UiRect minus_rc = { tx, cy, 22, 18 };
                    draw_rounded_rect(r, minus_rc.x, minus_rc.y, minus_rc.w, minus_rc.h, 3,
                                      COL_SURFACE, 0xFF);
                    draw_text_centered(r, "-", tx + 11, cy + 9, 9, COL_TEXT_DIM);
                }
            }
            cy += 20; /* sublayer tabs h=18+2 */

            /* Marnian synth profile selector */
            if (ly->inst_id == 0x14 || ly->inst_id == 0x18 ||
                ly->inst_id == 0x1C || ly->inst_id == 0x20) {
                cy += 2;
                draw_text(r, "Profile:", 24, cy + 3, 9, COL_TEXT_DIM);
                const char *prof_names[] = {"Basic", "Stereo", "Super", "SuperOct"};
                int pidx = ly->synth_profile;
                if (pidx < 0 || pidx > 3) pidx = 2; /* default Super */
                UiRect prof_rc = { 80, cy, 90, 20 };
                draw_ctk_entry(r, prof_rc, prof_names[pidx], 13, false);
                draw_dropdown_arrow(r, 80 + 90 - 10, cy + 8, 7, COL_TEXT_DIM);
                cy += 22;
            }

            (void)row_top;
        }

        /* add instrument button */
        cy += 4;
        {
            UiRect add_btn = { 8, cy, lp_content_w - 16, 28 };
            draw_ctk_button(r, add_btn, "+ Add Instrument", 13,
                           lp_hover && ui_rect_contains(add_btn, lp_mx, lp_my), false);
            app->_lp_add_inst = add_btn;
            cy += 32;
        }

        /* --- NOTE PROPERTIES --- */
        if (app->project.num_layers > 0) {
            cy += 10;
            draw_text_bold(r, "NOTE PROPERTIES", 4, cy, 11, COL_GOLD);
            cy += 28 + 4; /* h=28 + gap */

            MuseLayer *aly = &app->project.layers[app->project.active_layer];
            const MuseInstrument *ains = inst_by_id(aly->inst_id);

            /* default velocity for new notes */
            float vel_lh = text_line_height(13);
            draw_text(r, "Velocity:", 4, cy + (28 - vel_lh) / 2.0f, 13, COL_TEXT_DIM);
            cy += 28 + 4; /* label h=28 + gap to slider */
            {
                /* shows cur_vel - the default velocity for new notes */
                int sel_vel = app->cur_vel;
                /* slider fills to margin, value label on the right */
                float vel_sl_x = 4, vel_sl_w = lp_content_w - 40;
                draw_ctk_slider(r, vel_sl_x, cy, vel_sl_w,
                                (float)sel_vel, 1, 127,
                                app->lp_drag_slider == 2);
                char vstr[8];
                snprintf(vstr, sizeof(vstr), "%d", sel_vel);
                draw_text(r, vstr, lp_content_w - 30, cy + 8 - vel_lh / 2.0f, 13, COL_TEXT);
                app->_lp_vel_slider = (UiRect){ vel_sl_x, cy - 4, vel_sl_w, 24 };
                app->_lp_vel_slider_w = vel_sl_w;
            }
            cy += 24; /* slider area */

            /* technique selector */
            cy += 6;
            draw_text(r, "Technique:", 4, cy + (28 - vel_lh) / 2.0f, 13, COL_TEXT_DIM);
            cy += 28;
            {
                char tech_label[64] = "0: Normal";
                if (ains && ains->num_techniques > 0) {
                    /* Default: show cur_ntype (persisted technique selection) */
                    int cur_tidx = 0;
                    for (int t = 0; t < ains->num_techniques; t++) {
                        if (ains->techniques[t] == app->cur_ntype) { cur_tidx = t; break; }
                    }
                    const MuseTechnique *tc = technique_by_id(app->cur_ntype);
                    if (tc) snprintf(tech_label, sizeof(tech_label), "%d: %s", cur_tidx, tc->name);
                    /* Override: show selected note's technique if any */
                    NoteArray *na = app_active_notes(app);
                    if (na) {
                        for (int i = 0; i < na->count; i++) {
                            if (na->notes[i].selected) {
                                int tidx = 0;
                                for (int t = 0; t < ains->num_techniques; t++) {
                                    if (ains->techniques[t] == na->notes[i].ntype) { tidx = t; break; }
                                }
                                const MuseTechnique *ts = technique_by_id(na->notes[i].ntype);
                                if (ts) snprintf(tech_label, sizeof(tech_label), "%d: %s", tidx, ts->name);
                                break;
                            }
                        }
                    }
                }
                UiRect tech_entry = { 8, cy, lp_content_w - 16, 28 };
                draw_rounded_rect(r, tech_entry.x, tech_entry.y, tech_entry.w, tech_entry.h, 6,
                                  0x31, 0x32, 0x39, 0xFF);
                float tech_lh = text_line_height(13);
                float tech_ty = tech_entry.y + (tech_entry.h - tech_lh) / 2.0f;
                draw_text(r, tech_label, tech_entry.x + 8, tech_ty, 13, 0xE0, 0xE0, 0xE0);
                draw_dropdown_arrow(r, tech_entry.x + tech_entry.w - 16, cy + 14, 8, COL_TEXT_DIM);
                app->_lp_technique = tech_entry;
            }
            cy += 28 + 6; /* dropdown h=28 + padding to note info */

            /* selected note info + per-note velocity */
            {
                NoteArray *info_na = app_active_notes(app);
                MuseNote *sel_note = NULL;
                int info_sel_count = 0;
                int vel_sum = 0;
                if (info_na) {
                    for (int i = 0; i < info_na->count; i++) {
                        if (info_na->notes[i].selected & 1) {
                            info_sel_count++;
                            vel_sum += info_na->notes[i].vel;
                            if (!sel_note) sel_note = &info_na->notes[i];
                        }
                    }
                }
                if (sel_note && info_sel_count == 1) {
                    cy += 6;
                    char info_str[128];
                    int note_sem = sel_note->pitch % 12;
                    int note_oct = sel_note->pitch / 12 - 1 + (inst ? inst->oct_offset : 0);
                    const MuseTechnique *nt = technique_by_id(sel_note->ntype);
                    const char *tname_info = nt ? nt->name : "Normal";
                    snprintf(info_str, sizeof(info_str), "%s%d  vel:%d  %.0fms  dur:%.0fms  [%s]",
                             NOTE_NAMES[note_sem], note_oct, sel_note->vel,
                             sel_note->start, sel_note->dur, tname_info);
                    draw_text(r, info_str, 4, cy, 9, COL_TEXT_DIM);
                    cy += 16;
                } else if (info_sel_count > 1) {
                    cy += 6;
                    char info_str[64];
                    snprintf(info_str, sizeof(info_str), "%d notes selected", info_sel_count);
                    draw_text(r, info_str, 4, cy, 9, COL_TEXT_DIM);
                    cy += 16;
                }

                /* Per-note velocity slider: appears when >= 1 note is selected */
                app->_lp_note_vel_visible = false;
                if (sel_note && info_sel_count >= 1) {
                    int display_vel = (info_sel_count == 1) ? sel_note->vel : (vel_sum / info_sel_count);
                    float nv_lh = text_line_height(9);
                    draw_text(r, "Note Vel:", 4, cy + (28 - nv_lh) / 2.0f, 9, COL_TEXT_DIM);
                    float nv_sl_x = 68, nv_sl_w = lp_content_w - 68 - 40;
                    draw_ctk_slider(r, nv_sl_x, cy + 6, nv_sl_w,
                                    (float)display_vel, 1, 127,
                                    app->lp_drag_slider == 8);
                    char nvs[8];
                    snprintf(nvs, sizeof(nvs), "%d", display_vel);
                    draw_text(r, nvs, nv_sl_x + nv_sl_w + 4, cy + (28 - nv_lh) / 2.0f, 9, COL_TEXT);
                    app->_lp_note_vel_slider = (UiRect){ nv_sl_x, cy - 4, nv_sl_w, 24 };
                    app->_lp_note_vel_slider_w = nv_sl_w;
                    app->_lp_note_vel_visible = true;
                    cy += 26;
                }
            }

            /* --- EFFECTOR (GLOBAL) --- */
            cy += 10;
            draw_text_bold(r, "EFFECTOR (WIP)", 4, cy, 11, COL_GOLD);
            cy += 28 + 5; /* h=28 + gap */

            /* effect sliders - reverb, delay, chorus params */
            const char *fx_names[] = {"Reverb:", "Delay:", "Cho FB:", "Cho Depth:", "Cho Freq:"};
            int *fx_vals[] = {&app->fx_reverb, &app->fx_delay, &app->fx_chorus_fb,
                              &app->fx_chorus_depth, &app->fx_chorus_freq};
            float fx_sl_x = 74, fx_sl_w = lp_content_w - 74 - 36;
            app->_lp_fx_slider_x = fx_sl_x;
            app->_lp_fx_slider_w = fx_sl_w;
            for (int f = 0; f < 5; f++) {
                float fx_lh = text_line_height(13);
                /* Slider center is at cy + 4 + 8 = cy + 12; align text to that */
                float fx_text_y = cy + 12 - fx_lh / 2.0f;
                draw_text(r, fx_names[f], 8, fx_text_y, 13, COL_TEXT_DIM);
                draw_ctk_slider(r, fx_sl_x, cy + 4, fx_sl_w,
                                (float)*fx_vals[f], 0, 100,
                                app->lp_drag_slider == (3 + f));
                char fv[8];
                snprintf(fv, sizeof(fv), "%d", *fx_vals[f]);
                draw_text(r, fv, lp_content_w - 30, fx_text_y, 13, COL_TEXT);
                app->_lp_fx_sliders[f] = (UiRect){ fx_sl_x, cy, fx_sl_w, 28 };
                cy += 30;
            }
        }

        /* --- FILE --- */
        cy += 10;
        draw_text_bold(r, "FILE", 4, cy, 11, COL_GOLD);
        cy += 28 + 4;

        /* Import button */
        {
            UiRect imp_btn = { 8, cy, lp_content_w - 16, 28 };
            draw_ctk_button(r, imp_btn, "Import", 13,
                           lp_hover && ui_rect_contains(imp_btn, lp_mx, lp_my), false);
            app->_lp_import_midi = imp_btn;
            cy += 36;
        }

        /* account status */
        {
            float acct_lh = text_line_height(10);
            if (app->linked_owner_id != 0 && app->char_name[0]) {
                char acct_str[80];
                snprintf(acct_str, sizeof(acct_str), "Account: %s", app->char_name);
                draw_text(r, acct_str, 4, cy + (28 - acct_lh) / 2.0f, 10, COL_TEXT);
            } else {
                draw_text(r, "No account linked", 4, cy + (28 - acct_lh) / 2.0f, 10, COL_TEXT_DIM);
            }
            cy += 30;
        }

        /* Link Account button */
        {
            UiRect link_btn = { 8, cy, lp_content_w - 16, 28 };
            draw_ctk_button(r, link_btn, "Link Account (from BDO file)", 13,
                           lp_hover && ui_rect_contains(link_btn, lp_mx, lp_my), false);
            app->_lp_link_btn = link_btn;
            cy += 38;
        }

        /* filename */
        {
            float fn_lh = text_line_height(13);
            draw_text(r, "Filename:", 4, cy + (28 - fn_lh) / 2.0f, 13, COL_TEXT_DIM);
            cy += 28;
        }

        /* Filename entry */
        {
            UiRect fn_entry = { 8, cy, lp_content_w - 16, 28 };
            if (app->edit_field == 3) {
                /* Render entry background/border manually */
                draw_rounded_rect(r, fn_entry.x, fn_entry.y, fn_entry.w, fn_entry.h, 6,
                                  0x31, 0x32, 0x39, 0xFF);
                draw_rounded_rect_outline(r, fn_entry.x, fn_entry.y, fn_entry.w, fn_entry.h, 6,
                                          0xD8, 0xAD, 0x70);
                draw_rounded_rect_outline(r, fn_entry.x+1, fn_entry.y+1, fn_entry.w-2, fn_entry.h-2, 5,
                                          0xD8, 0xAD, 0x70);

                /* Measure text up to cursor to find cursor pixel position */
                int c = app->edit_cursor;
                int len = (int)strlen(app->edit_buf);
                if (c > len) c = len;
                char tmp[256];
                memcpy(tmp, app->edit_buf, c); tmp[c] = '\0';
                float cursor_x = text_width(tmp, 13);
                float inner_w = fn_entry.w - 16; /* usable width (8px padding each side) */

                /* Scroll to keep cursor visible */
                static float fn_scroll = 0;
                if (cursor_x - fn_scroll > inner_w - 4)
                    fn_scroll = cursor_x - inner_w + 4;
                if (cursor_x - fn_scroll < 0)
                    fn_scroll = cursor_x;
                if (fn_scroll < 0) fn_scroll = 0;

                /* Draw text with scroll offset */
                push_clip(r, fn_entry.x + 6, fn_entry.y, fn_entry.w - 12, fn_entry.h);
                float lh = text_line_height(13);
                float ty = fn_entry.y + (fn_entry.h - lh) / 2.0f;
                draw_text(r, app->edit_buf, fn_entry.x + 8 - fn_scroll, ty, 13, 0xE0, 0xE0, 0xE0);

                /* Draw cursor line */
                float cx_px = fn_entry.x + 8 + cursor_x - fn_scroll;
                SDL_SetRenderDrawColor(r, 0xE0, 0xE0, 0xE0, 0xFF);
                SDL_RenderLine(r, cx_px, fn_entry.y + 5, cx_px, fn_entry.y + fn_entry.h - 5);
                pop_clip(r);
            } else {
                draw_ctk_entry(r, fn_entry, app->filename, 13, false);
            }
            app->_lp_fn_entry = fn_entry;
            cy += 36;
        }

        /* export buttons */
        {
            UiRect exp_bdo = { 8, cy, lp_content_w - 16, 28 };
            draw_ctk_button(r, exp_bdo, "Export BDO", 13,
                           lp_hover && ui_rect_contains(exp_bdo, lp_mx, lp_my), false);
            app->_lp_export_bdo = exp_bdo;
            cy += 36;
        }

        /* WAV export */
        {
            UiRect exp_wav = { 8, cy, lp_content_w - 16, 28 };
            draw_ctk_button(r, exp_wav, "Export WAV", 13,
                           lp_hover && ui_rect_contains(exp_wav, lp_mx, lp_my), false);
            app->_lp_export_wav = exp_wav;
            cy += 36;
        }

        float content_h = (cy + app->left_panel_scroll) - lp_y + 16;
        app->left_panel_scroll_max = content_h;

        /* left panel scrollbar */
        {
            float sb_w = 6;
            float sb_x = LEFT_PANEL_W - sb_w - 3;
            float track_y = lp_y + 4;
            float track_h = lp_h - 8;

            /* Thumb proportional to visible fraction */
            float visible_frac = (content_h > 0) ? lp_h / content_h : 1.0f;
            if (visible_frac > 1.0f) visible_frac = 1.0f;
            float thumb_h = track_h * visible_frac;
            if (thumb_h < 24) thumb_h = 24;
            if (thumb_h > track_h) thumb_h = track_h;

            float max_scroll = content_h - lp_h;
            if (max_scroll < 0) max_scroll = 0;
            float scroll_frac = (max_scroll > 0) ? app->left_panel_scroll / max_scroll : 0;
            float thumb_y = track_y + scroll_frac * (track_h - thumb_h);

            /* Check hover */
            bool sb_hover = (lp_mx >= sb_x - 2 && lp_mx < sb_x + sb_w + 2 &&
                             lp_my >= thumb_y && lp_my < thumb_y + thumb_h);
            bool sb_active = (app->sb_dragging == 3);

            if (sb_active || sb_hover)
                draw_rounded_rect(r, sb_x, thumb_y, sb_w, thumb_h, sb_w / 2,
                                  0xCC, 0xA4, 0x71, 0xD0);
            else
                draw_rounded_rect(r, sb_x, thumb_y, sb_w, thumb_h, sb_w / 2,
                                  0x59, 0x5A, 0x62, 0x90);
        }

        /* Fade left panel during transition */
        if (persp_t > 0.01f) {
            uint8_t fa = (uint8_t)(fminf(persp_t * 3.0f, 1.0f) * 255);
            draw_filled_rect(r, 0, lp_y, LEFT_PANEL_W, lp_h, 0x16, 0x16, 0x18, fa);
        }

        pop_clip(r);
    }
    skip_left_panel:;
}

/* --- Instrument picker dialog --- */

typedef struct { const char *group; int ids[16]; int count; } InstGroup;
static const InstGroup INST_GROUPS[] = {
    {"Beginner", {0x00,0x01,0x02,0x04,0x05,0x06,0x07,0x08}, 8},
    {"Florchestra", {0x0A,0x0B,0x0D,0x0F,0x10,0x11,0x12,0x13,0x27,0x28}, 10},
    {"Marnian", {0x14,0x18,0x1C,0x20}, 4},
    {"Electric Guitar & Bass", {0x0E,0x24,0x25,0x26}, 4},
};
#define INST_GROUP_COUNT 4

void ui_instrument_picker_render(MuseApp *app) {
    if (!app->picker_open) return;
    SDL_Renderer *r = app->renderer;

    /* Dim background */
    draw_filled_rect(r, 0, 0, (float)app->win_w, (float)app->win_h,
                     0x00, 0x00, 0x00, 0x80);

    /* Dialog: 300x420, centered */
    float dw = 300, dh = 420;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;

    /* Background */
    draw_rounded_rect(r, dx, dy, dw, dh, 6, COL_SURFACE, 0xFF);

    /* Header bar */
    float hdr_h = 32;
    draw_filled_rect(r, dx, dy, dw, hdr_h, COL_BG, 0xFF);
    const char *title = app->picker_mode == 0 ? "Select Instrument" : "Move Selection to Instrument";
    draw_text_bold(r, title, dx + 8, dy + hdr_h / 2 - 5, 11, COL_TEXT);

    /* Close button (x) in top-right */
    draw_text(r, "x", dx + dw - 20, dy + hdr_h / 2 - 5, 13, COL_TEXT);

    /* Content area */
    float content_y = dy + hdr_h + 4;
    float content_h = dh - hdr_h - 12;

    /* Inner darker background */
    draw_rounded_rect(r, dx + 4, content_y - 2, dw - 8, content_h + 4, 4,
                      COL_BG, 0xFF);

    push_clip(r, dx + 8, content_y, dw - 16, content_h);

    float cy = content_y - app->picker_scroll;
    int item_idx = 0;

    for (int g = 0; g < INST_GROUP_COUNT; g++) {
        const InstGroup *grp = &INST_GROUPS[g];

        /* Group header */
        draw_text_bold(r, grp->group, dx + 12, cy, 11, COL_GOLD);
        cy += 20;

        for (int i = 0; i < grp->count; i++) {
            const MuseInstrument *inst = inst_by_id(grp->ids[i]);
            if (!inst) continue;

            /* Button row */
            float btn_y = cy;
            float btn_h = 26;
            int exclude = (app->picker_mode == 2) ? app->project.active_layer : -1;
            bool used = inst_already_used(&app->project, grp->ids[i], exclude);
            bool hovered = (item_idx == app->picker_hover) && !used;

            if (used)
                draw_rounded_rect(r, dx + 10, btn_y, dw - 20, btn_h, 4,
                                  COL_SURFACE, 0x60);
            else if (hovered)
                draw_rounded_rect(r, dx + 10, btn_y, dw - 20, btn_h, 4,
                                  COL_BG_LIGHT, 0xFF);
            else
                draw_rounded_rect(r, dx + 10, btn_y, dw - 20, btn_h, 4,
                                  COL_SURFACE, 0xFF);

            /* Icon or color swatch fallback */
            {
                SDL_Texture *icon = get_instrument_icon(grp->ids[i]);
                if (icon) {
                    SDL_FRect icon_dst = { dx + 12, btn_y + 3, 20, 20 };
                    if (used) SDL_SetTextureAlphaMod(icon, 0x50);
                    SDL_RenderTexture(r, icon, NULL, &icon_dst);
                    if (used) SDL_SetTextureAlphaMod(icon, 0xFF);
                } else {
                    draw_filled_rect(r, dx + 14, btn_y + 3, 10, 20,
                                     inst->color_r, inst->color_g, inst->color_b,
                                     used ? 0x50 : 0xFF);
                }
            }

            /* Name */
            if (used)
                draw_text(r, inst->name, dx + 36, btn_y + btn_h / 2 - 4, 10,
                          0x60, 0x60, 0x60);
            else
                draw_text(r, inst->name, dx + 36, btn_y + btn_h / 2 - 4, 10,
                          hovered ? COL_GOLD_LIGHT : COL_TEXT);

            cy += btn_h + 2;
            item_idx++;
        }
        cy += 6;
    }

    pop_clip(r);
}

/* --- Aux send dialog (per-instrument reverb/delay/chorus) --- */

void ui_aux_send_render(MuseApp *app) {
    if (!app->aux_open) return;
    if (app->aux_layer < 0 || app->aux_layer >= app->project.num_layers) {
        app->aux_open = false;
        return;
    }
    SDL_Renderer *r = app->renderer;
    MuseLayer *ly = &app->project.layers[app->aux_layer];
    const MuseInstrument *ins = inst_by_id(ly->inst_id);

    /* Dim background */
    draw_filled_rect(r, 0, 0, (float)app->win_w, (float)app->win_h,
                     0x00, 0x00, 0x00, 0x80);

    /* Dialog: 300x200, centered */
    float dw = 300, dh = 200;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;

    draw_rounded_rect(r, dx, dy, dw, dh, 6, COL_SURFACE, 0xFF);

    /* Header bar */
    float hdr_h = 28;
    draw_filled_rect(r, dx, dy, dw, hdr_h, COL_BG, 0xFF);
    char title[128];
    snprintf(title, sizeof(title), "Aux Send - %s", ins ? ins->name : "Unknown");
    draw_text_bold(r, title, dx + 8, dy + hdr_h / 2 - 5, 10, COL_TEXT);
    draw_text(r, "x", dx + dw - 20, dy + hdr_h / 2 - 5, 13, COL_TEXT);

    /* Content area: 3 send sliders */
    float content_y = dy + hdr_h + 12;
    float sl_x = dx + 80;
    float sl_w = 140;
    app->_aux_slider_x = sl_x;
    app->_aux_slider_w = sl_w;

    const char *send_names[] = {"Reverb:", "Delay:", "Chorus:"};
    uint8_t *send_vals[] = {&ly->reverb_send, &ly->delay_send, &ly->chorus_send};

    for (int i = 0; i < 3; i++) {
        float row_y = content_y + i * 40;
        draw_text(r, send_names[i], dx + 20, row_y + 4, 13, COL_TEXT);
        draw_ctk_slider(r, sl_x, row_y, sl_w, (float)*send_vals[i], 0, 100,
                        app->aux_drag_slider == (i + 1));

        char val_str[8];
        snprintf(val_str, sizeof(val_str), "%d", *send_vals[i]);
        draw_text(r, val_str, sl_x + sl_w + 8, row_y + 4, 9, COL_TEXT);

        app->_aux_sliders[i] = (UiRect){ sl_x, row_y - 4, sl_w, 24 };
    }

    /* OK button */
    float ok_y = content_y + 3 * 40 + 4;
    UiRect ok_btn = { dx + dw / 2 - 40, ok_y, 80, 28 };
    draw_ctk_button(r, ok_btn, "OK", 13,
                    ui_rect_contains(ok_btn, app->mouse_x, app->mouse_y), false);
}

/* --- Chord picker (middle-click to chordify notes) --- */

/* ChordDef is declared in ui_piano_roll.h */
typedef struct { const char *name; const ChordDef *chords; int count; } ChordGroup;

static const int CH_MAJOR[]       = {0,4,7};
static const int CH_MINOR[]       = {0,3,7};
static const int CH_DIM[]         = {0,3,6};
static const int CH_AUG[]         = {0,4,8};
static const int CH_SUS2[]        = {0,2,7};
static const int CH_SUS4[]        = {0,5,7};
static const int CH_MAJ7[]        = {0,4,7,11};
static const int CH_MIN7[]        = {0,3,7,10};
static const int CH_DOM7[]        = {0,4,7,10};
static const int CH_DIM7[]        = {0,3,6,9};
static const int CH_HDIM7[]       = {0,3,6,10};
static const int CH_MINMAJ7[]     = {0,3,7,11};
static const int CH_AUG7[]        = {0,4,8,10};
static const int CH_AUGMAJ7[]     = {0,4,8,11};
static const int CH_7SUS2[]       = {0,2,7,10};
static const int CH_7SUS4[]       = {0,5,7,10};
static const int CH_MAJ6[]        = {0,4,7,9};
static const int CH_MIN6[]        = {0,3,7,9};
static const int CH_MAJ9[]        = {0,4,7,11,14};
static const int CH_MIN9[]        = {0,3,7,10,14};
static const int CH_DOM9[]        = {0,4,7,10,14};
static const int CH_MINB9[]       = {0,3,7,10,13};
static const int CH_69[]          = {0,4,7,9,14};
static const int CH_MIN69[]       = {0,3,7,9,14};
static const int CH_MAJ11[]       = {0,4,7,11,14,17};
static const int CH_MIN11[]       = {0,3,7,10,14,17};
static const int CH_DOM11[]       = {0,4,7,10,14,17};
static const int CH_MAJ13[]       = {0,4,7,11,14,17,21};
static const int CH_MIN13[]       = {0,3,7,10,14,17,21};
static const int CH_DOM13[]       = {0,4,7,10,14,17,21};
static const int CH_ADD9[]        = {0,4,7,14};
static const int CH_MINADD9[]     = {0,3,7,14};
static const int CH_ADD11[]       = {0,4,7,17};
static const int CH_ADD13[]       = {0,4,7,21};
static const int CH_POW5[]        = {0,7};
static const int CH_POWOCT[]      = {0,7,12};
static const int CH_OCT[]         = {0,12};

static const ChordDef TRIADS[] = {
    {"Major",CH_MAJOR,3},{"Minor",CH_MINOR,3},{"Diminished",CH_DIM,3},
    {"Augmented",CH_AUG,3},{"Sus2",CH_SUS2,3},{"Sus4",CH_SUS4,3},
};
static const ChordDef SEVENTHS[] = {
    {"Major 7th",CH_MAJ7,4},{"Minor 7th",CH_MIN7,4},{"Dominant 7th",CH_DOM7,4},
    {"Dim 7th",CH_DIM7,4},{"Half-Dim 7th",CH_HDIM7,4},{"Min/Maj 7th",CH_MINMAJ7,4},
    {"Aug 7th",CH_AUG7,4},{"Aug Maj 7th",CH_AUGMAJ7,4},{"7sus2",CH_7SUS2,4},{"7sus4",CH_7SUS4,4},
};
static const ChordDef SIXTHS[] = {
    {"Major 6th",CH_MAJ6,4},{"Minor 6th",CH_MIN6,4},
};
static const ChordDef NINTHS[] = {
    {"Major 9th",CH_MAJ9,5},{"Minor 9th",CH_MIN9,5},{"Dominant 9th",CH_DOM9,5},
    {"Minor b9",CH_MINB9,5},{"6/9",CH_69,5},{"Min 6/9",CH_MIN69,5},
};
static const ChordDef ELEVENTHS[] = {
    {"Major 11th",CH_MAJ11,6},{"Minor 11th",CH_MIN11,6},{"Dominant 11th",CH_DOM11,6},
};
static const ChordDef THIRTEENTHS[] = {
    {"Major 13th",CH_MAJ13,7},{"Minor 13th",CH_MIN13,7},{"Dominant 13th",CH_DOM13,7},
};
static const ChordDef ADDS[] = {
    {"Add9",CH_ADD9,4},{"Min Add9",CH_MINADD9,4},{"Add11",CH_ADD11,4},{"Add13",CH_ADD13,4},
};
static const ChordDef POWERS[] = {
    {"Power (5th)",CH_POW5,2},{"Power + Oct",CH_POWOCT,3},{"Octave",CH_OCT,2},
};

static const ChordGroup CHORD_GROUPS[] = {
    {"Triads", TRIADS, 6},
    {"Seventh", SEVENTHS, 10},
    {"Sixth", SIXTHS, 2},
    {"Ninth", NINTHS, 6},
    {"Eleventh", ELEVENTHS, 3},
    {"Thirteenth", THIRTEENTHS, 3},
    {"Add", ADDS, 4},
    {"Power / Special", POWERS, 3},
};
#define CHORD_GROUP_COUNT 8

int chord_total_items(void) {
    int t = 0;
    for (int g = 0; g < CHORD_GROUP_COUNT; g++) t += CHORD_GROUPS[g].count;
    return t;
}

const ChordDef *chord_at(int idx) {
    int cur = 0;
    for (int g = 0; g < CHORD_GROUP_COUNT; g++) {
        if (idx < cur + CHORD_GROUPS[g].count)
            return &CHORD_GROUPS[g].chords[idx - cur];
        cur += CHORD_GROUPS[g].count;
    }
    return NULL;
}

void ui_chord_picker_render(MuseApp *app) {
    if (!app->chord_open) return;
    SDL_Renderer *r = app->renderer;

    /* --- Detect which chords the selected notes form --- */
    int total_chords = chord_total_items();
    bool *chord_matched = NULL;
    {
        /* Collect pitch classes of selected notes */
        bool pc_set[12] = {false};
        int pc_count = 0;
        NoteArray *sel_na = app_active_notes(app);
        if (sel_na) {
            for (int i = 0; i < sel_na->count; i++) {
                if (sel_na->notes[i].selected) {
                    int pc = sel_na->notes[i].pitch % 12;
                    if (!pc_set[pc]) { pc_set[pc] = true; pc_count++; }
                }
            }
        }
        if (pc_count >= 2) {
            bool chord_matched_buf[64] = {0};  /* total_chords is ~37 */
            chord_matched = chord_matched_buf;
            /* For each possible root (0-11), compute intervals and match */
            for (int root = 0; root < 12; root++) {
                /* Build interval set from root */
                bool intervals[24] = {false};
                int int_count = 0;
                for (int pc = 0; pc < 12; pc++) {
                    if (pc_set[pc]) {
                        int iv = (pc - root + 12) % 12;
                        if (!intervals[iv]) { intervals[iv] = true; int_count++; }
                    }
                }
                /* Check each chord definition */
                for (int ci = 0; ci < total_chords; ci++) {
                    const ChordDef *cd = chord_at(ci);
                    if (!cd) continue;
                    /* Reduce chord intervals to within octave for comparison */
                    bool cd_intervals[12] = {false};
                    int cd_pc_count = 0;
                    for (int k = 0; k < cd->count; k++) {
                        int iv = cd->intervals[k] % 12;
                        if (!cd_intervals[iv]) { cd_intervals[iv] = true; cd_pc_count++; }
                    }
                    if (cd_pc_count != int_count) continue;
                    bool match = true;
                    for (int iv = 0; iv < 12; iv++) {
                        if (cd_intervals[iv] != intervals[iv]) { match = false; break; }
                    }
                    if (match) chord_matched[ci] = true;
                }
            }
        }
    }

    /* Dim background */
    draw_filled_rect(r, 0, 0, (float)app->win_w, (float)app->win_h,
                     0x00, 0x00, 0x00, 0x80);

    /* Dialog: 320x480, centered */
    float dw = 320, dh = 480;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;

    draw_rounded_rect(r, dx, dy, dw, dh, 6, COL_SURFACE, 0xFF);

    /* Header */
    float hdr_h = 32;
    draw_filled_rect(r, dx, dy, dw, hdr_h, COL_BG, 0xFF);
    draw_text_bold(r, "Chordify", dx + 8, dy + hdr_h / 2 - 5, 11, COL_TEXT);
    draw_text(r, "x", dx + dw - 20, dy + hdr_h / 2 - 5, 13, COL_TEXT);

    /* Content */
    float content_y = dy + hdr_h + 4;
    float content_h = dh - hdr_h - 12;
    push_clip(r, dx + 8, content_y, dw - 16, content_h);

    float cy = content_y - app->chord_scroll;
    int item_idx = 0;

    for (int g = 0; g < CHORD_GROUP_COUNT; g++) {
        const ChordGroup *grp = &CHORD_GROUPS[g];
        draw_text_bold(r, grp->name, dx + 12, cy, 11, COL_GOLD);
        cy += 20;

        for (int i = 0; i < grp->count; i++) {
            const ChordDef *cd = &grp->chords[i];
            float btn_y = cy;
            float btn_h = 24;
            bool hovered = (item_idx == app->chord_hover);
            bool detected = chord_matched && chord_matched[item_idx];

            uint8_t bg_r, bg_g, bg_b;
            if (detected) {
                bg_r = 0xB0; bg_g = 0x90; bg_b = 0x46; /* COL_GOLD_DARK */
            } else if (hovered) {
                bg_r = 0x24; bg_g = 0x24; bg_b = 0x27; /* COL_BG_LIGHT */
            } else {
                bg_r = 0x31; bg_g = 0x32; bg_b = 0x39; /* COL_SURFACE */
            }
            draw_rounded_rect(r, dx + 10, btn_y, dw - 20, btn_h, 4,
                              bg_r, bg_g, bg_b, 0xFF);

            /* Label with intervals */
            char lbl[128];
            char ints_str[64] = "";
            for (int k = 0; k < cd->count; k++) {
                char tmp[8];
                snprintf(tmp, sizeof(tmp), "%s%d", k ? ", " : "", cd->intervals[k]);
                strncat(ints_str, tmp, sizeof(ints_str) - strlen(ints_str) - 1);
            }
            snprintf(lbl, sizeof(lbl), "%s  (%s)", cd->name, ints_str);
            draw_text(r, lbl, dx + 16, btn_y + btn_h / 2 - 4, 10,
                      hovered ? COL_GOLD_LIGHT : COL_TEXT);

            cy += btn_h + 2;
            item_idx++;
        }
        cy += 6;
    }

    pop_clip(r);
    (void)chord_matched; /* stack-allocated, no free needed */
}

/* --- dropdown menus --- */

void ui_dropdown_render(MuseApp *app) {
    if (app->dropdown_open == 0) return;
    SDL_Renderer *r = app->renderer;

    /* Determine items based on dropdown type */
    const char *items[16];
    int n_items = 0;

    if (app->dropdown_open == 1) {
        /* Time signature */
        items[0] = "2"; items[1] = "3"; items[2] = "4";
        items[3] = "6"; items[4] = "8"; n_items = 5;
    } else if (app->dropdown_open == 2) {
        /* Grid snap */
        items[0] = "1/4"; items[1] = "1/8"; items[2] = "1/16";
        items[3] = "1/32"; items[4] = "1/64"; items[5] = "Free";
        n_items = 6;
    } else if (app->dropdown_open == 3) {
        /* technique list depends on the active instrument */
        if (app->project.num_layers > 0) {
            MuseLayer *aly = &app->project.layers[app->project.active_layer];
            const MuseInstrument *ains = inst_by_id(aly->inst_id);
            if (ains) {
                for (int t = 0; t < ains->num_techniques && n_items < 16; t++) {
                    const MuseTechnique *tech = technique_by_id(ains->techniques[t]);
                    items[n_items++] = tech ? tech->name : "Unknown";
                }
            }
        }
        if (n_items == 0) { items[0] = "Normal"; n_items = 1; }
    } else if (app->dropdown_open == 4) {
        /* Marnian synth profile */
        items[0] = "Basic"; items[1] = "Stereo";
        items[2] = "Super"; items[3] = "SuperOct";
        n_items = 4;
    }

    /* dropdown styling */
    float item_h = 28;
    float menu_w = app->dropdown_anchor.w;
    if (menu_w < 80) menu_w = 80;
    float menu_h = n_items * item_h + 4;
    float dmx = app->dropdown_anchor.x;
    float dmy = app->dropdown_anchor.y + app->dropdown_anchor.h + 2;

    /* Clamp to window */
    if (dmy + menu_h > app->win_h) dmy = app->dropdown_anchor.y - menu_h - 2;

    /* Background with rounded corners */
    draw_rounded_rect(r, dmx, dmy, menu_w, menu_h, 6, 0x31, 0x32, 0x39, 0xFF);
    draw_rounded_rect_outline(r, dmx, dmy, menu_w, menu_h, 6, 0x44, 0x43, 0x48);

    push_clip(r, dmx, dmy, menu_w, menu_h);
    for (int i = 0; i < n_items; i++) {
        float iy = dmy + 2 + i * item_h;
        bool hovered = (i == app->dropdown_hover);
        if (hovered) {
            draw_rounded_rect(r, dmx + 2, iy, menu_w - 4, item_h, 4,
                              0xB0, 0x90, 0x46, 0xFF);
        }
        draw_text(r, items[i], dmx + 10, iy + item_h / 2 - 5, 13, COL_TEXT);
    }
    pop_clip(r);
}

/* --- MIDI import dialog --- */

void ui_midi_import_render(MuseApp *app) {
    if (!app->midi_dlg_open || !app->midi_dlg_data) return;
    SDL_Renderer *r = app->renderer;
    MidiImportData *mid = (MidiImportData *)app->midi_dlg_data;

    float dw = 500, dh = 430;
    float dx = ((float)app->win_w - dw) / 2;
    float dy = ((float)app->win_h - dh) / 2;

    /* Dim background */
    draw_filled_rect(r, 0, 0, (float)app->win_w, (float)app->win_h,
                     0x00, 0x00, 0x00, 0x80);

    /* Dialog background */
    draw_rounded_rect(r, dx, dy, dw, dh, 8, COL_BG, 0xFF);
    draw_rounded_rect_outline(r, dx, dy, dw, dh, 8, COL_BORDER);

    /* Title */
    draw_text_bold(r, "Import MIDI", dx + 16, dy + 12, 12, COL_TEXT);

    /* Info header */
    {
        int total_notes = 0;
        for (int i = 0; i < mid->num_channels; i++)
            total_notes += mid->channels[i].note_count;
        char info[128];
        snprintf(info, sizeof(info), "%d channels, %d notes, %d BPM",
                 mid->num_channels, total_notes, mid->bpm);
        draw_text(r, info, dx + 16, dy + 30, 10, COL_TEXT_DIM);
    }

    float content_y = dy + 50;
    if (mid->tempo_changes > 1) {
        char tc[64];
        snprintf(tc, sizeof(tc), "(%d tempo changes detected)", mid->tempo_changes);
        draw_text(r, tc, dx + 16, content_y, 9, COL_GOLD_DARK);
        content_y += 16;
    }

    /* Combine all checkbox + instrument selector */
    {
        float cb_x = dx + 16, cb_y = content_y;
        UiRect cb_rc = { cb_x, cb_y, 14, 14 };
        draw_rounded_rect(r, cb_rc.x, cb_rc.y, cb_rc.w, cb_rc.h, 3, COL_SURFACE, 0xFF);
        draw_rounded_rect_outline(r, cb_rc.x, cb_rc.y, cb_rc.w, cb_rc.h, 3, COL_BORDER);
        if (mid->combine_all) {
            draw_text_centered(r, "x", cb_rc.x + 7, cb_rc.y + 7, 10, COL_GOLD);
        }
        draw_text(r, "Combine all into:", cb_x + 20, cb_y + 1, 9, COL_TEXT);

        /* instrument selector for combine target */
        const MuseInstrument *cins = inst_by_id(mid->combine_inst_id);
        const char *cins_name = cins ? cins->name : "Flor. Piano";
        UiRect cinst_rc = { dx + 180, cb_y - 2, 160, 18 };
        if (mid->combine_all) {
            draw_ctk_entry(r, cinst_rc, cins_name, 13, false);
            draw_dropdown_arrow(r, dx + 180 + 160 - 10, cb_y + 7, 6, COL_TEXT_DIM);
        } else {
            draw_rounded_rect(r, cinst_rc.x, cinst_rc.y, cinst_rc.w, cinst_rc.h, 3,
                              COL_SURFACE, 0x60);
            draw_text(r, cins_name, cinst_rc.x + 6, cb_y + 1, 9, 0x60, 0x60, 0x60);
        }
        content_y += 22;
    }

    /* Column headers */
    draw_text(r, "Channel", dx + 16, content_y, 9, COL_TEXT_DIM);
    draw_text(r, "BDO Instrument", dx + 260, content_y, 9, COL_TEXT_DIM);
    content_y += 16;

    /* Scrollable channel list */
    float list_h = dh - (content_y - dy) - 50; /* leave room for buttons */
    push_clip(r, dx + 4, content_y, dw - 8, list_h);

    float row_h = 32;
    float my = app->mouse_y;
    for (int i = 0; i < mid->num_channels; i++) {
        float ry = content_y + i * row_h - app->midi_dlg_scroll;
        if (ry + row_h < content_y || ry > content_y + list_h) continue;

        MidiChannel *ch = &mid->channels[i];

        /* Row background on hover */
        bool row_hovered = (i == app->midi_dlg_hover);
        if (row_hovered) {
            draw_filled_rect(r, dx + 4, ry, dw - 8, row_h,
                             COL_BG_LIGHT, 0xFF);
        }

        /* Channel label */
        draw_text(r, ch->label, dx + 16, ry + row_h / 2 - 5, 10, COL_TEXT);

        /* Instrument selector (shows as an entry with dropdown arrow) */
        const MuseInstrument *ins = inst_by_id(ch->user_inst_id);
        const char *ins_name = ins ? ins->name : "Piano";
        UiRect inst_rc = { dx + 260, ry + 4, 180, 24 };
        bool ch_greyed = mid->combine_all && !ch->is_percussion;
        if (ch_greyed) {
            draw_rounded_rect(r, inst_rc.x, inst_rc.y, inst_rc.w, inst_rc.h, 3,
                              COL_SURFACE, 0x60);
            draw_text(r, ins_name, inst_rc.x + 6, ry + row_h / 2 - 5, 10,
                      0x60, 0x60, 0x60);
        } else {
            draw_ctk_entry(r, inst_rc, ins_name, 13, false);
            draw_dropdown_arrow(r, dx + 260 + 180 - 10, ry + 14, 7, COL_TEXT_DIM);
        }
    }
    pop_clip(r);

    /* Buttons at bottom */
    float btn_y = dy + dh - 42;
    UiRect cancel_btn = { dx + dw - 100, btn_y, 80, 30 };
    UiRect import_btn = { dx + dw - 190, btn_y, 80, 30 };

    draw_ctk_button(r, import_btn, "Import", 13,
                    app->midi_dlg_hover_btn == 1, false);
    draw_ctk_button(r, cancel_btn, "Cancel", 13,
                    app->midi_dlg_hover_btn == 2, false);

    /* MIDI instrument dropdown overlay (when a channel's or combine instrument is clicked) */
    if (app->midi_dlg_dropdown_ch >= -2 && app->midi_dlg_dropdown_ch != -1 &&
        (app->midi_dlg_dropdown_ch == -2 || app->midi_dlg_dropdown_ch < mid->num_channels)) {
        float dd_x, dd_anchor_y;
        if (app->midi_dlg_dropdown_ch == -2) {
            /* combine instrument selector - anchor below the combine row */
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
        /* compute total height: groups with headers + items + spacing */
        float dd_total_h = 4;
        for (int g = 0; g < INST_GROUP_COUNT; g++) {
            dd_total_h += dd_hdr_h;
            dd_total_h += INST_GROUPS[g].count * dd_item_h;
            if (g < INST_GROUP_COUNT - 1) dd_total_h += 4; /* group spacing */
        }
        float dd_h = dd_total_h;
        float dd_y = dd_anchor_y;
        /* If dropdown goes below dialog, place it above */
        if (dd_y + dd_h > dy + dh) dd_y = dd_anchor_y - dd_h - row_h - 4;
        /* Clamp to screen */
        if (dd_y < 0) dd_y = 0;
        float dd_max_h = (float)app->win_h - dd_y - 10;
        if (dd_h > dd_max_h) dd_h = dd_max_h;

        /* Background */
        draw_rounded_rect(r, dd_x, dd_y, dd_w, dd_h, 6, 0x31, 0x32, 0x39, 0xFF);
        draw_rounded_rect_outline(r, dd_x, dd_y, dd_w, dd_h, 6, 0x44, 0x43, 0x48);

        push_clip(r, dd_x, dd_y, dd_w, dd_h);
        float cy2 = dd_y + 2 - app->midi_dlg_dropdown_scroll;
        int item_idx2 = 0;
        for (int g = 0; g < INST_GROUP_COUNT; g++) {
            const InstGroup *grp = &INST_GROUPS[g];
            /* Group header */
            if (cy2 + dd_hdr_h >= dd_y && cy2 < dd_y + dd_h) {
                draw_text_bold(r, grp->group, dd_x + 8, cy2 + 2, 9, COL_GOLD);
            }
            cy2 += dd_hdr_h;
            for (int i = 0; i < grp->count; i++) {
                if (cy2 + dd_item_h >= dd_y && cy2 < dd_y + dd_h) {
                    const MuseInstrument *inst = inst_by_id(grp->ids[i]);
                    if (inst) {
                        bool hovered = (item_idx2 == app->midi_dlg_dropdown_hover);
                        if (hovered) {
                            draw_rounded_rect(r, dd_x + 2, cy2, dd_w - 4, dd_item_h, 4,
                                              0xB0, 0x90, 0x46, 0xFF);
                        }
                        /* Icon or color swatch */
                        SDL_Texture *icon = get_instrument_icon(grp->ids[i]);
                        if (icon) {
                            SDL_FRect icon_dst = { dd_x + 8, cy2 + 2, 20, 20 };
                            SDL_RenderTexture(r, icon, NULL, &icon_dst);
                        } else {
                            draw_filled_rect(r, dd_x + 10, cy2 + 3, 8, dd_item_h - 6,
                                             inst->color_r, inst->color_g, inst->color_b, 0xFF);
                        }
                        draw_text(r, inst->name, dd_x + 32, cy2 + dd_item_h / 2 - 4, 9,
                                  hovered ? COL_GOLD_LIGHT : COL_TEXT);
                    }
                }
                cy2 += dd_item_h;
                item_idx2++;
            }
            if (g < INST_GROUP_COUNT - 1) cy2 += 4;
        }
        pop_clip(r);
    }
}
