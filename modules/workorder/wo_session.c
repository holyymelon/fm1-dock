/* SPDX-License-Identifier: GPL-3.0-only */
/* Tickets: issue, tick, break, the clock, scoring, game modes, the Run, closing and learning.
 * workorder.html's issue(), buildItems(), startMode(), startRun(), dealHand(), itemPoints(),
 * closeSession(), in C. */
#include "wo.h"

static uint32_t now0(void) { return 0; }
uint32_t (*wo_now)(void) = now0;
void (*wo_filed)(const wo_ticket_t *c) = 0;        /* the library's: a closed ticket */

uint32_t wo_elapsed(const wo_ticket_t *c)
{
    return c->elapsed + (c->running ? wo_now() - c->started : 0u);
}

static const wo_mode_t *mode_of(const wo_ticket_t *c) { return &WO_MODES[c->mode < WO_NMODES ? c->mode : M_FREE]; }
static float weight_of(uint8_t pkey) { return pkey < WO_NSKEYS ? (float)WO_SCOREW[pkey] : 8.0f; }
static uint8_t tier_of_item(const wo_ticket_t *c, const wo_item_t *it)
{
    if (it->tier < 3) return it->tier;
    if (it->pkey < WO_NFIELDS && c->sev[it->pkey] < 3) return c->sev[it->pkey];
    return TIER_FIRM;
}

static uint32_t par_ms(const wo_ticket_t *c)
{
    const wo_mode_t *m = mode_of(c);
    uint32_t w;
    if (m->window) return (uint32_t)m->window * 60000u;
    if (c->limit_ms) return c->limit_ms;
    w = (uint32_t)(c->nitems ? c->nitems : 1) * WO_PAR_MS;
    if (w < WO_DECAY_MIN_MS) w = WO_DECAY_MIN_MS;
    if (w > WO_DECAY_MAX_MS) w = WO_DECAY_MAX_MS;
    return w;
}

static float speed_mult(const wo_ticket_t *c, uint32_t at)
{
    float frac = (float)at / (float)par_ms(c);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    return WO_SPEED_MAX - (WO_SPEED_MAX - WO_SPEED_MIN) * frac;
}

static float risk_mult(uint8_t r)
{
    return r == RISK_FIGHTS ? WO_RISK_FIGHTS : r == RISK_DOUBLES ? WO_RISK_DOUBLES : r == RISK_PILES ? WO_RISK_PILES : 1.0f;
}

static uint32_t roundf_u(float x) { return x <= 0 ? 0u : (uint32_t)(x + 0.5f); }

int wo_item_points(const wo_ticket_t *c, const wo_item_t *it, uint32_t at)
{
    const wo_mode_t *m = mode_of(c);
    float base;
    if (it->flags & (I_BROKEN | I_WASBROKEN)) return 0;
    if (c->is_run && c->run.ended && !c->run.won) return 0;
    base = weight_of(it->pkey) * WO_SEVMULT[tier_of_item(c, it)];
    if (it->risk) return (int)roundf_u(base * speed_mult(c, at) * m->mult * risk_mult(it->risk));
    if (c->limit_ms && at > c->limit_ms) return 0;
    return (int)roundf_u(base * speed_mult(c, at) * m->mult);
}

static int strict_blown(const wo_ticket_t *c)
{
    int i;
    if (!mode_of(c)->strict) return 0;
    for (i = 0; i < c->nitems; i++) if (c->item[i].flags & I_BROKEN) return 1;
    return 0;
}

uint32_t wo_ticket_score(const wo_ticket_t *c)
{
    uint32_t n = 0; int i;
    if (strict_blown(c)) return 0;
    for (i = 0; i < c->nitems; i++) if (c->item[i].flags & I_CHECKED) n += c->item[i].pts;
    if (c->is_run) n += c->run.bonus;
    return n;
}

uint32_t wo_ticket_max(const wo_ticket_t *c)
{
    uint32_t n = 0; int i;
    for (i = 0; i < c->nitems; i++)
        n += roundf_u(weight_of(c->item[i].pkey) * WO_SEVMULT[tier_of_item(c, &c->item[i])] * WO_SPEED_MAX * mode_of(c)->mult);
    return n;
}

