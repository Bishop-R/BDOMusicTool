#include "midi_import.h"
#include "instruments.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* MIDI file parser - handles format 0 and 1 with all the fun edge cases */

static uint16_t read_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_vlq(const uint8_t *p, int *bytes_read) {
    uint32_t val = 0;
    int i = 0;
    do {
        val = (val << 7) | (p[i] & 0x7F);
    } while (p[i++] & 0x80);
    *bytes_read = i;
    return val;
}

typedef struct {
    uint8_t  pitch, vel;
    double   start_tick;
} PendingNote;

/* note-off happened but sustain pedal was down, so it keeps ringing */
typedef struct {
    uint8_t  pitch, vel;
    double   start_tick;
} SustainedNote;

#define MAX_SUSTAINED 256

/* Best-effort GM program to BDO instrument mapping */
static uint8_t gm_to_bdo(uint8_t prog) {
    if (prog == 0)        return 0x11; /* Acoustic Grand Piano */
    if (prog <= 7)        return 0x07; /* Piano family */
    if (prog <= 15)       return 0x11; /* Chromatic Perc */
    if (prog <= 23)       return 0x10; /* Organ -> Harp */
    if (prog <= 31)       return 0x0A; /* Guitar */
    if (prog <= 39)       return 0x0E; /* Bass -> Marnibass */
    if (prog <= 47)       return 0x12; /* Strings -> Violin */
    if (prog <= 55)       return 0x12; /* Ensemble -> Violin */
    if (prog <= 63)       return 0x28; /* Brass -> Horn */
    if (prog <= 71)       return 0x27; /* Reed -> Clarinet */
    if (prog <= 79)       return 0x0B; /* Pipe -> Flute */
    if (prog <= 87)       return 0x14; /* Synth Lead -> Wavy */
    if (prog <= 95)       return 0x18; /* Synth Pad -> Illusion */
    return 0x11;
}

/* full GM program name table, all 128 of them */
static const char *gm_program_name(uint8_t prog) {
    static const char *names[] = {
        /* 0-7: Piano */
        "Acoustic Grand", "Bright Acoustic", "Electric Grand", "Honky-Tonk",
        "EP 1", "EP 2", "Harpsichord", "Clavinet",
        /* 8-15: Chromatic Percussion */
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
        "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        /* 16-23: Organ */
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
        "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        /* 24-31: Guitar */
        "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar",
        "Muted Guitar", "Overdrive Guitar", "Distortion Guitar", "Harmonics",
        /* 32-39: Bass */
        "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass",
        "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        /* 40-47: Strings */
        "Violin", "Viola", "Cello", "Contrabass",
        "Tremolo Strings", "Pizzicato", "Orchestral Harp", "Timpani",
        /* 48-55: Ensemble */
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
        "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        /* 56-63: Brass */
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
        "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        /* 64-71: Reed */
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
        "Oboe", "English Horn", "Bassoon", "Clarinet",
        /* 72-79: Pipe */
        "Piccolo", "Flute", "Recorder", "Pan Flute",
        "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        /* 80-87: Synth Lead */
        "Square Lead", "Sawtooth Lead", "Calliope Lead", "Chiff Lead",
        "Charang Lead", "Voice Lead", "Fifths Lead", "Bass+Lead",
        /* 88-95: Synth Pad */
        "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad",
        "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
        /* 96-103: Synth Effects */
        "FX 1 (Rain)", "FX 2 (Soundtrack)", "FX 3 (Crystal)", "FX 4 (Atmosphere)",
        "FX 5 (Brightness)", "FX 6 (Goblins)", "FX 7 (Echoes)", "FX 8 (Sci-fi)",
        /* 104-111: Ethnic */
        "Sitar", "Banjo", "Shamisen", "Koto",
        "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        /* 112-119: Percussive */
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
        "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        /* 120-127: Sound Effects */
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
        "Telephone Ring", "Helicopter", "Applause", "Gunshot",
    };
    if (prog < 128) return names[prog];
    return "Unknown";
}

