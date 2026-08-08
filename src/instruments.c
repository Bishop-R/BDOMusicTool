#include "instruments.h"
#include <stddef.h>

const MuseInstrument INSTRUMENTS[INST_COUNT] = {
    /*                                lo  hi  drum oct  R    G    B    techniques                              n */
    /* Beginner */
    { 0x00, "Beginner Guitar",    12, 119, false, 0, 0x60,0x90,0xC0, {0}, 1 },
    { 0x01, "Beginner Flute",     12, 107, false, 0, 0x60,0xC0,0x90, {0}, 1 },
    { 0x02, "Beginner Recorder",  48,  95, false,-1, 0x60,0xC0,0xC0, {0}, 1 },
    { 0x04, "Hand Drum",          60,  79, true,  0, 0xC0,0x60,0x60, {99}, 1 },
    { 0x05, "Cymbals",            60,  71, true,  0, 0xC0,0x80,0x60, {99}, 1 },
    { 0x06, "Beginner Harp",      48, 107, false,-1, 0x90,0x60,0xC0, {0}, 1 },
    { 0x07, "Beginner Piano",     24, 119, false,-1, 0x60,0x80,0xC0, {0}, 1 },
    { 0x08, "Beginner Violin",    48,  95, false,-1, 0x60,0xA0,0x60, {0}, 1 },
    /* Florchestra */
    { 0x0A, "Flor. Acoustic Guitar", 36, 88, false, 0, 0xC0,0x90,0x50, {0,3,12,13,14,15}, 6 },
    { 0x0B, "Flor. Flute",           48, 88, false, 0, 0x50,0xB0,0xA0, {0,1,2,3,4,15}, 6 },
    { 0x0D, "Drum Set",              48, 64, true,  0, 0xC0,0x50,0x50, {99}, 1 },
    { 0x0F, "Flor. Contrabass",      28, 64, false, 0, 0x70,0x80,0xA0, {0,3,12,13,14,23}, 6 },
    { 0x10, "Flor. Harp",            12, 90, false, 0, 0xA0,0x60,0xB0, {0,9,10,16}, 4 },
    { 0x11, "Flor. Piano",           12, 107,false, 0, 0x50,0x90,0xD0, {0,11}, 2 },
    { 0x12, "Flor. Violin",          43, 88, false, 0, 0x50,0xA0,0x70, {0,1,2,3,4,5,6,7,8}, 9 },
    { 0x13, "Handpan",               45, 88, false, 0, 0xB0,0x80,0x50, {0}, 1 },
    /* Marnian synths */
    { 0x14, "Wavy Planet",   12, 100,false, 0, 0xD0,0xA0,0x50, {0,1,2,3,4,5,6,7,8,17,18,19,20,21}, 14 },
    { 0x18, "Illusion Tree", 12, 100,false, 0, 0xC0,0xB0,0x60, {0,1,2,3,4,5,6,7,8,17,18,19,20,21}, 14 },
    { 0x1C, "Secret Note",   12, 100,false, 0, 0xD0,0x90,0x60, {0,1,2,3,4,5,6,7,8,17,18,19,20,21}, 14 },
    { 0x20, "Sandwich",      12, 100,false, 0, 0xC0,0xA0,0x70, {0,1,2,3,4,5,6,7,8,17,18,19,20,21}, 14 },
    /* Electric guitars & bass */
    { 0x0E, "Marnibass",             28, 64, false, 0, 0x80,0x70,0xB0, {0,3,12,13,14,16,22,23,24}, 9 },
    { 0x24, "Guitar Silver Wave",    24, 95, false, 0, 0xA0,0xA0,0xD0, {0,6,13,14,25}, 5 },
    { 0x25, "Guitar Highway",        24, 95, false, 0, 0xB0,0x90,0xC0, {0,6,13,14,25}, 5 },
    { 0x26, "Guitar Hexe Glam",      24, 95, false, 0, 0xC0,0x80,0xB0, {0,6,13,14,25}, 5 },
    /* Late additions */
    { 0x27, "Flor. Clarinet",        24, 95, false, 0, 0x60,0xB0,0xA0, {0,4,7,8,15,26,27,28}, 8 },
    { 0x28, "Flor. Horn",            24, 95, false, 0, 0xA0,0xA0,0x60, {0,3,4,12,26,27,28}, 7 },
};