int wo_burst(const wo_ticket_t *c)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0; int n = 0, i;
    for (i = 0; i < c->nitems; i++)
        if ((c->item[i].flags & I_CHECKED) && c->item[i].wall) {
            if (c->item[i].wall < lo) lo = c->item[i].wall;
            if (c->item[i].wall > hi) hi = c->item[i].wall;
            n++;
        }
    return n > 2 && (hi - lo) < (uint32_t)n * 4000u;
}

void wo_item_label(const wo_item_t *it, char *out, int cap)
{
    if (it->label == LBL_META) { wo_strcpy(out, "Meta", cap); return; }
    if (it->label >= 100) { wo_pool_name((uint8_t)(it->label - 100), out, cap); return; }   /* a Run card */
    wo_field_name(it->label, out, cap);
}

/* ---------------------------------------------------------------- the ticket from the draft */
static wo_item_t *add_item(wo_ticket_t *c, uint8_t label, const char *text, uint16_t wk, uint8_t pkey)
{
    wo_item_t *it;
    if (!text[0] || c->nitems >= WO_MAXITEMS) return 0;
    it = &c->item[c->nitems++];
    wo_strcpy(it->text, text, WO_LINE);
    it->why[0] = 0; it->wk = wk; it->entry = NONE16; it->label = label; it->pkey = pkey;
    it->tier = NONE8; it->flags = 0; it->risk = 0; it->round = 0; it->ndev = 0; it->pts = 0;
    it->at = it->wall = it->broke_at = 0;
    return it;
}

static uint16_t line_wk(const wo_line_t *l)
{
    if (l->entry != NONE16) return (uint16_t)(WK_ENTRY + l->entry);
    if (l->custom != NONE8) return (uint16_t)(WK_CUSTOM + l->custom);
    return NONE16;
}

static void add_line_item(wo_ticket_t *c, const wo_line_t *l, uint8_t label, uint8_t pkey)
{
    wo_item_t *it = add_item(c, label, l->text, line_wk(l), pkey);
    int k;
    if (!it) return;
    it->tier = l->tier;
    it->entry = (l->flags & (L_DEV | L_HEAD | L_GEN)) ? NONE16 : l->entry;
    if (l->flags & L_HEAD) it->flags |= I_HEAD;
    if (l->flags & L_META) it->flags |= I_META;
    it->ndev = l->ndev;
    for (k = 0; k < l->ndev; k++) it->dev[k] = l->dev[k];
}

static void build_items(wo_ticket_t *c, const wo_draft_t *d)
{
    static const uint8_t LP[] = {F_GENRE, F_INTENT, F_FORM, F_CHORDS, F_HARMONY, F_MELODY, F_BASS, F_RHYTHM,
                                 F_MODULATION, F_SOURCE, F_TEXTURE, F_MIX};
    char t[WO_LINE];
    unsigned k;
    int i;
    c->nitems = 0;
    if (c->has_key) { wo_key_line(&c->key, t, WO_LINE); add_item(c, F_KEY, t, (uint16_t)(WK_SCALE + c->key.scale), F_KEY); }
    if (c->has_tempo) { wo_tempo_line(&c->tempo, t, WO_LINE); add_item(c, F_TEMPO, t, (uint16_t)(WK_BAND + c->tempo.band), F_TEMPO); }
    if (d->on[F_GEAR] && d->has[F_GEAR]) {
        const wo_gear_t *g = &d->gear;
        if (g->empty) add_item(c, F_GEAR, "No categories feed GEAR. Assign one in the Gear page.", NONE16, F_GEAR);
        if (g->nbanned) {
            wo_strcpy(t, "Banned this session: ", WO_LINE);
            for (i = 0; i < g->nbanned; i++) { if (i) wo_strcat(t, ", ", WO_LINE); wo_strcat(t, W.inv[g->banned[i]].name, WO_LINE); }
            add_item(c, F_GEAR, t, NONE16, F_GEAR);
        }
        for (i = 0; i < g->nslots; i++) {
            wo_item_t *it;
            wo_slot_text(&g->slot[i], t, WO_LINE);
            it = add_item(c, F_GEAR, t, g->slot[i].item >= 0 ? (uint16_t)(WK_ITEM + g->slot[i].item) : NONE16, F_GEAR);
            if (it && g->slot[i].item >= 0) { it->ndev = 1; it->dev[0] = (uint8_t)g->slot[i].item; }
        }
        if (g->exclusive) add_item(c, F_GEAR, "Nothing else may be loaded.", NONE16, F_GEAR);
        for (i = 0; i < d->nlines; i++) if (d->line[i].field == F_GEAR) add_line_item(c, &d->line[i], F_GEAR, F_GEAR);
    }
    for (k = 0; k < sizeof LP; k++) {
        uint8_t f = LP[k];
        if (!d->on[f] || !d->has[f]) continue;
        for (i = 0; i < d->nlines; i++) if (d->line[i].field == f) add_line_item(c, &d->line[i], f, f);
    }
    if (d->on[F_PROCESS] && d->has[F_PROCESS])
        for (i = 0; i < d->nlines; i++) if (d->line[i].field == F_PROCESS)
            add_line_item(c, &d->line[i], (d->line[i].flags & L_META) ? LBL_META : F_PROCESS, F_PROCESS);
    if (d->on[F_RULES] && d->has[F_RULES])
        for (i = 0; i < d->nlines; i++) if (d->line[i].field == F_RULES) add_line_item(c, &d->line[i], F_RULES, F_RULES);
}

