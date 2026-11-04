/* SPDX-License-Identifier: GPL-3.0-only */
/* WORKORDER for the M-VAVE FM-1: the state every part shares.
 * The engine (wo_engine.c, wo_session.c) is plain C with no host calls, so it builds and tests on a
 * computer as well (tests/host_main.c). The FM-1 parts are wo_module.c, wo_ui.c and wo_synth.c. */
#ifndef WO_H
#define WO_H
#include <stdint.h>
#include "wo_data.h"

#define WO_LINE      200        /* one rule, filled */
#define WO_MAXLINES  72         /* lines on a draft */
#define WO_MAXITEMS  72         /* constraints on a ticket */
#define WO_MAXINV    64         /* gear items */
#define WO_NAMELEN   24
#define WO_WHY       40
#define WO_MAXCUSTOM 48         /* your own rules */
#define WO_CUSTOMLEN 96
#define WO_MAXMATADD 32         /* your own sample materials */
#define WO_MATLEN    48
#define WO_MAXSLOTS  6
#define WO_MAXCHORDS 6
#define WO_NCONF     5
#define WO_CONFLEN   96
#define WO_HAND      4

#define NONE8  255u
#define NONE16 0xFFFFu

enum { TIER_GENTLE, TIER_FIRM, TIER_BRUTAL };

/* ---- weights: one byte per thing a roll can pick, 64 = 1.0, clamped 0.25 .. 3.0 */
#define WK_ENTRY  0u
#define WK_SCALE  (WK_ENTRY + WO_NENTRIES)
#define WK_BAND   (WK_SCALE + WO_NSCALES)
#define WK_ITEM   (WK_BAND + WO_NBANDS)
#define WK_PARAM  (WK_ITEM + WO_MAXINV)
#define WK_PT     (WK_PARAM + WO_NFIELDS)
#define WK_CUSTOM (WK_PT + WO_NSKEYS * 3)
#define WK_N      (WK_CUSTOM + WO_MAXCUSTOM)
#define WQ_ONE    64u

/* ---- gear */
typedef struct {
    char name[WO_NAMELEN];
    uint8_t roles;              /* bit r: WO_ROLE[r] */
    uint8_t on;
} wo_inv_t;

/* ---- a draft: what the Roll page shows */
#define L_HEAD 1u               /* the genre's name, the chord set: a heading, not a rule */
#define L_DEV  2u               /* a device line (named gear with a job) */
#define L_META 4u               /* a brutal process meta-rule */
#define L_GEN  8u               /* generated wording (form, chords): no pool entry behind it */
typedef struct {
    char text[WO_LINE];
    uint16_t entry;             /* WO_ENTRIES index, NONE16 for none */
    uint8_t custom;             /* index into the custom rules, NONE8 */
    uint8_t field, tier, flags;
    uint8_t ndev, dev[4];       /* inventory items it names */
} wo_line_t;

typedef struct { int8_t root; uint8_t scale; int8_t forbidden; uint8_t nomod; } wo_key_t;
typedef struct { uint8_t band, meter, lo, hi, bpm, noauto, tier; } wo_tempo_t;   /* bpm 0: the range lo..hi */
typedef struct {
    uint8_t role; int8_t item; uint8_t count; int8_t job, limit;
    char text[WO_LINE];         /* a pairing's or a chain's own sentence, else "" */
} wo_slot_t;
typedef struct {
    uint8_t nslots; wo_slot_t slot[WO_MAXSLOTS];
    uint8_t nbanned; int8_t banned[2];
    uint8_t exclusive, empty;   /* empty: no category feeds GEAR */
} wo_gear_t;
typedef struct {
    char label[28];
    uint8_t npc, pcs[6];        /* pitch classes, the chord's tones above the key root */
    int8_t tone[6];             /* semitones above the key root (may pass 12) */
    uint8_t root;               /* the chord's own root pc */
} wo_chord_t;
typedef struct {
    uint8_t n; wo_chord_t c[WO_MAXCHORDS];
    uint8_t shape, alt, root, scale;  /* the key it was drawn from */
} wo_chords_t;

typedef struct {
    uint8_t has[WO_NFIELDS], skip[WO_NFIELDS];
    uint8_t sev[WO_NFIELDS], on[WO_NFIELDS];
    wo_key_t key; wo_tempo_t tempo; wo_gear_t gear; wo_chords_t chords;
    uint8_t genre;              /* NONE8 */
    uint8_t nlines; wo_line_t line[WO_MAXLINES];
} wo_draft_t;

