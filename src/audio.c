#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio.h"
#include "instruments.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* stb_vorbis for ogg decoding (fallback when libsndfile isn't around) */
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define SAMPLE_RATE     44100
#define MAX_VOICES      128
#define MAX_CACHED       512
#define CACHE_MEM_LIMIT (64 * 1024 * 1024)  /* 64 MB max sample cache */

/* sample data - decoded audio from BDO's WEM files */

typedef struct {
    float   *data;       /* interleaved stereo float32 */
    int      frames;
    int      orig_sr;
} SampleData;

/* Key zones - extracted from BDO's Wwise BNK files. Each zone maps a pitch
   range to one or more WEM sample files. */

#define MAX_ZONE_WEMS  16   /* max velocity layers per zone */
#define MAX_ZONES      4096

typedef struct {
    uint32_t wem;
    int      vel_min;
    int      vel_max;
} VelWem;

typedef struct {
    char     dir_name[80];   /* instrument directory name */
    int      ntype;          /* articulation (0=sustain, 99=drum, etc.) */
    int      key_min;
    int      key_max;
    int      root;           /* root pitch of zone */
    uint32_t wems[MAX_ZONE_WEMS]; /* WEM IDs sorted soft->loud */
    float    wem_vol_db[MAX_ZONE_WEMS]; /* per-WEM volume offset in dB */
    int      num_wems;
    VelWem   vel_wems[MAX_ZONE_WEMS]; /* velocity-mapped WEMs from BNK */
    int      num_vel_wems;
    bool     half_rate;     /* true = sample is half-rate encoded, use sr/2 for resample */
} KeyZone;

static KeyZone  g_zones[MAX_ZONES];
static int      g_zone_count;

/* per-instrument volume offset (dB) - tuned by ear to match in-game levels */
#define MAX_INST_VOL 64
static struct { char dir[80]; float db; } g_inst_vol[MAX_INST_VOL];
static int g_inst_vol_count;

static float inst_volume_db(const char *dir_name) {
    for (int i = 0; i < g_inst_vol_count; i++)
        if (strcmp(g_inst_vol[i].dir, dir_name) == 0) return g_inst_vol[i].db;
    return -6.0f;  /* default when instrument isn't in the volume table */
}

/* on-finished callback */
static void (*g_on_finished)(void *);
static void *g_on_finished_data;

void muse_audio_set_on_finished(void (*cb)(void *userdata), void *userdata) {
    g_on_finished = cb;
    g_on_finished_data = userdata;
}

/* Sample cache: keyed by (dir_name, wem_id). LRU eviction when full. */

typedef struct {
    char       dir_name[80];
    uint32_t   wem_id;
    int        root_pitch;     /* from zone */
    int        actual_orig_sr; /* original sample rate before resampling */
    SampleData sample;
    uint32_t   last_used;      /* LRU tick for eviction */
} CachedSample;

static CachedSample g_cache[MAX_CACHED];
static int          g_cache_count;
static size_t       g_cache_bytes;       /* total bytes of sample data in cache */
static uint32_t     g_cache_tick;        /* global access counter for LRU */

/* BDO inst_id -> sample directory name. These match the folder names
   from extracting the game's Wwise BNK archives. */
static const char *inst_dir_name(uint8_t id) {
    static const struct { uint8_t id; const char *dir; } MAP[] = {
        {0x00, "midi_instrument_00_acousticguitar"},
        {0x01, "midi_instrument_01_flute"},
        {0x02, "midi_instrument_02_recorder"},
        {0x03, "midi_instrument_03_snaredrum"},
        {0x04, "midi_instrument_04_handdrum"},
        {0x05, "midi_instrument_05_piatticymbals"},
        {0x06, "midi_instrument_06_harp"},
        {0x07, "midi_instrument_07_piano"},
        {0x08, "midi_instrument_08_violin"},
        {0x09, "midi_instrument_09_pandrum"},
        {0x0a, "midi_instrument_10_proguitar"},
        {0x0b, "midi_instrument_11_proflute"},
        {0x0d, "midi_instrument_13_prodrumset"},
        {0x0e, "midi_instrument_14_probasselectric"},
        {0x0f, "midi_instrument_15_probasscontra"},
        {0x10, "midi_instrument_16_proharp"},
        {0x11, "midi_instrument_17_propiano"},
        {0x12, "midi_instrument_18_proviolin"},
        {0x13, "midi_instrument_19_propandrum"},
        {0x14, "midi_instrument_synth_sine_basic"},
        {0x15, "midi_instrument_synth_sine_stereo"},
        {0x16, "midi_instrument_synth_sine_super"},
        {0x17, "midi_instrument_synth_sine_superoct"},
        {0x18, "midi_instrument_synth_triangle_basic"},
        {0x19, "midi_instrument_synth_triangle_stereo"},
        {0x1a, "midi_instrument_synth_triangle_super"},
        {0x1b, "midi_instrument_synth_triangle_superoct"},
        {0x1c, "midi_instrument_synth_square_basic"},
        {0x1d, "midi_instrument_synth_square_stereo"},
        {0x1e, "midi_instrument_synth_square_super"},
        {0x1f, "midi_instrument_synth_square_superoct"},
        {0x20, "midi_instrument_synth_saw_basic"},
        {0x21, "midi_instrument_synth_saw_stereo"},
        {0x22, "midi_instrument_synth_saw_super"},
        {0x23, "midi_instrument_synth_saw_superoct"},
        {0x24, "midi_instrument_24_proguitarelectricclean"},
        {0x25, "midi_instrument_25_proguitarelectricdrive"},
        {0x26, "midi_instrument_26_proguitarelectricdist"},
        {0x27, "midi_instrument_27_proclarinet"},
        {0x28, "midi_instrument_28_prohorn"},
    };
    for (int i = 0; i < (int)(sizeof(MAP)/sizeof(MAP[0])); i++)
        if (MAP[i].id == id) return MAP[i].dir;
    return NULL;
}

static char g_samples_dir[512] = "";

void muse_audio_set_samples_dir(const char *path) {
    snprintf(g_samples_dir, sizeof(g_samples_dir), "%s", path);
}

/* Minimal JSON parser for key_zones.json */
static const char *jskip(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* parse a JSON string, no unicode support needed */
static const char *jstr(const char *p, char *buf, int bufsz) {
    if (*p != '"') { buf[0] = 0; return p; }
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; if (!*p) break; }
        if (i < bufsz - 1) buf[i++] = *p;
        p++;
    }
    buf[i] = 0;
    if (*p == '"') p++;
    return p;
}

/* parse a number */
static const char *jnum(const char *p, double *out) {
    char *end;
    *out = strtod(p, &end);
    return end;
}

/* skip any JSON value */
static const char *jskip_value(const char *p) {
    p = jskip(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\') p++; p++; }
        if (*p == '"') p++;
    } else if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
    } else if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } }
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n') p++;
    }
    return p;
}

/* sort WEMs by file size (smallest=softest, largest=loudest).
   This is how Wwise orders velocity layers */
static void sort_wems_by_size(KeyZone *z) {
    if (z->num_wems <= 1) return;
    char path[600];
    struct { uint32_t id; long size; float vol_db; } ws[MAX_ZONE_WEMS];
    for (int i = 0; i < z->num_wems; i++) {
        ws[i].id = z->wems[i];
        ws[i].size = 0;
        ws[i].vol_db = z->wem_vol_db[i];
        snprintf(path, sizeof(path), "%s/%s/%u.ogg", g_samples_dir, z->dir_name, z->wems[i]);
        FILE *f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof(path), "%s/%s/%u.wav", g_samples_dir, z->dir_name, z->wems[i]);
            f = fopen(path, "rb");
        }
        if (f) {
            fseek(f, 0, SEEK_END);
            ws[i].size = ftell(f);
            fclose(f);
        }
    }
    /* Insertion sort by size ascending */
    for (int i = 1; i < z->num_wems; i++) {
        typeof(ws[0]) tmp = ws[i];
        int j = i - 1;
        while (j >= 0 && ws[j].size > tmp.size) { ws[j+1] = ws[j]; j--; }
        ws[j+1] = tmp;
    }
    for (int i = 0; i < z->num_wems; i++) {
        z->wems[i] = ws[i].id;
        z->wem_vol_db[i] = ws[i].vol_db;
    }
}

/* Wwise LPF: velocity controls how dark the sound is (0=bright, 100=super muffled).
   Reverse-engineered from RTPC curves in the BNK files.
   Maps to 1-pole filter: fc = 20kHz at 0 down to 80Hz at 100. */
static float lpf_coeff_from_wwise(float lpf_val) {
    if (lpf_val <= 0.0f) return 1.0f; /* bypass */
    if (lpf_val >= 100.0f) lpf_val = 100.0f;
    /* exponential map (matches Wwise's curve) */
    float fc = 20000.0f * powf(10.0f, -lpf_val * 2.4f / 100.0f);
    if (fc >= (float)SAMPLE_RATE * 0.45f) return 1.0f; /* Nyquist bypass */
    float coeff = 1.0f - expf(-2.0f * 3.14159265f * fc / (float)SAMPLE_RATE);
    return coeff;
}

/* per-instrument LPF curves, extracted from BNK RTPC data.
   Each instrument has different velocity-to-filter mappings. */
static float inst_vel_lpf(const char *dir, int velocity) {
    float t = (float)velocity / 127.0f;
    /* piano: vel 0->62 (dark), vel 127->22 (brighter) */
    if (strstr(dir, "07_piano"))
        return 62.0f + t * (22.0f - 62.0f);
    /* Flor. Piano: vel 0->0 (no filter), vel 127->34 */
    if (strstr(dir, "17_propiano"))
        return t * 34.0f;
    /* synths: vel 0->100 (very dark), vel 127->0 (wide open) */
    if (strstr(dir, "synth_"))
        return 100.0f + t * (0.0f - 100.0f);
    return 0.0f; /* no filtering */
}

static int zone_cmp_fn(const void *a, const void *b) {
    const KeyZone *za = a, *zb = b;
    int cmp = strcmp(za->dir_name, zb->dir_name);
    if (cmp == 0) cmp = za->ntype - zb->ntype;
    if (cmp == 0) cmp = za->key_min - zb->key_min;
    return cmp;
}