static uint8_t top_sev(const wo_draft_t *d)
{
    uint8_t best = 0; int i;
    for (i = 0; i < WO_NFIELDS; i++) if (W.on[i] && d->sev[i] > best) best = d->sev[i];
    return best;
}

static void slug_into(char *o, const char *s, int cap)
{
    int n = wo_strlen(o), i;
    for (i = 0; s[i] && n < cap - 1; i++) {
        char ch = s[i];
        if (ch == '#') ch = 's';
        if (ch == ' ') ch = '-';
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) continue;
        o[n++] = wo_lower(ch);
    }
    o[n] = 0;
}

/* makeSetName: the FM-1 has no calendar, so the ticket's number stands in for the date */
static void make_set_name(const wo_draft_t *d, uint32_t id, char *o, int cap)
{
    char b[48];
    o[0] = 0;
    if (d->on[F_TEMPO] && d->has[F_TEMPO]) {
        if (d->tempo.bpm) { if (d->tempo.bpm < 100) wo_strcat(o, "0", cap); wo_catu(o, d->tempo.bpm, cap); }
        else { wo_catu(o, d->tempo.lo, cap); wo_strcat(o, "-", cap); wo_catu(o, d->tempo.hi, cap); }
    } else wo_strcat(o, "xxx", cap);
    wo_strcat(o, "_", cap);
    if (d->on[F_GENRE] && d->has[F_GENRE] && d->genre != NONE8) {
        wo_str(WO_GENRE[d->genre], b, 48); slug_into(o, b, cap); wo_strcat(o, "-", cap);
    }
    if (d->on[F_KEY] && d->has[F_KEY]) {
        if (d->key.root >= 0) wo_str(WO_ROOT[d->key.root], b, 48); else wo_strcpy(b, "any", 48);
        slug_into(o, b, cap); wo_strcat(o, "-", cap);
        wo_str(WO_SCALES[d->key.scale].name, b, 48); slug_into(o, b, cap);
    } else wo_strcat(o, "nokey", cap);
    wo_strcat(o, "_", cap);
    wo_strcat(o, top_sev(d) == 2 ? "BRUTAL" : top_sev(d) == 1 ? "FIRM" : "GENTLE", cap);
    wo_strcat(o, "_#", cap);
    if (id < 1000) wo_strcat(o, "0", cap);
    if (id < 100) wo_strcat(o, "0", cap);
    if (id < 10) wo_strcat(o, "0", cap);
    wo_catu(o, id, cap);
}

static void ticket_clear(wo_ticket_t *c)
{
    uint8_t *p = (uint8_t *)c; unsigned i;
    for (i = 0; i < sizeof *c; i++) p[i] = 0;
    c->genre = NONE8; c->anchor = NONE8;
}

