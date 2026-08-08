#ifndef BDO_MIDI_INPUT_H
#define BDO_MIDI_INPUT_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint8_t status,channel,note,velocity; } MidiInputEvent;
bool midi_input_init(void);
void midi_input_shutdown(void);
int midi_input_poll(MidiInputEvent *events,int capacity);
bool midi_input_available(void);
#endif
