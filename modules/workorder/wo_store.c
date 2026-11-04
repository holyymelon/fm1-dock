/* SPDX-License-Identifier: GPL-3.0-only */
/* Keeping things across power cycles, in fm1-dock's flash banks (8176 bytes each, a CRC each).
 *   bank 0  settings, roll setup, gear, learning weights, switched-off rules
 *   bank 1  your own rules and sample materials
 *   bank 2  the ticket in play          bank 3  the draft on the Roll page
 *   bank 4  the leaderboard             banks 5, 6  parked tickets
 *   banks 7, 8, 9  the library, as one ring of filed tickets, newest first
 * Text goes in byte-pair encoded (wo_bpe_encode), so a ticket of 30 rules fits in about 2 KB.
 * A bank that will not load (new FM-1, or a format change) leaves the defaults. */
#include "wo.h"
#include "wo_store.h"

wo_banks_t WB;

#define BANK_MAX 8176u
#define MAGIC0 0x57u                 /* 'W' */
#define FMT    3u                    /* bump when a record's layout changes */

static uint8_t buf[BANK_MAX];
static uint32_t bp, blen;
static int bover;

/* ---------------------------------------------------------------- writer / reader */
static void w8(uint32_t v) { if (bp < BANK_MAX) buf[bp++] = (uint8_t)v; else bover = 1; }
static void w16(uint32_t v) { w8(v & 0xFFu); w8(v >> 8); }
static void w32(uint32_t v) { w16(v & 0xFFFFu); w16(v >> 16); }
static void wraw(const void *p, uint32_t n) { const uint8_t *q = p; while (n--) w8(*q++); }
static void wstr(const char *s)
{
    uint8_t e[WO_LINE + 8];
    int n = wo_bpe_encode(s, e, (int)sizeof e);
    wraw(e, (uint32_t)n);
}
static void warr(const void *p, uint32_t n) { w16(n); wraw(p, n); }
static uint32_t r8(void) { return bp < blen ? buf[bp++] : (bover = 1, 0u); }
static uint32_t r16(void) { uint32_t a = r8(); return a | (r8() << 8); }
static uint32_t r32(void) { uint32_t a = r16(); return a | (r16() << 16); }
static void rraw(void *p, uint32_t n) { uint8_t *q = p; while (n--) *q++ = (uint8_t)r8(); }
static void rarr(void *p, uint32_t n, int take)
{
    uint32_t len = bp + 2 <= blen ? (uint32_t)buf[bp] | ((uint32_t)buf[bp + 1] << 8) : 0u, i;
    bp += 2;
    if (take && len == n) { uint8_t *q = p; for (i = 0; i < n; i++) q[i] = buf[bp + i]; }
    bp += len;
    if (bp > blen) bover = 1;
}
static void rstr(char *s, int cap)
{
    uint8_t e[WO_LINE + 8]; int n = 0;
    uint32_t c;
    do { c = r8(); if (n < (int)sizeof e - 1) e[n++] = (uint8_t)c; } while (c && !bover);
    e[n] = 0;
    wo_bpe_decode(e, s, cap);
}

static int save_bank(uint32_t idx)
{
    if (bover || !WB.save) return -1;
    return WB.save(idx, buf, bp);
}
static int load_bank(uint32_t idx)
{
    int n;
    bp = 0; bover = 0; blen = 0;
    if (!WB.load) return -1;
    n = WB.load(idx, buf, BANK_MAX);
    if (n <= 0) return -1;
    blen = (uint32_t)n;
    if (r8() != MAGIC0 || r8() != FMT) return -1;
    return 0;
}
static void begin(uint32_t kind) { bp = 0; bover = 0; w8(MAGIC0); w8(FMT); w8(kind); }

