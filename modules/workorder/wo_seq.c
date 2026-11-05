/* SPDX-License-Identifier: GPL-3.0-only */
/* The sketch, on the main loop's side: what the ticket locks (key, tempo, meter, chords), a fresh
 * sketch built from it, editing a step, and rolling a row the way the roll rolls a parameter: in the
 * scale, around the chords, with a drummer's habits. Playing it is wo_synth.c's. */
#include "wo.h"
#include "wo_seq.h"
#include "wo_synth.h"

/* ================================================================ what the ticket locks */
static int num_at(const char *s, int *i)
{
    int v = 0, any = 0;
    while (s[*i] >= '0' && s[*i] <= '9') { v = v * 10 + (s[*i] - '0'); (*i)++; any = 1; }
    return any ? v : -1;
}

/* the meter's words as bars of steps: "7/8 into 4/4" -> 14, 16 */
void wo_basics_meter(wo_basics_t *b, const char *m)
{
    int i = 0, n16[4], nn = 0, k, big = 0;
    int num[4], den[4];
    b->nbars = 0; b->div = 4; b->every4 = 0;
    while (m && m[i] && nn < 4) {
        int a, d, j;
        if (m[i] < '0' || m[i] > '9') { i++; continue; }
        a = num_at(m, &i);
        if (m[i] != '/') continue;
        j = i + 1;
        d = num_at(m, &j);
        i = j;
        if (a < 1 || (d != 2 && d != 4 && d != 8 && d != 16)) continue;
        num[nn] = a; den[nn] = d;
        n16[nn] = a * 16 / d;
        if (n16[nn] > SQ_STEPS) big = 1;
        nn++;
    }
    if (m && wo_stristr(m, "every fourth")) b->every4 = 1;
    if (!nn) { b->nbars = 1; b->bar[0] = 16; return; }
    if (big) b->div = 2;
    for (k = 0; k < nn; k++) {
        int st = big ? num[k] * 8 / den[k] : n16[k];
        if (st < 2) st = 2;
        if (st > SQ_STEPS) st = SQ_STEPS;
        b->bar[k] = (uint8_t)st;
    }
    b->nbars = (uint8_t)nn;
}

static void basics_common(wo_basics_t *b, const wo_key_t *k, const wo_tempo_t *t, const wo_chords_t *c)
{
    char m[64];
    uint8_t *p = (uint8_t *)b; unsigned q;
    for (q = 0; q < sizeof *b; q++) p[q] = 0;
    b->root = 0; b->mask = 0x0FFFu; b->bpm = 90;
    if (k) { b->root = k->root >= 0 ? k->root % 12 : 0; b->mask = WO_SCALES[k->scale].mask; }
    else if (c && c->n) { b->root = c->root % 12; b->mask = WO_SCALES[c->scale].mask; }
    if (!b->mask) b->mask = 0x0FFFu;
    if (t) b->bpm = t->bpm ? t->bpm : (t->lo + t->hi) / 2;
    if (b->bpm < 40) b->bpm = 90;
    m[0] = 0;
    if (t && t->tier < 3 && t->meter < WO_METER_N[t->tier]) wo_str(WO_METER[t->tier][t->meter], m, (int)sizeof m);
    wo_basics_meter(b, m);
    if (c) b->ch = *c;
}

void wo_basics_ticket(const wo_ticket_t *c, wo_basics_t *b)
{
    basics_common(b, c->has_key ? &c->key : 0, c->has_tempo ? &c->tempo : 0, c->has_chords ? &c->chords : 0);
}

void wo_basics_draft(wo_basics_t *b)
{
    const wo_draft_t *d = &W.draft;
    int on = W.has_draft;
    basics_common(b, on && d->has[F_KEY] && W.on[F_KEY] ? &d->key : 0, on && d->has[F_TEMPO] ? &d->tempo : 0,
                  on && d->has[F_CHORDS] ? &d->chords : 0);
}