/* push a completed note - stores tick positions in start/dur for now,
   converted to ms in midi_apply using the tempo map */
static void emit_note(MidiChannel *ch, uint8_t pitch, uint8_t vel, double start_tick, double end_tick) {
    double dur = end_tick - start_tick;
    if (dur < 1) dur = 1;
    MuseNote note = {
        .pitch = pitch, .vel = vel, .ntype = 0,
        .start = start_tick, .dur = dur,
    };
    note_array_push(&ch->notes, note);
}

/* shift notes into BDO's playable range (24-108) by octaves */
static void octave_shift_notes(NoteArray *na) {
    for (int i = 0; i < na->count; i++) {
        MuseNote *n = &na->notes[i];
        while (n->pitch < 24 && n->pitch + 12 <= 108) n->pitch += 12;
        while (n->pitch > 108 && n->pitch - 12 >= 24) n->pitch -= 12;
        if (n->pitch < 24) n->pitch = 24;
        if (n->pitch > 108) n->pitch = 108;
    }
}

/* GM drum (35-81) -> BDO Drum Set (48-64) mapping */
static uint8_t gm_to_bdo_drum(uint8_t gm) {
    switch (gm) {
    case 35: case 36: return 48; /* Kick */
    case 37:          return 51; /* Side Stick -> Rim Shot */
    case 38: case 40: return 50; /* Snare -> Snare Hit */
    case 39:          return 52; /* Clap -> Snare Flam */
    case 41:          return 59; /* Low Tom -> Tom 4 */
    case 42:          return 54; /* HiHat Closed */
    case 43:          return 57; /* Low Tom -> Tom 3 */
    case 44:          return 56; /* HiHat Pedal */
    case 45:          return 55; /* Mid Tom -> Tom 2 */
    case 46:          return 58; /* HiHat Open */
    case 47:          return 53; /* Mid Tom -> Tom 1 */
    case 48:          return 60; /* High Tom -> Tom 5 */
    case 49: case 57: return 61; /* Crash */
    case 50:          return 53; /* High Tom -> Tom 1 */
    case 51: case 59: return 62; /* Ride */
    case 52:          return 61; /* Chinese Cym -> Crash */
    case 53:          return 62; /* Ride Bell -> Ride */
    case 54:          return 61; /* Tambourine -> Crash */
    case 55:          return 61; /* Splash -> Crash */
    case 56:          return 51; /* Cowbell -> Rim Shot */
    case 58:          return 61; /* Vibraslap -> Crash */
    default:
        if (gm < 48) return 48; /* below range -> kick */
        if (gm > 64) return 62; /* above range -> ride */
        return gm; /* already in BDO range */
    }
}

static void remap_drum_notes(NoteArray *na) {
    for (int i = 0; i < na->count; i++)
        na->notes[i].pitch = gm_to_bdo_drum(na->notes[i].pitch);
}

/* tempo map entry for tick-to-ms conversion */
typedef struct { uint32_t tick; double us_per_tick; } TempoEvent;
#define MAX_TEMPO_EVENTS 1024
static TempoEvent g_tempo_map[MAX_TEMPO_EVENTS];
static int g_tempo_map_count;

static double tick_to_ms(uint32_t tick) {
    double ms = 0;
    uint32_t prev_tick = 0;
    double upt = g_tempo_map[0].us_per_tick;
    for (int i = 1; i < g_tempo_map_count; i++) {
        if (g_tempo_map[i].tick >= tick) break;
        ms += (g_tempo_map[i].tick - prev_tick) * upt / 1000.0;
        prev_tick = g_tempo_map[i].tick;
        upt = g_tempo_map[i].us_per_tick;
    }
    ms += (tick - prev_tick) * upt / 1000.0;
    return ms;
}