/* ---------------------------------------------------------------- bank 0: the state */
static void put_state(void)
{
    int i;
    begin(0);
    w16(WO_NENTRIES); w16(WO_NSTR); w16(WK_N);
    wraw(W.on, WO_NFIELDS); wraw(W.sev, WO_NFIELDS); wraw(W.lock, WO_NFIELDS);
    wraw(W.count, WO_NFIELDS); wraw(W.mixin, WO_NFIELDS);
    w8(sizeof W.set); wraw(&W.set, sizeof W.set);
    w32(W.next_id);
    warr(W.weight, WK_N);
    warr(W.disabled, sizeof W.disabled); warr(W.matoff, sizeof W.matoff);
    warr(W.cool, sizeof W.cool); warr(W.coolc, sizeof W.coolc);
    w8(W.ninv);
    for (i = 0; i < W.ninv; i++) { wstr(W.inv[i].name); w8(W.inv[i].roles); w8(W.inv[i].on); }
    for (i = 0; i < WO_NROLES; i++) w32(W.bucket[i]);
}
static void get_state(void)
{
    int i, same, n;
    if (load_bank(0) || r8() != 0) return;
    same = r16() == WO_NENTRIES;
    same &= r16() == WO_NSTR;
    same &= r16() == WK_N;
    rraw(W.on, WO_NFIELDS); rraw(W.sev, WO_NFIELDS); rraw(W.lock, WO_NFIELDS);
    rraw(W.count, WO_NFIELDS); rraw(W.mixin, WO_NFIELDS);
    if (r8() == sizeof W.set) rraw(&W.set, sizeof W.set); else return;
    W.next_id = r32();
    /* a rule pool exported from a newer workorder.html numbers its rules differently: the learning,
       the switched-off rules and the cooldown start again, the rest stays */
    rarr(W.weight, WK_N, same); rarr(W.disabled, sizeof W.disabled, same); rarr(W.matoff, sizeof W.matoff, same);
    rarr(W.cool, sizeof W.cool, same); rarr(W.coolc, sizeof W.coolc, same);
    n = (int)r8();
    if (n > WO_MAXINV || bover) return;
    W.ninv = (uint8_t)n;
    for (i = 0; i < n; i++) { rstr(W.inv[i].name, WO_NAMELEN); W.inv[i].roles = (uint8_t)r8(); W.inv[i].on = (uint8_t)r8(); }
    for (i = 0; i < WO_NROLES; i++) W.bucket[i] = r32();
    if (W.set.theme >= WO_NTHEMES) W.set.theme = 0;
}

/* ---------------------------------------------------------------- bank 1: your rules and materials */
static void put_custom(void)
{
    int i;
    begin(1);
    w8(W.ncustom);
    for (i = 0; i < W.ncustom; i++) { w8(W.custom[i].pool); w8(W.custom[i].tier); wstr(W.custom[i].text); }
    w8(W.nmatadd);
    for (i = 0; i < W.nmatadd; i++) { w8(W.matadd[i].cat); wstr(W.matadd[i].text); }
}
static void get_custom(void)
{
    int i, n;
    if (load_bank(1) || r8() != 1) return;
    n = (int)r8(); if (n > WO_MAXCUSTOM) return;
    for (i = 0; i < n; i++) { W.custom[i].pool = (uint8_t)r8(); W.custom[i].tier = (uint8_t)r8(); rstr(W.custom[i].text, WO_CUSTOMLEN); }
    W.ncustom = (uint8_t)n;
    n = (int)r8(); if (n > WO_MAXMATADD) return;
    for (i = 0; i < n; i++) { W.matadd[i].cat = (uint8_t)r8(); rstr(W.matadd[i].text, WO_MATLEN); }
    W.nmatadd = (uint8_t)n;
}

/* ---------------------------------------------------------------- tickets */
static void put_chords(const wo_chords_t *c)
{
    int i;
    w8(c->n);
    for (i = 0; i < c->n; i++) {
        wstr(c->c[i].label); w8(c->c[i].npc); wraw(c->c[i].pcs, 6); wraw(c->c[i].tone, 6); w8(c->c[i].root);
    }
    w8(c->shape); w8(c->alt); w8(c->root); w8(c->scale);
}
static void get_chords(wo_chords_t *c)
{
    int i;
    c->n = (uint8_t)r8(); if (c->n > WO_MAXCHORDS) c->n = 0;
    for (i = 0; i < c->n; i++) {
        rstr(c->c[i].label, 28); c->c[i].npc = (uint8_t)r8(); rraw(c->c[i].pcs, 6); rraw(c->c[i].tone, 6); c->c[i].root = (uint8_t)r8();
    }
    c->shape = (uint8_t)r8(); c->alt = (uint8_t)r8(); c->root = (uint8_t)r8(); c->scale = (uint8_t)r8();
}

