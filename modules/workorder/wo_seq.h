/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WO_SEQ_H
#define WO_SEQ_H
#include "wo.h"
#define WO_SEQ_ROWS (SQ_LANES + 3)            /* eight drums, bass, chords, lead */
void wo_basics_meter(wo_basics_t *b, const char *meter);
void wo_basics_ticket(const wo_ticket_t *c, wo_basics_t *b);
void wo_basics_draft(wo_basics_t *b);
int wo_seq_snap(const wo_basics_t *b, int midi);
int wo_seq_step_scale(const wo_basics_t *b, int midi, int d);
int wo_seq_degree(const wo_basics_t *b, int midi);
void wo_seq_note_name(int midi, char *o, int cap);
void wo_song_seed(wo_song_t *s, const wo_basics_t *b);
void wo_song_ready(wo_song_t *s, const wo_basics_t *b);
int wo_seq_row_track(int row);
int wo_seq_pat_len(const wo_song_t *s, int pat);
int wo_seq_row_empty(const wo_song_t *s, int pat, int row);
int wo_seq_pat_empty(const wo_song_t *s, int pat);
int wo_seq_chord_at(const wo_song_t *s, const wo_basics_t *b, int pat, int step);
int wo_seq_toggle(wo_song_t *s, const wo_basics_t *b, int pat, int row, int step);
void wo_seq_adjust(wo_song_t *s, const wo_basics_t *b, int pat, int row, int step, int what, int d);
void wo_seq_write(wo_song_t *s, int pat, int row, int step, int note, int vel);
void wo_seq_set_len(wo_song_t *s, int pat, int row, int step, int len);
void wo_seq_clear_row(wo_song_t *s, int pat, int row);
void wo_seq_clear_pat(wo_song_t *s, int pat);
int wo_seq_copy_next(wo_song_t *s, int pat);
int wo_seq_chain_add(wo_song_t *s, int pat);
void wo_seq_chain_drop(wo_song_t *s);
void wo_seq_chain_clear(wo_song_t *s);
void wo_seq_roll_row(wo_song_t *s, const wo_basics_t *b, int pat, int row);
void wo_seq_roll_pat(wo_song_t *s, const wo_basics_t *b, int pat);
void wo_seq_mutate_row(wo_song_t *s, const wo_basics_t *b, int pat, int row);
#endif