void wo_issue(void)
{
    wo_ticket_t *c = &W.cur;
    wo_draft_t *d = &W.draft;
    int i;
    if (!wo_draft_ready()) return;
    ticket_clear(c);
    c->id = W.next_id++;
    c->mode = W.set.mode;
    c->anchor = W.set.anchor;
    c->has_key = d->on[F_KEY] && d->has[F_KEY];
    c->has_tempo = d->on[F_TEMPO] && d->has[F_TEMPO];
    c->has_chords = d->on[F_CHORDS] && d->has[F_CHORDS];
    c->key = d->key; c->tempo = d->tempo; c->chords = d->chords;
    c->genre = (d->on[F_GENRE] && d->has[F_GENRE]) ? d->genre : NONE8;
    for (i = 0; i < WO_NFIELDS; i++) c->sev[i] = d->sev[i];
    make_set_name(d, c->id, c->name, (int)sizeof c->name);
    c->nconf = (uint8_t)wo_conflicts(d, c->conf, WO_NCONF);
    c->started = wo_now(); c->running = 1;
    c->target_min = W.set.target_min;
    build_items(c, d);
    W.has_cur = 1;
}

/* ---------------------------------------------------------------- game modes */
void wo_start_mode(uint8_t mk)
{
    const wo_mode_t *m = &WO_MODES[mk];
    uint8_t keys[WO_NFIELDS], chosen[WO_NFIELDS], prevcount[WO_NFIELDS];
    int nk = 0, nc = 0, n, i, j, hasg = 0;
    uint8_t prevlock = W.set.genrelock;
    if (m->is_run) { wo_start_run(mk); return; }
    if (mk == M_FREE) { W.set.mode = M_FREE; return; }
    for (i = 0; i < WO_NFIELDS; i++) if (!m->focus || (m->focus >> i & 1u)) keys[nk++] = (uint8_t)i;
    if (m->focus && nk < m->lo) {               /* pad a narrow focus with random others */
        uint8_t all[WO_NFIELDS]; int na = 0;
        for (i = 0; i < WO_NFIELDS; i++) if (!(m->focus >> i & 1u)) all[na++] = (uint8_t)i;
        for (i = na - 1; i > 0; i--) { int r = wo_ri(0, i); uint8_t t = all[i]; all[i] = all[r]; all[r] = t; }
        for (i = 0; nk < m->lo && i < na; i++) keys[nk++] = all[i];
    }
    n = wo_ri(m->lo < nk ? m->lo : nk, m->hi < nk ? m->hi : nk);
    if (m->genre) W.set.genrelock = (uint8_t)wo_ri(0, WO_NGENRES - 1);
    while (nc < n && nk) {
        float wt[WO_NFIELDS], tot = 0, r;
        int k;
        for (k = 0; k < nk; k++) { wt[k] = wo_w(WK_PARAM + keys[k]) * wo_pool_bias(keys[k]); tot += wt[k]; }
        r = wo_frand() * tot;
        for (k = 0; k < nk - 1; k++) { r -= wt[k]; if (r <= 0) break; }
        chosen[nc++] = keys[k];
        keys[k] = keys[--nk];
    }
    if (m->genre) {
        for (i = 0; i < nc; i++) if (chosen[i] == F_GENRE) hasg = 1;
        if (!hasg) { if (nc) nc--; chosen[nc++] = F_GENRE; }
    }
    for (i = 0; i < WO_NFIELDS; i++) {
        int in = 0;
        for (j = 0; j < nc; j++) if (chosen[j] == i) in = 1;
        W.on[i] = (uint8_t)in;
    }
    {
        uint8_t ord[WO_NFIELDS]; int no = 0;
        for (i = 0; i < WO_NFIELDS; i++) if (W.on[WO_ORDER[i]]) ord[no++] = WO_ORDER[i];
        for (i = 0; i < no; i++) {
            uint8_t f = ord[i];
            if (m->sev == MSEV_LADDER) {
                float third = (float)no / 3.0f;
                W.sev[f] = (float)i < third ? TIER_GENTLE : ((float)i < third * 2 ? TIER_FIRM : TIER_BRUTAL);
            } else if (m->sev == MSEV_ESCALATE) {
                float r = wo_frand();
                W.sev[f] = r < 0.2f ? TIER_GENTLE : (r < 0.55f ? TIER_FIRM : TIER_BRUTAL);
            } else if (m->sev >= MSEV_GENTLE) W.sev[f] = (uint8_t)(m->sev - MSEV_GENTLE);
            else W.sev[f] = wo_pick_tier(f);
        }
    }
    for (i = 0; i < WO_NFIELDS; i++) { prevcount[i] = W.count[i]; W.lock[i] = 0; }
    if (m->count) for (i = 0; i < nc; i++) W.count[chosen[i]] = m->count;
    if (m->count_field != NONE8) W.count[m->count_field] = m->count_n;
    W.set.rollsize = 0;
    W.has_draft = 0;
    W.set.mode = mk;
    for (i = 0; i < WO_NFIELDS; i++) if (W.on[WO_ORDER[i]]) wo_roll_field(WO_ORDER[i]);
    wo_resolve_conflicts(16, 0);
    wo_issue();
    if (W.has_cur) {
        W.cur.mode = mk;
        W.cur.limit_ms = (uint32_t)m->limit * 60000u;
    }
    W.set.genrelock = prevlock;
    for (i = 0; i < WO_NFIELDS; i++) W.count[i] = prevcount[i];
}

