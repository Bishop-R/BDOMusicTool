#ifndef MUSE_UI_H
#define MUSE_UI_H

/* Colors — lifted charcoal palette */
#define COL_BG_DARK         0x1E, 0x1F, 0x22
#define COL_BG              0x25, 0x26, 0x2A
#define COL_BG_LIGHT        0x2E, 0x2F, 0x34
#define COL_SURFACE         0x38, 0x3A, 0x42
#define COL_SURFACE_ALT     0x3C, 0x3E, 0x44
#define COL_BORDER          0x4A, 0x4C, 0x54
#define COL_BORDER_LIGHT    0x5E, 0x60, 0x6A

/* Gold accent — warmer, slightly more saturated */
#define COL_GOLD            0xE0, 0xB4, 0x6C
#define COL_GOLD_LIGHT      0xE8, 0xCC, 0x9A
#define COL_GOLD_BRIGHT     0xFF, 0xF0, 0xD6
#define COL_GOLD_DARK       0xC0, 0x98, 0x4C

/* Text */
#define COL_TEXT            0xE8, 0xE8, 0xEB
#define COL_TEXT_DIM        0xA0, 0xA2, 0xAA
#define COL_TEXT_GOLD       0xDC, 0xC4, 0x9C
#define COL_ERROR           0xE8, 0x5C, 0x5C

/* Piano keys — better white/black contrast */
#define COL_WHITE_KEY       0xC8, 0xCA, 0xD0
#define COL_BLACK_KEY       0x28, 0x29, 0x2E
#define COL_OOR_BG          0x18, 0x19, 0x1C
#define COL_OOR_TEXT        0x3A, 0x3B, 0x40
#define COL_OOR_DARK        0x14, 0x15, 0x18

/* Grid — stronger hierarchy */
#define COL_GRID_MEASURE    0x62, 0x64, 0x6A
#define COL_GRID_BEAT       0x44, 0x46, 0x4C
#define COL_GRID_SUB        0x34, 0x36, 0x3A

/* Playhead */
#define COL_PLAYHEAD        0xFF, 0x52, 0x52

/* Layout */
#define KEYS_WIDTH          52
#define KEY_HEIGHT_DEFAULT  14
#define KEY_HEIGHT_MIN      4
#define KEY_HEIGHT_MAX      40
#define HEADER_HEIGHT       24
#define VEL_PANE_H_DEFAULT  80
#define VEL_PANE_H_MIN      40
#define VEL_PANE_H_MAX      300
#define BEAT_WIDTH_DEFAULT  120

#define PITCH_MIN           12   /* C0  */
#define PITCH_MAX           119  /* B8  */
#define NUM_PITCHES         108

#define LEFT_PANEL_W        240
#define TRANSPORT_H         40
#define STATUS_BAR_H        36
#define CORNER_TAB_W        18

#define DEFAULT_WIN_W       1400
#define DEFAULT_WIN_H       800
#define MIN_WIN_W           900
#define MIN_WIN_H           600

#endif /* MUSE_UI_H */