MidiImportData *midi_parse(const char *path) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 14) { fclose(f); return NULL; }

    uint8_t *data = malloc((size_t)fsize);
    fread(data, 1, (size_t)fsize, f);
    fclose(f);

    if (memcmp(data, "MThd", 4) != 0) { free(data); return NULL; }
    uint32_t hdr_len = read_be32(data + 4);
    uint16_t ntracks = read_be16(data + 10);
    uint16_t tpqn    = read_be16(data + 12);

    if (tpqn & 0x8000) { free(data); return NULL; }

    MidiImportData *mid = calloc(1, sizeof(*mid));
    mid->bpm = 120;
    mid->tpqn = tpqn;
    mid->time_sig = 4;
    mid->tempo_changes = 0;

    /* 16 MIDI channel slots - group notes by channel, compact later */
    MidiChannel slots[16];
    memset(slots, 0, sizeof(slots));
    uint8_t slot_program[16];
    memset(slot_program, 0, sizeof(slot_program));
    bool slot_has_program[16];
    memset(slot_has_program, 0, sizeof(slot_has_program));

    for (int i = 0; i < 16; i++) {
        slots[i].midi_ch = (uint8_t)i;
        slots[i].is_percussion = (i == 9); /* ch 9 is always drums in MIDI */
        /* default to drums for ch9, piano for everything else */
        if (i == 9) {
            slots[i].auto_inst_id = 0x0D; /* Drum Set */
            slots[i].user_inst_id = 0x0D;
        } else {
            slots[i].auto_inst_id = 0x11; /* Piano */
            slots[i].user_inst_id = 0x11;
        }
    }

    size_t off = 8 + hdr_len;

    /* build a tempo map from all tracks first (Type 1 MIDIs put tempo in track 0) */
    g_tempo_map_count = 0;
    g_tempo_map[0].tick = 0;
    g_tempo_map[0].us_per_tick = 500000.0 / tpqn;
    g_tempo_map_count = 1;

    {
        size_t scan_off = 8 + hdr_len;
        for (int t = 0; t < ntracks; t++) {
            if (scan_off + 8 > (size_t)fsize) break;
            if (memcmp(data + scan_off, "MTrk", 4) != 0) break;
            uint32_t trk_len = read_be32(data + scan_off + 4);
            size_t trk_start = scan_off + 8;
            size_t trk_end = trk_start + trk_len;
            if (trk_end > (size_t)fsize) trk_end = (size_t)fsize;
            size_t pos = trk_start;
            uint32_t abs_tick = 0;
            uint8_t rs = 0;
            while (pos < trk_end) {
                int vlq_bytes;
                uint32_t delta = read_vlq(data + pos, &vlq_bytes);
                pos += vlq_bytes;
                abs_tick += delta;
                if (pos >= trk_end) break;
                uint8_t st = data[pos];
                if (st & 0x80) { rs = st; pos++; } else { st = rs; }
                uint8_t tp = st & 0xF0;
                if (st == 0xFF) {
                    if (pos >= trk_end) break;
                    uint8_t mt = data[pos++];
                    int lb;
                    uint32_t ml = read_vlq(data + pos, &lb);
                    pos += lb;
                    if (mt == 0x51 && ml == 3 && pos + 3 <= trk_end) {
                        uint32_t uspq = ((uint32_t)data[pos]<<16)|((uint32_t)data[pos+1]<<8)|data[pos+2];
                        if (g_tempo_map_count < MAX_TEMPO_EVENTS) {
                            g_tempo_map[g_tempo_map_count].tick = abs_tick;
                            g_tempo_map[g_tempo_map_count].us_per_tick = (double)uspq / tpqn;
                            g_tempo_map_count++;
                        }
                        mid->tempo_changes++;
                    }
                    pos += ml;
                } else if (st == 0xF0 || st == 0xF7) {
                    int lb; uint32_t sl = read_vlq(data + pos, &lb); pos += lb + sl;
                } else if (tp == 0xC0 || tp == 0xD0) { pos += 1; }
                else { pos += 2; }
            }
            scan_off = trk_end;
        }
    }
    /* use the first real tempo event as BPM (fall back to default if none) */
    {
        /* find the last tempo event at tick 0, or the first one overall */
        int best = 0;
        for (int i = 1; i < g_tempo_map_count; i++) {
            if (g_tempo_map[i].tick == 0) best = i;
            else break;
        }
        uint32_t first_uspq = (uint32_t)(g_tempo_map[best].us_per_tick * tpqn);
        if (first_uspq > 0)
            mid->bpm = (uint16_t)(60000000.0 / first_uspq);
    }


    for (int t = 0; t < ntracks; t++) {
        if (off + 8 > (size_t)fsize) break;
        if (memcmp(data + off, "MTrk", 4) != 0) break;
        uint32_t trk_len = read_be32(data + off + 4);
        size_t trk_start = off + 8;
        size_t trk_end = trk_start + trk_len;
        if (trk_end > (size_t)fsize) trk_end = (size_t)fsize;

        /* pending note-on events, flat array: channel * 128 + pitch */
        PendingNote pending[16 * 128];
        int pending_active[16 * 128];
        memset(pending, 0, sizeof(pending));
        memset(pending_active, 0, sizeof(pending_active));

        /* sustain pedal tracking per channel */
        bool sustain[16];
        memset(sustain, 0, sizeof(sustain));

        /* notes waiting for pedal release */
        SustainedNote sustained_notes[16][MAX_SUSTAINED];
        int sustained_count[16];
        memset(sustained_notes, 0, sizeof(sustained_notes));
        memset(sustained_count, 0, sizeof(sustained_count));

        uint32_t abs_tick = 0;
        uint8_t running_status = 0;
        size_t pos = trk_start;

        while (pos < trk_end) {
            int vlq_bytes;
            uint32_t delta = read_vlq(data + pos, &vlq_bytes);
            pos += vlq_bytes;
            abs_tick += delta;
            (void)0; /* tick_to_ms conversion deferred to midi_apply */

            if (pos >= trk_end) break;
            uint8_t status = data[pos];
            if (status & 0x80) {
                running_status = status;
                pos++;
            } else {
                status = running_status;
            }

            uint8_t type = status & 0xF0;
            uint8_t midi_ch = status & 0x0F;

            /* note-off handler - accounts for sustain pedal being held */
            #define DO_NOTE_OFF(p) do { \
                int pidx_ = midi_ch * 128 + (p); \
                if (pending_active[pidx_]) { \
                    if (sustain[midi_ch]) { \
                        if (sustained_count[midi_ch] < MAX_SUSTAINED) { \
                            sustained_notes[midi_ch][sustained_count[midi_ch]++] = (SustainedNote){ \
                                .pitch = pending[pidx_].pitch, \
                                .vel = pending[pidx_].vel, \
                                .start_tick = pending[pidx_].start_tick, \
                            }; \
                        } \
                        pending_active[pidx_] = 0; \
                    } else { \
                        emit_note(&slots[midi_ch], pending[pidx_].pitch, \
                                  pending[pidx_].vel, pending[pidx_].start_tick, (double)abs_tick); \
                        pending_active[pidx_] = 0; \
                    } \
                } \
            } while(0)

            switch (type) {
            case 0x90: { /* Note On */
                if (pos + 1 >= trk_end) { pos = trk_end; break; }
                uint8_t pitch = data[pos++];
                uint8_t vel   = data[pos++];
                int pidx = midi_ch * 128 + pitch;
                MidiChannel *ch = &slots[midi_ch];

                if (vel > 0) {
                    /* if this pitch is sustained, end it before starting a new one */
                    for (int si = 0; si < sustained_count[midi_ch]; si++) {
                        if (sustained_notes[midi_ch][si].pitch == pitch) {
                            emit_note(ch,
                                      sustained_notes[midi_ch][si].pitch,
                                      sustained_notes[midi_ch][si].vel,
                                      sustained_notes[midi_ch][si].start_tick,
                                      (double)abs_tick);
                            /* swap-remove from sustained list */
                            sustained_count[midi_ch]--;
                            sustained_notes[midi_ch][si] = sustained_notes[midi_ch][sustained_count[midi_ch]];
                            break;
                        }
                    }
                    /* end any currently sounding instance too */
                    if (pending_active[pidx]) {
                        emit_note(ch, pitch, pending[pidx].vel,
                                  pending[pidx].start_tick, (double)abs_tick);
                        pending_active[pidx] = 0;
                    }
                    pending[pidx] = (PendingNote){ pitch, vel, (double)abs_tick };
                    pending_active[pidx] = 1;
                } else {
                    /* vel 0 = note-off, classic MIDI quirk */
                    DO_NOTE_OFF(pitch);
                }
                break;
            }
            case 0x80: { /* Note Off */
                if (pos + 1 >= trk_end) { pos = trk_end; break; }
                uint8_t pitch = data[pos++];
                pos++; /* vel */
                DO_NOTE_OFF(pitch);
                break;
            }

            #undef DO_NOTE_OFF
            case 0xA0: pos += 2; break; /* Aftertouch */
            case 0xB0: { /* Control Change */
                if (pos + 1 >= trk_end) { pos = trk_end; break; }
                uint8_t cc_num = data[pos++];
                uint8_t cc_val = data[pos++];

                /* CC64 = sustain pedal */
                if (cc_num == 64) {
                    if (cc_val >= 64) {
                        sustain[midi_ch] = true;
                    } else {
                        sustain[midi_ch] = false;
                        /* pedal up - release everything that was being held */
                        for (int si = 0; si < sustained_count[midi_ch]; si++) {
                            emit_note(&slots[midi_ch],
                                      sustained_notes[midi_ch][si].pitch,
                                      sustained_notes[midi_ch][si].vel,
                                      sustained_notes[midi_ch][si].start_tick,
                                      (double)abs_tick);
                        }
                        sustained_count[midi_ch] = 0;
                    }
                }
                break;
            }
            case 0xC0: { /* Program Change */
                if (pos >= trk_end) { pos = trk_end; break; }
                uint8_t prog = data[pos++];
                slot_program[midi_ch] = prog;
                slot_has_program[midi_ch] = true;
                /* don't let program changes mess with the drum channel */
                if (midi_ch != 9) {
                    slots[midi_ch].auto_inst_id = gm_to_bdo(prog);
                    slots[midi_ch].user_inst_id = slots[midi_ch].auto_inst_id;
                }
                slots[midi_ch].gm_program = prog;
                break;
            }
            case 0xD0: pos += 1; break; /* Channel Pressure */
            case 0xE0: pos += 2; break; /* Pitch Bend */
            case 0xF0: { /* System / Meta */
                if (status == 0xFF) {
                    if (pos >= trk_end) { pos = trk_end; break; }
                    uint8_t meta_type = data[pos++];
                    int len_bytes;
                    uint32_t meta_len = read_vlq(data + pos, &len_bytes);
                    pos += len_bytes;
                    if (meta_type == 0x51 && meta_len == 3 && pos + 3 <= trk_end) {
                        /* tempo handled by tempo map built above */
                    } else if (meta_type == 0x58 && meta_len >= 2 && pos + 2 <= trk_end) {
                        mid->time_sig = data[pos]; /* numerator */
                    }
                    pos += meta_len;
                } else if (status == 0xF0 || status == 0xF7) {
                    int len_bytes;
                    uint32_t sysex_len = read_vlq(data + pos, &len_bytes);
                    pos += len_bytes + sysex_len;
                }
                break;
            }
            default: break;
            }
        }

        /* clean up any notes that never got a note-off */
        double end_tick = (double)abs_tick;
        for (int i = 0; i < 16 * 128; i++) {
            if (pending_active[i]) {
                int ch_idx = i / 128;
                emit_note(&slots[ch_idx], pending[i].pitch, pending[i].vel,
                          pending[i].start_tick, end_tick);
            }
        }
        /* flush any notes still held by sustain pedal */
        for (int c = 0; c < 16; c++) {
            for (int si = 0; si < sustained_count[c]; si++) {
                emit_note(&slots[c],
                          sustained_notes[c][si].pitch,
                          sustained_notes[c][si].vel,
                          sustained_notes[c][si].start_tick,
                          end_tick);
            }
            sustained_count[c] = 0;
        }

        off = trk_end;
    }

    /* compact - only keep channels that actually have notes */
    mid->num_channels = 0;
    for (int i = 0; i < 16 && mid->num_channels < MIDI_MAX_CHANNELS; i++) {
        slots[i].note_count = slots[i].notes.count;
        if (slots[i].note_count > 0) {
            /* shift into BDO's pitch range */
            if (slots[i].is_percussion)
                remap_drum_notes(&slots[i].notes);
            else
                octave_shift_notes(&slots[i].notes);

            uint8_t gm_prog = slot_has_program[i] ? slot_program[i] : 0;
            const char *perc_label = slots[i].is_percussion ? "Drums" : gm_program_name(gm_prog);
            snprintf(slots[i].label, sizeof(slots[i].label), "Ch %d: %s (%d notes)",
                     i + 1, perc_label, slots[i].note_count);

            mid->channels[mid->num_channels] = slots[i];
            mid->num_channels++;
        } else {
            /* nothing here, free it */
            free(slots[i].notes.notes);
        }
    }


    /* default combine instrument to Flor. Piano */
    mid->combine_inst_id = 0x11;

    /* default velocity mode */
    mid->vel_mode = VEL_MODE_LAYERED;
    mid->vel_min = 80;
    mid->vel_max = 127;

    free(data);
    return mid;
}

