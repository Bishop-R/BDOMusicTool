#ifndef COMPOSITION_TOOLS_H
#define COMPOSITION_TOOLS_H

#include "model.h"

int notes_quantize(NoteArray *a, double beat_ms, int divisions, int strength_pct,
                   int swing_pct, bool quantize_ends);
int notes_humanize(NoteArray *a, int timing_ms, int velocity_amount,
                   int length_pct, uint32_t seed);
int notes_arpeggiate(NoteArray *a, double beat_ms, int divisions,
                     int octaves, int gate_pct);
int notes_strum(NoteArray *a, int spread_ms, int velocity_taper_pct,
                int jitter_ms, uint32_t seed, bool upward);

#endif