/* the scale: is it in, the note at or below, n degrees up or down */
static int in_scale(const wo_basics_t *b, int midi) { return b->mask >> (((midi - b->root) % 12 + 12) % 12) & 1u; }
int wo_seq_snap(const wo_basics_t *b, int midi)
{
    int k;
    for (k = 0; k < 12; k++) if (in_scale(b, midi - k)) return midi - k;
    return midi;
}
int wo_seq_step_scale(const wo_basics_t *b, int midi, int d)
{
    int n = wo_seq_snap(b, midi), guard = 0;
    while (d > 0 && guard++ < 40) { n++; if (in_scale(b, n)) d--; }
    while (d < 0 && guard++ < 40) { n--; if (in_scale(b, n)) d++; }
    return n < 24 ? 24 : n > 108 ? 108 : n;
}
int wo_seq_degree(const wo_basics_t *b, int midi)       /* 1 for the root ... ; 0: outside the scale */
{
    int rel = ((midi - b->root) % 12 + 12) % 12, k, deg = 0;
    if (!in_scale(b, midi)) return 0;
    for (k = 0; k <= rel; k++) if (b->mask >> k & 1u) deg++;
    return deg;
}
void wo_seq_note_name(int midi, char *o, int cap)
{
    static const char *const NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    o[0] = 0;
    if (midi < 0 || midi > 127) return;
    wo_strcpy(o, NM[midi % 12], cap);
    wo_catu(o, (uint32_t)(midi / 12 - 1), cap);
}

/* ================================================================ a fresh sketch */
static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

void wo_song_seed(wo_song_t *s, const wo_basics_t *b)
{
    uint8_t *p = (uint8_t *)s, cyc[4];
    unsigned q;
    int ncyc, npat, i, l, n = b->ch.n;
    for (q = 0; q < sizeof *s; q++) p[q] = 0;
    s->div = b->div ? b->div : 4;
    s->clock = 1;
    for (i = 0; i < TR_N; i++) s->tr[i] = WO_TRACK_DEF[i];
    for (l = 0; l < SQ_LANES; l++) s->dr[l] = WO_DRUM_DEF;
    if (b->every4 && b->nbars >= 2) { cyc[0] = cyc[1] = cyc[2] = b->bar[0]; cyc[3] = b->bar[1]; ncyc = 4; }
    else { ncyc = b->nbars ? b->nbars : 1; for (i = 0; i < ncyc; i++) cyc[i] = b->nbars ? b->bar[i] : 16; }
    if (n > 0) {
        int lcm = n / gcd(n, ncyc) * ncyc;
        npat = lcm <= SQ_PATS ? lcm : ((n + ncyc - 1) / ncyc) * ncyc;
        if (npat > SQ_PATS) npat = SQ_PATS;
    } else npat = ncyc;
    for (i = 0; i < SQ_PATS; i++) s->pat[i].len = cyc[i % ncyc];
    for (i = 0; i < npat; i++) {
        sq_pat_t *pt = &s->pat[i];
        if (n > 0) {
            const wo_chord_t *c = &b->ch.c[i % n];
            pt->n[TR_CHORDS - 1][0].note = (uint8_t)(1 + i % n);
            pt->n[TR_CHORDS - 1][0].len = pt->len;
            pt->n[TR_CHORDS - 1][0].vel = 84;
            pt->n[TR_BASS - 1][0].note = (uint8_t)(36 + c->root % 12);
            pt->n[TR_BASS - 1][0].len = pt->len;
            pt->n[TR_BASS - 1][0].vel = 96;
        }
    }
    if (npat > 1) { s->nchain = (uint8_t)npat; for (i = 0; i < npat; i++) s->chain[i] = (uint8_t)i; s->chain_on = 1; }
}

void wo_song_ready(wo_song_t *s, const wo_basics_t *b) { if (!s->div) wo_song_seed(s, b); }

