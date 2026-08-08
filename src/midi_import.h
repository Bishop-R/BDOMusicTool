#ifndef MUSE_MIDI_IMPORT_H
#define MUSE_MIDI_IMPORT_H

#include "model.h"

/* quick import - no dialog, just dump it in */
int midi_import(const char *path, MuseProject *out);

/* two-phase import: parse first, let user pick instruments, then apply */
#define MIDI_MAX_CHANNELS 32

typedef struct {
    char      label[64];       /* "Ch 1: Acoustic Grand (42 notes)" */
    uint8_t   auto_inst_id;    /* auto-detected BDO instrument */
    uint8_t   user_inst_id;    /* user-selected BDO instrument */
    uint8_t   user_ntype;      /* user-selected technique (dev builds only) */
    uint8_t   user_volume;     /* proportional volume 0-100, default 100 */
    uint8_t   midi_ch;         /* source MIDI channel (0-15) */
    uint8_t   gm_program;      /* GM program number from program change */
    bool      is_percussion;   /* true if MIDI channel 9 (drums) */
    bool      synth_emulate;   /* convert drum hits to synth notes */
    int       note_count;
    NoteArray notes;
} MidiChannel;

typedef enum {
    VEL_MODE_RAW,       /* keep original MIDI velocities */
    VEL_MODE_RESCALE,   /* rescale to min-max range */
    VEL_MODE_LAYERED,   /* map to BDO-optimized levels [80, 90, 100, 121] */
    VEL_MODE_COUNT
} VelMode;

typedef struct {
    int         num_channels;
    MidiChannel channels[MIDI_MAX_CHANNELS];
    uint16_t    bpm;
    uint16_t    time_sig;
    uint16_t    tpqn;
    int         tempo_changes;
    bool        combine_all;
    uint8_t     combine_inst_id;
    VelMode     vel_mode;
    int         vel_min, vel_max;    /* for VEL_MODE_RESCALE */
    bool        has_orig;            /* true after first apply saves originals */
    NoteArray   orig_notes[MIDI_MAX_CHANNELS]; /* pre-conversion backup */
} MidiImportData;

/* parse MIDI into channel data for the import dialog */
MidiImportData *midi_parse(const char *path);

/* apply parsed data with the user's instrument choices */
void midi_apply(MidiImportData *mid, MuseProject *out);

/* clean up */
void midi_import_data_free(MidiImportData *mid);

#endif /* MUSE_MIDI_IMPORT_H */