/* parse key_zones.json - the big one that maps instruments to their samples */
static void load_key_zones(void) {
    g_zone_count = 0;
    g_inst_vol_count = 0;
    if (g_samples_dir[0] == 0) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/key_zones.json", g_samples_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_Log("key_zones.json not found at %s", path); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(sz + 1);
    fread(json, 1, sz, f);
    json[sz] = 0;
    fclose(f);

    SDL_Log("key_zones.json: %ld bytes loaded", sz);

    const char *p = jskip(json);
    if (*p != '{') { free(json); return; }
    p = jskip(p + 1); /* skip opening { */

    /* v2 format has a _meta key with volume offsets, v1 is the older flat format */
    bool is_v2 = (strstr(json, "\"_meta\"") != NULL);
    SDL_Log("key_zones: v2=%d", is_v2);

    if (is_v2) {
    /* v2: top-level has "_meta" and "instruments" */
    while (*p && *p != '}') {
        char key[128];
        p = jstr(p, key, sizeof(key));
        p = jskip(p);
        if (*p == ':') p = jskip(p + 1);

        if (strcmp(key, "_meta") == 0) {
            /* parse _meta for per-instrument volume offsets */
            if (*p != '{') { p = jskip_value(p); goto next_top; }
            p = jskip(p + 1);
            while (*p && *p != '}') {
                char mkey[64];
                p = jstr(p, mkey, sizeof(mkey));
                p = jskip(p);
                if (*p == ':') p = jskip(p + 1);

                if (strcmp(mkey, "inst_volume_db") == 0 && *p == '{') {
                    p = jskip(p + 1);
                    while (*p && *p != '}') {
                        char iname[80]; double db;
                        p = jstr(p, iname, sizeof(iname));
                        p = jskip(p);
                        if (*p == ':') p = jskip(p + 1);
                        p = jnum(p, &db);
                        p = jskip(p);
                        if (g_inst_vol_count < MAX_INST_VOL) {
                            snprintf(g_inst_vol[g_inst_vol_count].dir, 80, "%s", iname);
                            g_inst_vol[g_inst_vol_count].db = (float)db;
                            g_inst_vol_count++;
                        }
                        if (*p == ',') p = jskip(p + 1);
                    }
                    if (*p == '}') p = jskip(p + 1);
                } else {
                    p = jskip_value(p);
                }
                p = jskip(p);
                if (*p == ',') p = jskip(p + 1);
            }
            if (*p == '}') p = jskip(p + 1);
        } else if (strcmp(key, "instruments") == 0) {
            /* parse instruments */
            if (*p != '{') { p = jskip_value(p); goto next_top; }
            p = jskip(p + 1);

            while (*p && *p != '}') {
                char dir_name[80];
                p = jstr(p, dir_name, sizeof(dir_name));
                p = jskip(p);
                if (*p == ':') p = jskip(p + 1);

                /* Value is {ntype_str: [zones]} */
                if (*p != '{') { p = jskip_value(p); goto next_inst; }
                p = jskip(p + 1);

                while (*p && *p != '}') {
                    char ntype_str[16];
                    p = jstr(p, ntype_str, sizeof(ntype_str));
                    int ntype = atoi(ntype_str);
                    p = jskip(p);
                    if (*p == ':') p = jskip(p + 1);

                    /* Value is array of zone objects */
                    if (*p != '[') { p = jskip_value(p); goto next_ntype; }
                    p = jskip(p + 1);

                    while (*p && *p != ']') {
                        if (g_zone_count >= MAX_ZONES) { p = jskip_value(p); goto skip_zone; }

                        KeyZone *z = &g_zones[g_zone_count];
                        snprintf(z->dir_name, sizeof(z->dir_name), "%s", dir_name);
                        z->ntype = ntype;
                        z->key_min = 0;
                        z->key_max = 127;
                        z->root = 60;
                        z->num_wems = 0;
                        z->num_vel_wems = 0;
                        memset(z->wem_vol_db, 0, sizeof(z->wem_vol_db));

                        /* Parse zone object */
                        if (*p != '{') { p = jskip_value(p); goto skip_zone; }
                        p = jskip(p + 1);

                        while (*p && *p != '}') {
                            char zkey[32];
                            p = jstr(p, zkey, sizeof(zkey));
                            p = jskip(p);
                            if (*p == ':') p = jskip(p + 1);

                            if (strcmp(zkey, "root") == 0) {
                                double v; p = jnum(p, &v); z->root = (int)v;
                            } else if (strcmp(zkey, "half_rate") == 0) {
                                double v; p = jnum(p, &v); z->half_rate = (v != 0);
                            } else if (strcmp(zkey, "key_min") == 0) {
                                double v; p = jnum(p, &v); z->key_min = (int)v;
                            } else if (strcmp(zkey, "key_max") == 0) {
                                double v; p = jnum(p, &v); z->key_max = (int)v;
                            } else if (strcmp(zkey, "wems") == 0 && *p == '[') {
                                p = jskip(p + 1);
                                while (*p && *p != ']') {
                                    double v; p = jnum(p, &v);
                                    if (z->num_wems < MAX_ZONE_WEMS)
                                        z->wems[z->num_wems++] = (uint32_t)v;
                                    p = jskip(p);
                                    if (*p == ',') p = jskip(p + 1);
                                }
                                if (*p == ']') p = jskip(p + 1);
                            } else if (strcmp(zkey, "wem_volumes") == 0 && *p == '{') {
                                /* Parse per-WEM volumes: {"wem_id": db, ...} */
                                p = jskip(p + 1);
                                while (*p && *p != '}') {
                                    char wid_str[32]; double db;
                                    p = jstr(p, wid_str, sizeof(wid_str));
                                    p = jskip(p);
                                    if (*p == ':') p = jskip(p + 1);
                                    p = jnum(p, &db);
                                    /* Match wem_id to wems[] index */
                                    uint32_t wid = (uint32_t)strtoul(wid_str, NULL, 10);
                                    for (int wi = 0; wi < z->num_wems; wi++) {
                                        if (z->wems[wi] == wid) {
                                            z->wem_vol_db[wi] = (float)db;
                                            break;
                                        }
                                    }
                                    p = jskip(p);
                                    if (*p == ',') p = jskip(p + 1);
                                }
                                if (*p == '}') p = jskip(p + 1);
                            } else if (strcmp(zkey, "velocity_wems") == 0 && *p == '[') {
                                /* Parse velocity_wems: [{"wem": id, "vel_min": x, "vel_max": y}, ...] */
                                p = jskip(p + 1);
                                while (*p && *p != ']') {
                                    if (*p != '{') { p = jskip_value(p); goto skip_vwem; }
                                    if (z->num_vel_wems >= MAX_ZONE_WEMS) { p = jskip_value(p); goto skip_vwem; }
                                    p = jskip(p + 1);
                                    VelWem *vw = &z->vel_wems[z->num_vel_wems];
                                    vw->wem = 0; vw->vel_min = 0; vw->vel_max = 127;
                                    while (*p && *p != '}') {
                                        char vk[16];
                                        p = jstr(p, vk, sizeof(vk));
                                        p = jskip(p);
                                        if (*p == ':') p = jskip(p + 1);
                                        double v; p = jnum(p, &v);
                                        if (strcmp(vk, "wem") == 0) vw->wem = (uint32_t)v;
                                        else if (strcmp(vk, "vel_min") == 0) vw->vel_min = (int)v;
                                        else if (strcmp(vk, "vel_max") == 0) vw->vel_max = (int)v;
                                        p = jskip(p);
                                        if (*p == ',') p = jskip(p + 1);
                                    }
                                    if (*p == '}') p = jskip(p + 1);
                                    z->num_vel_wems++;
                                skip_vwem:
                                    p = jskip(p);
                                    if (*p == ',') p = jskip(p + 1);
                                }
                                if (*p == ']') p = jskip(p + 1);
                            } else {
                                p = jskip_value(p);
                            }
                            p = jskip(p);
                            if (*p == ',') p = jskip(p + 1);
                        }
                        if (*p == '}') p = jskip(p + 1);
                        /* keep original order - it's Wwise's interleaved velocity ordering */
                        g_zone_count++;
                    skip_zone:
                        p = jskip(p);
                        if (*p == ',') p = jskip(p + 1);
                    }
                    if (*p == ']') p = jskip(p + 1);
                next_ntype:
                    p = jskip(p);
                    if (*p == ',') p = jskip(p + 1);
                }
                if (*p == '}') p = jskip(p + 1);
            next_inst:
                p = jskip(p);
                if (*p == ',') p = jskip(p + 1);
            }
            if (*p == '}') p = jskip(p + 1);
        } else {
            p = jskip_value(p);
        }
    next_top:
        p = jskip(p);
        if (*p == ',') p = jskip(p + 1);
    }
    } else {
        /* v1 format: top-level object IS the instruments map directly */
        while (*p && *p != '}') {
            char dir_name[80];
            p = jstr(p, dir_name, sizeof(dir_name));
            p = jskip(p);
            if (*p == ':') p = jskip(p + 1);

            /* Value is {ntype_str: [zones]} */
            if (*p != '{') { p = jskip_value(p); goto next_inst_v1; }
            p = jskip(p + 1);

            while (*p && *p != '}') {
                char ntype_str[16];
                p = jstr(p, ntype_str, sizeof(ntype_str));
                int ntype = atoi(ntype_str);
                p = jskip(p);
                if (*p == ':') p = jskip(p + 1);

                /* Value is array of zone objects */
                if (*p != '[') { p = jskip_value(p); goto next_ntype_v1; }
                p = jskip(p + 1);

                while (*p && *p != ']') {
                    if (g_zone_count >= MAX_ZONES) { p = jskip_value(p); goto skip_zone_v1; }

                    KeyZone *z = &g_zones[g_zone_count];
                    snprintf(z->dir_name, sizeof(z->dir_name), "%s", dir_name);
                    z->ntype = ntype;
                    z->key_min = 0;
                    z->key_max = 127;
                    z->root = 60;
                    z->num_wems = 0;
                    memset(z->wem_vol_db, 0, sizeof(z->wem_vol_db));

                    if (*p != '{') { p = jskip_value(p); goto skip_zone_v1; }
                    p = jskip(p + 1);

                    while (*p && *p != '}') {
                        char zkey[32];
                        p = jstr(p, zkey, sizeof(zkey));
                        p = jskip(p);
                        if (*p == ':') p = jskip(p + 1);

                        if (strcmp(zkey, "root") == 0) {
                            double v; p = jnum(p, &v); z->root = (int)v;
                        } else if (strcmp(zkey, "key_min") == 0) {
                            double v; p = jnum(p, &v); z->key_min = (int)v;
                        } else if (strcmp(zkey, "key_max") == 0) {
                            double v; p = jnum(p, &v); z->key_max = (int)v;
                        } else if (strcmp(zkey, "wem_ids") == 0 && *p == '[') {
                            p = jskip(p + 1);
                            while (*p && *p != ']' && z->num_wems < MAX_ZONE_WEMS) {
                                double v; p = jnum(p, &v);
                                z->wems[z->num_wems++] = (uint32_t)v;
                                p = jskip(p);
                                if (*p == ',') p = jskip(p + 1);
                            }
                            if (*p == ']') p = jskip(p + 1);
                        } else {
                            p = jskip_value(p);
                        }
                        p = jskip(p);
                        if (*p == ',') p = jskip(p + 1);
                    }
                    if (*p == '}') p = jskip(p + 1);
                    /* Keep original JSON order */
                    g_zone_count++;
                skip_zone_v1:
                    p = jskip(p);
                    if (*p == ',') p = jskip(p + 1);
                }
                if (*p == ']') p = jskip(p + 1);
            next_ntype_v1:
                p = jskip(p);
                if (*p == ',') p = jskip(p + 1);
            }
            if (*p == '}') p = jskip(p + 1);
        next_inst_v1:
            p = jskip(p);
            if (*p == ',') p = jskip(p + 1);
        }
    }

    free(json);

    /* sort zones for fast lookup */
    qsort(g_zones, g_zone_count, sizeof(KeyZone), zone_cmp_fn);

    /* fix up unset key_max values - if key_max is still 127 and there's a zone
       above it for the same instrument, cap it at next_zone.key_min - 1 */
    for (int i = 0; i < g_zone_count - 1; i++) {
        KeyZone *cur = &g_zones[i];
        KeyZone *next = &g_zones[i + 1];
        if (cur->key_max == 127 &&
            strcmp(cur->dir_name, next->dir_name) == 0 &&
            cur->ntype == next->ntype &&
            next->key_min > cur->key_min) {
            cur->key_max = next->key_min - 1;
        }
    }

    SDL_Log("Loaded %d key zones, %d instrument volumes", g_zone_count, g_inst_vol_count);
}

/* Loop points - extracted from Wwise BNK HIRC objects. These tell us where
   sustained instruments should loop back to keep playing. */

#define MAX_LOOP_ENTRIES 4096

typedef struct {
    char     dir[80];
    uint32_t wem_id;
    int      loop_start;  /* in original sample frames */
    int      loop_end;
} LoopEntry;

static LoopEntry g_loops[MAX_LOOP_ENTRIES];
static int       g_loop_count;

/* find loop points for a specific sample */
static bool find_loop_points(const char *dir, uint32_t wem_id, int *ls, int *le) {
    for (int i = 0; i < g_loop_count; i++) {
        if (g_loops[i].wem_id == wem_id && strcmp(g_loops[i].dir, dir) == 0) {
            *ls = g_loops[i].loop_start;
            *le = g_loops[i].loop_end;
            return true;
        }
    }
    return false;
}

/* parse loop_points.json */
static void load_loop_points(void) {
    g_loop_count = 0;
    if (g_samples_dir[0] == 0) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s/loop_points.json", g_samples_dir);
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_Log("loop_points.json not found"); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json = malloc(sz + 1);
    fread(json, 1, sz, f);
    json[sz] = 0;
    fclose(f);

    const char *p = jskip(json);
    if (*p != '{') { free(json); return; }
    p = jskip(p + 1);

    while (*p && *p != '}') {
        char dir_name[80];
        p = jstr(p, dir_name, sizeof(dir_name));
        p = jskip(p);
        if (*p == ':') p = jskip(p + 1);

        if (*p != '{') { p = jskip_value(p); goto lp_next; }
        p = jskip(p + 1);

        while (*p && *p != '}') {
            char wid_str[32];
            p = jstr(p, wid_str, sizeof(wid_str));
            p = jskip(p);
            if (*p == ':') p = jskip(p + 1);

            /* Value is [loop_start, loop_end] */
            if (*p == '[' && g_loop_count < MAX_LOOP_ENTRIES) {
                p = jskip(p + 1);
                double v1 = 0, v2 = 0;
                p = jnum(p, &v1);
                p = jskip(p);
                if (*p == ',') p = jskip(p + 1);
                p = jnum(p, &v2);
                p = jskip(p);
                if (*p == ']') p = jskip(p + 1);

                LoopEntry *lp = &g_loops[g_loop_count];
                snprintf(lp->dir, sizeof(lp->dir), "%s", dir_name);
                lp->wem_id = (uint32_t)strtoul(wid_str, NULL, 10);
                lp->loop_start = (int)v1;
                lp->loop_end = (int)v2;
                g_loop_count++;
            } else {
                p = jskip_value(p);
            }
            p = jskip(p);
            if (*p == ',') p = jskip(p + 1);
        }
        if (*p == '}') p = jskip(p + 1);
    lp_next:
        p = jskip(p);
        if (*p == ',') p = jskip(p + 1);
    }

    free(json);
    SDL_Log("Loaded %d loop point entries", g_loop_count);
}

/* Audio file loading via libsndfile (dlopen'd so it's optional).
   We define the types ourselves to avoid needing the sndfile header. */
typedef void*   SNDFILE;
typedef int64_t sf_count_t;
typedef struct {
    sf_count_t frames;
    int        samplerate;
    int        channels;
    int        format;
    int        sections;
    int        seekable;
} SF_INFO;
#define SFM_READ 0x10

/* dlopen'd function pointers - loaded at runtime */
static SNDFILE*    (*p_sf_open)(const char*, int, SF_INFO*);
static sf_count_t  (*p_sf_readf_float)(SNDFILE*, float*, sf_count_t);
static int         (*p_sf_close)(SNDFILE*);
static void        *g_sndfile_lib;

static bool sndfile_init(void) {
    if (g_sndfile_lib) return true;
#ifdef _WIN32
    g_sndfile_lib = (void*)LoadLibraryA("sndfile.dll");
    if (!g_sndfile_lib) g_sndfile_lib = (void*)LoadLibraryA("libsndfile-1.dll");
    if (!g_sndfile_lib) {
        SDL_Log("Cannot load sndfile.dll");
        return false;
    }
    HMODULE h = (HMODULE)g_sndfile_lib;
    p_sf_open        = (void*)GetProcAddress(h, "sf_open");
    p_sf_readf_float = (void*)GetProcAddress(h, "sf_readf_float");
    p_sf_close       = (void*)GetProcAddress(h, "sf_close");
    if (!p_sf_open || !p_sf_readf_float || !p_sf_close) {
        SDL_Log("libsndfile symbols not found");
        FreeLibrary(h);
        g_sndfile_lib = NULL;
        return false;
    }
#else
    g_sndfile_lib = dlopen("libsndfile.so.1", RTLD_LAZY);
    if (!g_sndfile_lib) {
        SDL_Log("Cannot load libsndfile.so.1: %s", dlerror());
        return false;
    }
    p_sf_open       = dlsym(g_sndfile_lib, "sf_open");
    p_sf_readf_float = dlsym(g_sndfile_lib, "sf_readf_float");
    p_sf_close      = dlsym(g_sndfile_lib, "sf_close");
    if (!p_sf_open || !p_sf_readf_float || !p_sf_close) {
        SDL_Log("libsndfile symbols not found");
        dlclose(g_sndfile_lib);
        g_sndfile_lib = NULL;
        return false;
    }
#endif
    return true;
}

/* Catmull-Rom cubic resampler. Much better than linear for pitch-shifting
   samples, and not too expensive. The BDO samples come in various sample rates
   so we need this for basically everything. */