/* ================================================================ rows: 0..7 the drums, then bass, chords, lead */
int wo_seq_row_track(int row) { return row < SQ_LANES ? TR_DRUMS : row - SQ_LANES + 1; }
static sq_note_t *row_notes(wo_song_t *s, int pat, int row) { return s->pat[pat].n[row - SQ_LANES]; }

int wo_seq_pat_len(const wo_song_t *s, int pat)
{
    int l = s->pat[pat % SQ_PATS].len;
    return l < 1 ? 16 : l > SQ_STEPS ? SQ_STEPS : l;
}

int wo_seq_row_empty(const wo_song_t *s, int pat, int row)
{
    int i;
    if (row < SQ_LANES) return !s->pat[pat].hit[row];
    for (i = 0; i < SQ_STEPS; i++) if (s->pat[pat].n[row - SQ_LANES][i].note) return 0;
    return 1;
}
int wo_seq_pat_empty(const wo_song_t *s, int pat)
{
    int r;
    for (r = 0; r < WO_SEQ_ROWS; r++) if (!wo_seq_row_empty(s, pat, r)) return 0;
    return 1;
}

/* the chord sounding at a step: the last chord hit at or before it, else the pattern's place in the progression */
int wo_seq_chord_at(const wo_song_t *s, const wo_basics_t *b, int pat, int step)
{
    const sq_note_t *n = s->pat[pat].n[TR_CHORDS - 1];
    int i;
    if (!b->ch.n) return -1;
    for (i = step; i >= 0; i--) if (n[i].note && n[i].note - 1 < b->ch.n) return n[i].note - 1;
    return pat % b->ch.n;
}
static int chord_root_pc(const wo_song_t *s, const wo_basics_t *b, int pat, int step)
{
    int c = wo_seq_chord_at(s, b, pat, step);
    return c < 0 ? b->root : b->ch.c[c].root % 12;
}
static int is_chord_tone(const wo_song_t *s, const wo_basics_t *b, int pat, int step, int midi)
{
    int c = wo_seq_chord_at(s, b, pat, step), i, pc = midi % 12;
    if (c < 0) return wo_seq_degree(b, midi) == 1 || wo_seq_degree(b, midi) == 3 || wo_seq_degree(b, midi) == 5;
    for (i = 0; i < b->ch.c[c].npc; i++) if (b->ch.c[c].pcs[i] == pc) return 1;
    return 0;
}

static int default_note(const wo_song_t *s, const wo_basics_t *b, int pat, int row, int step)
{
    sq_note_t *n = (sq_note_t *)s->pat[pat].n[row - SQ_LANES];
    int i, tr = wo_seq_row_track(row);
    if (tr == TR_CHORDS) { int c = wo_seq_chord_at(s, b, pat, step); return c < 0 ? 0 : c + 1; }
    for (i = step - 1; i >= 0; i--) if (n[i].note) return n[i].note;
    for (i = step + 1; i < SQ_STEPS; i++) if (n[i].note) return n[i].note;
    if (tr == TR_BASS) return 36 + chord_root_pc(s, b, pat, step);
    return wo_seq_snap(b, 60 + chord_root_pc(s, b, pat, step) + 7 > 76 ? 60 + chord_root_pc(s, b, pat, step) : 60 + chord_root_pc(s, b, pat, step) + 7);
}

/* a key in step mode: the step on or off. Returns 1 when it is on now */
int wo_seq_toggle(wo_song_t *s, const wo_basics_t *b, int pat, int row, int step)
{
    sq_pat_t *p = &s->pat[pat];
    s->made = 1;
    if (row < SQ_LANES) {
        uint16_t bit = (uint16_t)(1u << step);
        p->hit[row] ^= bit;
        if (!(p->hit[row] & bit)) p->acc[row] &= (uint16_t)~bit;
        return !!(p->hit[row] & bit);
    } else {
        sq_note_t *n = &row_notes(s, pat, row)[step];
        if (n->note) { n->note = 0; return 0; }
        n->note = (uint8_t)default_note(s, b, pat, row, step);
        if (!n->note) return 0;
        n->len = (uint8_t)(wo_seq_row_track(row) == TR_CHORDS ? s->div : 1);
        n->vel = 100;
        return 1;
    }
}