/* ---- Velocity processing ---- */

static void vel_rescale(NoteArray *na, int vmin, int vmax) {
    int src_min = 127, src_max = 0;
    for (int i = 0; i < na->count; i++) {
        if (na->notes[i].ntype != 0) continue;
        if (na->notes[i].vel < src_min) src_min = na->notes[i].vel;
        if (na->notes[i].vel > src_max) src_max = na->notes[i].vel;
    }
    if (src_min == src_max) {
        int flat = (vmin + vmax) / 2;
        for (int i = 0; i < na->count; i++)
            if (na->notes[i].ntype == 0) na->notes[i].vel = (uint8_t)flat;
        return;
    }
    for (int i = 0; i < na->count; i++) {
        if (na->notes[i].ntype != 0) continue;
        float scaled = (float)vmin + (float)(na->notes[i].vel - src_min)
                     / (float)(src_max - src_min) * (float)(vmax - vmin);
        int v = (int)(scaled + 0.5f);
        if (v < 1) v = 1; if (v > 127) v = 127;
        na->notes[i].vel = (uint8_t)v;
    }
}

static const int BDO_VEL_LEVELS[] = { 80, 90, 100, 121 };
#define BDO_VEL_NLEVELS 4

static void vel_layered(NoteArray *na) {
    /* collect unique velocities */
    uint8_t seen[128] = {0};
    int unique[128], nunique = 0;
    for (int i = 0; i < na->count; i++) {
        if (na->notes[i].ntype != 0) continue;
        if (!seen[na->notes[i].vel]) {
            seen[na->notes[i].vel] = 1;
            unique[nunique++] = na->notes[i].vel;
        }
    }
    if (nunique == 0) return;
    /* sort */
    for (int i = 0; i < nunique - 1; i++)
        for (int j = i + 1; j < nunique; j++)
            if (unique[j] < unique[i]) { int t = unique[i]; unique[i] = unique[j]; unique[j] = t; }
    /* build map: distribute evenly across BDO levels */
    uint8_t vel_map[128] = {0};
    if (nunique == 1) {
        vel_map[unique[0]] = (uint8_t)BDO_VEL_LEVELS[BDO_VEL_NLEVELS / 2];
    } else {
        for (int i = 0; i < nunique; i++) {
            int idx = (int)((float)i / (float)(nunique - 1) * (float)(BDO_VEL_NLEVELS - 1) + 0.5f);
            vel_map[unique[i]] = (uint8_t)BDO_VEL_LEVELS[idx];
        }
    }
    /* apply */
    for (int i = 0; i < na->count; i++) {
        if (na->notes[i].ntype != 0) continue;
        if (vel_map[na->notes[i].vel])
            na->notes[i].vel = vel_map[na->notes[i].vel];
    }
}