static float *resample_cubic(const float *src, int src_frames, int src_sr,
                              int *out_frames) {
    if (src_sr == SAMPLE_RATE) {
        *out_frames = src_frames;
        float *copy = malloc(sizeof(float) * 2 * src_frames);
        memcpy(copy, src, sizeof(float) * 2 * src_frames);
        return copy;
    }
    double ratio = (double)SAMPLE_RATE / src_sr;
    int dst_frames = (int)(src_frames * ratio) + 1;
    float *dst = malloc(sizeof(float) * 2 * dst_frames);
    int written = 0;
    for (int i = 0; i < dst_frames; i++) {
        double src_pos = i / ratio;
        int idx = (int)src_pos;
        float t = (float)(src_pos - idx);
        /* Catmull-Rom needs 4 points: p0, p1, p2, p3 */
        int i0 = idx - 1; if (i0 < 0) i0 = 0;
        int i1 = idx;      if (i1 >= src_frames) i1 = src_frames - 1;
        int i2 = idx + 1;  if (i2 >= src_frames) i2 = src_frames - 1;
        int i3 = idx + 2;  if (i3 >= src_frames) i3 = src_frames - 1;
        for (int ch = 0; ch < 2; ch++) {
            float p0 = src[i0 * 2 + ch];
            float p1 = src[i1 * 2 + ch];
            float p2 = src[i2 * 2 + ch];
            float p3 = src[i3 * 2 + ch];
            /* Catmull-Rom: 0.5 * ((2*p1) + (-p0+p2)*t + (2*p0-5*p1+4*p2-p3)*t^2 + (-p0+3*p1-3*p2+p3)*t^3) */
            float t2 = t * t, t3 = t2 * t;
            dst[i * 2 + ch] = 0.5f * ((2.0f * p1) +
                (-p0 + p2) * t +
                (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
        }
        written++;
    }
    *out_frames = written;
    return dst;
}

/* convert decoded audio to stereo float and resample to our output rate */
static bool finalize_sample(float *raw, int ch, int frames, int samplerate, SampleData *out) {
    if (frames <= 0 || ch <= 0) { free(raw); return false; }

    float *stereo;
    if (ch == 1) {
        stereo = malloc(sizeof(float) * 2 * frames);
        for (int i = 0; i < frames; i++) {
            stereo[i * 2 + 0] = raw[i];
            stereo[i * 2 + 1] = raw[i];
        }
        free(raw);
    } else if (ch == 2) {
        stereo = raw;
    } else {
        stereo = malloc(sizeof(float) * 2 * frames);
        for (int i = 0; i < frames; i++) {
            stereo[i * 2 + 0] = raw[i * ch + 0];
            stereo[i * 2 + 1] = (ch > 1) ? raw[i * ch + 1] : raw[i * ch + 0];
        }
        free(raw);
    }

    out->orig_sr = samplerate;

    if (samplerate != SAMPLE_RATE) {
        int resampled_frames;
        float *resampled = resample_cubic(stereo, frames, samplerate, &resampled_frames);
        free(stereo);
        out->data = resampled;
        out->frames = resampled_frames;
    } else {
        out->data = stereo;
        out->frames = frames;
    }
    return true;
}

/* simple WAV PCM reader - no external deps */
static bool load_sample_wav(const char *path, SampleData *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) < 44) { fclose(f); return false; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return false; }

    /* find fmt and data chunks */
    fseek(f, 12, SEEK_SET);
    int ch = 0, sr = 0, bps = 0, fmt_tag = 0;
    long data_start = 0;
    uint32_t data_size = 0;
    uint8_t chunk_hdr[8];
    while (fread(chunk_hdr, 1, 8, f) == 8) {
        uint32_t tag = chunk_hdr[0]|(chunk_hdr[1]<<8)|(chunk_hdr[2]<<16)|(chunk_hdr[3]<<24);
        uint32_t sz = chunk_hdr[4]|(chunk_hdr[5]<<8)|(chunk_hdr[6]<<16)|(chunk_hdr[7]<<24);
        if (tag == 0x20746D66) { /* 'fmt ' */
            uint8_t fmt[40] = {0};
            size_t rd = sz < 40 ? sz : 40;
            fread(fmt, 1, rd, f);
            fseek(f, (long)sz - (long)rd, SEEK_CUR);
            fmt_tag = fmt[0]|(fmt[1]<<8);
            ch = fmt[2]|(fmt[3]<<8);
            sr = fmt[4]|(fmt[5]<<8)|(fmt[6]<<16)|(fmt[7]<<24);
            bps = fmt[14]|(fmt[15]<<8);
        } else if (tag == 0x61746164) { /* 'data' */
            data_start = ftell(f);
            data_size = sz;
            break;
        } else {
            fseek(f, (long)sz, SEEK_CUR);
        }
    }
    /* only handle PCM (1) and IEEE float (3) */
    if ((fmt_tag != 1 && fmt_tag != 3) || ch == 0 || sr == 0 || data_start == 0) {
        fclose(f); return false;
    }
    int sample_bytes = bps / 8;
    int frames = (int)(data_size / (ch * sample_bytes));
    if (frames <= 0) { fclose(f); return false; }

    float *raw = malloc(sizeof(float) * ch * frames);
    fseek(f, data_start, SEEK_SET);
    if (fmt_tag == 3 && bps == 32) {
        fread(raw, sizeof(float), ch * frames, f);
    } else if (fmt_tag == 1 && bps == 16) {
        int16_t *tmp = malloc(sizeof(int16_t) * ch * frames);
        fread(tmp, sizeof(int16_t), ch * frames, f);
        for (int i = 0; i < ch * frames; i++) raw[i] = tmp[i] / 32768.0f;
        free(tmp);
    } else if (fmt_tag == 1 && bps == 24) {
        uint8_t *tmp = malloc(3 * ch * frames);
        fread(tmp, 3, ch * frames, f);
        for (int i = 0; i < ch * frames; i++) {
            int32_t s = (tmp[i*3+2]<<24)|(tmp[i*3+1]<<16)|(tmp[i*3]<<8);
            raw[i] = s / 2147483648.0f;
        }
        free(tmp);
    } else {
        free(raw); fclose(f); return false;
    }
    fclose(f);
    return finalize_sample(raw, ch, frames, sr, out);
}

/* try loading with stb_vorbis (built-in, no deps) */
static bool load_sample_vorbis(const char *path, SampleData *out) {
    int ch, sr;
    short *raw_short = NULL;
    int frames = stb_vorbis_decode_filename(path, &ch, &sr, &raw_short);
    if (frames <= 0 || !raw_short) { free(raw_short); return false; }

    /* Convert short to float */
    float *raw = malloc(sizeof(float) * ch * frames);
    for (int i = 0; i < ch * frames; i++)
        raw[i] = raw_short[i] / 32768.0f;
    free(raw_short);

    return finalize_sample(raw, ch, frames, sr, out);
}

static bool load_sample_file(const char *path, SampleData *out) {
    /* try libsndfile first (handles wav + more) */
    if (sndfile_init()) {
        SF_INFO info = {0};
        SNDFILE *sf = p_sf_open(path, SFM_READ, &info);
        if (sf) {
            int ch = info.channels;
            int frames = (int)info.frames;
            if (frames > 0 && ch > 0) {
                float *raw = malloc(sizeof(float) * ch * frames);
                sf_count_t rd = p_sf_readf_float(sf, raw, frames);
                p_sf_close(sf);
                if (rd > 0)
                    return finalize_sample(raw, ch, (int)rd, info.samplerate, out);
                free(raw);
            } else {
                p_sf_close(sf);
            }
        }
    }
    /* try built-in WAV reader */
    if (load_sample_wav(path, out)) return true;
    /* try stb_vorbis for .ogg */
    return load_sample_vorbis(path, out);
}

/* Find the best zone for a given instrument + pitch + ntype.
   Picks the narrowest zone containing the pitch, with root-distance tiebreak.
   Falls back to ntype 0 (sustain) then 99 (drum) if exact match fails. */
static int find_zone_for_ntype(const char *dir_name, int pitch, int ntype) {
    /* narrowest zone containing the pitch wins, root distance breaks ties */
    int best_match = -1, best_range = 99999, best_root_dist = 99999;
    int best_near = -1, best_near_dist = 999;
    for (int i = 0; i < g_zone_count; i++) {
        KeyZone *z = &g_zones[i];
        if (strcmp(z->dir_name, dir_name) != 0 || z->ntype != ntype) continue;
        if (pitch >= z->key_min && pitch <= z->key_max) {
            int range = z->key_max - z->key_min;
            int rdist = abs(z->root - pitch);
            if (range < best_range || (range == best_range && rdist < best_root_dist)) {
                best_range = range;
                best_root_dist = rdist;
                best_match = i;
            }
        }
        int d = (pitch < z->key_min) ? z->key_min - pitch : pitch - z->key_max;
        if (d < best_near_dist) { best_near_dist = d; best_near = i; }
    }
    return best_match >= 0 ? best_match : best_near;
}

static int find_zone(const char *dir_name, int pitch, int ntype) {
    int zi = find_zone_for_ntype(dir_name, pitch, ntype);
    if (zi >= 0) return zi;
    /* Fallback: ntype 0 (sustain) */
    if (ntype != 0) {
        zi = find_zone_for_ntype(dir_name, pitch, 0);
        if (zi >= 0) return zi;
    }
    /* Fallback: ntype 99 (drum) */
    if (ntype != 99) {
        zi = find_zone_for_ntype(dir_name, pitch, 99);
        if (zi >= 0) return zi;
    }
    return -1;
}

/* select velocity layer from zone's WEM list.
   BDO uses 3 velocity tiers: soft (1-99), medium (100-120), hard (121-127).
   Took a lot of A/B testing against the game to get these thresholds right. */
static int select_vel_layer(int num_wems, int velocity) {
    /* WEMs are in sequential velocity groups, divide into 3 tiers */
    if (num_wems <= 1) return 0;
    if (num_wems == 2) return (velocity < 100) ? 0 : 1;

    int tier;
    if (velocity < 100)       tier = 0; /* soft: 1-99 */
    else if (velocity <= 120) tier = 1; /* medium: 100-120 */
    else                      tier = 2; /* hard */

    /* map tier to the right third of the WEM list */
    int idx = (num_wems * tier) / 3;
    if (idx >= num_wems) idx = num_wems - 1;
    return idx;
}

/* LRU cache eviction */
static void cache_evict_lru(void) {
    /* Find least-recently-used entry */
    int lru = 0;
    for (int i = 1; i < g_cache_count; i++)
        if (g_cache[i].last_used < g_cache[lru].last_used) lru = i;
    /* Free sample data */
    size_t freed = (size_t)g_cache[lru].sample.frames * 2 * sizeof(float);
    free(g_cache[lru].sample.data);
    g_cache_bytes -= freed;
    /* Move last entry into the gap */
    if (lru != g_cache_count - 1)
        g_cache[lru] = g_cache[g_cache_count - 1];
    g_cache_count--;
}

static CachedSample *load_wem(const char *dir_name, uint32_t wem_id, int root_pitch) {
    /* Check if already cached */
    g_cache_tick++;
    for (int i = 0; i < g_cache_count; i++) {
        if (g_cache[i].wem_id == wem_id && strcmp(g_cache[i].dir_name, dir_name) == 0) {
            g_cache[i].last_used = g_cache_tick;
            return &g_cache[i];
        }
    }

    /* Evict until we have room (slot count and memory budget) */
    while (g_cache_count >= MAX_CACHED ||
           (g_cache_count > 0 && g_cache_bytes > CACHE_MEM_LIMIT))
        cache_evict_lru();

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%u.ogg", g_samples_dir, dir_name, wem_id);

    CachedSample *cs = &g_cache[g_cache_count];
    snprintf(cs->dir_name, sizeof(cs->dir_name), "%s", dir_name);
    cs->wem_id = wem_id;
    cs->root_pitch = root_pitch;

    if (!load_sample_file(path, &cs->sample)) {
        /* Try .wav extension */
        snprintf(path, sizeof(path), "%s/%s/%u.wav", g_samples_dir, dir_name, wem_id);
        if (!load_sample_file(path, &cs->sample)) return NULL;
    }

    cs->actual_orig_sr = cs->sample.orig_sr;
    cs->last_used = g_cache_tick;
    g_cache_bytes += (size_t)cs->sample.frames * 2 * sizeof(float);
    g_cache_count++;
    return cs;
}

/* main sample lookup: inst_id + pitch + vel + ntype -> loaded sample.
   Returns NULL sample if nothing found (falls back to synth waveform). */
typedef struct {
    SampleData *sample;
    int         root_pitch;
    float       vol_db;       /* combined inst + wem volume offset */
    bool        half_rate;    /* true = apply 0.5x rate correction */
    bool        has_loop;
    int         loop_start;   /* in resampled (44100) frames */
    int         loop_end;     /* in resampled (44100) frames */
} SampleResult;

static SampleResult find_or_load_sample(uint8_t inst_id, int pitch, int velocity,
                                        uint8_t ntype, uint8_t synth_profile) {
    SampleResult result = { NULL, 60, 0.0f, false, 0, 0 };

    const char *dir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
    if (!dir || g_samples_dir[0] == 0) return result;

    int zi = find_zone(dir, pitch, ntype);
    if (zi < 0) return result;

    KeyZone *z = &g_zones[zi];
    uint32_t wem_id;
    int vel_idx = 0;

    if (z->num_vel_wems > 0) {
        /* Collect all matching WEMs for this velocity */
        uint32_t matches[MAX_ZONE_WEMS];
        int n_matches = 0;
        for (int i = 0; i < z->num_vel_wems; i++) {
            if (velocity >= z->vel_wems[i].vel_min && velocity <= z->vel_wems[i].vel_max) {
                matches[n_matches++] = z->vel_wems[i].wem;
            }
        }
        if (n_matches > 1) {
            /* Round-robin: alternate per pitch+tier combination */
            static uint8_t rr_counter[128][3]; /* [pitch][tier] */
            int pi = pitch & 127;
            int tier = (velocity < 100) ? 0 : (velocity <= 120) ? 1 : 2;
            int pick = rr_counter[pi][tier] % n_matches;
            rr_counter[pi][tier]++;
            wem_id = matches[pick];
        } else if (n_matches == 1) {
            wem_id = matches[0];
        } else {
            wem_id = z->vel_wems[0].wem; /* fallback */
        }
        /* Find matching wem_vol_db from the wems[] array */
        for (int w = 0; w < z->num_wems; w++) {
            if (z->wems[w] == wem_id) {
                vel_idx = w;
                break;
            }
        }
    } else {
        /* legacy path: split into sustained (looped) and transient (one-shot) */
        uint32_t sustained[MAX_ZONE_WEMS], transient[MAX_ZONE_WEMS];
        int sus_idx[MAX_ZONE_WEMS], trans_idx[MAX_ZONE_WEMS];
        int n_sus = 0, n_trans = 0;
        for (int w = 0; w < z->num_wems; w++) {
            int ls, le;
            bool has_loop = find_loop_points(dir, z->wems[w], &ls, &le);
            if (has_loop) {
                sus_idx[n_sus] = w;
                sustained[n_sus++] = z->wems[w];
            } else {
                trans_idx[n_trans] = w;
                transient[n_trans++] = z->wems[w];
            }
        }

        if (n_sus > 0) {
            int idx = select_vel_layer(n_sus, velocity);
            wem_id = sustained[idx];
            vel_idx = sus_idx[idx];
        } else if (n_trans > 0) {
            int idx = select_vel_layer(n_trans, velocity);
            wem_id = transient[idx];
            vel_idx = trans_idx[idx];
        } else {
            vel_idx = 0;
            wem_id = z->wems[0];
        }
    }

    CachedSample *cs = load_wem(dir, wem_id, z->root);
    if (!cs) return result;

    result.sample = &cs->sample;
    result.root_pitch = z->root;
    result.half_rate = z->half_rate;

    /* combine instrument base volume + per-WEM dB offset from BNK */
    result.vol_db = inst_volume_db(dir) + z->wem_vol_db[vel_idx];

    /* look up loop points and convert from original SR to our output rate */
    int ls_orig, le_orig;
    if (find_loop_points(dir, wem_id, &ls_orig, &le_orig)) {
        double sr_ratio = (double)SAMPLE_RATE / (double)cs->actual_orig_sr;
        result.has_loop = true;
        result.loop_start = (int)(ls_orig * sr_ratio);
        result.loop_end   = (int)(le_orig * sr_ratio);
        /* Clamp to sample bounds */
        if (result.loop_start < 0) result.loop_start = 0;
        if (result.loop_end > cs->sample.frames) result.loop_end = cs->sample.frames;
        if (result.loop_end <= result.loop_start) result.has_loop = false;
    }

    return result;
}

/* Voice - a single playing note with per-instrument release, LFO, envelopes.
   Parameters derived from Wwise HIRC objects. */

#define LOOP_XFADE 512 /* crossfade frames at loop boundaries */