/* the knobs on a step: what it plays (in the scale, or which chord), how long, how hard */
void wo_seq_adjust(wo_song_t *s, const wo_basics_t *b, int pat, int row, int step, int what, int d)
{
    s->made = 1;
    if (row < SQ_LANES) {
        uint16_t bit = (uint16_t)(1u << step);
        if (what == 2 && (s->pat[pat].hit[row] & bit)) s->pat[pat].acc[row] ^= bit;
        return;
    } else {
        sq_note_t *n = &row_notes(s, pat, row)[step];
        int tr = wo_seq_row_track(row), v;
        if (!n->note) return;
        if (what == 0) {
            if (tr == TR_CHORDS) { int k = b->ch.n ? b->ch.n : 1; n->note = (uint8_t)(1 + ((n->note - 1 + d) % k + k) % k); }
            else n->note = (uint8_t)wo_seq_step_scale(b, n->note, d);
        } else if (what == 1) { v = n->len + d; n->len = (uint8_t)(v < 1 ? 1 : v > 16 ? 16 : v); }
        else if (what == 2) { v = n->vel + d * 8; n->vel = (uint8_t)(v < 8 ? 8 : v > 127 ? 127 : v); }
    }
}

/* live: a note or a drum written at a step */
void wo_seq_write(wo_song_t *s, int pat, int row, int step, int note, int vel)
{
    s->made = 1;
    if (row < SQ_LANES) {
        s->pat[pat].hit[row] |= (uint16_t)(1u << step);
        if (vel > 110) s->pat[pat].acc[row] |= (uint16_t)(1u << step);
    } else {
        sq_note_t *n = &row_notes(s, pat, row)[step];
        n->note = (uint8_t)note; n->len = 1; n->vel = (uint8_t)(vel < 8 ? 8 : vel > 127 ? 127 : vel);
    }
}
void wo_seq_set_len(wo_song_t *s, int pat, int row, int step, int len)
{
    if (row < SQ_LANES) return;
    if (len < 1) len = 1;
    if (len > 16) len = 16;
    row_notes(s, pat, row)[step].len = (uint8_t)len;
}

void wo_seq_clear_row(wo_song_t *s, int pat, int row)
{
    int i;
    s->made = 1;
    if (row < SQ_LANES) { s->pat[pat].hit[row] = 0; s->pat[pat].acc[row] = 0; return; }
    for (i = 0; i < SQ_STEPS; i++) row_notes(s, pat, row)[i].note = 0;
}
void wo_seq_clear_pat(wo_song_t *s, int pat) { int r; for (r = 0; r < WO_SEQ_ROWS; r++) wo_seq_clear_row(s, pat, r); }

int wo_seq_copy_next(wo_song_t *s, int pat)
{
    uint8_t len;
    if (pat + 1 >= SQ_PATS) return -1;
    len = s->pat[pat + 1].len;
    s->pat[pat + 1] = s->pat[pat];
    s->pat[pat + 1].len = len;          /* the bar keeps the meter's length */
    s->made = 1;
    return pat + 1;
}

int wo_seq_chain_add(wo_song_t *s, int pat)
{
    if (s->nchain >= SQ_CHAIN) return -1;
    s->chain[s->nchain++] = (uint8_t)pat;
    s->chain_on = 1; s->made = 1;
    return 0;
}
void wo_seq_chain_drop(wo_song_t *s) { if (s->nchain) s->nchain--; if (!s->nchain) s->chain_on = 0; s->made = 1; }
void wo_seq_chain_clear(wo_song_t *s) { s->nchain = 0; s->chain_on = 0; s->made = 1; }

