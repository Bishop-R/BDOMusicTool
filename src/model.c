#include "model.h"
#include <stdlib.h>
#include <string.h>

/* note array operations */

void note_array_init(NoteArray *a) {
    a->notes    = NULL;
    a->count    = 0;
    a->capacity = 0;
}

void note_array_free(NoteArray *a) {
    free(a->notes);
    a->notes    = NULL;
    a->count    = 0;
    a->capacity = 0;
}

void note_array_push(NoteArray *a, MuseNote n) {
    if (a->count >= a->capacity) {
        int cap = a->capacity ? a->capacity * 2 : 64;
        a->notes = realloc(a->notes, (size_t)cap * sizeof(MuseNote));
        a->capacity = cap;
    }
    a->notes[a->count++] = n;
}

void note_array_remove(NoteArray *a, int idx) {
    if (idx < 0 || idx >= a->count) return;
    memmove(&a->notes[idx], &a->notes[idx + 1],
            (size_t)(a->count - idx - 1) * sizeof(MuseNote));
    a->count--;
}

void note_array_clear(NoteArray *a) {
    a->count = 0;
}

int muse_layer_note_count(const MuseLayer *ly) {
    int total = 0;
    for (int s = 0; s < ly->num_sublayers; s++)
        total += ly->sublayers[s].count;
    return total;
}

static int cmp_note_start(const void *a, const void *b) {
    double da = ((const MuseNote *)a)->start;
    double db = ((const MuseNote *)b)->start;
    return (da > db) - (da < db);
}

double muse_layer_exceed_ms(const MuseLayer *ly) {
    int total = muse_layer_note_count(ly);
    if (total <= MAX_NOTES_PER_INSTRUMENT) return -1.0;

    /* grab all notes, sort them, find where the 10001st one starts */
    MuseNote *all = malloc((size_t)total * sizeof(MuseNote));
    if (!all) return -1.0;
    int idx = 0;
    for (int s = 0; s < ly->num_sublayers; s++)
        for (int n = 0; n < ly->sublayers[s].count; n++)
            all[idx++] = ly->sublayers[s].notes[n];
    qsort(all, (size_t)total, sizeof(MuseNote), cmp_note_start);
    double ms = all[MAX_NOTES_PER_INSTRUMENT].start;
    free(all);
    return ms;
}

/* project management */

void muse_project_init(MuseProject *p) {
    memset(p, 0, sizeof(*p));
    p->bpm      = 120;
    p->time_sig = 4;
}

void muse_project_free(MuseProject *p) {
    for (int i = 0; i < p->num_layers; i++) {
        MuseLayer *ly = &p->layers[i];
        for (int s = 0; s < ly->num_sublayers; s++)
            note_array_free(&ly->sublayers[s]);
        free(ly->sublayers);
    }
    free(p->layers);
    memset(p, 0, sizeof(*p));
}

int muse_project_add_layer(MuseProject *p, uint8_t inst_id) {
    int idx = p->num_layers;
    p->layers = realloc(p->layers, (size_t)(idx + 1) * sizeof(MuseLayer));
    MuseLayer *ly = &p->layers[idx];
    memset(ly, 0, sizeof(*ly));
    ly->inst_id       = inst_id;
    ly->volume         = 70;   /* 0x46, what BDO defaults to */
    /* marnian synths get different default profiles */
    if (inst_id == 0x14 || inst_id == 0x18)
        ly->synth_profile = 2;  /* Super */
    else if (inst_id == 0x1C || inst_id == 0x20)
        ly->synth_profile = 1;  /* Stereo */
    ly->num_sublayers  = 1;
    ly->sublayers      = calloc(1, sizeof(NoteArray));
    note_array_init(&ly->sublayers[0]);
    p->num_layers++;
    return idx;
}

void muse_project_remove_layer(MuseProject *p, int idx) {
    if (idx < 0 || idx >= p->num_layers) return;
    MuseLayer *ly = &p->layers[idx];
    for (int s = 0; s < ly->num_sublayers; s++)
        note_array_free(&ly->sublayers[s]);
    free(ly->sublayers);
    memmove(&p->layers[idx], &p->layers[idx + 1],
            (size_t)(p->num_layers - idx - 1) * sizeof(MuseLayer));
    p->num_layers--;
    if (p->active_layer >= p->num_layers && p->num_layers > 0)
        p->active_layer = p->num_layers - 1;
}

int muse_layer_add_sublayer(MuseLayer *ly) {
    if (ly->num_sublayers >= 16) return -1;
    int idx = ly->num_sublayers;
    ly->sublayers = realloc(ly->sublayers, (size_t)(idx + 1) * sizeof(NoteArray));
    note_array_init(&ly->sublayers[idx]);
    ly->num_sublayers++;
    ly->active_sub = idx;
    return idx;
}

void muse_layer_remove_sublayer(MuseLayer *ly, int idx) {
    if (ly->num_sublayers <= 1 || idx < 0 || idx >= ly->num_sublayers) return;
    note_array_free(&ly->sublayers[idx]);
    memmove(&ly->sublayers[idx], &ly->sublayers[idx + 1],
            (size_t)(ly->num_sublayers - idx - 1) * sizeof(NoteArray));
    ly->num_sublayers--;
    if (ly->active_sub >= ly->num_sublayers)
        ly->active_sub = ly->num_sublayers - 1;
}