typedef struct {
    bool    active;

    /* Sample data (not owned) */
    float  *data;
    int     total_frames;  /* frames in the loaded sample (at SAMPLE_RATE) */

    /* Playback position */
    double  frac_pos;      /* fractional position in sample */
    double  rate;          /* playback rate for pitch shift */
    int     pos;           /* output frame counter */

    /* Volume */
    float   volume;        /* (vel/127)^2 * db_gain * layer_vol */

    /* Note duration */
    int     note_frames;   /* note-on duration in output frames */

    /* Loop info */
    bool    is_looped;
    int     loop_start;    /* in sample frames (at SAMPLE_RATE) */
    int     loop_end;
    int     loop_len;

    /* Loop crossfade state */
    double  xfade_old_pos; /* secondary read position during crossfade */
    int     xfade_remain;  /* output frames remaining in crossfade */
    int     xfade_total;   /* total crossfade length */

    /* Release state */
    bool    releasing;
    int     release_pos;   /* output frames since release started */
    int     release_frames;/* total release duration (output frames) */
    int     fade_frames;   /* fade portion within release */

    /* Synth waveform (data == NULL when active) */
    double  sine_freq;     /* 0 if sample-based */
    double  synth_phase;   /* accumulated phase for L channel (0.0-1.0) */
    double  synth_phase_r; /* accumulated phase for R channel (stereo detune) */
    int     sine_attack;   /* attack ramp frames */
    int     waveform;      /* 0=sine, 1=saw, 2=square, 3=triangle */
    bool    stereo_detune; /* true for "stereo" profiles */
    float   haas_buf[32];  /* tiny delay buffer for Haas stereo (0.6ms at 48kHz) */
    int     haas_pos;      /* write position in delay buffer */
    float   pitch_bend;    /* semitones to bend at onset (e.g. -1.0 for Tab) */
    int     bend_frames;   /* frames over which bend decays to 0 */

    /* Synth technique modulation */
    float   env_attack;       /* envelope attack time in frames */
    float   env_hold;         /* hold at full volume before decay (frames) */
    float   env_decay;        /* envelope decay time in frames (0 = infinite) */
    float   env_sustain;      /* sustain level 0.0-1.0 */
    float   env_release_t;    /* envelope release time in frames */
    float   pitch_env_depth;  /* pitch envelope depth in cents (+ or -) */
    float   pitch_env_attack; /* pitch envelope attack frames */
    float   pitch_env_decay;  /* pitch envelope decay frames */
    int     lfo_type;         /* 0=none, 1=vol_triangle, 2=pitch_square, 3=pitch_sine */
    float   lfo_freq;         /* LFO frequency in Hz */
    float   lfo_depth;        /* LFO depth (vol: 0-1 amp mod, pitch: cents) */
    float   lfo_attack;       /* LFO fade-in time in frames */
    float   vol_offset_db;    /* additional volume offset in dB */
    float   pitch_offset;     /* permanent pitch offset in cents */

    /* Per-voice FX sends (from layer, 0.0-1.0) */
    float   rev_send;
    float   dly_send;
    float   cho_send;

    /* Source tracking  - for live edit detection */
    int     src_layer;       /* layer index */
    uint8_t src_inst_id;     /* instrument at trigger time */
    int     src_pitch;
    int     src_vel;
    double  src_note_start;  /* note start ms */
    double  src_note_dur;    /* note duration ms */
    uint8_t src_ntype;
    uint8_t src_synth_prof;

    /* For live volume updates */
    float   src_layer_vol;   /* layer_vol at trigger time */
    float   src_base_vol;    /* volume without layer_vol */

    /* Crossfade-out (when replaced by re-trigger) */
    float   fade_mult;       /* 1.0 = full, decreasing to 0 = kill */
    bool    fading_out;

    /* LPF state (1-pole low-pass filter, velocity-controlled) */
    float   lpf_coeff;       /* 0.0 = no filtering, >0 = filter active */
    float   lpf_z1_l;        /* filter state left */
    float   lpf_z1_r;        /* filter state right */
} Voice;

static Voice g_voices[MAX_VOICES];

/* playback state */

typedef struct {
    bool        playing;
    MuseProject *project;
    double      start_ms;
    double      position_ms;
    double      prev_tick_ms;  /* previous tick time for window detection */
} PlaybackState;

static PlaybackState g_pb;
static ma_device     g_device;
static bool          g_initialized;

/* FX processing
   ============
   Reverb is Jezar's Freeverb, tuned to match BDO's in-game reverb.
   Recorded the game output, analyzed it in Audacity: ~3s RT60, heavy HF damping,
   fully decorrelated stereo (correlation 0.048), very wide. The game uses Wwise
   RoomVerb but Freeverb gets close enough with the right damping values.
   8 parallel combs + 4 allpass per channel, 1-pole LPF in each comb for HF. */
#define FV_NUM_COMBS    8
#define FV_NUM_ALLPASS  4
#define FV_STEREO_SPREAD 23  /* sample offset between L and R comb delays */

/* scale factor from 44100 (Freeverb reference rate) to 48000 */
#define FV_SCALE_RATE (48000.0f / 44100.0f)

/* buffer sizes after scaling (rounded up) */
#define FV_COMB_MAX   1800
#define FV_AP_MAX      700

typedef struct {
    float buf[FV_COMB_MAX];
    float filterstore;  /* 1-pole LPF state */
    int   len, pos;
} FVComb;

typedef struct {
    float buf[FV_AP_MAX];
    int   len, pos;
} FVAllpass;

typedef struct {
    FVComb    comb_l[FV_NUM_COMBS];
    FVComb    comb_r[FV_NUM_COMBS];
    FVAllpass ap_l[FV_NUM_ALLPASS];
    FVAllpass ap_r[FV_NUM_ALLPASS];
    bool      initialized;
} ReverbState;

static ReverbState g_reverb;

static void reverb_init(ReverbState *rv) {
    /* Freeverb standard comb delays at 44100Hz */
    const int comb_delays_44k[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    /* allpass delays */
    const int ap_delays_44k[] = {556, 441, 341, 225};

    for (int i = 0; i < FV_NUM_COMBS; i++) {
        int base = (int)(comb_delays_44k[i] * FV_SCALE_RATE + 0.5f);
        int offset = (int)(FV_STEREO_SPREAD * FV_SCALE_RATE + 0.5f);

        rv->comb_l[i].len = base;
        rv->comb_l[i].pos = 0;
        rv->comb_l[i].filterstore = 0.0f;
        memset(rv->comb_l[i].buf, 0, sizeof(rv->comb_l[i].buf));

        rv->comb_r[i].len = base + offset;
        rv->comb_r[i].pos = 0;
        rv->comb_r[i].filterstore = 0.0f;
        memset(rv->comb_r[i].buf, 0, sizeof(rv->comb_r[i].buf));
    }

    for (int i = 0; i < FV_NUM_ALLPASS; i++) {
        int base = (int)(ap_delays_44k[i] * FV_SCALE_RATE + 0.5f);
        int offset = (int)(FV_STEREO_SPREAD * FV_SCALE_RATE + 0.5f);

        rv->ap_l[i].len = base;
        rv->ap_l[i].pos = 0;
        memset(rv->ap_l[i].buf, 0, sizeof(rv->ap_l[i].buf));

        rv->ap_r[i].len = base + offset;
        rv->ap_r[i].pos = 0;
        memset(rv->ap_r[i].buf, 0, sizeof(rv->ap_r[i].buf));
    }

    rv->initialized = true;
}

static void reverb_process(ReverbState *rv, float in_l, float in_r,
                           float feedback, float *out_l, float *out_r) {
    if (!rv->initialized) reverb_init(rv);

    /* feedback ~0.85-0.95 for 3s RT60, damp 0.4 for heavy HF rolloff,
       width 1.0 for full stereo spread (matches game recording) */
    const float damp1 = 0.4f;          /* HF damping amount */
    const float damp2 = 1.0f - damp1;  /* complement */
    const float width = 1.0f;          /* stereo width */
    const float wet1 = width * 0.5f + 0.5f;   /* wet signal scaling (width=1 -> wet1=1.0) */
    const float wet2 = (1.0f - width) * 0.5f;  /* cross-mix (width=1 -> wet2=0.0) */

    /* attenuate input to prevent feedback buildup */
    const float gain = 0.015f;
    float input_l = in_l * gain;
    float input_r = in_r * gain;

    float out_comb_l = 0.0f;
    float out_comb_r = 0.0f;

    /* parallel comb filters with 1-pole LPF for HF damping */
    for (int i = 0; i < FV_NUM_COMBS; i++) {
        /* Left channel comb */
        {
            FVComb *c = &rv->comb_l[i];
            float output = c->buf[c->pos];
            /* 1-pole lowpass filter (HF damping) */
            c->filterstore = output * damp2 + c->filterstore * damp1;
            /* Write back with feedback and new input */
            c->buf[c->pos] = input_l + c->filterstore * feedback;
            c->pos = (c->pos + 1) % c->len;
            out_comb_l += output;
        }
        /* Right channel comb */
        {
            FVComb *c = &rv->comb_r[i];
            float output = c->buf[c->pos];
            c->filterstore = output * damp2 + c->filterstore * damp1;
            c->buf[c->pos] = input_r + c->filterstore * feedback;
            c->pos = (c->pos + 1) % c->len;
            out_comb_r += output;
        }
    }

    /* cascaded allpass for diffusion */
    float ap_out_l = out_comb_l;
    float ap_out_r = out_comb_r;
    const float ap_feedback = 0.5f; /* Jezar's default allpass feedback */

    for (int i = 0; i < FV_NUM_ALLPASS; i++) {
        /* Left allpass */
        {
            FVAllpass *a = &rv->ap_l[i];
            float bufout = a->buf[a->pos];
            float ap_in = ap_out_l;
            a->buf[a->pos] = ap_in + bufout * ap_feedback;
            a->pos = (a->pos + 1) % a->len;
            ap_out_l = bufout - ap_in * ap_feedback;
        }
        /* Right allpass */
        {
            FVAllpass *a = &rv->ap_r[i];
            float bufout = a->buf[a->pos];
            float ap_in = ap_out_r;
            a->buf[a->pos] = ap_in + bufout * ap_feedback;
            a->pos = (a->pos + 1) % a->len;
            ap_out_r = bufout - ap_in * ap_feedback;
        }
    }

    /* Stereo width mixing */
    *out_l = ap_out_l * wet1 + ap_out_r * wet2;
    *out_r = ap_out_r * wet1 + ap_out_l * wet2;
}

/* Stereo delay - L 500ms, R 250ms (measured from in-game audio) */
#define MAX_DELAY_BUF (SAMPLE_RATE * 2) /* 2s max */
typedef struct {
    float buf_l[MAX_DELAY_BUF];
    float buf_r[MAX_DELAY_BUF];
    int   pos;
    bool  initialized;
} DelayState;

static DelayState g_delay;

static void delay_init(DelayState *d) {
    memset(d->buf_l, 0, sizeof(d->buf_l));
    memset(d->buf_r, 0, sizeof(d->buf_r));
    d->pos = 0;
    d->initialized = true;
}

/* process one sample through the stereo delay */
static void delay_process(DelayState *d, float input, float feedback, float wet,
                          float *add_l, float *add_r) {
    if (!d->initialized) delay_init(d);
    int delay_l = (int)(0.500f * SAMPLE_RATE); /* L: 500ms */
    int delay_r = (int)(0.250f * SAMPLE_RATE); /* R: 250ms (twice as fast) */
    if (delay_l >= MAX_DELAY_BUF) delay_l = MAX_DELAY_BUF - 1;

    /* Read each channel's delayed output independently */
    float del_l = d->buf_l[(d->pos - delay_l + MAX_DELAY_BUF) % MAX_DELAY_BUF];
    float del_r = d->buf_r[(d->pos - delay_r + MAX_DELAY_BUF) % MAX_DELAY_BUF];

    /* Each channel gets the same input and its own feedback */
    d->buf_l[d->pos] = input + del_l * feedback;
    d->buf_r[d->pos] = input + del_r * feedback;

    d->pos = (d->pos + 1) % MAX_DELAY_BUF;

    *add_l = del_l * wet;
    *add_r = del_r * wet;
}

/* chorus - basic LFO-modulated delay */
#define MAX_CHORUS_BUF 2048
typedef struct {
    float buf[MAX_CHORUS_BUF];
    int   pos;
    double phase;
    bool initialized;
} ChorusState;

static ChorusState g_chorus;

/* global FX param pointers (set by app on init) */
static int *g_fx_reverb, *g_fx_delay, *g_fx_chorus_fb;
static int *g_fx_chorus_depth, *g_fx_chorus_freq;

void muse_audio_set_fx_params(int *reverb, int *delay, int *chorus_fb,
                               int *chorus_depth, int *chorus_freq) {
    g_fx_reverb = reverb;
    g_fx_delay = delay;
    g_fx_chorus_fb = chorus_fb;
    g_fx_chorus_depth = chorus_depth;
    g_fx_chorus_freq = chorus_freq;
}

static void chorus_init(ChorusState *c) {
    memset(c->buf, 0, sizeof(c->buf));
    c->pos = 0;
    c->phase = 0;
    c->initialized = true;
}

static float chorus_process(ChorusState *c, float input, float depth, float freq, float wet) {
    if (!c->initialized) chorus_init(c);
    float lfo_freq = 0.1f + (freq / 100.0f) * 3.0f;
    float max_delay = 0.003f + (depth / 100.0f) * 0.012f;
    float lfo = sinf((float)(2.0 * 3.14159265 * c->phase));
    c->phase += lfo_freq / SAMPLE_RATE;
    if (c->phase > 1.0) c->phase -= 1.0;

    float delay_samples = (0.5f + 0.5f * lfo) * max_delay * SAMPLE_RATE;
    int di = (int)delay_samples;
    if (di >= MAX_CHORUS_BUF - 1) di = MAX_CHORUS_BUF - 2;
    int idx = (c->pos - di + MAX_CHORUS_BUF * 2) % MAX_CHORUS_BUF;
    float out = c->buf[idx];

    c->buf[c->pos] = input;
    c->pos = (c->pos + 1) % MAX_CHORUS_BUF;
    return out * wet;
}

/* grab a free voice slot, steal the oldest if all taken */
static Voice *alloc_voice(void) {
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!g_voices[i].active) return &g_voices[i];
    }
    /* Steal oldest (highest pos = most frames rendered) */
    int oldest = 0;
    for (int i = 1; i < MAX_VOICES; i++) {
        if (g_voices[i].pos > g_voices[oldest].pos) oldest = i;
    }
    return &g_voices[oldest];
}

