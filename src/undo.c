#include "undo.h"
#include <stdlib.h>
#include <string.h>

#define MAX_UNDO 100

/* deep copy of the project state for undo/redo */
typedef struct {
    MuseLayer *layers;
    int        num_layers, active_layer;
    uint16_t   bpm, time_sig;
    uint32_t   owner_id;
} Snapshot;

static Snapshot undo_stack[MAX_UNDO];
static int      undo_top = 0;
static int      undo_count = 0;

static Snapshot redo_stack[MAX_UNDO];
static int      redo_top = 0;
static int      redo_count = 0;

static void snapshot_free(Snapshot *s) {
    for (int i = 0; i < s->num_layers; i++) {
        for (int j = 0; j < s->layers[i].num_sublayers; j++)
            note_array_free(&s->layers[i].sublayers[j]);
        free(s->layers[i].sublayers);
    }
    free(s->layers);
    memset(s, 0, sizeof(*s));
}

static Snapshot snapshot_copy(const MuseProject *p) {
    Snapshot s = {0};
    s.num_layers   = p->num_layers;
    s.active_layer = p->active_layer;
    s.bpm          = p->bpm;
    s.time_sig     = p->time_sig;
    s.owner_id     = p->owner_id;
    s.layers = calloc((size_t)p->num_layers, sizeof(MuseLayer));
    for (int i = 0; i < p->num_layers; i++) {
        const MuseLayer *src = &p->layers[i];
        MuseLayer *dst = &s.layers[i];
        *dst = *src;
        dst->sublayers = calloc((size_t)src->num_sublayers, sizeof(NoteArray));
        for (int j = 0; j < src->num_sublayers; j++) {
            note_array_init(&dst->sublayers[j]);
            const NoteArray *sna = &src->sublayers[j];
            for (int k = 0; k < sna->count; k++)
                note_array_push(&dst->sublayers[j], sna->notes[k]);
        }
    }
    return s;
}

static void snapshot_restore(const Snapshot *s, MuseProject *p) {
    muse_project_free(p);
    p->num_layers   = s->num_layers;
    p->active_layer = s->active_layer;
    p->bpm          = s->bpm;
    p->time_sig     = s->time_sig;
    p->owner_id     = s->owner_id;
    p->dirty        = true;
    p->layers = calloc((size_t)s->num_layers, sizeof(MuseLayer));
    for (int i = 0; i < s->num_layers; i++) {
        const MuseLayer *src = &s->layers[i];
        MuseLayer *dst = &p->layers[i];
        *dst = *src;
        dst->sublayers = calloc((size_t)src->num_sublayers, sizeof(NoteArray));
        for (int j = 0; j < src->num_sublayers; j++) {
            note_array_init(&dst->sublayers[j]);
            const NoteArray *sna = &src->sublayers[j];
            for (int k = 0; k < sna->count; k++)
                note_array_push(&dst->sublayers[j], sna->notes[k]);
        }
    }
}

void undo_init(void) {
    undo_top = undo_count = 0;
    redo_top = redo_count = 0;
}

void undo_clear(void) {
    for (int i = 0; i < undo_count; i++) {
        int idx = (undo_top - 1 - i + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&undo_stack[idx]);
    }
    for (int i = 0; i < redo_count; i++) {
        int idx = (redo_top - 1 - i + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&redo_stack[idx]);
    }
    undo_top = undo_count = 0;
    redo_top = redo_count = 0;
}

void undo_free(void) {
    for (int i = 0; i < undo_count; i++) {
        int idx = (undo_top - 1 - i + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&undo_stack[idx]);
    }
    for (int i = 0; i < redo_count; i++) {
        int idx = (redo_top - 1 - i + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&redo_stack[idx]);
    }
    undo_top = undo_count = 0;
    redo_top = redo_count = 0;
}

void undo_push(const MuseProject *p) {
    /* any new edit kills the redo stack */
    for (int i = 0; i < redo_count; i++) {
        int idx = (redo_top - 1 - i + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&redo_stack[idx]);
    }
    redo_top = redo_count = 0;

    /* drop the oldest if we're full */
    if (undo_count >= MAX_UNDO) {
        int oldest = (undo_top - undo_count + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&undo_stack[oldest]);
        undo_count--;
    }

    undo_stack[undo_top] = snapshot_copy(p);
    undo_top = (undo_top + 1) % MAX_UNDO;
    undo_count++;
}

int undo_pop(MuseProject *p) {
    if (undo_count <= 0) return -1;

    /* save current state for redo before restoring */
    if (redo_count >= MAX_UNDO) {
        int oldest = (redo_top - redo_count + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&redo_stack[oldest]);
        redo_count--;
    }
    redo_stack[redo_top] = snapshot_copy(p);
    redo_top = (redo_top + 1) % MAX_UNDO;
    redo_count++;

    /* restore from undo stack */
    undo_top = (undo_top - 1 + MAX_UNDO) % MAX_UNDO;
    undo_count--;
    snapshot_restore(&undo_stack[undo_top], p);
    snapshot_free(&undo_stack[undo_top]);
    return 0;
}

int redo_pop(MuseProject *p) {
    if (redo_count <= 0) return -1;

    /* save current state for undo */
    if (undo_count >= MAX_UNDO) {
        int oldest = (undo_top - undo_count + MAX_UNDO) % MAX_UNDO;
        snapshot_free(&undo_stack[oldest]);
        undo_count--;
    }
    undo_stack[undo_top] = snapshot_copy(p);
    undo_top = (undo_top + 1) % MAX_UNDO;
    undo_count++;

    /* restore from redo stack */
    redo_top = (redo_top - 1 + MAX_UNDO) % MAX_UNDO;
    redo_count--;
    snapshot_restore(&redo_stack[redo_top], p);
    snapshot_free(&redo_stack[redo_top]);
    return 0;
}
