#include "muse_format.h"
#include "app.h"
#include "model.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* grid snap to divisor mapping */
static int snap_to_div(GridSnap gs) {
    switch (gs) {
    case GRID_1_4:  return 1;
    case GRID_1_8:  return 2;
    case GRID_1_16: return 4;
    case GRID_1_32: return 8;
    case GRID_1_64: return 16;
    default:        return 4;
    }
}

static GridSnap div_to_snap(int d) {
    switch (d) {
    case 1:  return GRID_1_4;
    case 2:  return GRID_1_8;
    case 4:  return GRID_1_16;
    case 8:  return GRID_1_32;
    case 16: return GRID_1_64;
    default: return GRID_1_16;
    }
}

/* json string escaping - handles the usual suspects */

static void write_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputs("\\\"", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') fputs("\\r", f);
        else if (*s == '\t') fputs("\\t", f);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", (unsigned char)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

/* --- save --- */

int muse_save(const char *path, const MuseApp *app) {
    FILE *f = fopen_utf8(path, "w");
    if (!f) return -1;

    const MuseProject *p = &app->project;

    fprintf(f, "{\n");
    fprintf(f, "  \"version\": 1,\n");
    fprintf(f, "  \"bpm\": %d,\n", p->bpm);
    fprintf(f, "  \"time_sig\": %d,\n", p->time_sig);
    fprintf(f, "  \"grid_div\": %d,\n", snap_to_div(app->grid_snap));
    fprintf(f, "  \"measures\": %d,\n", app->measures);
    fprintf(f, "  \"velocity\": %d,\n", app->cur_vel);
    fprintf(f, "  \"technique\": %d,\n", app->cur_ntype);

    fprintf(f, "  \"char_name\": "); write_json_string(f, app->char_name); fprintf(f, ",\n");
    fprintf(f, "  \"owner_id\": %u,\n", p->owner_id);
    fprintf(f, "  \"filename\": "); write_json_string(f, app->filename); fprintf(f, ",\n");

    fprintf(f, "  \"reverb\": %d,\n", app->fx_reverb);
    fprintf(f, "  \"delay\": %d,\n", app->fx_delay);
    fprintf(f, "  \"chorus_fb\": %d,\n", app->fx_chorus_fb);
    fprintf(f, "  \"chorus_depth\": %d,\n", app->fx_chorus_depth);
    fprintf(f, "  \"chorus_freq\": %d,\n", app->fx_chorus_freq);

    fprintf(f, "  \"active_layer_idx\": %d,\n", p->active_layer);

    fprintf(f, "  \"layers\": [\n");
    for (int li = 0; li < p->num_layers; li++) {
        const MuseLayer *ly = &p->layers[li];
        fprintf(f, "    {\n");
        fprintf(f, "      \"inst_id\": %d,\n", ly->inst_id);
        fprintf(f, "      \"volume\": %d,\n", ly->volume);
        fprintf(f, "      \"muted\": %s,\n", ly->muted ? "true" : "false");
        fprintf(f, "      \"solo\": %s,\n", ly->solo ? "true" : "false");
        fprintf(f, "      \"reverb_send\": %d,\n", ly->reverb_send);
        fprintf(f, "      \"delay_send\": %d,\n", ly->delay_send);
        fprintf(f, "      \"chorus_send\": %d,\n", ly->chorus_send);
        fprintf(f, "      \"synth_profile\": %d,\n", ly->synth_profile);
        fprintf(f, "      \"active_sub\": %d,\n", ly->active_sub);

        fprintf(f, "      \"sublayers\": [\n");
        for (int si = 0; si < ly->num_sublayers; si++) {
            const NoteArray *na = &ly->sublayers[si];
            fprintf(f, "        [\n");
            for (int ni = 0; ni < na->count; ni++) {
                const MuseNote *n = &na->notes[ni];
                fprintf(f, "          [%d, %d, %.2f, %.2f, %d]",
                        n->pitch, n->vel, n->start, n->dur, n->ntype);
                if (ni + 1 < na->count) fprintf(f, ",");
                fprintf(f, "\n");
            }
            fprintf(f, "        ]");
            if (si + 1 < ly->num_sublayers) fprintf(f, ",");
            fprintf(f, "\n");
        }
        fprintf(f, "      ]\n");

        fprintf(f, "    }");
        if (li + 1 < p->num_layers) fprintf(f, ",");
        fprintf(f, "\n");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

/* Minimal JSON parser for .composer format */

static const char *jskip(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jstr(const char *p, char *buf, int bufsz) {
    if (*p != '"') { buf[0] = 0; return p; }
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        if (i < bufsz - 1) buf[i++] = *p;
        p++;
    }
    buf[i] = 0;
    if (*p == '"') p++;
    return p;
}

static const char *jnum(const char *p, double *out) {
    char *end;
    *out = strtod(p, &end);
    return end;
}

static const char *jskip_value(const char *p) {
    p = jskip(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
    } else if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
    } else if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
    } else if (strncmp(p, "true", 4) == 0) {
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "null", 4) == 0) {
        p += 4;
    } else {
        /* must be a number */
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
               *p != '\n' && *p != '\r' && *p != '\t') p++;
    }
    return p;
}

