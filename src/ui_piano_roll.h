#ifndef MUSE_UI_PIANO_ROLL_H
#define MUSE_UI_PIANO_ROLL_H

typedef struct MuseApp MuseApp;
void ui_piano_roll_render(MuseApp *app);
void ui_instrument_picker_render(MuseApp *app);
void ui_aux_send_render(MuseApp *app);
void ui_chord_picker_render(MuseApp *app);

/* Chord data access (used by event handler) */
typedef struct { const char *name; const int *intervals; int count; } ChordDef;
int chord_total_items(void);
const ChordDef *chord_at(int idx);

/* Dropdown menu overlay */
void ui_dropdown_render(MuseApp *app);

/* MIDI import dialog */
void ui_midi_import_render(MuseApp *app);

#endif /* MUSE_UI_PIANO_ROLL_H */