/* trigger a note - this is the big one that sets up everything for a voice */
static Voice *trigger_note_ex(uint8_t inst_id, int pitch, int velocity,
                               double duration_ms, uint8_t ntype,
                               float layer_vol, float rev_send, float dly_send, float cho_send,
                               uint8_t synth_profile) {
    SampleResult sr = find_or_load_sample(inst_id, pitch, velocity, ntype, synth_profile);

    /* Basic and Stereo synth profiles: use generated waveform, not samples */
    bool force_synth = false;
    {
        const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
        if (idir && (strstr(idir, "basic") || strstr(idir, "stereo")))
            force_synth = true;
    }

    Voice *v = alloc_voice();
    memset(v, 0, sizeof(*v));
    v->active = true;
    v->note_frames = (int)(fmax(10.0, fmin(duration_ms, 30000.0)) / 1000.0 * SAMPLE_RATE);

    /* Check if this is a synth instrument (for technique modulation) */
    bool is_synth_inst = (inst_id >= 0x14 && inst_id <= 0x17) ||
                         (inst_id >= 0x18 && inst_id <= 0x1B) ||
                         (inst_id >= 0x1C && inst_id <= 0x1F) ||
                         (inst_id >= 0x20 && inst_id <= 0x23);

    if (!force_synth && sr.sample && sr.sample->data && sr.sample->frames > 0) {
        v->data = sr.sample->data;
        v->total_frames = sr.sample->frames;
        int shift = pitch - sr.root_pitch;
        v->rate = pow(2.0, shift / 12.0);
        v->sine_freq = 0;

        /* Loop setup */
        if (sr.has_loop && sr.loop_end > sr.loop_start) {
            v->is_looped = true;
            v->loop_start = sr.loop_start;
            v->loop_end = sr.loop_end;
            v->loop_len = sr.loop_end - sr.loop_start;
        }
    } else {
        /* Synthesized waveform */
        v->data = NULL;
        v->total_frames = v->note_frames + (int)(1.0 * SAMPLE_RATE);
        v->sine_freq = 440.0 * pow(2.0, (pitch - 69 + 12) / 12.0);
        v->sine_attack = (int)(0.001 * SAMPLE_RATE);
        v->waveform = 0;
        v->stereo_detune = false;
        {
            const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
            if (idir) {
                if (strstr(idir, "saw"))       v->waveform = 1;
                else if (strstr(idir, "square"))  v->waveform = 2;
                else if (strstr(idir, "triangle")) v->waveform = 3;
                if (strstr(idir, "stereo"))    v->stereo_detune = true;
            }
        }
    }

    /* Synth technique modulation - each technique number maps to different
       envelope/LFO settings. Reverse-engineered from Wwise HIRC objects +
       a lot of careful listening in-game. */
    v->pitch_env_depth = 0;
    v->pitch_env_attack = 0;
    v->pitch_env_decay = 0;
    v->lfo_type = 0;
    v->lfo_freq = 0;
    v->lfo_depth = 0;
    v->lfo_attack = 0;
    v->vol_offset_db = 0;
    v->pitch_offset = 0;
    v->env_attack = 0;
    v->env_hold = 0;
    v->env_decay = 0;
    v->env_sustain = 1.0f;
    v->env_release_t = 0.05f * SAMPLE_RATE;

    if (is_synth_inst) {
        switch (ntype) {
        case 1: /* Tab: downward dip then recover */
            v->pitch_env_depth = -200.0f;
            v->pitch_env_attack = 0.03f * SAMPLE_RATE;
            break;
        case 2: /* Cut: slide down from above, quiet, short release */
            v->pitch_env_depth = 300.0f;
            v->pitch_env_attack = 0.03f * SAMPLE_RATE;
            v->vol_offset_db = -2.0f;
            v->env_release_t = 0.01f * SAMPLE_RATE;
            break;
        case 3: /* Slide Up: slow ramp from below (~300ms) */
            v->pitch_env_depth = -200.0f;
            v->pitch_env_attack = 0.3f * SAMPLE_RATE;
            break;
        case 4: /* Short */
            v->env_decay = 0.1f * SAMPLE_RATE;
            v->env_sustain = 0.0f;
            v->env_release_t = 0.5f * SAMPLE_RATE;
            break;
        case 5: /* Pizzicato */
            v->env_decay = 0.3f * SAMPLE_RATE;
            v->env_sustain = 0.0f;
            v->env_release_t = 0.3f * SAMPLE_RATE;
            break;
        case 6: /* Tremolo */
            v->lfo_type = 1;
            v->lfo_freq = 18.0f;
            v->lfo_depth = 1.0f;
            break;
        case 7: /* Trill Minor: bipolar sine ±100 cents at 8Hz, shifted up 50c */
            v->lfo_type = 3;
            v->lfo_freq = 8.0f;
            v->lfo_depth = 100.0f;
            v->lfo_attack = 0;
            v->pitch_offset = 50.0f;
            break;
        case 8: /* Trill Major: bipolar sine ±200 cents at 8Hz, shifted up 90c */
            v->lfo_type = 3;
            v->lfo_freq = 8.0f;
            v->lfo_depth = 200.0f;
            v->lfo_attack = 0;
            v->pitch_offset = 90.0f;
            break;
        case 17: /* Vibrato: bipolar sine ±161 cents, 3.53s fade-in */
            v->lfo_type = 3;
            v->lfo_freq = 5.9f;
            v->lfo_depth = 161.0f;
            v->lfo_attack = 3.53f * SAMPLE_RATE;
            break;
        case 18: /* Marcato: sustain with gradual falloff + reverb for expansion */
            v->env_decay = 1.5f * SAMPLE_RATE;
            v->env_sustain = 0.1f;
            v->env_release_t = 0.5f * SAMPLE_RATE;
            v->rev_send = 0.4f;
            break;
        case 19: /* Filter Sustain: nearly identical to sustain */
            v->env_attack = 0.01f * SAMPLE_RATE;
            break;
        case 20: /* Filter Brassy: quick slide up, hold, then abrupt drop */
            v->pitch_env_depth = -100.0f;
            v->pitch_env_attack = 0.03f * SAMPLE_RATE;
            v->env_attack = 0.03f * SAMPLE_RATE;
            v->env_hold = 0.35f * SAMPLE_RATE;
            v->env_decay = 0.08f * SAMPLE_RATE;
            v->env_sustain = 0.25f;
            v->vol_offset_db = 4.0f;
            break;
        case 21: /* Filter Pluck: like pizzicato but louder */
            v->env_decay = 0.3f * SAMPLE_RATE;
            v->env_sustain = 0.0f;
            v->env_release_t = 0.3f * SAMPLE_RATE;
            v->vol_offset_db = 3.0f;
            break;
        default: break;
        }

    }

    /* Velocity->volume: linear curve. The BNK data says -1 to 0dB which is
       basically nothing, but that only works with the game's full mixing chain.
       Linear sounds way better for the compositor. */
    float vel_vol = (float)velocity / 127.0f;
    float db_gain = powf(10.0f, sr.vol_db / 20.0f);
    v->volume = vel_vol * db_gain * layer_vol;
    /* synth waveforms are loud, tame them */
    if (!sr.sample || !sr.sample->data || sr.sample->frames <= 0 || force_synth)
        v->volume *= 0.085f;

    /* apply synth technique volume offset */
    if (v->vol_offset_db != 0.0f)
        v->volume *= powf(10.0f, v->vol_offset_db / 20.0f);

    /* velocity-controlled LPF (from Wwise RTPC curves) */
    {
        const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
        float lpf_val = inst_vel_lpf(idir, velocity);
        v->lpf_coeff = lpf_coeff_from_wwise(lpf_val);
        v->lpf_z1_l = 0.0f;
        v->lpf_z1_r = 0.0f;
    }

    /* Per-voice FX sends */
    v->rev_send = rev_send;
    v->dly_send = dly_send;
    v->cho_send = cho_send;

    /* source tracking (-1 = preview, not tied to any layer) */
    v->src_layer = -1;
    v->src_inst_id = inst_id;
    v->src_pitch = pitch;
    v->src_vel = velocity;
    v->src_ntype = ntype;
    v->src_synth_prof = synth_profile;
    v->src_layer_vol = layer_vol;
    v->src_base_vol = v->volume / (layer_vol > 0.001f ? layer_vol : 0.001f);
    v->src_note_start = -1;
    v->src_note_dur = 0;
    v->fade_mult = 1.0f;
    v->fading_out = false;

    v->pos = 0;
    v->frac_pos = 0;
    return v;
}

/* convenience wrapper for preview (no layer routing) */
static void trigger_note(uint8_t inst_id, int pitch, int velocity,
                          double duration_ms, uint8_t ntype, uint8_t synth_profile) {
    trigger_note_ex(inst_id, pitch, velocity, duration_ms, ntype, 1.0f, 0, 0, 0, synth_profile);

    /* Super: extra detuned voices for unison */
    if (synth_profile == 2) {
        for (int det = 0; det < 2; det++) {
            Voice *uv = trigger_note_ex(inst_id, pitch, velocity, duration_ms, ntype,
                                        0.5f, 0, 0, 0, synth_profile);
            if (uv) uv->rate *= (det == 0) ? 1.00405f : 0.99597f;
        }
    }
    /* SuperOct: extra voice one octave up */
    else if (synth_profile == 3) {
        trigger_note_ex(inst_id, pitch + 12, velocity, duration_ms, ntype,
                        0.5f, 0, 0, 0, synth_profile);
    }
}

/* read a stereo sample with linear interpolation */
static inline void read_sample(const float *data, int total_frames,
                                double frac_pos, float *l, float *r) {
    int idx = (int)frac_pos;
    if (idx < 0) { *l = 0; *r = 0; return; }
    if (idx >= total_frames) { *l = 0; *r = 0; return; }
    /* At last frame: use current sample only (no interpolation past end) */
    if (idx + 1 >= total_frames) {
        *l = data[idx * 2];
        *r = data[idx * 2 + 1];
        return;
    }
    float frac = (float)(frac_pos - idx);
    *l = data[idx * 2]     + (data[(idx + 1) * 2]     - data[idx * 2])     * frac;
    *r = data[idx * 2 + 1] + (data[(idx + 1) * 2 + 1] - data[idx * 2 + 1]) * frac;
}