/* ---------------------------------------------------------------- the Run */
static uint32_t deck_tags(const wo_ticket_t *c)
{
    uint32_t t = 0; int i;
    for (i = 0; i < c->nitems; i++)
        t |= c->item[i].entry != NONE16 ? WO_ENTRIES[c->item[i].entry].tags : wo_tags_of(c->item[i].text);
    return t;
}

static uint8_t risk_of(const wo_ticket_t *c, uint8_t pool, uint8_t tier, uint32_t tags, uint32_t have)
{
    int i, nb = 0;
    if (wo_clashes(tags, have)) return RISK_FIGHTS;
    for (i = 0; i < c->nitems; i++) if (c->item[i].label == 100 + pool) return RISK_DOUBLES;
    for (i = 0; i < c->nitems; i++) if (c->item[i].tier == TIER_BRUTAL) nb++;
    if (tier == TIER_BRUTAL && nb >= 2) return RISK_PILES;
    return RISK_NONE;
}

static uint16_t card_points(const wo_ticket_t *c, uint8_t pool, uint8_t tier, uint8_t risk)
{
    float base = weight_of(WO_POOL_SKEY[pool]) * WO_SEVMULT[tier] * mode_of(c)->mult * risk_mult(risk);
    return (uint16_t)roundf_u(base * speed_mult(c, wo_elapsed(c)));
}

static int run_pools(const wo_ticket_t *c, uint8_t *out)
{
    int n = 0, p;
    for (p = 0; p < WO_NPOOLS; p++) if (c->run.genre != NONE8 || p != P_GENRE) out[n++] = (uint8_t)p;
    return n;
}

static int draw_card(wo_ticket_t *c, uint8_t pool, uint8_t tier, wo_line_t *l)
{
    uint8_t g = pool == P_GENRE ? c->run.genre : NONE8;
    return wo_draw_pool(pool, tier, 1, l, 0, c->mode, g);
}

static void mk_card(wo_ticket_t *c, wo_card_t *k, uint8_t pool, uint8_t tier, const wo_line_t *l, uint32_t have)
{
    uint32_t tags = l->entry != NONE16 ? WO_ENTRIES[l->entry].tags : wo_tags_of(l->text);
    k->pool = pool; k->tier = tier;
    k->risk = risk_of(c, pool, tier, tags, have);
    k->entry = l->entry;
    k->wk = l->entry != NONE16 ? (uint16_t)(WK_ENTRY + l->entry) : (l->custom != NONE8 ? (uint16_t)(WK_CUSTOM + l->custom) : NONE16);
    wo_strcpy(k->text, l->text, WO_LINE);
    k->pts = card_points(c, pool, tier, k->risk);
}