/* ---- a ticket: what the Session page shows */
#define I_HEAD      1u
#define I_CHECKED   2u
#define I_BROKEN    4u
#define I_WASBROKEN 8u
#define I_META      16u
enum { RISK_NONE, RISK_FIGHTS, RISK_DOUBLES, RISK_PILES };
#define LBL_META   (WO_NFIELDS)       /* item labels: a field, or one of these */
typedef struct {
    char text[WO_LINE];
    char why[WO_WHY];
    uint16_t wk;                /* weight key, NONE16 */
    uint16_t entry;             /* pool entry, NONE16 (retire takes it out of the pool) */
    uint8_t label, pkey, tier, flags, risk, round;
    uint8_t ndev, dev[4];
    uint16_t pts;
    uint32_t at, wall, broke_at;
} wo_item_t;

typedef struct {
    uint8_t pool, tier, risk; uint16_t entry, pts, wk;
    char text[WO_LINE];
} wo_card_t;

typedef struct {
    uint8_t round, lives, maxlives, ended, won, reason, nladder, ladder[8], handsize, genre;
    uint32_t bonus;
    uint8_t nhand; wo_card_t hand[WO_HAND];
} wo_run_t;

enum { RATE_KEEPER, RATE_NEUTRAL, RATE_DEAD, RATE_ABANDONED };

typedef struct {
    uint32_t id;                /* the ticket's number on this FM-1 */
    char name[56];
    uint8_t mode, anchor, used;
    uint8_t has_key, has_tempo, has_chords;
    wo_key_t key; wo_tempo_t tempo; wo_chords_t chords;
    uint8_t genre;
    uint8_t sev[WO_NFIELDS];
    uint32_t elapsed, started;  /* started: ms() when the clock last ran */
    uint8_t running;
    uint32_t limit_ms, target_min;
    uint16_t pauses, parks;
    uint32_t paused_ms, paused_at, parked_ms;
    uint8_t nconf; char conf[WO_NCONF][WO_CONFLEN];
    uint8_t nitems; wo_item_t item[WO_MAXITEMS];
    uint8_t is_run; wo_run_t run;
    uint8_t rating, powered_off; char note[WO_WHY];
    uint32_t score;
} wo_ticket_t;

/* the leaderboard: one line per closed ticket */
typedef struct {
    uint32_t id, score, ms;
    uint8_t mode, rating, done, total, broke, run_won, run_reached, run_total, burst, pauses;
    char name[40];
} wo_score_t;
#define WO_MAXSCORES 160

/* ---- settings and roll setup */
typedef struct {
    uint8_t theme, anchor, cooldown, noclash, rollsize, sound, volume, genrelock, mode;
    uint8_t chaos, minp, maxp, rsev, holds, ranchor, mixsev, target_min, mutn;
} wo_settings_t;

typedef struct { uint8_t pool, tier; char text[WO_CUSTOMLEN]; } wo_custom_t;
typedef struct { uint8_t cat; char text[WO_MATLEN]; } wo_matadd_t;

typedef struct {
    /* the roll setup */
    uint8_t on[WO_NFIELDS], sev[WO_NFIELDS], lock[WO_NFIELDS], count[WO_NFIELDS], mixin[WO_NFIELDS];
    wo_settings_t set;
    /* what you own and what you changed */
    uint8_t ninv; wo_inv_t inv[WO_MAXINV];
    uint32_t bucket[WO_NROLES];             /* role -> the fields it feeds */
    uint8_t ncustom; wo_custom_t custom[WO_MAXCUSTOM];
    uint8_t nmatadd; wo_matadd_t matadd[WO_MAXMATADD];
    uint8_t disabled[(WO_NENTRIES + 7) / 8];
    uint8_t matoff[(WO_NMAT + 7) / 8];
    uint8_t cool[(WO_NENTRIES + 7) / 8];    /* drawn since its pool last cycled */
    uint8_t coolc[(WO_MAXCUSTOM + 7) / 8];
    uint8_t weight[WK_N];
    uint32_t next_id;
    /* now */
    uint8_t has_draft; wo_draft_t draft;
    uint8_t has_cur; wo_ticket_t cur;
    uint8_t nscores; wo_score_t score[WO_MAXSCORES];
} wo_state_t;

extern wo_state_t W;

