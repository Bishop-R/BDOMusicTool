#ifndef MUSE_INSTRUMENTS_H
#define MUSE_INSTRUMENTS_H

#include <stdbool.h>
#include <stdint.h>

#define INST_COUNT       26
#define MAX_TECHNIQUES   16

typedef struct {
    uint8_t  id;
    const char *name;
    uint8_t  pitch_lo;
    uint8_t  pitch_hi;
    bool     is_drum;
    int8_t   oct_offset;  /* display octave offset vs standard (pitch/12-1) */
    uint8_t  color_r, color_g, color_b;
    uint8_t  techniques[MAX_TECHNIQUES];
    int      num_techniques;
} MuseInstrument;

/* technique info, looked up by ntype */
typedef struct {
    uint8_t     id;
    const char *name;
    uint8_t     r, g, b;
} MuseTechnique;

extern const MuseInstrument INSTRUMENTS[INST_COUNT];
extern const MuseTechnique  TECHNIQUES[];
extern const int            TECHNIQUE_COUNT;

const MuseInstrument *inst_by_id(uint8_t id);
const MuseInstrument *inst_at(int index);  /* by array index */
int                   inst_count(void);
const MuseTechnique  *technique_by_id(uint8_t id);
bool                  inst_is_drum(uint8_t id);
bool                  inst_has_technique(uint8_t id, uint8_t ntype);
void                  inst_pitch_range(uint8_t id, int *lo, int *hi);
int                   inst_oct_offset(uint8_t id);
const char           *drum_key_name(uint8_t inst_id, uint8_t pitch);
bool                  inst_is_spacer_key(uint8_t inst_id, uint8_t pitch);
uint8_t               inst_synth_variant(uint8_t base_id, uint8_t profile);

#endif /* MUSE_INSTRUMENTS_H */
