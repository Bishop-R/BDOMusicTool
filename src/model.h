#ifndef MUSE_MODEL_H
#define MUSE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/* single note */
typedef struct {
    uint8_t  pitch;
    uint8_t  vel;
    uint8_t  ntype;
    uint8_t  selected;
    double   start;   /* milliseconds */
    double   dur;     /* milliseconds */
} MuseNote;

/* growable note array */
typedef struct {
    MuseNote *notes;
    int       count;
    int       capacity;
} NoteArray;

void  note_array_init(NoteArray *a);
void  note_array_free(NoteArray *a);
void  note_array_push(NoteArray *a, MuseNote n);
void  note_array_remove(NoteArray *a, int idx);
void  note_array_clear(NoteArray *a);

/* one instrument layer (maps to a BDO "group") */
typedef struct {
    uint8_t    inst_id;
    uint8_t    synth_profile;
    uint8_t    volume;
    bool       muted;
    bool       solo;
    uint8_t    reverb_send;
    uint8_t    delay_send;
    uint8_t    chorus_send;
    NoteArray *sublayers;
    int        num_sublayers;
    int        active_sub;
} MuseLayer;

#define MAX_NOTES_PER_INSTRUMENT 10000

/* total notes across all sublayers */
int muse_layer_note_count(const MuseLayer *ly);

/* where does the 10k note limit get exceeded? returns -1 if we're fine */
double muse_layer_exceed_ms(const MuseLayer *ly);

/* the whole project */
typedef struct {
    MuseLayer *layers;
    int        num_layers;
    int        active_layer;
    uint16_t   bpm;
    uint16_t   time_sig;
    uint32_t   owner_id;
    char       char_name[64];
    bool       dirty;
    /* global effect knobs - pulled from BDO track settings bytes */
    uint8_t    effector_reverb;
    uint8_t    effector_delay;
    uint8_t    effector_chorus_fb;
    uint8_t    effector_chorus_depth;
    uint8_t    effector_chorus_freq;
} MuseProject;

void muse_project_init(MuseProject *p);
void muse_project_free(MuseProject *p);
int  muse_project_add_layer(MuseProject *p, uint8_t inst_id);
void muse_project_remove_layer(MuseProject *p, int idx);

/* sublayer helpers */
int  muse_layer_add_sublayer(MuseLayer *ly);   /* returns new index, or -1 if max(5) */
void muse_layer_remove_sublayer(MuseLayer *ly, int idx);

#endif /* MUSE_MODEL_H */