static void deal_hand(wo_ticket_t *c)
{
    wo_run_t *r = &c->run;
    uint8_t tier = r->round >= 1 && r->round <= r->nladder ? r->ladder[r->round - 1] : TIER_BRUTAL;
    uint32_t have = deck_tags(c);
    uint8_t src[WO_NPOOLS], pools[WO_NPOOLS], used[WO_NPOOLS] = {0};
    int ns = run_pools(c, src), np = 0, want = r->handsize ? r->handsize : 3, guard = 0, q, i;
    wo_line_t l;
    if (want > WO_HAND) want = WO_HAND;
    r->nhand = 0;
    while (ns) {                                 /* the genre decides which pools come up */
        float wt[WO_NPOOLS], tot = 0, x;
        int k;
        for (k = 0; k < ns; k++) {
            uint8_t gg = c->run.genre;
            float pb = (gg < WO_NGENRES && WO_GENREBIAS[gg].has_pools) ? WO_GENREBIAS[gg].pools[src[k]] : 1.0f;
            wt[k] = wo_w(WK_PARAM + WO_POOL_FIELD[src[k]]) * pb;
            tot += wt[k];
        }
        x = wo_frand() * tot;
        for (k = 0; k < ns - 1; k++) { x -= wt[k]; if (x <= 0) break; }
        pools[np++] = src[k];
        src[k] = src[--ns];
    }
    if (r->round > 1)                            /* from round two: one card that fights the deck */
        for (q = 0; q < 45 && !r->nhand; q++) {
            uint8_t rp = pools[q % np];
            uint32_t tags;
            if (!draw_card(c, rp, tier, &l)) continue;
            tags = l.entry != NONE16 ? WO_ENTRIES[l.entry].tags : wo_tags_of(l.text);
            if (!risk_of(c, rp, tier, tags, have)) continue;
            used[rp] = 1;
            mk_card(c, &r->hand[r->nhand++], rp, tier, &l, have);
        }
    while (r->nhand < want && guard++ < 60) {
        uint8_t pool = pools[(r->nhand + guard) % np];
        int dup = 0;
        if (used[pool] && guard < 40) continue;
        if (!draw_card(c, pool, tier, &l)) continue;
        for (i = 0; i < r->nhand; i++) if (wo_streq(r->hand[i].text, l.text)) dup = 1;
        if (dup) continue;
        used[pool] = 1;
        mk_card(c, &r->hand[r->nhand++], pool, tier, &l, have);
    }
    for (i = r->nhand - 1; i > 0; i--) {
        int j = wo_ri(0, i);
        wo_card_t t = r->hand[i]; r->hand[i] = r->hand[j]; r->hand[j] = t;
    }
    guard = 0;
    while (!r->nhand && guard++ < 40) {          /* never leave the player with nothing to take */
        uint8_t lp = src[0];
        (void)lp;
        lp = pools[wo_ri(0, np - 1)];
        if (draw_card(c, lp, tier, &l) || draw_card(c, lp, (uint8_t)wo_ri(0, 2), &l))
            mk_card(c, &r->hand[r->nhand++], lp, tier, &l, have);
    }
}

void wo_start_run(uint8_t mk)
{
    const wo_mode_t *m = &WO_MODES[mk];
    wo_ticket_t *c = &W.cur;
    char b[40];
    int i;
    ticket_clear(c);
    c->id = W.next_id++;
    c->mode = mk; c->anchor = W.set.anchor;
    c->is_run = 1;
    c->run.round = 1;
    c->run.lives = c->run.maxlives = m->lives ? m->lives : 2;
    c->run.nladder = m->nladder;
    for (i = 0; i < m->nladder; i++) c->run.ladder[i] = m->ladder[i];
    c->run.handsize = m->hand ? m->hand : 3;
    c->run.genre = m->run_genre ? (uint8_t)wo_ri(0, WO_NGENRES - 1) : NONE8;
    for (i = 0; i < WO_NFIELDS; i++) c->sev[i] = NONE8;
    wo_strcpy(c->name, "RUN", (int)sizeof c->name);
    if (mk != M_ROGUELIKE) {
        wo_str(m->name, b, 40);
        wo_strcat(c->name, "_", (int)sizeof c->name);
        slug_into(c->name, wo_stristr(b, "The ") == b ? b + 4 : b, (int)sizeof c->name);
    }
    if (c->run.genre != NONE8) {
        wo_str(WO_GENRE[c->run.genre], b, 40);
        wo_strcat(c->name, "_", (int)sizeof c->name);
        slug_into(c->name, b, (int)sizeof c->name);
    }
    wo_strcat(c->name, "_#", (int)sizeof c->name);
    wo_catu(c->name, c->id, (int)sizeof c->name);
    c->started = wo_now(); c->running = 1;
    W.set.mode = mk;
    W.has_cur = 1;
    deal_hand(c);
}