static const char *jbool(const char *p, bool *out) {
    p = jskip(p);
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return p + 4; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return p + 5; }
    *out = false;
    return p;
}

/* --- load --- */

int muse_load(const char *path, MuseApp *app) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 50 * 1024 * 1024) { fclose(f); return -2; }
    rewind(f);
    char *json = (char *)malloc((size_t)sz + 1);
    if (!json) { fclose(f); return -3; }
    if ((long)fread(json, 1, (size_t)sz, f) != sz) { free(json); fclose(f); return -4; }
    json[sz] = 0;
    fclose(f);

    /* wipe and start fresh */
    muse_project_free(&app->project);
    muse_project_init(&app->project);

    const char *p = jskip(json);
    if (*p != '{') { free(json); return -5; }
    p = jskip(p + 1);

    int grid_div_val = 4;

    while (*p && *p != '}') {
        char key[64];
        p = jstr(p, key, sizeof(key));
        p = jskip(p);
        if (*p == ':') p = jskip(p + 1);

        if (strcmp(key, "bpm") == 0) {
            double v; p = jnum(p, &v); app->project.bpm = (uint16_t)v;
        } else if (strcmp(key, "time_sig") == 0) {
            double v; p = jnum(p, &v); app->project.time_sig = (uint16_t)v;
        } else if (strcmp(key, "grid_div") == 0) {
            double v; p = jnum(p, &v); grid_div_val = (int)v;
        } else if (strcmp(key, "measures") == 0) {
            double v; p = jnum(p, &v); app->measures = (int)v;
        } else if (strcmp(key, "velocity") == 0) {
            double v; p = jnum(p, &v); app->cur_vel = (uint8_t)v;
        } else if (strcmp(key, "technique") == 0) {
            double v; p = jnum(p, &v); app->cur_ntype = (uint8_t)v;
        } else if (strcmp(key, "owner_id") == 0) {
            double v; p = jnum(p, &v); app->project.owner_id = (uint32_t)v;
        } else if (strcmp(key, "char_name") == 0) {
            char buf[64];
            p = jstr(p, buf, sizeof(buf));
            snprintf(app->char_name, sizeof(app->char_name), "%s", buf);
            snprintf(app->project.char_name, sizeof(app->project.char_name), "%s", buf);
        } else if (strcmp(key, "filename") == 0) {
            char buf[256];
            p = jstr(p, buf, sizeof(buf));
            snprintf(app->filename, sizeof(app->filename), "%s", buf);
        } else if (strcmp(key, "reverb") == 0) {
            double v; p = jnum(p, &v); app->fx_reverb = (int)v;
            app->project.effector_reverb = (uint8_t)v;
        } else if (strcmp(key, "delay") == 0) {
            double v; p = jnum(p, &v); app->fx_delay = (int)v;
            app->project.effector_delay = (uint8_t)v;
        } else if (strcmp(key, "chorus_fb") == 0) {
            double v; p = jnum(p, &v); app->fx_chorus_fb = (int)v;
            app->project.effector_chorus_fb = (uint8_t)v;
        } else if (strcmp(key, "chorus_depth") == 0) {
            double v; p = jnum(p, &v); app->fx_chorus_depth = (int)v;
            app->project.effector_chorus_depth = (uint8_t)v;
        } else if (strcmp(key, "chorus_freq") == 0) {
            double v; p = jnum(p, &v); app->fx_chorus_freq = (int)v;
            app->project.effector_chorus_freq = (uint8_t)v;
        } else if (strcmp(key, "active_layer_idx") == 0) {
            double v; p = jnum(p, &v); app->project.active_layer = (int)v;
        } else if (strcmp(key, "layers") == 0) {
            /* parse the layers array */
            p = jskip(p);
            if (*p != '[') { p = jskip_value(p); goto muse_next; }
            p = jskip(p + 1);

            while (*p && *p != ']') {
                if (*p != '{') { p = jskip_value(p); goto layer_next; }
                p = jskip(p + 1);

                /* defaults */
                uint8_t inst_id = 0x11;
                uint8_t volume = 70;
                bool muted = false;
                bool solo = false;
                uint8_t reverb_send = 0, delay_send = 0, chorus_send = 0;
                uint8_t synth_profile = 0;
                int active_sub = 0;

                /* temp buffer for sublayers while we parse */
                typedef struct { NoteArray *subs; int nsubs; } SubBuf;
                SubBuf sbuf = {NULL, 0};

                while (*p && *p != '}') {
                    char lkey[64];
                    p = jstr(p, lkey, sizeof(lkey));
                    p = jskip(p);
                    if (*p == ':') p = jskip(p + 1);

                    if (strcmp(lkey, "inst_id") == 0) {
                        double v; p = jnum(p, &v); inst_id = (uint8_t)v;
                    } else if (strcmp(lkey, "volume") == 0) {
                        double v; p = jnum(p, &v); volume = (uint8_t)v;
                    } else if (strcmp(lkey, "muted") == 0) {
                        p = jbool(p, &muted);
                    } else if (strcmp(lkey, "solo") == 0) {
                        p = jbool(p, &solo);
                    } else if (strcmp(lkey, "reverb_send") == 0) {
                        double v; p = jnum(p, &v); reverb_send = (uint8_t)v;
                    } else if (strcmp(lkey, "delay_send") == 0) {
                        double v; p = jnum(p, &v); delay_send = (uint8_t)v;
                    } else if (strcmp(lkey, "chorus_send") == 0) {
                        double v; p = jnum(p, &v); chorus_send = (uint8_t)v;
                    } else if (strcmp(lkey, "synth_profile") == 0) {
                        double v; p = jnum(p, &v); synth_profile = (uint8_t)v;
                    } else if (strcmp(lkey, "active_sub") == 0) {
                        double v; p = jnum(p, &v); active_sub = (int)v;
                    } else if (strcmp(lkey, "sublayers") == 0) {
                        /* sublayers: array of note arrays */
                        p = jskip(p);
                        if (*p != '[') { p = jskip_value(p); goto lkey_next; }
                        p = jskip(p + 1);

                        int sub_cap = 4;
                        sbuf.subs = (NoteArray *)calloc((size_t)sub_cap, sizeof(NoteArray));
                        sbuf.nsubs = 0;

                        while (*p && *p != ']') {
                            /* each sublayer is just an array of notes */
                            if (*p != '[') { p = jskip_value(p); goto sub_next; }
                            p = jskip(p + 1);

                            if (sbuf.nsubs >= sub_cap) {
                                sub_cap *= 2;
                                sbuf.subs = (NoteArray *)realloc(sbuf.subs, (size_t)sub_cap * sizeof(NoteArray));
                            }
                            NoteArray *na = &sbuf.subs[sbuf.nsubs];
                            note_array_init(na);

                            while (*p && *p != ']') {
                                /* note: [pitch, vel, start, dur, ntype] */
                                if (*p != '[') { p = jskip_value(p); goto note_next; }
                                p = jskip(p + 1);

                                double vals[5] = {0};
                                for (int vi = 0; vi < 5; vi++) {
                                    p = jskip(p);
                                    p = jnum(p, &vals[vi]);
                                    p = jskip(p);
                                    if (*p == ',') p = jskip(p + 1);
                                }
                                p = jskip(p);
                                if (*p == ']') p = jskip(p + 1);

                                MuseNote note = {
                                    .pitch    = (uint8_t)vals[0],
                                    .vel      = (uint8_t)vals[1],
                                    .start    = vals[2],
                                    .dur      = vals[3],
                                    .ntype    = (uint8_t)vals[4],
                                    .selected = 0,
                                };
                                note_array_push(na, note);

                            note_next:
                                p = jskip(p);
                                if (*p == ',') p = jskip(p + 1);
                            }
                            if (*p == ']') p = jskip(p + 1);
                            sbuf.nsubs++;

                        sub_next:
                            p = jskip(p);
                            if (*p == ',') p = jskip(p + 1);
                        }
                        if (*p == ']') p = jskip(p + 1);
                    } else {
                        p = jskip_value(p);
                    }

                lkey_next:
                    p = jskip(p);
                    if (*p == ',') p = jskip(p + 1);
                }
                if (*p == '}') p = jskip(p + 1);

                /* add it to the project */
                int li = muse_project_add_layer(&app->project, inst_id);
                if (li >= 0) {
                    MuseLayer *ly = &app->project.layers[li];
                    ly->volume = volume;
                    ly->muted = muted;
                    ly->solo = solo;
                    ly->reverb_send = reverb_send;
                    ly->delay_send = delay_send;
                    ly->chorus_send = chorus_send;
                    ly->synth_profile = synth_profile;
                    ly->active_sub = active_sub;

                    /* swap in the loaded sublayer data */
                    if (sbuf.subs && sbuf.nsubs > 0) {
                        /* free the default empty sublayer */
                        for (int si = 0; si < ly->num_sublayers; si++)
                            note_array_free(&ly->sublayers[si]);
                        free(ly->sublayers);

                        ly->sublayers = sbuf.subs;
                        ly->num_sublayers = sbuf.nsubs;
                        sbuf.subs = NULL; /* ownership transferred */
                    }
                }
                if (sbuf.subs) {
                    for (int si = 0; si < sbuf.nsubs; si++)
                        note_array_free(&sbuf.subs[si]);
                    free(sbuf.subs);
                }

            layer_next:
                p = jskip(p);
                if (*p == ',') p = jskip(p + 1);
            }
            if (*p == ']') p = jskip(p + 1);
        } else {
            /* unknown key, skip it */
            p = jskip_value(p);
        }

    muse_next:
        p = jskip(p);
        if (*p == ',') p = jskip(p + 1);
    }

    /* apply loaded settings */
    app->grid_snap = div_to_snap(grid_div_val);

    /* reset view */
    app->project.dirty = false;
    app->playhead_ms = 0;
    app->scroll_x = 0;
    if (app->measures < 4) app->measures = 4;

    free(json);
    return 0;
}