/* render one output frame from a voice. false = voice is done */
static bool voice_render_frame(Voice *vc, float *out_l, float *out_r) {
    float l = 0, r = 0;

    if (vc->data) {
        /* sample-based playback */

        /* Synth technique: dynamic pitch modulation on sample rate */
        /* Precompute constant pitch_offset multiplier (doesn't change per sample) */
        double cur_rate = vc->rate;
        if (vc->pitch_offset != 0.0f)
            cur_rate *= exp2((double)vc->pitch_offset / 1200.0);
        if (vc->pitch_env_depth != 0.0f) {
            float env = 1.0f;
            if (vc->pitch_env_attack > 0 && vc->pos < (int)vc->pitch_env_attack)
                env = (float)vc->pos / vc->pitch_env_attack;
            float pmod = vc->pitch_env_depth * (1.0f - env);
            cur_rate *= exp2((double)pmod / 1200.0);
        }
        if (vc->lfo_type == 2) {
            double t = (double)vc->pos / SAMPLE_RATE;
            double lfo_val = sin(2.0 * 3.14159265 * vc->lfo_freq * t) >= 0 ? 1.0 : 0.0;
            cur_rate *= exp2((vc->lfo_depth * lfo_val) / 1200.0);
        } else if (vc->lfo_type == 3) {
            double t = (double)vc->pos / SAMPLE_RATE;
            float depth = vc->lfo_depth;
            if (vc->lfo_attack > 0 && vc->pos < (int)vc->lfo_attack)
                depth *= (float)vc->pos / vc->lfo_attack;
            double lfo_val = sin(2.0 * 3.14159265 * vc->lfo_freq * t);
            cur_rate *= exp2((depth * lfo_val) / 1200.0);
        } else if (vc->lfo_type == 5) {
            double t = (double)vc->pos / SAMPLE_RATE;
            double lfo_val = 0.5 + 0.5 * sin(2.0 * 3.14159265 * vc->lfo_freq * t);
            cur_rate *= exp2((vc->lfo_depth * lfo_val) / 1200.0);
        }

        read_sample(vc->data, vc->total_frames, vc->frac_pos, &l, &r);

        /* Synth technique: volume envelope on samples */
        if (vc->env_decay > 0 || vc->env_attack > 0) {
            float env = 1.0f;
            int pos = vc->pos;
            if (vc->env_attack > 0 && pos < (int)vc->env_attack) {
                env = (float)pos / vc->env_attack;
            } else if (vc->env_hold > 0 && pos < (int)(vc->env_attack + vc->env_hold)) {
                env = 1.0f;
            } else if (vc->env_decay > 0) {
                float since_hold = (float)pos - vc->env_attack - vc->env_hold;
                if (since_hold < vc->env_decay)
                    env = 1.0f - (1.0f - vc->env_sustain) * (since_hold / vc->env_decay);
                else
                    env = vc->env_sustain;
            }
            l *= env; r *= env;
        }

        /* Synth technique: volume LFO on samples */
        if (vc->lfo_type == 1) {
            double lfo_phase = fmod((double)vc->pos * vc->lfo_freq / SAMPLE_RATE, 1.0);
            double tri = lfo_phase < 0.5 ? (4.0 * lfo_phase - 1.0) : (3.0 - 4.0 * lfo_phase);
            float mod = (float)(1.0 - vc->lfo_depth * (0.5 + 0.5 * tri));
            l *= mod; r *= mod;
        } else if (vc->lfo_type == 4) {
            double lfo_phase = fmod((double)vc->pos * vc->lfo_freq / SAMPLE_RATE, 1.0);
            float mod = lfo_phase < 0.5 ? 1.0f : 0.0f;
            l *= mod; r *= mod;
        }

        /* Loop boundary crossfade: blend old position with new */
        if (vc->xfade_remain > 0) {
            float old_l, old_r;
            read_sample(vc->data, vc->total_frames, vc->xfade_old_pos, &old_l, &old_r);
            float t = 1.0f - (float)vc->xfade_remain / (float)vc->xfade_total;
            l = l * t + old_l * (1.0f - t);
            r = r * t + old_r * (1.0f - t);
            vc->xfade_old_pos += cur_rate;
            vc->xfade_remain--;
        }

        /* Advance position */
        vc->frac_pos += cur_rate;

        /* Loop handling (sustain phase only) */
        if (vc->is_looped && !vc->releasing && vc->loop_len > 0) {
            if ((int)vc->frac_pos >= vc->loop_end) {
                /* Wrap around to loop start */
                double overshoot = vc->frac_pos - vc->loop_end;
                /* Start crossfade: old signal is from loop_end region */
                int xf = LOOP_XFADE;
                if (xf > vc->loop_len / 2) xf = vc->loop_len / 2;
                if (xf > 0) {
                    vc->xfade_old_pos = vc->loop_end - (double)xf * vc->rate;
                    vc->xfade_remain = xf;
                    vc->xfade_total = xf;
                }
                vc->frac_pos = vc->loop_start + fmod(overshoot, vc->loop_len);
            }
        }

        /* End of sample: fade last portion to avoid click.
           Scale fade length by 1/rate so slower playback gets more fade. */
        {
            int fade_len = (int)(256 / (vc->rate > 0.1 ? vc->rate : 0.1));
            if (fade_len < 64) fade_len = 64;
            if (fade_len > 2048) fade_len = 2048;
            if ((int)vc->frac_pos >= vc->total_frames - fade_len) {
                int frames_left = vc->total_frames - (int)vc->frac_pos;
                if (frames_left <= 0) {
                    /* Sample exhausted  - output silence but keep voice alive
                       if release fade is still running, so it fades to zero */
                    l = 0; r = 0;
                    goto apply_volume;
                }
                float fade = (float)frames_left / (float)fade_len;
                l *= fade;
                r *= fade;
            }
        }
    } else {
        /* waveform synthesis (Basic/Stereo synth profiles) */
        double t = (double)vc->pos / SAMPLE_RATE;
        double freq = vc->sine_freq;

        /* Permanent pitch offset (for centering trills) */
        if (vc->pitch_offset != 0.0f)
            freq *= exp2((double)vc->pitch_offset / 1200.0);

        /* pitch envelope: starts offset, decays to 0 (Wwise ADSR style) */
        if (vc->pitch_env_depth != 0.0f) {
            float env = 1.0f;
            int pos = vc->pos;
            if (vc->pitch_env_attack > 0 && pos < (int)vc->pitch_env_attack) {
                env = (float)pos / vc->pitch_env_attack;
            }
            float pmod = vc->pitch_env_depth * (1.0f - env);
            freq *= exp2((double)pmod / 1200.0);
        }

        /* Pitch LFO (types 2 and 3): apply before waveform generation */
        if (vc->lfo_type == 2) {
            double lfo_val = sin(2.0 * 3.14159265 * vc->lfo_freq * t) >= 0 ? 1.0 : 0.0;
            freq *= exp2((vc->lfo_depth * lfo_val) / 1200.0);
        } else if (vc->lfo_type == 3) {
            /* Bipolar sine pitch LFO (vibrato: -depth to +depth) */
            float depth = vc->lfo_depth;
            if (vc->lfo_attack > 0 && vc->pos < (int)vc->lfo_attack) {
                depth *= (float)vc->pos / vc->lfo_attack;
            }
            double lfo_val = sin(2.0 * 3.14159265 * vc->lfo_freq * t);
            freq *= exp2((depth * lfo_val) / 1200.0);
        } else if (vc->lfo_type == 5) {
            /* Unipolar sine pitch LFO (trill: 0 to +depth) */
            double lfo_val = 0.5 + 0.5 * sin(2.0 * 3.14159265 * vc->lfo_freq * t);
            freq *= exp2((vc->lfo_depth * lfo_val) / 1200.0);
        } else if (vc->lfo_type == 7) {
            /* Major chord arpeggio: root(0) -> 3rd(+400c) -> 5th(+700c) -> 3rd(+400c) */
            static const double chord_cents[] = {0.0, 400.0, 700.0, 400.0};
            double lfo_phase = fmod((double)vc->pos * vc->lfo_freq / SAMPLE_RATE, 1.0);
            int step = (int)(lfo_phase * 4) % 4;
            /* Smooth interpolation between steps */
            double frac = fmod(lfo_phase * 4.0, 1.0);
            int next = (step + 1) % 4;
            double cents = chord_cents[step] + (chord_cents[next] - chord_cents[step]) * frac;
            freq *= exp2(cents / 1200.0);
        }

        /* Phase accumulation (avoids clicks on frequency changes) */
        double phase_inc = freq / SAMPLE_RATE;
        vc->synth_phase += phase_inc;
        vc->synth_phase -= (int)vc->synth_phase; /* wrap to 0-1 */
        double phase = vc->synth_phase;
        float val;

        /* polyBLEP anti-aliasing for discontinuous waveforms */
        double dt = phase_inc; /* normalized freq = freq/SR */
        #define POLYBLEP(t, dt) ((t) < (dt) ? \
            ((t)/(dt) * 2.0 - (t)/(dt) * ((t)/(dt)) - 1.0) : \
            ((t) > 1.0 - (dt) ? \
            (((t) - 1.0)/(dt) * ((t) - 1.0)/(dt) + ((t) - 1.0)/(dt) * 2.0 + 1.0) : 0.0))

        switch (vc->waveform) {
        case 1: /* saw with polyBLEP */
            val = (float)(2.0 * phase - 1.0);
            val -= (float)POLYBLEP(phase, dt);
            break;
        case 2: /* square with polyBLEP */
            val = phase < 0.5 ? 1.0f : -1.0f;
            val += (float)POLYBLEP(phase, dt);
            val -= (float)POLYBLEP(fmod(phase + 0.5, 1.0), dt);
            break;
        case 3: /* triangle (no aliasing issues) */
            val = (float)(phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase));
            break;
        default: /* 0 = sine */
            val = sinf((float)(2.0 * 3.14159265 * phase));
            break;
        }
        #undef POLYBLEP

        /* Click-prevention attack ramp */
        if (vc->pos < vc->sine_attack && vc->sine_attack > 0) {
            val *= (float)vc->pos / (float)vc->sine_attack;
        }

        /* Volume envelope (AHDS  - R handled by release logic) */
        {
            float env = 1.0f;
            int pos = vc->pos;
            if (vc->env_attack > 0 && pos < (int)vc->env_attack) {
                env = (float)pos / vc->env_attack;
            } else if (vc->env_hold > 0 && pos < (int)(vc->env_attack + vc->env_hold)) {
                env = 1.0f; /* hold at full volume */
            } else if (vc->env_decay > 0) {
                float since_hold = (float)pos - vc->env_attack - vc->env_hold;
                if (since_hold < vc->env_decay) {
                    env = 1.0f - (1.0f - vc->env_sustain) * (since_hold / vc->env_decay);
                } else {
                    env = vc->env_sustain;
                }
            }
            val *= env;
        }

        /* Volume LFO */
        if (vc->lfo_type == 1) {
            /* Triangle volume LFO */
            double lfo_phase = fmod((double)vc->pos * vc->lfo_freq / SAMPLE_RATE, 1.0);
            double tri = lfo_phase < 0.5 ? (4.0 * lfo_phase - 1.0) : (3.0 - 4.0 * lfo_phase);
            val *= (float)(1.0 - vc->lfo_depth * (0.5 + 0.5 * tri));
        } else if (vc->lfo_type == 4) {
            /* Square volume LFO (on/off pulsing) */
            double lfo_phase = fmod((double)vc->pos * vc->lfo_freq / SAMPLE_RATE, 1.0);
            val *= lfo_phase < 0.5 ? 1.0f : 0.0f;
        } else if (vc->lfo_type == 6) {
            /* Breathy noise mix for flute-like quality */
            float noise = ((float)(rand() % 65536) / 32768.0f - 1.0f) * 0.15f;
            val = val * 0.85f + noise;
        }

        /* Stereo: Haas delay for width + detuned layer for depth */
        if (vc->stereo_detune) {
            /* Second oscillator at slight detune for chorus/depth */
            double freq2 = freq * 1.003;
            double phase_inc2 = freq2 / SAMPLE_RATE;
            vc->synth_phase_r += phase_inc2;
            vc->synth_phase_r -= (int)vc->synth_phase_r;
            double phase2 = vc->synth_phase_r;
            float val2;
            switch (vc->waveform) {
            case 1: { float v2 = (float)(2.0*phase2-1.0); double dt2=phase_inc2; v2-=(float)((phase2<dt2)?((phase2/dt2)*2.0-(phase2/dt2)*(phase2/dt2)-1.0):((phase2>1.0-dt2)?(((phase2-1.0)/dt2)*((phase2-1.0)/dt2)+((phase2-1.0)/dt2)*2.0+1.0):0.0)); val2=v2; break; }
            case 2: { val2 = phase2 < 0.5 ? 1.0f : -1.0f; double dt2=phase_inc2; val2+=(float)((phase2<dt2)?((phase2/dt2)*2.0-(phase2/dt2)*(phase2/dt2)-1.0):((phase2>1.0-dt2)?(((phase2-1.0)/dt2)*((phase2-1.0)/dt2)+((phase2-1.0)/dt2)*2.0+1.0):0.0)); double p2h=fmod(phase2+0.5,1.0); val2-=(float)((p2h<dt2)?((p2h/dt2)*2.0-(p2h/dt2)*(p2h/dt2)-1.0):((p2h>1.0-dt2)?(((p2h-1.0)/dt2)*((p2h-1.0)/dt2)+((p2h-1.0)/dt2)*2.0+1.0):0.0)); break; }
            case 3: val2 = (float)(phase2 < 0.5 ? (4.0*phase2-1.0) : (3.0-4.0*phase2)); break;
            default: val2 = sinf((float)(2.0*3.14159265*phase2)); break;
            }
            /* Mix both oscillators into both channels, slightly offset, boosted */
            l = (val * 0.6f + val2 * 0.4f) * 1.3f;
            r = (val * 0.4f + val2 * 0.6f) * 1.3f;
        } else {
            l = val;
            r = val;
        }
    }

apply_volume:
    /* Attack ramp: fade in first 32 output frames to avoid start click
       (skip for recorder  - its instant cutoff handles transitions) */
    if (vc->pos < 32 && vc->src_inst_id != 0x02) {
        float ramp = (float)(vc->pos + 1) / 32.0f;
        l *= ramp;
        r *= ramp;
    }

    /* Apply volume */
    l *= vc->volume;
    r *= vc->volume;

    /* Apply LPF (1-pole low-pass: y[n] = y[n-1] + coeff * (x[n] - y[n-1])) */
    if (vc->lpf_coeff > 0.0f && vc->lpf_coeff < 1.0f) {
        vc->lpf_z1_l += vc->lpf_coeff * (l - vc->lpf_z1_l);
        vc->lpf_z1_r += vc->lpf_coeff * (r - vc->lpf_z1_r);
        l = vc->lpf_z1_l;
        r = vc->lpf_z1_r;
    }

    /* release handling - note-off transition.
       Every instrument has different release behavior because of course it does.
       Piano rings out naturally, recorder cuts instantly, flute has a short fade...
       all tuned to match the game. */
    if (vc->pos >= vc->note_frames && !vc->releasing) {
        vc->releasing = true;
        vc->release_pos = 0;

        if (vc->data && vc->is_looped && vc->loop_end > 0) {
            if ((vc->src_inst_id >= 0x14 && vc->src_inst_id <= 0x17) ||
                (vc->src_inst_id >= 0x18 && vc->src_inst_id <= 0x1B) ||
                (vc->src_inst_id >= 0x1C && vc->src_inst_id <= 0x1F) ||
                (vc->src_inst_id >= 0x20 && vc->src_inst_id <= 0x23)) {
                /* Synth samples: short fade */
                vc->release_frames = (int)(0.02 * SAMPLE_RATE);
                vc->fade_frames = (int)(0.02 * SAMPLE_RATE);
            } else if (vc->src_inst_id == 0x02) {  /* Beginner Recorder */
                vc->release_frames = (int)(0.01 * SAMPLE_RATE);
                vc->fade_frames = (int)(0.01 * SAMPLE_RATE);
            } else if (vc->src_inst_id == 0x0B) { /* Flor. Flute */
                vc->release_frames = (int)(0.05 * SAMPLE_RATE);
                vc->fade_frames = (int)(0.05 * SAMPLE_RATE);
            } else if (vc->src_inst_id == 0x12) { /* Flor. Violin */
                vc->release_frames = (int)(0.05 * SAMPLE_RATE);
                vc->fade_frames = (int)(0.05 * SAMPLE_RATE);
            } else {
                /* Looped: crossfade from current loop position to release tail */
                vc->xfade_old_pos = vc->frac_pos;
                vc->frac_pos = (double)vc->loop_end;
                vc->xfade_remain = LOOP_XFADE;
                vc->xfade_total = LOOP_XFADE;
                int remaining = (int)((vc->total_frames - vc->loop_end) / vc->rate);
                int max_fade = (int)(0.5 * SAMPLE_RATE);
                vc->release_frames = remaining < max_fade ? remaining : max_fade;
                if (vc->release_frames < (int)(0.05 * SAMPLE_RATE))
                    vc->release_frames = (int)(0.05 * SAMPLE_RATE);
                vc->fade_frames = vc->release_frames;
            }
        } else if (vc->data) {
            /* Non-looped sample release */
            int remaining = (int)((vc->total_frames - (int)vc->frac_pos) / vc->rate);
            int max_rel;
            if (vc->src_inst_id == 0x02) /* Beginner Recorder: instant kill */
                max_rel = 1;
            else if (vc->src_inst_id == 0x07 || /* Beginner Piano: natural decay */
                     vc->src_inst_id == 0x11)   /* Flor. Piano */
                max_rel = (int)(2.0 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x06)   /* Beginner Harp: natural decay */
                max_rel = (int)(1.0 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x13)   /* Handpan: natural decay */
                max_rel = (int)(1.5 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x10) { /* Flor. Harp */
                if (vc->src_ntype == 0)     /* Sustain: medium ring */
                    max_rel = (int)(0.15 * SAMPLE_RATE);
                else                        /* Chords/Gliss: short fade */
                    max_rel = (int)(0.03 * SAMPLE_RATE);
            }
            else if (vc->src_inst_id == 0x01)   /* Beginner Flute: 200ms */
                max_rel = (int)(0.2 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x0A) /* Flor. Guitar: medium ring */
                max_rel = (int)(0.15 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x0B) /* Flor. Flute */
                max_rel = (int)(0.01 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x28) /* Flor. Horn */
                max_rel = (int)(0.08 * SAMPLE_RATE);
            else if (vc->src_inst_id == 0x24 || vc->src_inst_id == 0x25 ||
                     vc->src_inst_id == 0x26) /* Electric Guitars */
                max_rel = (int)(0.1 * SAMPLE_RATE);
            else if ((vc->src_inst_id >= 0x14 && vc->src_inst_id <= 0x17) ||
                     (vc->src_inst_id >= 0x18 && vc->src_inst_id <= 0x1B) ||
                     (vc->src_inst_id >= 0x1C && vc->src_inst_id <= 0x1F) ||
                     (vc->src_inst_id >= 0x20 && vc->src_inst_id <= 0x23))
                max_rel = (int)(0.02 * SAMPLE_RATE); /* Synth samples: 20ms fade */
            else /* All others: short cutoff */
                max_rel = (int)(0.05 * SAMPLE_RATE);
            vc->release_frames = remaining < max_rel ? remaining : max_rel;
            vc->fade_frames = vc->release_frames;
        } else {
            /* Synth: use per-technique release time */
            vc->release_frames = (int)vc->env_release_t;
            if (vc->release_frames < (int)(0.01 * SAMPLE_RATE))
                vc->release_frames = (int)(0.05 * SAMPLE_RATE);
            vc->fade_frames = vc->release_frames;
        }
    }

    /* Apply release fade */
    if (vc->releasing) {
        if (vc->release_pos >= vc->fade_frames) {
            *out_l = 0; *out_r = 0;
            return false; /* deactivate */
        }
        float ft = 1.0f - (float)vc->release_pos / (float)vc->fade_frames;
        float fade;
        if (vc->src_inst_id == 0x07 || vc->src_inst_id == 0x11 ||
            vc->src_inst_id == 0x06 || vc->src_inst_id == 0x13) {
            fade = ft * ft * ft;      /* Piano/Harp/Handpan: cubic for natural decay */
        } else if (vc->data && vc->is_looped) {
            fade = ft * ft;           /* Looped: quadratic */
        } else {
            fade = ft;                /* Default: linear for clean cutoff */
        }
        l *= fade;
        r *= fade;
        vc->release_pos++;
    }

    *out_l = l;
    *out_r = r;
    vc->pos++;
    return true;
}

/* audio callback - mix all active voices with per-voice FX routing */
static bool g_fade_out = false;
static bool g_faded = false;
static int  g_fade_in_frames = 0;
static bool g_playback_paused = false;  /* freeze playback voices but keep device running */