void wo_take_card(uint8_t idx)
{
    wo_ticket_t *c = &W.cur;
    wo_card_t *k;
    wo_item_t *it;
    if (!W.has_cur || !c->is_run || c->run.ended || idx >= c->run.nhand) return;
    k = &c->run.hand[idx];
    it = add_item(c, (uint8_t)(100 + k->pool), k->text, k->wk, WO_POOL_SKEY[k->pool]);
    if (it) { it->tier = k->tier; it->entry = k->entry; it->round = c->run.round; it->risk = k->risk; }
    c->run.nhand = 0;
}

void wo_redeal(void)
{
    if (!W.has_cur || !W.cur.is_run || W.cur.run.ended) return;
    deal_hand(&W.cur);
}

static int run_round_done(const wo_ticket_t *c)
{
    int i, live = 0;
    for (i = 0; i < c->nitems; i++) {
        if (c->item[i].round != c->run.round) continue;
        live++;
        if (!(c->item[i].flags & (I_CHECKED | I_BROKEN))) return 0;
    }
    return live > 0;
}

void wo_run_advance(void)
{
    wo_ticket_t *c = &W.cur;
    if (!W.has_cur || !c->is_run || c->run.ended || c->run.nhand || !run_round_done(c)) return;
    if (c->run.round >= c->run.nladder) {
        c->run.ended = 1; c->run.won = 1; c->run.reason = 0;
        c->run.bonus = roundf_u(120.0f * (float)c->run.nladder * mode_of(c)->mult);
        return;
    }
    c->run.round++;
    deal_hand(c);
}

static void run_lose_life(wo_ticket_t *c, uint8_t tier)
{
    int lose = tier == TIER_BRUTAL ? 2 : 1;
    if (!c->is_run || c->run.ended) return;
    c->run.lives = c->run.lives > lose ? (uint8_t)(c->run.lives - lose) : 0;
    if (!c->run.lives) {
        c->run.ended = 1; c->run.won = 0;
        c->run.reason = tier == TIER_BRUTAL ? 1 : 2;     /* 1 a brutal card broken, 2 out of lives */
        c->run.bonus = 0;
    }
}

/* ---------------------------------------------------------------- the session's actions */
void wo_tick(uint8_t i)
{
    wo_ticket_t *c = &W.cur;
    wo_item_t *it;
    if (!W.has_cur || i >= c->nitems) return;
    it = &c->item[i];
    if (!c->running && !(it->flags & I_CHECKED)) return;      /* resume the clock to tick */
    if (!(it->flags & I_CHECKED)) {
        it->flags |= I_CHECKED;
        if (it->flags & I_BROKEN) {
            if (wo_elapsed(c) - it->broke_at > WO_GRACE_MS) it->flags |= I_WASBROKEN;
        }
        it->flags &= (uint8_t)~I_BROKEN;
        it->at = wo_elapsed(c);
        it->wall = wo_now() ? wo_now() : 1u;
        it->pts = (uint16_t)wo_item_points(c, it, it->at);
    } else {
        it->flags &= (uint8_t)~I_CHECKED;
        it->pts = 0; it->at = 0;
    }
    if (c->is_run) wo_run_advance();
}

void wo_break(uint8_t i, const char *why)
{
    wo_ticket_t *c = &W.cur;
    wo_item_t *it;
    if (!W.has_cur || i >= c->nitems) return;
    it = &c->item[i];
    it->flags |= I_BROKEN;
    it->flags &= (uint8_t)~I_CHECKED;
    wo_strcpy(it->why, why && why[0] ? why : "no reason given", WO_WHY);
    it->pts = 0; it->at = 0;
    it->broke_at = wo_elapsed(c);
    if (c->is_run) { run_lose_life(c, tier_of_item(c, it)); wo_run_advance(); }
}

void wo_retire(uint8_t i, const char *why)
{
    wo_ticket_t *c = &W.cur;
    uint16_t e;
    if (!W.has_cur || i >= c->nitems) return;
    e = c->item[i].entry;
    wo_break(i, why && why[0] ? why : "too vague");
    if (e != NONE16) wo_setbit(W.disabled, e, 1);
}