/* ================================================================ rolling a row */
static int pct(int p) { return wo_ri(0, 99) < p; }
/* where a step falls: 0 the bar's first, 1 a beat, 2 an off-beat eighth, 3 a sixteenth */
static int place(const wo_song_t *s, int st)
{
    if (st == 0) return 0;
    if (st % s->div == 0) return 1;
    if (s->div == 4) return st % 2 == 0 ? 2 : 3;
    return 2;
}

static void roll_drum(wo_song_t *s, int pat, int lane)
{
    static const uint8_t P[SQ_LANES][4] = {
        {96, 40, 18, 6},    /* kick */
        {0, 0, 5, 6},       /* snare: the backbeat below */
        {55, 70, 75, 22},   /* closed hat */
        {0, 4, 20, 3},      /* open hat */
        {0, 0, 3, 4},       /* clap: a backbeat, sometimes */
        {4, 8, 10, 8},      /* low tom */
        {4, 8, 10, 8},      /* high tom */
        {8, 14, 18, 14},    /* perc */
    };
    sq_pat_t *p = &s->pat[pat];
    int len = wo_seq_pat_len(s, pat), st, hats16 = pct(35), clap = pct(40);
    p->hit[lane] = 0; p->acc[lane] = 0;
    for (st = 0; st < len; st++) {
        int pl = place(s, st), beat = st / s->div, on = 0;
        if (lane == 1) on = (pl == 1 && (beat & 1)) ? pct(88) : pct(P[1][pl]);
        else if (lane == 4) on = (pl == 1 && (beat & 1)) ? pct(clap ? 70 : 8) : pct(P[4][pl]);
        else if (lane == 2 && hats16 && pl == 3) on = pct(70);
        else on = pct(P[lane][pl]);
        if (on) {
            p->hit[lane] |= (uint16_t)(1u << st);
            if (pl <= 1 && pct(35)) p->acc[lane] |= (uint16_t)(1u << st);
        }
    }
    if (lane == 3) p->hit[2] &= (uint16_t)~p->hit[3];           /* the open hat takes the closed one's place */
    if (lane == 2) p->hit[2] &= (uint16_t)~p->hit[3];
}

static void roll_melody(wo_song_t *s, const wo_basics_t *b, int pat, int row)
{
    sq_note_t *n = row_notes(s, pat, row);
    int tr = wo_seq_row_track(row), len = wo_seq_pat_len(s, pat), st, last = -1, cur;
    for (st = 0; st < SQ_STEPS; st++) n[st].note = 0;
    if (tr == TR_CHORDS) {
        int c0 = wo_seq_chord_at(s, b, pat, 0), style = wo_ri(0, 3), k;
        if (c0 < 0) return;
        for (st = 0; st < len; st++) {
            int pl = place(s, st), on = 0;
            if (style == 0) on = st == 0;                               /* held */
            else if (style == 1) on = pl <= 1;                          /* every beat */
            else if (style == 2) on = pl == 2 || st == 0;               /* off-beat stabs */
            else on = st == 0 || (pl == 2 && pct(45)) || (pl == 3 && pct(10));
            if (!on) continue;
            k = c0;
            if (st >= len / 2 && b->ch.n > 1 && pct(20)) k = (c0 + 1) % b->ch.n;
            n[st].note = (uint8_t)(k + 1);
            n[st].vel = (uint8_t)(pl <= 1 ? 92 : 78);
            last = st;
        }
        for (st = 0; st < len; st++) if (n[st].note) {           /* each chord rings until the next */
            int e = st + 1;
            while (e < len && !n[e].note) e++;
            n[st].len = (uint8_t)(style == 2 ? 1 : e - st);
        }
        (void)last;
        return;
    }
    cur = tr == TR_BASS ? 36 + chord_root_pc(s, b, pat, 0) : wo_seq_snap(b, 64 + wo_ri(0, 7));
    for (st = 0; st < len; st++) {
        int pl = place(s, st), on;
        static const uint8_t PB[4] = {92, 50, 34, 12}, PL[4] = {62, 50, 40, 20};
        on = pct(tr == TR_BASS ? PB[pl] : PL[pl]);
        if (!on) continue;
        if (tr == TR_BASS) {
            int r = 36 + chord_root_pc(s, b, pat, st), x = wo_ri(0, 99);
            if (r > 47) r -= 12;
            cur = x < 58 ? r : x < 76 ? wo_seq_snap(b, r + 7) : x < 86 ? r + 12 : wo_seq_step_scale(b, r, pct(50) ? 1 : -1);
        } else {
            int step = wo_ri(-2, 2), tries;
            cur = wo_seq_step_scale(b, cur, step);
            if (cur > 81) cur = wo_seq_step_scale(b, cur, -4);
            if (cur < 57) cur = wo_seq_step_scale(b, cur, 4);
            for (tries = 0; pl <= 1 && tries < 3 && !is_chord_tone(s, b, pat, st, cur); tries++) cur = wo_seq_step_scale(b, cur, 1);
        }
        n[st].note = (uint8_t)cur;
        n[st].vel = (uint8_t)(pl <= 1 ? 104 : 86);
        n[st].len = (uint8_t)(tr == TR_BASS ? (pct(45) ? 2 : 1) : wo_ri(1, 3));
        last = st;
    }
    (void)last;
}