static void audio_callback(ma_device *dev, void *output, const void *input,
                           ma_uint32 frames) {
    (void)dev; (void)input;
    float *out = (float *)output;
    memset(out, 0, frames * 2 * sizeof(float));

    /* Get global FX parameters */
    float g_rev = g_fx_reverb ? (float)*g_fx_reverb : 0;
    float g_dly = g_fx_delay ? (float)*g_fx_delay : 0;
    float g_cho_fb = g_fx_chorus_fb ? (float)*g_fx_chorus_fb : 0;
    float g_cho_depth = g_fx_chorus_depth ? (float)*g_fx_chorus_depth : 0;
    float g_cho_freq = g_fx_chorus_freq ? (float)*g_fx_chorus_freq : 0;
    bool has_fx = (g_rev > 0 || g_dly > 0 || g_cho_fb > 0 || g_cho_depth > 0);

    /* FX accumulation buffers (on stack, small: 512 frames typical) */
    float rev_buf_l[1024], rev_buf_r[1024];
    float dly_buf[1024], cho_buf[1024];
    if (has_fx && frames <= 1024) {
        memset(rev_buf_l, 0, frames * sizeof(float));
        memset(rev_buf_r, 0, frames * sizeof(float));
        memset(dly_buf, 0, frames * sizeof(float));
        memset(cho_buf, 0, frames * sizeof(float));
    } else {
        has_fx = false; /* safety: don't overflow stack */
    }

    /* Mix voices */
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice *vc = &g_voices[v];
        if (!vc->active) continue;
        /* When paused: freeze playback voices, still render previews */
        if (g_playback_paused && vc->src_layer >= 0) continue;

        for (ma_uint32 f = 0; f < frames; f++) {
            float l, r;
            if (!voice_render_frame(vc, &l, &r)) {
                /* Voice ended  - don't just stop, let fade_mult handle it */
                if (!vc->fading_out) {
                    vc->fading_out = true;
                    vc->fade_mult = 0.0f;
                }
                vc->active = false;
                break;
            }

            /* Apply crossfade multiplier */
            if (vc->fading_out) {
                vc->fade_mult -= 1.0f / (0.005f * SAMPLE_RATE); /* 5ms fade */
                if (vc->fade_mult <= 0) { vc->active = false; break; }
            }
            l *= vc->fade_mult;
            r *= vc->fade_mult;

            out[f * 2 + 0] += l;
            out[f * 2 + 1] += r;

            /* route to FX buses based on per-voice sends */
            if (has_fx) {
                if (vc->rev_send > 0 && g_rev > 0) {
                    rev_buf_l[f] += l * vc->rev_send;
                    rev_buf_r[f] += r * vc->rev_send;
                }
                float mono = (l + r) * 0.5f;
                if (vc->dly_send > 0 && g_dly > 0) {
                    dly_buf[f] += mono * vc->dly_send;
                }
                if (vc->cho_send > 0 && (g_cho_fb > 0 || g_cho_depth > 0)) {
                    cho_buf[f] += mono * vc->cho_send;
                }
            }
        }
    }

    /* Process FX buses and add wet signal to output */
    if (has_fx) {
        for (ma_uint32 f = 0; f < frames; f++) {
            float wet_l = 0, wet_r = 0;

            /* reverb - tuned to approximate Wwise RoomVerb defaults */
            if (g_rev > 0) {
                float wet = g_rev / 100.0f;
                float fb = 0.88f + 0.07f * wet;
                float rl, rr;
                reverb_process(&g_reverb, rev_buf_l[f], rev_buf_r[f], fb, &rl, &rr);
                wet_l += rl * wet * 10.0f;
                wet_r += rr * wet * 10.0f;
            }

            /* Stereo delay */
            if (g_dly > 0) {
                float wet = g_dly / 100.0f;
                float fb = 0.40f + 0.10f * wet;
                float dl, dr;
                delay_process(&g_delay, dly_buf[f], fb, wet, &dl, &dr);
                wet_l += dl * 6.5f;
                wet_r += dr * 6.5f;
            }

            /* Chorus */
            if (g_cho_fb > 0 || g_cho_depth > 0) {
                float wet = (g_cho_fb > g_cho_depth ? g_cho_fb : g_cho_depth) / 100.0f;
                float cho = chorus_process(&g_chorus, cho_buf[f], g_cho_depth, g_cho_freq, wet);
                wet_l += cho;
                wet_r += cho;
            }

            out[f * 2 + 0] += wet_l;
            out[f * 2 + 1] += wet_r;
        }
    }

    /* soft limiter with smooth attack/release to prevent clipping */
    {
        static float gain = 1.0f;
        float attack_coeff  = 1.0f - expf(-1.0f / (0.002f * SAMPLE_RATE)); /* 2ms attack */
        float release_coeff = 1.0f - expf(-1.0f / (0.200f * SAMPLE_RATE)); /* 200ms release */
        for (ma_uint32 f = 0; f < frames; f++) {
            float peak = fabsf(out[f * 2]) > fabsf(out[f * 2 + 1])
                         ? fabsf(out[f * 2]) : fabsf(out[f * 2 + 1]);
            float target = (peak > 0.001f && peak * gain > 0.9f)
                           ? 0.9f / peak : 1.0f;
            float coeff = (target < gain) ? attack_coeff : release_coeff;
            gain += coeff * (target - gain);
            out[f * 2 + 0] *= gain;
            out[f * 2 + 1] *= gain;
        }
    }

    /* Fade out/in only affects the full mix when transitioning.
       Preview voices are always mixed in above since paused playback
       voices are skipped entirely in the mix loop. */
    if (g_fade_out) {
        g_faded = true;
    }
    if (g_fade_in_frames > 0) {
        g_fade_in_frames = 0;
    }
}

/* public API */

bool muse_audio_init(void) {
    /* load key zones and loop points first */
    load_key_zones();
    load_loop_points();

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = audio_callback;
    cfg.periodSizeInFrames = 512;

    if (ma_device_init(NULL, &cfg, &g_device) != MA_SUCCESS)
        return false;

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        ma_device_uninit(&g_device);
        return false;
    }

    g_initialized = true;
    return true;
}

void muse_audio_shutdown(void) {
    if (g_initialized) {
        ma_device_uninit(&g_device);
        g_initialized = false;
    }
    /* Free cached samples */
    for (int i = 0; i < g_cache_count; i++) {
        free(g_cache[i].sample.data);
    }
    g_cache_count = 0;
    g_cache_bytes = 0;
}

/* solo/mute helpers */
static bool any_solo(const MuseProject *proj) {
    for (int i = 0; i < proj->num_layers; i++)
        if (proj->layers[i].solo) return true;
    return false;
}

/* should this layer be silent? */
static bool layer_skip(const MuseLayer *ly, bool has_solo) {
    if (ly->muted) return true;
    if (has_solo && !ly->solo) return true;
    return false;
}

void muse_audio_play(MuseProject *proj, double start_ms) {
    /* resume from pause - keep voices where they were */
    if (g_pb.project == proj && !g_pb.playing &&
        fabs(g_pb.position_ms - start_ms) < 1.0) {
        muse_audio_resume();
        return;
    }

    muse_audio_stop();

    /* Reset effects state */
    reverb_init(&g_reverb);
    delay_init(&g_delay);
    chorus_init(&g_chorus);

    g_pb.project = proj;
    g_pb.start_ms = start_ms;
    g_pb.position_ms = start_ms;
    g_pb.prev_tick_ms = start_ms - 50.0; /* well before so first-beat notes always trigger */
    g_pb.playing = true;

    /* make sure device is running */
    if (g_initialized)
        ma_device_start(&g_device);
}

void muse_audio_stop(void) {
    g_pb.playing = false;
    g_playback_paused = false;
    /* Silence all voices */
    for (int i = 0; i < MAX_VOICES; i++)
        g_voices[i].active = false;
    g_pb.position_ms = 0;
}

void muse_audio_pause(void) {
    g_pb.playing = false;
    g_playback_paused = true;
    /* voices stay alive but frozen - device keeps running for previews */
}

void muse_audio_resume(void) {
    g_pb.playing = true;
    g_playback_paused = false;
    g_fade_in_frames = (int)(0.003 * SAMPLE_RATE); /* 3ms fade-in */
    /* voices resume where they left off */
}

bool muse_audio_is_playing(void) {
    return g_pb.playing;
}

double muse_audio_position_ms(void) {
    return g_pb.position_ms;
}

void muse_audio_seek(double ms) {
    if (!g_pb.playing) return;
    g_pb.prev_tick_ms = ms - 50.0; /* offset so notes at seek position trigger */
    g_pb.position_ms = ms;
}

/* find a note by start time + pitch (stable across edits) */
static MuseNote *find_voice_note(MuseProject *proj, int layer, double start, int pitch) {
    if (layer < 0 || layer >= proj->num_layers) return NULL;
    MuseLayer *ly = &proj->layers[layer];
    for (int si = 0; si < ly->num_sublayers; si++) {
        NoteArray *na = &ly->sublayers[si];
        for (int ni = 0; ni < na->count; ni++) {
            if (fabs(na->notes[ni].start - start) < 0.5 && na->notes[ni].pitch == pitch)
                return &na->notes[ni];
        }
    }
    return NULL;
}

/* main-thread tick: advance playback and trigger notes.
   Live-scans the project each frame so edits take effect immediately.
   This is what makes the "play while editing" workflow feel responsive. */
void muse_audio_tick(double current_ms) {
    if (!g_pb.playing || !g_pb.project) return;

    double prev_ms = g_pb.prev_tick_ms;
    g_pb.position_ms = current_ms;
    g_pb.prev_tick_ms = current_ms;

    if (prev_ms < current_ms - 40) {
        /* first tick after play/seek - reset state */
    }

    MuseProject *proj = g_pb.project;
    bool has_solo = any_solo(proj);
    bool any_triggered = false;

    /* Scan all notes: trigger any whose start falls in (prev_ms, current_ms] */
    for (int li = 0; li < proj->num_layers; li++) {
        MuseLayer *ly = &proj->layers[li];
        if (layer_skip(ly, has_solo)) continue;

        for (int si = 0; si < ly->num_sublayers; si++) {
            NoteArray *na = &ly->sublayers[si];
            for (int ni = 0; ni < na->count; ni++) {
                MuseNote *n = &na->notes[ni];
                if (n->start > prev_ms && n->start <= current_ms) {
                    float layer_vol = ly->volume / 70.0f;
                    float rev_s = ly->reverb_send / 100.0f;
                    float dly_s = ly->delay_send / 100.0f;
                    float cho_s = ly->chorus_send / 100.0f;

                    Voice *v = trigger_note_ex(ly->inst_id, n->pitch, n->vel,
                                               n->dur, n->ntype,
                                               layer_vol, rev_s, dly_s, cho_s, ly->synth_profile);
                    if (v) {
                        v->src_layer = li;
                        v->src_note_start = n->start;
                        v->src_note_dur = n->dur;
                    }

                    /* Super: spawn 2 extra detuned voices for unison thickness */
                    if (ly->synth_profile == 2) {
                        for (int det = 0; det < 2; det++) {
                            Voice *uv = trigger_note_ex(ly->inst_id, n->pitch, n->vel,
                                                        n->dur, n->ntype,
                                                        layer_vol * 0.5f, rev_s, dly_s, cho_s, ly->synth_profile);
                            if (uv) {
                                /* Apply slight detune: +7 and -7 cents */
                                float detune = (det == 0) ? 1.00405f : 0.99597f;
                                uv->rate *= detune;
                                uv->src_layer = li;
                                uv->src_note_start = n->start;
                                uv->src_note_dur = n->dur;
                            }
                        }
                    }
                    /* SuperOct: spawn an extra voice one octave up */
                    else if (ly->synth_profile == 3) {
                        Voice *ov = trigger_note_ex(ly->inst_id, n->pitch + 12, n->vel,
                                                    n->dur, n->ntype,
                                                    layer_vol * 0.5f, rev_s, dly_s, cho_s, ly->synth_profile);
                        if (ov) {
                            ov->src_layer = li;
                            ov->src_note_start = n->start;
                            ov->src_note_dur = n->dur;
                        }
                    }
                    any_triggered = true;
                }
            }
        }
    }

    /* live-change detection: update/retrigger voices when user edits during playback */
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice *vc = &g_voices[v];
        if (!vc->active || vc->fading_out) continue;
        if (vc->src_layer < 0 || vc->src_layer >= proj->num_layers) continue;

        MuseLayer *ly = &proj->layers[vc->src_layer];

        /* 1. Mute/Solo: fade out voices on silenced layers */
        if (layer_skip(ly, has_solo)) {
            vc->fading_out = true;
            continue;
        }

        /* 2. Volume: instant update */
        float cur_layer_vol = ly->volume / 70.0f;
        if (fabsf(cur_layer_vol - vc->src_layer_vol) > 0.001f) {
            vc->volume = vc->src_base_vol * cur_layer_vol;
            vc->src_layer_vol = cur_layer_vol;
        }

        /* 3. Aux sends: instant update */
        vc->rev_send = ly->reverb_send / 100.0f;
        vc->dly_send = ly->delay_send / 100.0f;
        vc->cho_send = ly->chorus_send / 100.0f;

        /* 4. Find the source note to check for property changes */
        MuseNote *sn = find_voice_note(proj, vc->src_layer, vc->src_note_start, vc->src_pitch);

        /* Note was deleted  - fade out */
        if (!sn) {
            vc->fading_out = true;
            continue;
        }

        /* 5. Duration change: update note_frames */
        if (fabs(sn->dur - vc->src_note_dur) > 0.5) {
            vc->src_note_dur = sn->dur;
            vc->note_frames = (int)(fmax(10.0, fmin(sn->dur, 30000.0)) / 1000.0 * SAMPLE_RATE);
        }

        /* 6. Changes requiring retrigger: instrument, velocity, technique, synth profile */
        bool need_retrigger = false;
        if (ly->inst_id != vc->src_inst_id) need_retrigger = true;
        if (sn->vel != vc->src_vel) need_retrigger = true;
        if (sn->ntype != vc->src_ntype) need_retrigger = true;
        if (ly->synth_profile != vc->src_synth_prof) need_retrigger = true;

        if (need_retrigger) {
            double elapsed_ms = current_ms - vc->src_note_start;
            double remaining_ms = vc->src_note_dur - elapsed_ms;
            if (remaining_ms < 10) continue;

            /* Fade out old voice */
            vc->fading_out = true;

            /* Trigger replacement with current properties at correct position */
            Voice *nv = trigger_note_ex(ly->inst_id, sn->pitch, sn->vel,
                                        remaining_ms, sn->ntype,
                                        cur_layer_vol,
                                        ly->reverb_send / 100.0f,
                                        ly->delay_send / 100.0f,
                                        ly->chorus_send / 100.0f,
                                        ly->synth_profile);
            if (nv) {
                nv->src_layer = vc->src_layer;
                nv->src_note_start = vc->src_note_start;
                nv->src_note_dur = vc->src_note_dur;
                int skip = (int)(elapsed_ms / 1000.0 * SAMPLE_RATE * nv->rate);
                nv->pos = skip;
                nv->frac_pos = (double)skip;
            }
        }
    }

    /* check if playback is done - no future notes and no active voices */
    if (!any_triggered && current_ms > prev_ms) {
        bool has_future = false;
        for (int li = 0; li < proj->num_layers && !has_future; li++) {
            MuseLayer *ly = &proj->layers[li];
            if (layer_skip(ly, has_solo)) continue;
            for (int si = 0; si < ly->num_sublayers && !has_future; si++) {
                NoteArray *na = &ly->sublayers[si];
                for (int ni = 0; ni < na->count; ni++) {
                    if (na->notes[ni].start > current_ms) { has_future = true; break; }
                }
            }
        }
        if (!has_future) {
            bool any_active = false;
            for (int i = 0; i < MAX_VOICES; i++) {
                if (g_voices[i].active) { any_active = true; break; }
            }
            if (!any_active) {
                g_pb.playing = false;
                if (g_on_finished) g_on_finished(g_on_finished_data);
            }
        }
    }
}

void muse_audio_preview(int pitch, int velocity, int duration_ms,
                        uint8_t inst_id, uint8_t ntype, uint8_t synth_profile) {
    trigger_note(inst_id, pitch, velocity, (double)duration_ms, ntype, synth_profile);
}