static void put_ticket(const wo_ticket_t *c, uint32_t now_elapsed)
{
    int i;
    w32(c->id); wstr(c->name); w8(c->mode); w8(c->anchor);
    w8(c->has_key); w8(c->has_tempo); w8(c->has_chords);
    wraw(&c->key, sizeof c->key); wraw(&c->tempo, sizeof c->tempo); put_chords(&c->chords);
    w8(c->genre); wraw(c->sev, WO_NFIELDS);
    w32(now_elapsed); w8(c->running);
    w32(c->limit_ms); w32(c->target_min); w16(c->pauses); w16(c->parks);
    w32(c->paused_ms); w32(c->parked_ms);
    w8(c->nconf); for (i = 0; i < c->nconf; i++) wstr(c->conf[i]);
    w8(c->nitems);
    for (i = 0; i < c->nitems; i++) {
        const wo_item_t *it = &c->item[i];
        int k;
        wstr(it->text); wstr(it->why);
        w16(it->wk); w16(it->entry);
        w8(it->label); w8(it->pkey); w8(it->tier); w8(it->flags); w8(it->risk); w8(it->round);
        w8(it->ndev); for (k = 0; k < it->ndev; k++) w8(it->dev[k]);
        w16(it->pts); w32(it->at); w32(it->wall); w32(it->broke_at);
    }
    w8(c->is_run);
    if (c->is_run) {
        const wo_run_t *r = &c->run;
        w8(r->round); w8(r->lives); w8(r->maxlives); w8(r->ended); w8(r->won); w8(r->reason);
        w8(r->nladder); wraw(r->ladder, 8); w8(r->handsize); w8(r->genre); w32(r->bonus);
        w8(r->nhand);
        for (i = 0; i < r->nhand; i++) {
            w8(r->hand[i].pool); w8(r->hand[i].tier); w8(r->hand[i].risk);
            w16(r->hand[i].entry); w16(r->hand[i].pts); w16(r->hand[i].wk); wstr(r->hand[i].text);
        }
    }
    w8(c->rating); wstr(c->note); w32(c->score);
}

static int get_ticket(wo_ticket_t *c)
{
    int i;
    uint8_t *p = (uint8_t *)c; unsigned q;
    for (q = 0; q < sizeof *c; q++) p[q] = 0;
    c->id = r32(); rstr(c->name, (int)sizeof c->name); c->mode = (uint8_t)r8(); c->anchor = (uint8_t)r8();
    c->has_key = (uint8_t)r8(); c->has_tempo = (uint8_t)r8(); c->has_chords = (uint8_t)r8();
    rraw(&c->key, sizeof c->key); rraw(&c->tempo, sizeof c->tempo); get_chords(&c->chords);
    c->genre = (uint8_t)r8(); rraw(c->sev, WO_NFIELDS);
    c->elapsed = r32(); c->running = (uint8_t)r8();
    c->limit_ms = r32(); c->target_min = r32(); c->pauses = (uint16_t)r16(); c->parks = (uint16_t)r16();
    c->paused_ms = r32(); c->parked_ms = r32();
    c->nconf = (uint8_t)r8(); if (c->nconf > WO_NCONF) return -1;
    for (i = 0; i < c->nconf; i++) rstr(c->conf[i], WO_CONFLEN);
    c->nitems = (uint8_t)r8(); if (c->nitems > WO_MAXITEMS) return -1;
    for (i = 0; i < c->nitems; i++) {
        wo_item_t *it = &c->item[i];
        int k;
        rstr(it->text, WO_LINE); rstr(it->why, WO_WHY);
        it->wk = (uint16_t)r16(); it->entry = (uint16_t)r16();
        it->label = (uint8_t)r8(); it->pkey = (uint8_t)r8(); it->tier = (uint8_t)r8(); it->flags = (uint8_t)r8();
        it->risk = (uint8_t)r8(); it->round = (uint8_t)r8();
        it->ndev = (uint8_t)r8(); if (it->ndev > 4) return -1;
        for (k = 0; k < it->ndev; k++) it->dev[k] = (uint8_t)r8();
        it->pts = (uint16_t)r16(); it->at = r32(); it->wall = r32(); it->broke_at = r32();
        if (it->entry != NONE16 && it->entry >= WO_NENTRIES) it->entry = NONE16;
        if (it->wk != NONE16 && it->wk >= WK_N) it->wk = NONE16;
    }
    c->is_run = (uint8_t)r8();
    if (c->is_run) {
        wo_run_t *r = &c->run;
        r->round = (uint8_t)r8(); r->lives = (uint8_t)r8(); r->maxlives = (uint8_t)r8(); r->ended = (uint8_t)r8();
        r->won = (uint8_t)r8(); r->reason = (uint8_t)r8(); r->nladder = (uint8_t)r8(); rraw(r->ladder, 8);
        r->handsize = (uint8_t)r8(); r->genre = (uint8_t)r8(); r->bonus = r32();
        r->nhand = (uint8_t)r8(); if (r->nhand > WO_HAND || r->nladder > 8) return -1;
        for (i = 0; i < r->nhand; i++) {
            r->hand[i].pool = (uint8_t)r8(); r->hand[i].tier = (uint8_t)r8(); r->hand[i].risk = (uint8_t)r8();
            r->hand[i].entry = (uint16_t)r16(); r->hand[i].pts = (uint16_t)r16(); r->hand[i].wk = (uint16_t)r16();
            rstr(r->hand[i].text, WO_LINE);
        }
    }
    c->rating = (uint8_t)r8(); rstr(c->note, WO_WHY); c->score = r32();
    if (c->mode >= WO_NMODES) c->mode = M_FREE;
    return bover ? -1 : 0;
}