const MuseTechnique TECHNIQUES[] = {
    {  0, "Sustain",         80,145,210 },
    {  1, "Tab",            210,160, 60 },
    {  2, "Cut",            210, 75, 75 },
    {  3, "Slide Up",       100,200,100 },
    {  4, "Short",          180,100,210 },
    {  5, "Pizzicato",       60,190,180 },
    {  6, "Tremolo",        170,130, 60 },
    {  7, "Trill Minor",    210,130,180 },
    {  8, "Trill Major",    230,170,200 },
    {  9, "Chord Major",    130,180,130 },
    { 10, "Chord Minor",    130,140,180 },
    { 11, "Sustain Pedal",  110,180,230 },
    { 12, "Slide Down",      80,180, 80 },
    { 13, "Mute",           160,160,160 },
    { 14, "Harmonics",      200,200,130 },
    { 15, "Triplet",        180,120,180 },
    { 16, "Glissando",      120,200,200 },
    { 17, "Vibrato",        230,200, 70 },
    { 18, "Marcato",        200,100,100 },
    { 19, "Filter Sustain", 100,160,140 },
    { 20, "Filter Brassy",  210,140, 50 },
    { 21, "Filter Pluck",   160,120, 80 },
    { 22, "Slap",           220,180,100 },
    { 23, "Gliss Up",       140,210,140 },
    { 24, "X-notes",        180, 80, 80 },
    { 25, "FX",             100,180,220 },
    { 26, "SusPiano",       100,120,180 },
    { 27, "SusMezzoForte",  140,140,200 },
    { 28, "SusForte",       180,160,220 },
    { 99, "Drum",           190, 70, 70 },
};
const int TECHNIQUE_COUNT = sizeof(TECHNIQUES) / sizeof(TECHNIQUES[0]);

const MuseInstrument *inst_by_id(uint8_t id) {
    for (int i = 0; i < INST_COUNT; i++)
        if (INSTRUMENTS[i].id == id) return &INSTRUMENTS[i];
    return NULL;
}

const MuseInstrument *inst_at(int index) {
    if (index >= 0 && index < INST_COUNT) return &INSTRUMENTS[index];
    return NULL;
}

int inst_count(void) { return INST_COUNT; }

const MuseTechnique *technique_by_id(uint8_t id) {
    for (int i = 0; i < TECHNIQUE_COUNT; i++)
        if (TECHNIQUES[i].id == id) return &TECHNIQUES[i];
    return NULL;
}

bool inst_is_drum(uint8_t id) {
    return id == 0x04 || id == 0x05 || id == 0x0D;
}

bool inst_has_technique(uint8_t id, uint8_t ntype) {
    const MuseInstrument *inst = inst_by_id(id);
    if (!inst) return false;
    for (int i = 0; i < inst->num_techniques; i++)
        if (inst->techniques[i] == ntype) return true;
    return false;
}

int inst_oct_offset(uint8_t id) {
    const MuseInstrument *inst = inst_by_id(id);
    return inst ? inst->oct_offset : 0;
}

void inst_pitch_range(uint8_t id, int *lo, int *hi) {
    const MuseInstrument *inst = inst_by_id(id);
    if (inst) {
        *lo = inst->pitch_lo;
        *hi = inst->pitch_hi;
    } else {
        *lo = 12;   /* PITCH_MIN fallback */
        *hi = 119;  /* PITCH_MAX fallback */
    }
}

const char *drum_key_name(uint8_t inst_id, uint8_t pitch) {
    if (inst_id == 0x0D) {
        switch (pitch) {
        case 48: return "Kck";      case 49: return "SnrSide";
        case 50: return "SnrHit";   case 51: return "RimShot";
        case 52: return "SnrFlam";  case 53: return "Tom1";
        case 54: return "HiHatC";   case 55: return "Tom2";
        case 56: return "HatPdl";   case 57: return "Tom3";
        case 58: return "HihatO";   case 59: return "Tom4";
        case 60: return "Tom5";     case 61: return "CymCrsh";
        case 62: return "CymRide";  case 63: return "SnrRollS";
        case 64: return "SnrRollL";
        }
    } else if (inst_id == 0x04) {
        switch (pitch) {
        case 60: return "Bng1-O";   case 65: return "Bng2-O";
        case 66: return "Bng2-C";   case 67: return "Bng2-F";
        case 72: return "Cng1-O";   case 73: return "Cng1-C";
        case 74: return "Cng1-F";   case 77: return "Cng2-O";
        case 78: return "Cng2-C";   case 79: return "Cng2-F";
        }
    } else if (inst_id == 0x05) {
        if (pitch == 60 || pitch == 65 || pitch == 71) return "HIT";
    }
    return NULL;
}

bool inst_is_spacer_key(uint8_t inst_id, uint8_t pitch) {
    const MuseInstrument *inst = inst_by_id(inst_id);
    if (!inst || !inst->is_drum) return false;
    /* unnamed keys on drum instruments are just spacers */
    return drum_key_name(inst_id, pitch) == NULL;
}

uint8_t inst_synth_variant(uint8_t base_id, uint8_t profile) {
    if (profile == 0) return base_id;
    switch (base_id) {
        case 0x14: case 0x18: case 0x1C: case 0x20:
            return base_id + profile;
        default: return base_id;
    }
}
