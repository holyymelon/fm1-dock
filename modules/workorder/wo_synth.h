/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WO_SYNTH_H
#define WO_SYNTH_H
#include "wo.h"
extern const sq_track_t WO_TRACK_DEF[TR_N];
extern const sq_drum_t WO_DRUM_DEF;
extern const uint8_t WO_DRUM_GM[SQ_LANES];
/* render() */
void wo_synth_render(int32_t *out, uint32_t n);
/* the main loop */
void wo_synth_init(void);
void wo_synth_ticks(uint32_t (*ticks)(void));     /* the host's 24 MHz counter, to measure render() */
void wo_synth_load(int *last, int *peak);          /* render()'s share of its time, in percent */
void wo_synth_volume(int vol);
void wo_synth_song(const wo_song_t *s);          /* the tracks' sounds (the ticket's sketch) */
void wo_synth_keys_track(int tr);                 /* which track the keys play; -1 their own sound */
int wo_synth_get_keys_track(void);
void wo_synth_note(int note, int vel);           /* through the keys' track */
void wo_synth_track_note(int tr, int note, int vel);
void wo_synth_drum(int lane, int vel);
void wo_synth_all_off(void);
int wo_synth_busy(void);
void wo_synth_midi(void (*send)(uint32_t, uint32_t, uint32_t));   /* MIDI out, queued by render() */
/* the keys in the ticket's scale */
void wo_synth_scale(int root, uint16_t mask);
void wo_synth_octave(int d);
int wo_synth_get_octave(void);
int wo_synth_in_scale(int key);
int wo_synth_snap(int key);
int wo_synth_snap_midi(int midi);
void wo_synth_key(int key, int down, int vel);
/* the sequencer */
void wo_seq_play(const wo_song_t *s, const wo_basics_t *b, int pat);
void wo_seq_stop(void);
int wo_seq_playing(void);
void wo_seq_edit_pat(int pat);
int wo_seq_pos(int *pat, int *step, int *frac, int *slot);
void wo_seq_skip(int tr, int step);
void wo_seq_skip_drum(int lane, int step);
#endif