static void put_cur(void)
{
    begin(2);
    w8(W.has_cur);
    if (W.has_cur) put_ticket(&W.cur, wo_elapsed(&W.cur));
}
static void get_cur(void)
{
    if (load_bank(2) || r8() != 2) return;
    if (!r8()) return;
    if (get_ticket(&W.cur)) return;
    W.has_cur = 1;
    if (W.cur.running) { W.cur.running = 0; W.cur.powered_off = 1; W.cur.paused_at = 0; }   /* the clock stopped with the power */
}

/* ---------------------------------------------------------------- bank 3: the draft */
static void put_line(const wo_line_t *l)
{
    int k;
    wstr(l->text); w16(l->entry); w8(l->custom); w8(l->field); w8(l->tier); w8(l->flags);
    w8(l->ndev); for (k = 0; k < l->ndev; k++) w8(l->dev[k]);
}
static void get_line(wo_line_t *l)
{
    int k;
    rstr(l->text, WO_LINE); l->entry = (uint16_t)r16(); l->custom = (uint8_t)r8(); l->field = (uint8_t)r8();
    l->tier = (uint8_t)r8(); l->flags = (uint8_t)r8();
    l->ndev = (uint8_t)r8(); if (l->ndev > 4) { l->ndev = 0; bover = 1; }
    for (k = 0; k < l->ndev; k++) l->dev[k] = (uint8_t)r8();
    if (l->entry != NONE16 && l->entry >= WO_NENTRIES) l->entry = NONE16;
}
static void put_draft(void)
{
    const wo_draft_t *d = &W.draft;
    int i;
    begin(3);
    w8(W.has_draft);
    if (!W.has_draft) return;
    wraw(d->has, WO_NFIELDS); wraw(d->skip, WO_NFIELDS); wraw(d->sev, WO_NFIELDS); wraw(d->on, WO_NFIELDS);
    wraw(&d->key, sizeof d->key); wraw(&d->tempo, sizeof d->tempo);
    w8(d->gear.nslots);
    for (i = 0; i < d->gear.nslots; i++) {
        const wo_slot_t *s = &d->gear.slot[i];
        w8(s->role); w8((uint8_t)s->item); w8(s->count); w8((uint8_t)s->job); w8((uint8_t)s->limit); wstr(s->text);
    }
    w8(d->gear.nbanned); w8((uint8_t)d->gear.banned[0]); w8((uint8_t)d->gear.banned[1]);
    w8(d->gear.exclusive); w8(d->gear.empty);
    put_chords(&d->chords); w8(d->genre);
    w8(d->nlines);
    for (i = 0; i < d->nlines; i++) put_line(&d->line[i]);
}
static void get_draft(void)
{
    wo_draft_t *d = &W.draft;
    int i;
    if (load_bank(3) || r8() != 3 || !r8()) return;
    rraw(d->has, WO_NFIELDS); rraw(d->skip, WO_NFIELDS); rraw(d->sev, WO_NFIELDS); rraw(d->on, WO_NFIELDS);
    rraw(&d->key, sizeof d->key); rraw(&d->tempo, sizeof d->tempo);
    d->gear.nslots = (uint8_t)r8(); if (d->gear.nslots > WO_MAXSLOTS) return;
    for (i = 0; i < d->gear.nslots; i++) {
        wo_slot_t *s = &d->gear.slot[i];
        s->role = (uint8_t)r8(); s->item = (int8_t)r8(); s->count = (uint8_t)r8(); s->job = (int8_t)r8(); s->limit = (int8_t)r8();
        rstr(s->text, WO_LINE);
        if (s->item >= W.ninv) return;
    }
    d->gear.nbanned = (uint8_t)r8(); d->gear.banned[0] = (int8_t)r8(); d->gear.banned[1] = (int8_t)r8();
    d->gear.exclusive = (uint8_t)r8(); d->gear.empty = (uint8_t)r8();
    get_chords(&d->chords); d->genre = (uint8_t)r8();
    d->nlines = (uint8_t)r8(); if (d->nlines > WO_MAXLINES) return;
    for (i = 0; i < d->nlines; i++) get_line(&d->line[i]);
    if (!bover) W.has_draft = 1;
}

