#ifndef MUSE_UI_H
#define MUSE_UI_H

/* Colors */
#define COL_BG_DARK         0x16, 0x16, 0x18
#define COL_BG              0x1D, 0x1D, 0x1F
#define COL_BG_LIGHT        0x24, 0x24, 0x27
#define COL_SURFACE         0x31, 0x32, 0x39
#define COL_SURFACE_ALT     0x34, 0x34, 0x36
#define COL_BORDER          0x44, 0x43, 0x48
#define COL_BORDER_LIGHT    0x59, 0x5A, 0x62

/* Gold accent */
#define COL_GOLD            0xD8, 0xAD, 0x70
#define COL_GOLD_LIGHT      0xDD, 0xC3, 0x9E
#define COL_GOLD_BRIGHT     0xFF, 0xED, 0xD4
#define COL_GOLD_DARK       0xB0, 0x90, 0x46

/* Text */
#define COL_TEXT            0xE0, 0xE0, 0xE0
#define COL_TEXT_DIM        0x9A, 0x9A, 0x9E
#define COL_TEXT_GOLD       0xD4, 0xBC, 0x98
#define COL_ERROR           0xE0, 0x55, 0x55

/* Piano keys */
#define COL_WHITE_KEY       0x2A, 0x2A, 0x2D
#define COL_BLACK_KEY       0x1A, 0x1A, 0x1D
#define COL_OOR_BG          0x11, 0x11, 0x13
#define COL_OOR_TEXT        0x33, 0x33, 0x35
#define COL_OOR_DARK        0x0C, 0x0C, 0x0E

/* Grid */
#define COL_GRID_MEASURE    0x55, 0x55, 0x58
#define COL_GRID_BEAT       0x3A, 0x3A, 0x3D
#define COL_GRID_SUB        0x2E, 0x2E, 0x31

/* Playhead */
#define COL_PLAYHEAD        0xFF, 0x44, 0x44

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
