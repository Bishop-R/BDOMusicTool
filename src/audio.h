#ifndef MUSE_AUDIO_H
#define MUSE_AUDIO_H

#include "model.h"
#include <stdbool.h>
#include <stdint.h>

/* init/shutdown the audio device */
bool muse_audio_init(void);
void muse_audio_shutdown(void);

/* playback control */
void muse_audio_play(MuseProject *proj, double start_ms);
void muse_audio_stop(void);
void muse_audio_pause(void);
void muse_audio_resume(void);
bool muse_audio_is_playing(void);
double muse_audio_position_ms(void);

/* reposition without stopping voices or resetting FX */
void muse_audio_seek(double ms);

/* preview a single note (non-blocking) */
void muse_audio_preview(int pitch, int velocity, int duration_ms, uint8_t inst_id, uint8_t ntype,
                        uint8_t synth_profile);

/* called each frame from main thread to advance playback and trigger notes */
void muse_audio_tick(double current_ms);

/* set global FX param pointers (called once from app init) */
void muse_audio_set_fx_params(int *reverb, int *delay, int *chorus_fb,
                               int *chorus_depth, int *chorus_freq);

/* set samples dir (where the extracted WEM->OGG files live) */
void muse_audio_set_samples_dir(const char *path);

/* callback when playback ends naturally */
void muse_audio_set_on_finished(void (*cb)(void *userdata), void *userdata);

/* offline WAV export - renders entire project to a file. 0=ok, -1=error */
int muse_audio_export_wav(const char *path, MuseProject *proj, int measures);

/* pitch range for a given instrument+technique. true if zones exist */
bool muse_audio_technique_range(uint8_t inst_id, uint8_t ntype, int *lo, int *hi);

#endif /* MUSE_AUDIO_H */