/* ---------------------------------------------------------------- bank 4: scores */
static void put_scores(void)
{
    int i;
    begin(4);
    w8(W.nscores);
    for (i = 0; i < W.nscores; i++) {
        const wo_score_t *s = &W.score[i];
        w32(s->id); w32(s->score); w32(s->ms);
        w8(s->mode); w8(s->rating); w8(s->done); w8(s->total); w8(s->broke);
        w8(s->run_won); w8(s->run_reached); w8(s->run_total); w8(s->burst); w8(s->pauses);
        wstr(s->name);
    }
}
static void get_scores(void)
{
    int i, n;
    if (load_bank(4) || r8() != 4) return;
    n = (int)r8(); if (n > WO_MAXSCORES) return;
    for (i = 0; i < n && !bover; i++) {
        wo_score_t *s = &W.score[i];
        s->id = r32(); s->score = r32(); s->ms = r32();
        s->mode = (uint8_t)r8(); s->rating = (uint8_t)r8(); s->done = (uint8_t)r8(); s->total = (uint8_t)r8(); s->broke = (uint8_t)r8();
        s->run_won = (uint8_t)r8(); s->run_reached = (uint8_t)r8(); s->run_total = (uint8_t)r8(); s->burst = (uint8_t)r8();
        s->pauses = (uint8_t)r8();
        rstr(s->name, 40);
        if (s->mode >= WO_NMODES) s->mode = M_FREE;
    }
    W.nscores = (uint8_t)(bover ? 0 : n);
}

/* ---------------------------------------------------------------- parked tickets: banks 5, 6 */
#define NPARK 2
static uint8_t park_used[NPARK];
static char park_name[NPARK][56];
static uint32_t park_elapsed[NPARK], park_items[NPARK];

static void scan_park(void)
{
    int i;
    for (i = 0; i < NPARK; i++) {
        park_used[i] = 0;
        if (load_bank(5u + (uint32_t)i) || r8() != 5 || !r8()) continue;
        park_elapsed[i] = r32(); rstr(park_name[i], 56);
        park_used[i] = 1;
    }
}