void wo_seq_roll_row(wo_song_t *s, const wo_basics_t *b, int pat, int row)
{
    s->made = 1;
    if (row < SQ_LANES) roll_drum(s, pat, row);
    else roll_melody(s, b, pat, row);
}

/* everything but the chords: the kick, snare and hats always, two of the rest, bass and lead */
void wo_seq_roll_pat(wo_song_t *s, const wo_basics_t *b, int pat)
{
    int l;
    for (l = 0; l < SQ_LANES; l++) {
        if (l <= 2 || pct(30)) roll_drum(s, pat, l);
        else { s->pat[pat].hit[l] = 0; s->pat[pat].acc[l] = 0; }
    }
    roll_melody(s, b, pat, SQ_LANES + TR_BASS - 1);
    if (pct(70)) roll_melody(s, b, pat, SQ_LANES + TR_LEAD - 1);
    else wo_seq_clear_row(s, pat, SQ_LANES + TR_LEAD - 1);
    if (wo_seq_row_empty(s, pat, SQ_LANES + TR_CHORDS - 1)) roll_melody(s, b, pat, SQ_LANES + TR_CHORDS - 1);
    s->made = 1;
}

/* mutate: one or two small changes to a row */
void wo_seq_mutate_row(wo_song_t *s, const wo_basics_t *b, int pat, int row)
{
    int len = wo_seq_pat_len(s, pat), k, times = wo_ri(1, 2);
    s->made = 1;
    for (k = 0; k < times; k++) {
        int st = wo_ri(0, len - 1);
        if (row < SQ_LANES) {
            if (place(s, st) <= 1 && wo_ri(0, 2)) st = (st + 1) % len;    /* mostly off the beat */
            s->pat[pat].hit[row] ^= (uint16_t)(1u << st);
        } else {
            sq_note_t *n = row_notes(s, pat, row);
            int i, have[SQ_STEPS], nh = 0, tr = wo_seq_row_track(row);
            for (i = 0; i < len; i++) if (n[i].note) have[nh++] = i;
            if (!nh || pct(25)) { wo_seq_toggle(s, b, pat, row, st); continue; }
            i = have[wo_ri(0, nh - 1)];
            if (tr == TR_CHORDS) { if (b->ch.n > 1) n[i].note = (uint8_t)(1 + (n[i].note % b->ch.n)); }
            else if (pct(60)) n[i].note = (uint8_t)wo_seq_step_scale(b, n[i].note, pct(50) ? 1 : -1);
            else {
                int to = (i + (pct(50) ? 1 : len - 1)) % len;
                if (!n[to].note) { n[to] = n[i]; n[i].note = 0; }
            }
        }
    }
}