void wo_unbreak(uint8_t i)
{
    wo_ticket_t *c = &W.cur;
    wo_item_t *it;
    if (!W.has_cur || i >= c->nitems) return;
    it = &c->item[i];
    if (!(it->flags & I_BROKEN)) return;
    it->flags &= (uint8_t)~I_BROKEN;
    it->why[0] = 0;
    if (wo_elapsed(c) - it->broke_at > WO_GRACE_MS) it->flags |= I_WASBROKEN;
    if (c->is_run && c->run.lives < c->run.maxlives) c->run.lives++;   /* the life comes back with the break */
}

void wo_pause(void)
{
    wo_ticket_t *c = &W.cur;
    if (!W.has_cur) return;
    if (c->running) {
        c->elapsed = wo_elapsed(c); c->running = 0;
        c->paused_at = wo_now();
        c->pauses++;
    } else {
        if (c->paused_at) c->paused_ms += wo_now() - c->paused_at;
        c->paused_at = 0;
        c->started = wo_now(); c->running = 1;
    }
}

void wo_card_label(const wo_card_t *k, char *out, int cap) { wo_pool_name(k->pool, out, cap); }

/* closeSession: rate it, learn from it, file it */
void wo_close(uint8_t rating, const char *note)
{
    wo_ticket_t *c = &W.cur;
    float f;
    int i, k, done = 0, broke = 0;
    if (!W.has_cur) return;
    c->elapsed = wo_elapsed(c); c->running = 0;
    c->rating = rating;
    wo_strcpy(c->note, note ? note : "", WO_WHY);
    f = rating == RATE_KEEPER ? 1.15f : (rating == RATE_DEAD || rating == RATE_ABANDONED) ? 0.9f : 1.0f;
    if (f != 1.0f) {
        for (i = 0; i < c->nitems; i++) {
            const wo_item_t *it = &c->item[i];
            int brokenwk = 0, j;
            for (j = 0; j < c->nitems; j++)
                if ((c->item[j].flags & I_BROKEN) && c->item[j].wk != NONE16 && c->item[j].wk == it->wk) brokenwk = 1;
            if (it->wk != NONE16 && !(f > 1.0f && brokenwk)) wo_bump(it->wk, f);
            for (k = 0; k < it->ndev; k++) if (it->dev[k] < WO_MAXINV && it->wk != WK_ITEM + it->dev[k])
                wo_bump(WK_ITEM + it->dev[k], f);
            if (it->tier < 3 && it->pkey < WO_NSKEYS) {
                uint8_t sk = it->pkey == F_RULES ? SK_ARRANGEMENT : it->pkey;
                uint32_t pt = WK_PT + (uint32_t)sk * 3u + it->tier;
                if (it->flags & I_BROKEN) wo_bump(pt, 0.93f);
                else if (it->flags & I_CHECKED) wo_bump(pt, f > 1.0f ? 1.08f : 0.96f);
            }
        }
    }
    if (rating == RATE_ABANDONED)
        for (i = 0; i < c->nitems; i++)
            if ((c->item[i].flags & I_BROKEN) && c->item[i].wk != NONE16) wo_bump(c->item[i].wk, 0.75f);
    c->score = wo_ticket_score(c);
    for (i = 0; i < c->nitems; i++) {
        if (c->item[i].flags & I_CHECKED) done++;
        if (c->item[i].flags & I_BROKEN) broke++;
    }
    if (c->score > 0 || c->is_run) {             /* a run that ended badly is still on the record */
        wo_score_t *s;
        if (W.nscores >= WO_MAXSCORES) W.nscores = WO_MAXSCORES - 1;
        for (i = W.nscores; i > 0; i--) W.score[i] = W.score[i - 1];
        W.nscores++;
        s = &W.score[0];
        s->id = c->id; s->score = c->score; s->ms = c->elapsed;
        s->mode = c->mode; s->rating = rating; s->done = (uint8_t)done; s->total = c->nitems; s->broke = (uint8_t)broke;
        s->run_won = c->is_run && c->run.won;
        s->run_total = c->is_run ? c->run.nladder : 0;
        s->run_reached = c->is_run ? (c->run.round < c->run.nladder ? c->run.round : c->run.nladder) : 0;
        s->burst = (uint8_t)wo_burst(c);
        s->pauses = (uint8_t)(c->pauses > 255 ? 255 : c->pauses);
        wo_strcpy(s->name, c->name, 40);
    }
    if (wo_filed) wo_filed(c);
    W.has_cur = 0;
    W.has_draft = 0;
}