int wo_parked_count(void) { int i, n = 0; for (i = 0; i < NPARK; i++) n += park_used[i]; return n; }
int wo_parked_slot(int k) { int i; for (i = 0; i < NPARK; i++) if (park_used[i] && !k--) return i; return -1; }
const char *wo_parked_name(int slot) { return slot >= 0 && slot < NPARK ? park_name[slot] : ""; }
uint32_t wo_parked_elapsed(int slot) { (void)park_items; return slot >= 0 && slot < NPARK ? park_elapsed[slot] : 0; }

/* park: set the ticket aside with its clock stopped (0, or -1: both slots taken) */
int wo_park(void)
{
    int i, slot = -1;
    wo_ticket_t *c = &W.cur;
    if (!W.has_cur) return -1;
    for (i = 0; i < NPARK; i++) if (!park_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    if (c->running) { c->elapsed = wo_elapsed(c); c->running = 0; }
    if (c->paused_at) { c->paused_ms += wo_now() - c->paused_at; c->paused_at = 0; }
    c->parks++;
    begin(5); w8(1); w32(c->elapsed); wstr(c->name); put_ticket(c, c->elapsed);
    if (save_bank(5u + (uint32_t)slot)) return -2;
    park_used[slot] = 1;
    wo_strcpy(park_name[slot], c->name, 56);
    park_elapsed[slot] = c->elapsed;
    W.has_cur = 0;
    wo_store_mark(ST_CUR);
    return 0;
}

int wo_unpark(int slot)
{
    static wo_ticket_t tmp;
    if (slot < 0 || slot >= NPARK || !park_used[slot]) return -1;
    if (load_bank(5u + (uint32_t)slot) || r8() != 5 || !r8()) return -1;
    { char nm[56]; r32(); rstr(nm, 56); }
    if (get_ticket(&tmp)) return -1;
    if (W.has_cur && wo_park()) return -2;               /* the ticket in play goes where this one was */
    W.cur = tmp;
    W.cur.started = wo_now(); W.cur.running = 1; W.cur.powered_off = 0;
    W.has_cur = 1;
    W.set.mode = W.cur.mode;
    if (park_used[slot] && wo_streq(park_name[slot], tmp.name)) {
        begin(5); w8(0); save_bank(5u + (uint32_t)slot);
        park_used[slot] = 0;
    }
    wo_store_mark(ST_CUR | ST_STATE);
    return 0;
}

void wo_discard_parked(int slot)
{
    if (slot < 0 || slot >= NPARK || !park_used[slot]) return;
    begin(5); w8(0); save_bank(5u + (uint32_t)slot);
    park_used[slot] = 0;
}

/* ---------------------------------------------------------------- the library ring: banks 7..9 */
#define LIBBANKS 3u
#define LIBMAX (LIBBANKS * (BANK_MAX - 8u))
static uint8_t lib[LIBMAX];
static uint32_t liblen;
static uint16_t libcount;

static void get_lib(void)
{
    uint32_t b, n;
    liblen = 0; libcount = 0;
    for (b = 0; b < LIBBANKS; b++) {
        if (load_bank(7u + b) || r8() != 7) break;
        n = r16();
        if (n > BANK_MAX - 8u || bp + n > blen) break;
        rraw(lib + liblen, n);
        liblen += n;
        if (n < BANK_MAX - 8u) break;
    }
    {   /* count whole records */
        uint32_t p = 0;
        while (p + 2 <= liblen) {
            uint32_t l = lib[p] | ((uint32_t)lib[p + 1] << 8);
            if (!l || p + 2 + l > liblen) break;
            p += 2 + l; libcount++;
        }
        liblen = p;
    }
}

static int put_lib(void)
{
    uint32_t b, off = 0;
    int bad = 0;
    for (b = 0; b < LIBBANKS; b++) {
        uint32_t n = liblen - off > BANK_MAX - 8u ? BANK_MAX - 8u : liblen - off;
        begin(7); w16(n); wraw(lib + off, n);
        if (save_bank(7u + b)) bad = 1;
        off += n;
        if (n < BANK_MAX - 8u) break;
    }
    return bad ? -1 : 0;
}

/* file a closed ticket: newest first, the oldest fall off the end */
static void lib_add(const wo_ticket_t *c)
{
    uint32_t n, p, keep;
    begin(9); put_ticket(c, c->elapsed);
    if (bover) return;
    n = bp;
    keep = 0; p = 0;
    while (p + 2 <= liblen) {
        uint32_t l = lib[p] | ((uint32_t)lib[p + 1] << 8);
        if (p + 2 + l + n + 2 > LIBMAX) break;
        p += 2 + l; keep = p;
    }
    {
        uint32_t i;
        for (i = keep; i > 0; i--) lib[i - 1 + n + 2] = lib[i - 1];
        lib[0] = (uint8_t)(n & 0xFFu); lib[1] = (uint8_t)(n >> 8);
        for (i = 0; i < n; i++) lib[2 + i] = buf[i];
        liblen = keep + n + 2;
    }
    libcount = 0;
    p = 0;
    while (p + 2 <= liblen) { uint32_t l = lib[p] | ((uint32_t)lib[p + 1] << 8); p += 2 + l; libcount++; }
    wo_store_mark(ST_LIB);
}

int wo_lib_count(void) { return libcount; }

int wo_lib_get(int idx, wo_ticket_t *out)
{
    uint32_t p = 0, l;
    int k = 0;
    while (p + 2 <= liblen) {
        l = lib[p] | ((uint32_t)lib[p + 1] << 8);
        if (k == idx) {
            uint32_t i;
            for (i = 0; i < l && i < BANK_MAX; i++) buf[i] = lib[p + 2 + i];
            bp = 3; blen = l; bover = 0;              /* past begin()'s three bytes */
            return get_ticket(out);
        }
        p += 2 + l; k++;
    }
    return -1;
}

void wo_lib_delete(int idx)
{
    uint32_t p = 0, l, i;
    int k = 0;
    while (p + 2 <= liblen) {
        l = lib[p] | ((uint32_t)lib[p + 1] << 8);
        if (k == idx) {
            for (i = p; i + 2 + l < liblen; i++) lib[i] = lib[i + 2 + l];
            liblen -= 2 + l; libcount--;
            wo_store_mark(ST_LIB);
            return;
        }
        p += 2 + l; k++;
    }
}

/* ---------------------------------------------------------------- dirty records, flushed when quiet */
static uint32_t dirty, dirty_at, failed;

void wo_store_mark(uint32_t what) { dirty |= what; dirty_at = wo_now(); }
uint32_t wo_store_failed(void) { uint32_t f = failed; failed = 0; return f; }

int wo_store_flush(void)
{
    int bad = 0;
    if (dirty & ST_STATE) { put_state(); if (save_bank(0)) bad |= ST_STATE; }
    if (dirty & ST_CUSTOM) { put_custom(); if (save_bank(1)) bad |= ST_CUSTOM; }
    if (dirty & ST_CUR) { put_cur(); if (save_bank(2)) bad |= ST_CUR; }
    if (dirty & ST_DRAFT) { put_draft(); if (save_bank(3)) bad |= ST_DRAFT; }
    if (dirty & ST_SCORES) { put_scores(); if (save_bank(4)) bad |= ST_SCORES; }
    if (dirty & ST_LIB) { if (put_lib()) bad |= ST_LIB; }
    failed |= (uint32_t)bad;
    dirty = 0;
    return bad;
}

/* from the main loop: write what changed once things have been still for a moment */
int wo_store_poll(int quiet)
{
    uint32_t t = wo_now();
    if (!dirty) return 0;
    if ((quiet && t - dirty_at > 1500u) || t - dirty_at > 20000u) { wo_store_flush(); return 1; }
    return 0;
}

static void filed(const wo_ticket_t *c)
{
    lib_add(c);
    wo_store_mark(ST_SCORES | ST_STATE | ST_CUR | ST_DRAFT);
}

void wo_store_load_all(void)
{
    get_state();
    get_custom();
    get_draft();
    get_cur();
    get_scores();
    scan_park();
    get_lib();
    wo_filed = filed;
    dirty = 0;
}

void wo_store_wipe(void)
{
    int i;
    wo_defaults();
    for (i = 0; i < NPARK; i++) park_used[i] = 0;
    liblen = 0; libcount = 0;
    begin(5); w8(0); save_bank(5); save_bank(6);
    wo_store_mark(ST_ALL);
    wo_store_flush();
}