/* offline WAV export - render a note using the same voice_render_frame()
   as the real-time callback so the export sounds identical to playback */
static void render_note_into(float *buf, int total_frames,
                              int offset, uint8_t inst_id, int pitch,
                              int velocity, double duration_ms, uint8_t ntype,
                              float layer_vol, uint8_t synth_profile) {
    /* Create a temporary voice with the same setup as trigger_note_ex */
    SampleResult sr = find_or_load_sample(inst_id, pitch, velocity, ntype, synth_profile);

    bool force_synth2 = false;
    {
        const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
        if (idir && (strstr(idir, "basic") || strstr(idir, "stereo")))
            force_synth2 = true;
    }

    Voice tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.active = true;
    tmp.note_frames = (int)(fmax(10.0, fmin(duration_ms, 30000.0)) / 1000.0 * SAMPLE_RATE);

    if (!force_synth2 && sr.sample && sr.sample->data && sr.sample->frames > 0) {
        tmp.data = sr.sample->data;
        tmp.total_frames = sr.sample->frames;
        int shift = pitch - sr.root_pitch;
        tmp.rate = pow(2.0, shift / 12.0);
        if (sr.has_loop && sr.loop_end > sr.loop_start) {
            tmp.is_looped = true;
            tmp.loop_start = sr.loop_start;
            tmp.loop_end = sr.loop_end;
            tmp.loop_len = sr.loop_end - sr.loop_start;
        }
    } else {
        tmp.sine_freq = 440.0 * pow(2.0, (pitch - 69 + 12) / 12.0);
        tmp.sine_attack = (int)(0.001 * SAMPLE_RATE);
        tmp.total_frames = tmp.note_frames + (int)(0.05 * SAMPLE_RATE);
        /* Detect waveform type from instrument ID */
        tmp.waveform = 0;
        tmp.stereo_detune = false;
        {
            const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
            if (idir) {
                if (strstr(idir, "saw"))       tmp.waveform = 1;
                else if (strstr(idir, "square"))  tmp.waveform = 2;
                else if (strstr(idir, "triangle")) tmp.waveform = 3;
                if (strstr(idir, "stereo"))    tmp.stereo_detune = true;
            }
        }
        /* Per-technique synth modulation defaults */
        tmp.env_attack = 0;
        tmp.env_hold = 0;
        tmp.env_decay = 0;
        tmp.env_sustain = 1.0f;
        tmp.env_release_t = 0.05f * SAMPLE_RATE;
        tmp.pitch_env_depth = 0;
        tmp.pitch_env_attack = 0;
        tmp.pitch_env_decay = 0;
        tmp.lfo_type = 0;
        tmp.lfo_freq = 0;
        tmp.lfo_depth = 0;
        tmp.lfo_attack = 0;
        tmp.vol_offset_db = 0;
        tmp.pitch_offset = 0;

        switch (ntype) {
        case 1:
            tmp.pitch_env_depth = -200.0f;
            tmp.pitch_env_attack = 0.03f * SAMPLE_RATE;
            break;
        case 2:
            tmp.pitch_env_depth = 300.0f;
            tmp.pitch_env_attack = 0.03f * SAMPLE_RATE;
            tmp.vol_offset_db = -2.0f;
            tmp.env_release_t = 0.01f * SAMPLE_RATE;
            break;
        case 3:
            tmp.pitch_env_depth = -200.0f;
            tmp.pitch_env_attack = 0.3f * SAMPLE_RATE;
            break;
        case 4:
            tmp.env_decay = 0.1f * SAMPLE_RATE;
            tmp.env_sustain = 0.0f;
            tmp.env_release_t = 0.5f * SAMPLE_RATE;
            break;
        case 5:
            tmp.env_decay = 0.3f * SAMPLE_RATE;
            tmp.env_sustain = 0.0f;
            tmp.env_release_t = 0.3f * SAMPLE_RATE;
            break;
        case 6:
            tmp.lfo_type = 1;
            tmp.lfo_freq = 18.0f;
            tmp.lfo_depth = 1.0f;
            break;
        case 7:
            tmp.lfo_type = 3;
            tmp.lfo_freq = 8.0f;
            tmp.lfo_depth = 100.0f;
            tmp.lfo_attack = 0;
            tmp.pitch_offset = 50.0f;
            break;
        case 8:
            tmp.lfo_type = 3;
            tmp.lfo_freq = 8.0f;
            tmp.lfo_depth = 200.0f;
            tmp.lfo_attack = 0;
            tmp.pitch_offset = 90.0f;
            break;
        case 17:
            tmp.lfo_type = 3;
            tmp.lfo_freq = 5.9f;
            tmp.lfo_depth = 161.0f;
            tmp.lfo_attack = 3.53f * SAMPLE_RATE;
            break;
        case 18:
            tmp.env_decay = 1.5f * SAMPLE_RATE;
            tmp.env_sustain = 0.1f;
            tmp.env_release_t = 0.5f * SAMPLE_RATE;
            break;
        case 19:
            tmp.env_attack = 0.01f * SAMPLE_RATE;
            break;
        case 20:
            tmp.pitch_env_depth = -100.0f;
            tmp.pitch_env_attack = 0.03f * SAMPLE_RATE;
            tmp.env_attack = 0.03f * SAMPLE_RATE;
            tmp.env_hold = 0.35f * SAMPLE_RATE;
            tmp.env_decay = 0.08f * SAMPLE_RATE;
            tmp.env_sustain = 0.25f;
            tmp.vol_offset_db = 4.0f;
            break;
        case 21:
            tmp.env_decay = 0.3f * SAMPLE_RATE;
            tmp.env_sustain = 0.0f;
            tmp.env_release_t = 0.3f * SAMPLE_RATE;
            tmp.vol_offset_db = 3.0f;
            break;
        default: break;
        }
    }

    /* same velocity->volume as real-time path */
    float vel_vol = (float)velocity / 127.0f;
    float db_gain = powf(10.0f, sr.vol_db / 20.0f);
    tmp.volume = vel_vol * db_gain * layer_vol;
    /* synth waveform attenuation */
    if (!sr.sample || !sr.sample->data || sr.sample->frames <= 0 || force_synth2)
        tmp.volume *= 0.085f;

    /* technique volume offset */
    if (tmp.vol_offset_db != 0.0f)
        tmp.volume *= powf(10.0f, tmp.vol_offset_db / 20.0f);

    /* velocity LPF for offline render */
    {
        const char *idir = inst_dir_name(inst_synth_variant(inst_id, synth_profile));
        float lpf_val = inst_vel_lpf(idir, velocity);
        tmp.lpf_coeff = lpf_coeff_from_wwise(lpf_val);
        tmp.lpf_z1_l = 0.0f;
        tmp.lpf_z1_r = 0.0f;
    }

    /* Set instrument ID for per-instrument release behavior */
    tmp.src_inst_id = inst_id;

    /* Step through frames using voice_render_frame */
    int max_output = tmp.note_frames + (int)(2.0 * SAMPLE_RATE);
    for (int f = 0; f < max_output; f++) {
        int out_idx = offset + f;
        if (out_idx >= total_frames) break;

        float l, r;
        if (!voice_render_frame(&tmp, &l, &r)) break;

        buf[out_idx * 2 + 0] += l;
        buf[out_idx * 2 + 1] += r;
    }
}

bool muse_audio_technique_range(uint8_t inst_id, uint8_t ntype, int *lo, int *hi) {
    const char *dir = inst_dir_name(inst_id);
    if (!dir) return false;
    int found = 0, mn = 127, mx = 0;
    for (int i = 0; i < g_zone_count; i++) {
        if (strcmp(g_zones[i].dir_name, dir) == 0 && g_zones[i].ntype == ntype) {
            if (g_zones[i].key_min < mn) mn = g_zones[i].key_min;
            if (g_zones[i].key_max > mx) mx = g_zones[i].key_max;
            found = 1;
        }
    }
    if (!found) return false;
    *lo = mn; *hi = mx;
    return true;
}

int muse_audio_export_wav(const char *path, MuseProject *proj, int measures) {
    if (!proj || !path) return -1;

    /* total duration from actual note content */
    int bpm = proj->bpm > 0 ? proj->bpm : 120;
    double max_end_ms = 0;
    for (int li = 0; li < proj->num_layers; li++) {
        MuseLayer *ly = &proj->layers[li];
        if (layer_skip(ly, any_solo(proj))) continue;
        for (int si = 0; si < ly->num_sublayers; si++) {
            NoteArray *na = &ly->sublayers[si];
            for (int ni = 0; ni < na->count; ni++) {
                double end = na->notes[ni].start + na->notes[ni].dur;
                if (end > max_end_ms) max_end_ms = end;
            }
        }
    }
    double total_ms = max_end_ms;
    if (total_ms <= 0) total_ms = 1000; /* fallback: 1 second */

    /* 3 second tail for release/reverb tails */
    int tail_samples = (int)(3.0 * SAMPLE_RATE);
    int total_frames = (int)(total_ms / 1000.0 * SAMPLE_RATE) + tail_samples;

    /* Allocate stereo float buffer */
    float *buf = calloc((size_t)total_frames * 2, sizeof(float));
    if (!buf) return -1;

    /* Render all notes and route to FX send buses per-layer */
    bool exp_solo = any_solo(proj);

    float rev_amt = g_fx_reverb ? (float)*g_fx_reverb : 0;
    float dly_amt = g_fx_delay ? (float)*g_fx_delay : 0;
    float cho_fb_val = g_fx_chorus_fb ? (float)*g_fx_chorus_fb : 0;
    float cho_dep = g_fx_chorus_depth ? (float)*g_fx_chorus_depth : 0;
    float cho_frq = g_fx_chorus_freq ? (float)*g_fx_chorus_freq : 0;
    bool need_fx = (rev_amt > 0 || dly_amt > 0 || cho_fb_val > 0 || cho_dep > 0);

    float *rev_bus_l = NULL, *rev_bus_r = NULL, *dly_bus = NULL, *cho_bus = NULL;
    float *layer_buf = NULL;
    if (need_fx) {
        rev_bus_l = calloc((size_t)total_frames, sizeof(float));
        rev_bus_r = calloc((size_t)total_frames, sizeof(float));
        dly_bus   = calloc((size_t)total_frames, sizeof(float));
        cho_bus   = calloc((size_t)total_frames, sizeof(float));
        layer_buf = calloc((size_t)total_frames * 2, sizeof(float));
    }

    for (int li = 0; li < proj->num_layers; li++) {
        MuseLayer *ly = &proj->layers[li];
        if (layer_skip(ly, exp_solo)) continue;
        float vol_scale = ly->volume / 70.0f;

        if (need_fx && layer_buf) {
            memset(layer_buf, 0, (size_t)total_frames * 2 * sizeof(float));
        }
        float *target = (need_fx && layer_buf) ? layer_buf : buf;

        for (int si = 0; si < ly->num_sublayers; si++) {
            NoteArray *na = &ly->sublayers[si];
            for (int ni = 0; ni < na->count; ni++) {
                MuseNote *n = &na->notes[ni];
                int offset = (int)(n->start / 1000.0 * SAMPLE_RATE);
                if (offset >= total_frames) continue;
                render_note_into(target, total_frames, offset,
                                 ly->inst_id, n->pitch, n->vel, n->dur, n->ntype,
                                 vol_scale, ly->synth_profile);
            }
        }

        if (need_fx && layer_buf) {
            float r_send = ly->reverb_send / 100.0f;
            float d_send = ly->delay_send / 100.0f;
            float c_send = ly->chorus_send / 100.0f;
            for (int f = 0; f < total_frames; f++) {
                float l = layer_buf[f * 2 + 0];
                float r = layer_buf[f * 2 + 1];
                buf[f * 2 + 0] += l;
                buf[f * 2 + 1] += r;
                if (r_send > 0 && rev_amt > 0) {
                    rev_bus_l[f] += l * r_send;
                    rev_bus_r[f] += r * r_send;
                }
                float mono = (l + r) * 0.5f;
                if (d_send > 0 && dly_amt > 0) dly_bus[f] += mono * d_send;
                if (c_send > 0 && (cho_fb_val > 0 || cho_dep > 0)) cho_bus[f] += mono * c_send;
            }
        }
    }

    /* Process FX buses and add wet signal to output */
    if (need_fx) {
        ReverbState  rv; reverb_init(&rv);
        DelayState   dl; delay_init(&dl);
        ChorusState  ch; chorus_init(&ch);

        for (int f = 0; f < total_frames; f++) {
            float wet_l = 0, wet_r = 0;
            if (rev_amt > 0) {
                float wet = rev_amt / 100.0f;
                float fb = 0.7f + 0.25f * wet;
                float rl, rr;
                reverb_process(&rv, rev_bus_l[f], rev_bus_r[f], fb, &rl, &rr);
                wet_l += rl * wet;
                wet_r += rr * wet;
            }
            if (dly_amt > 0) {
                float wet = dly_amt / 100.0f;
                float fb = 0.3f + 0.3f * wet;
                float dla, dra;
                delay_process(&dl, dly_bus[f], fb, wet, &dla, &dra);
                wet_l += dla;
                wet_r += dra;
            }
            if (cho_fb_val > 0 || cho_dep > 0) {
                float wet = (cho_fb_val > cho_dep ? cho_fb_val : cho_dep) / 100.0f;
                float c_out = chorus_process(&ch, cho_bus[f], cho_dep, cho_frq, wet);
                wet_l += c_out;
                wet_r += c_out;
            }
            buf[f * 2 + 0] += wet_l;
            buf[f * 2 + 1] += wet_r;
        }
        free(rev_bus_l); free(rev_bus_r); free(dly_bus); free(cho_bus); free(layer_buf);
    }

    /* normalize if too loud */
    {
        float peak = 0;
        for (int i = 0; i < total_frames * 2; i++) {
            float a = fabsf(buf[i]);
            if (a > peak) peak = a;
        }
        if (peak > 0.9f) {
            float scale = 0.9f / peak;
            for (int i = 0; i < total_frames * 2; i++)
                buf[i] *= scale;
        }
    }

    /* Convert to int16 */
    int16_t *pcm = malloc((size_t)total_frames * 2 * sizeof(int16_t));
    if (!pcm) { free(buf); return -1; }
    for (int i = 0; i < total_frames * 2; i++) {
        float s = buf[i] * 32000.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32767.0f) s = -32767.0f;
        pcm[i] = (int16_t)s;
    }
    free(buf);

    /* Write WAV file */
    FILE *f = fopen(path, "wb");
    if (!f) { free(pcm); return -1; }

    uint32_t data_size = (uint32_t)(total_frames * 2 * sizeof(int16_t));
    uint32_t file_size = 36 + data_size;

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1; /* PCM */
    fwrite(&audio_fmt, 2, 1, f);
    uint16_t channels = 2;
    fwrite(&channels, 2, 1, f);
    uint32_t sample_rate = SAMPLE_RATE;
    fwrite(&sample_rate, 4, 1, f);
    uint32_t byte_rate = SAMPLE_RATE * 2 * 2;
    fwrite(&byte_rate, 4, 1, f);
    uint16_t block_align = 4;
    fwrite(&block_align, 2, 1, f);
    uint16_t bits = 16;
    fwrite(&bits, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(pcm, 1, data_size, f);

    fclose(f);
    free(pcm);
    return 0;
}