/* ---- wo_util.c */
uint32_t wo_rand(void);
void wo_seed(uint32_t s);
int wo_ri(int lo, int hi);                  /* lo..hi inclusive */
float wo_frand(void);                       /* 0 .. <1 */
void wo_str(uint16_t idx, char *out, int cap);          /* a string of the blob */
int wo_strlen(const char *s);
void wo_strcpy(char *d, const char *s, int cap);
void wo_strcat(char *d, const char *s, int cap);
void wo_catu(char *d, uint32_t v, int cap);
int wo_streq(const char *a, const char *b);
const char *wo_stristr(const char *hay, const char *needle);
char wo_lower(char c);
void wo_cap1(char *s);
void wo_mmss(char *d, uint32_t sec, int cap);
void wo_hhmmss(char *d, uint32_t ms, int cap);
int wo_bpe_encode(const char *s, uint8_t *out, int cap);    /* returns bytes incl. the 0 */
int wo_bpe_decode(const uint8_t *in, char *out, int cap);   /* returns bytes read incl. the 0 */
static inline int wo_bit(const uint8_t *b, uint32_t i) { return (b[i >> 3] >> (i & 7u)) & 1; }
static inline void wo_setbit(uint8_t *b, uint32_t i, int on)
{ if (on) b[i >> 3] |= (uint8_t)(1u << (i & 7u)); else b[i >> 3] &= (uint8_t)~(1u << (i & 7u)); }

/* ---- wo_tags.c */
uint32_t wo_tags_of(const char *text);
int wo_clashes(uint32_t tags, uint32_t have);
int wo_only_claim(const char *t, char *noun, int cap);  /* "X is the only drum source": the noun */

/* ---- wo_engine.c */
void wo_defaults(void);                     /* a fresh FM-1: stock gear, defaults, no history */
void wo_reset_weights(void);
float wo_w(uint32_t wk);
void wo_bump(uint32_t wk, float f);
int wo_field_lines(const wo_draft_t *d, uint8_t f, uint8_t *idx, int cap);
void wo_roll_field(uint8_t f);
void wo_roll_all(void);
void wo_mutate(void);
void wo_full_random(void);
int wo_reroll_line(uint8_t f, uint8_t li);
int wo_draft_ready(void);
int wo_conflicts(const wo_draft_t *d, char out[][WO_CONFLEN], int cap);
int wo_resolve_conflicts(int limit, uint32_t only);
void wo_key_line(const wo_key_t *k, char *out, int cap);
void wo_tempo_line(const wo_tempo_t *t, char *out, int cap);
void wo_slot_text(const wo_slot_t *s, char *out, int cap);
uint8_t wo_pick_tier(uint8_t f);
void wo_apply_builtin(uint8_t b);
int wo_draw_pool(uint8_t pool, uint8_t tier, int n, wo_line_t *out, uint32_t *have, uint8_t mode, uint8_t genre);
uint8_t wo_live_genre(void);
float wo_pool_bias(uint8_t f);
int wo_inv_on(uint8_t i);
const char *wo_field_name(uint8_t f, char *buf, int cap);
const char *wo_pool_name(uint8_t p, char *buf, int cap);
const char *wo_tier_name(uint8_t t);
void wo_fill(const char *tmpl, wo_line_t *l);

/* ---- wo_session.c */
void wo_issue(void);
void wo_start_mode(uint8_t m);
void wo_start_run(uint8_t m);
uint32_t wo_elapsed(const wo_ticket_t *c);
void wo_tick(uint8_t i);                    /* tick / untick */
void wo_break(uint8_t i, const char *why);
void wo_unbreak(uint8_t i);
void wo_retire(uint8_t i, const char *why);
void wo_pause(void);
void wo_take_card(uint8_t i);
void wo_redeal(void);
void wo_run_advance(void);
uint32_t wo_ticket_score(const wo_ticket_t *c);
uint32_t wo_ticket_max(const wo_ticket_t *c);
int wo_item_points(const wo_ticket_t *c, const wo_item_t *it, uint32_t at);
void wo_close(uint8_t rating, const char *note);
void wo_card_label(const wo_card_t *k, char *out, int cap);
void wo_item_label(const wo_item_t *it, char *out, int cap);
int wo_burst(const wo_ticket_t *c);
extern uint32_t (*wo_now)(void);            /* ms, the host's */
extern void (*wo_filed)(const wo_ticket_t *c);

#endif
