/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WO_SYNTH_H
#define WO_SYNTH_H
#include "wo.h"
void wo_synth_render(int32_t *out, uint32_t n);
void wo_synth_volume(int vol);
void wo_synth_note(int note, int vel);
void wo_synth_all_off(void);
int wo_synth_busy(void);
void wo_synth_scale(int root, uint16_t mask);
void wo_synth_octave(int d);
int wo_synth_get_octave(void);
int wo_synth_in_scale(int key);
int wo_synth_snap(int key);
void wo_synth_key(int key, int down, int vel);
void wo_synth_play(const wo_chords_t *c, int bpm, uint32_t now);
void wo_synth_stop(void);
void wo_synth_poll(uint32_t now);
int wo_synth_playing(void);
int wo_synth_step(void);
int wo_synth_chord_note(int pc);
#endif