static void apply_vel_mode(MidiImportData *mid, NoteArray *na) {
    switch (mid->vel_mode) {
        case VEL_MODE_RAW:     break;
        case VEL_MODE_RESCALE: vel_rescale(na, mid->vel_min, mid->vel_max); break;
        case VEL_MODE_LAYERED: vel_layered(na); break;
        default: break;
    }
}

void midi_apply(MidiImportData *mid, MuseProject *out) {
    muse_project_free(out);
    muse_project_init(out);

    out->bpm = mid->bpm;
    if (mid->time_sig >= 2 && mid->time_sig <= 8)
        out->time_sig = mid->time_sig;

    /* convert tick positions to ms using the tempo map, then compress if needed */
    for (int i = 0; i < mid->num_channels; i++) {
        MidiChannel *ch = &mid->channels[i];
        for (int n = 0; n < ch->notes.count; n++) {
            MuseNote *note = &ch->notes.notes[n];
            double start_tick = note->start;
            double dur_ticks = note->dur;
            {
                note->start = tick_to_ms((uint32_t)start_tick);
                double end_ms = tick_to_ms((uint32_t)(start_tick + dur_ticks));
                note->dur = end_ms - note->start;
            }
            if (note->dur < 1) note->dur = 1;
        }
    }

    /* apply velocity processing */
    for (int i = 0; i < mid->num_channels; i++) {
        MidiChannel *ch = &mid->channels[i];
        if (ch->note_count == 0 || ch->is_percussion) continue;
        apply_vel_mode(mid, &ch->notes);
    }

    for (int i = 0; i < mid->num_channels; i++) {
        MidiChannel *ch = &mid->channels[i];
        if (ch->note_count == 0) continue;

        uint8_t eff_inst = ch->user_inst_id;
        if (mid->combine_all && !ch->is_percussion)
            eff_inst = mid->combine_inst_id;

        int target = -1;
        for (int li = 0; li < out->num_layers; li++) {
            if (out->layers[li].inst_id == eff_inst) {
                target = li;
                break;
            }
        }
        if (target < 0) {
            target = muse_project_add_layer(out, eff_inst);
        }

        NoteArray *dst = &out->layers[target].sublayers[0];
        for (int n = 0; n < ch->notes.count; n++) {
            note_array_push(dst, ch->notes.notes[n]);
        }
    }

    out->dirty = true;
}

void midi_import_data_free(MidiImportData *mid) {
    if (!mid) return;
    for (int i = 0; i < mid->num_channels; i++)
        free(mid->channels[i].notes.notes);
    free(mid);
}

/* one-shot import for drag-and-drop */
int midi_import(const char *path, MuseProject *out) {
    MidiImportData *mid = midi_parse(path);
    if (!mid) return -1;
    midi_apply(mid, out);
    midi_import_data_free(mid);
    return 0;
}
