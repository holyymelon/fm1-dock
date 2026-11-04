/* SPDX-License-Identifier: GPL-3.0-only */
/* The roll: workorder.html's rollers, drawPool, token fill and conflict checks, in C.
 * The names follow the HTML's functions so the two can be read side by side. */
#include "wo.h"

wo_state_t W;

static const char *const TIERNAME[3] = {"gentle", "firm", "brutal"};
const char *wo_tier_name(uint8_t t) { return t < 3 ? TIERNAME[t] : "?"; }

const char *wo_field_name(uint8_t f, char *buf, int cap)
{
    if (f < WO_NFIELDS) wo_str(WO_FIELD_LABEL[f], buf, cap);
    else wo_strcpy(buf, f == LBL_META ? "Meta" : "?", cap);
    return buf;
}

const char *wo_pool_name(uint8_t p, char *buf, int cap)
{
    wo_str(WO_POOL_LABEL[p], buf, cap);
    return buf;
}

/* ================================================================ state */
void wo_reset_weights(void)
{
    uint32_t i;
    for (i = 0; i < WK_N; i++) W.weight[i] = WQ_ONE;
}

void wo_defaults(void)
{
    int i;
    uint8_t *p = (uint8_t *)&W;
    for (i = 0; i < (int)sizeof W; i++) p[i] = 0;
    for (i = 0; i < WO_NFIELDS; i++) {
        W.on[i] = WO_FIELD_DEFON[i];
        W.sev[i] = WO_FIELD_DEFSEV[i];
    }
    W.ninv = WO_NSEED < WO_MAXINV ? WO_NSEED : WO_MAXINV;
    for (i = 0; i < W.ninv; i++) {
        wo_str(WO_SEED[i].name, W.inv[i].name, WO_NAMELEN);
        W.inv[i].roles = (uint8_t)(1u << WO_SEED[i].role);
        W.inv[i].on = 1;
    }
    for (i = 0; i < WO_NROLES; i++) W.bucket[i] = WO_DEFBUCKET[i];
    W.set.theme = 0; W.set.anchor = NONE8; W.set.cooldown = 1; W.set.noclash = 1;
    W.set.sound = 1; W.set.volume = 9; W.set.genrelock = NONE8; W.set.mode = M_FREE;
    W.set.minp = 4; W.set.maxp = 8; W.set.rsev = 0; W.set.holds = 1;
    wo_reset_weights();
    W.next_id = 1;
}

/* ================================================================ weights */
float wo_w(uint32_t wk) { return wk < WK_N ? (float)W.weight[wk] * (1.0f / 64.0f) : 1.0f; }

void wo_bump(uint32_t wk, float f)
{
    float v;
    if (wk >= WK_N) return;
    v = (float)W.weight[wk] * f;
    if (v < 16.0f) v = 16.0f;
    if (v > 192.0f) v = 192.0f;
    W.weight[wk] = (uint8_t)(v + 0.5f);
}

static uint8_t skey_of_field(uint8_t f) { return f == F_RULES ? SK_ARRANGEMENT : f; }
static uint32_t wk_pt(uint8_t skey, uint8_t tier) { return WK_PT + skey * 3u + tier; }

/* weighted severity for one parameter: learnt per pool and severity */
uint8_t wo_pick_tier(uint8_t f)
{
    float w[3], tot = 0, r;
    uint8_t t, sk = skey_of_field(f);
    for (t = 0; t < 3; t++) { w[t] = wo_w(wk_pt(sk, t)); tot += w[t]; }
    if (tot <= 0) return (uint8_t)wo_ri(0, 2);
    r = wo_frand() * tot;
    for (t = 0; t < 3; t++) { r -= w[t]; if (r <= 0) return t; }
    return 2;
}

/* pick one of n by weight: wt[] already holds w(key) * bias */
static int pick_w(const float *wt, int n)
{
    float tot = 0, r;
    int i;
    if (n <= 0) return -1;
    for (i = 0; i < n; i++) tot += wt[i];
    if (tot <= 0) return wo_ri(0, n - 1);
    r = wo_frand() * tot;
    for (i = 0; i < n; i++) { r -= wt[i]; if (r <= 0) return i; }
    return n - 1;
}

static void shuffle8(uint8_t *a, int n)
{
    int i;
    for (i = n - 1; i > 0; i--) {
        int j = wo_ri(0, i);
        uint8_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

/* ================================================================ bias: anchor x genre */
typedef struct { const wo_bias_t *a, *g; uint8_t anchor; } bias_t;

uint8_t wo_live_genre(void)
{
    if (W.has_cur && W.cur.is_run && W.cur.run.genre != NONE8) return W.cur.run.genre;
    if (W.set.genrelock != NONE8) return W.set.genrelock;
    if (W.has_draft && W.on[F_GENRE] && W.draft.has[F_GENRE] && W.draft.genre != NONE8) return W.draft.genre;
    return NONE8;
}

static bias_t bias(void)
{
    bias_t b;
    uint8_t g = wo_live_genre();
    b.anchor = W.set.anchor;
    b.a = W.set.anchor < WO_NANCHORS ? &WO_ANCHORBIAS[W.set.anchor] : 0;
    b.g = g < WO_NGENRES ? &WO_GENREBIAS[g] : 0;
    return b;
}
static float bias_band(const bias_t *b, int i)
{ return (b->a ? b->a->bands[i] : 1.0f) * (b->g ? b->g->bands[i] : 1.0f); }
static float bias_scale(const bias_t *b, int t)
{ return (b->a ? b->a->scales[t] : 1.0f) * (b->g ? b->g->scales[t] : 1.0f); }
static float bias_role(const bias_t *b, int r)
{ return (b->a ? b->a->roles[r] : 1.0f) * (b->g ? b->g->roles[r] : 1.0f); }
static float bias_rule(const bias_t *b, int16_t rule)
{
    int i;
    if (!b->a || rule <= 0) return 1.0f;
    for (i = 0; i < WO_NANCHORRULE; i++)
        if (WO_ANCHORRULE[i].anchor == b->anchor && WO_ANCHORRULE[i].rule == rule) return WO_ANCHORRULE[i].mul;
    return 1.0f;
}

/* the genre's lean towards a parameter */
float wo_pool_bias(uint8_t f)
{
    uint8_t g = wo_live_genre(), p;
    if (g >= WO_NGENRES || !WO_GENREBIAS[g].has_pools) return 1.0f;
    p = WO_FIELD_POOL[f];
    if (p == NONE8) return 1.0f;
    return WO_GENREBIAS[g].pools[p];
}

/* ================================================================ gear */
int wo_inv_on(uint8_t i) { return i < W.ninv && W.inv[i].on && W.inv[i].name[0]; }
static int feeds(uint8_t role, uint8_t f) { return (W.bucket[role] >> f) & 1u; }
static int item_feeds(uint8_t i, uint8_t f)
{
    uint8_t r;
    for (r = 0; r < WO_NROLES; r++) if ((W.inv[i].roles >> r & 1u) && feeds(r, f)) return 1;
    return 0;
}
static int bucket_items(uint8_t f, uint8_t *out)
{
    int n = 0; uint8_t i;
    for (i = 0; i < W.ninv; i++) if (wo_inv_on(i) && item_feeds(i, f)) out[n++] = i;
    return n;
}
static int by_role(uint8_t r, uint8_t *out)
{
    int n = 0; uint8_t i;
    for (i = 0; i < W.ninv; i++) if (wo_inv_on(i) && (W.inv[i].roles >> r & 1u)) out[n++] = i;
    return n;
}
static uint8_t first_role(uint8_t i)
{
    uint8_t r;
    for (r = 0; r < WO_NROLES; r++) if (W.inv[i].roles >> r & 1u) return r;
    return 0;
}
static float item_bias(const bias_t *b, uint8_t i)
{
    float m = 1.0f; uint8_t r;
    if (!b->a && !b->g) return 1.0f;
    for (r = 0; r < WO_NROLES; r++)
        if (W.inv[i].roles >> r & 1u) { float x = bias_role(b, r); if (x > m) m = x; }
    return m;
}
/* pickUnused: weighted by learning and the bias, never twice in one roll */
static int pick_unused(const uint8_t *pool, int n, uint8_t *used, const bias_t *b)
{
    uint8_t cand[WO_MAXINV]; float wt[WO_MAXINV];
    int k = 0, i, j;
    for (i = 0; i < n; i++) if (!wo_bit(used, pool[i])) cand[k++] = pool[i];
    if (!k) return -1;
    for (i = 0; i < k; i++) wt[i] = wo_w(WK_ITEM + cand[i]) * item_bias(b, cand[i]);
    j = pick_w(wt, k);
    wo_setbit(used, cand[j], 1);
    return cand[j];
}

/* ================================================================ token fill */
static int mat_live_items(uint8_t cat, uint16_t *out)      /* stock (not switched off), then yours */
{
    int n = 0, i;
    for (i = WO_MAT_START[cat]; i < WO_MAT_START[cat + 1]; i++)
        if (!wo_bit(W.matoff, (uint32_t)i)) out[n++] = (uint16_t)i;
    for (i = 0; i < W.nmatadd; i++)
        if (W.matadd[i].cat == cat) out[n++] = (uint16_t)(0x8000u | (uint16_t)i);
    return n;
}
static void any_material(char *out, int cap)
{
    uint8_t cats[WO_NMATCATS]; uint16_t it[64];
    int nc = 0, c, n;
    for (c = 0; c < WO_NMATCATS; c++) if (mat_live_items((uint8_t)c, it)) cats[nc++] = (uint8_t)c;
    if (!nc) { wo_strcpy(out, "something you recorded", cap); return; }
    c = cats[wo_ri(0, nc - 1)];
    n = mat_live_items((uint8_t)c, it);
    c = it[wo_ri(0, n - 1)];
    if (c & 0x8000) wo_strcpy(out, W.matadd[c & 0x7FFF].text, cap);
    else wo_str(WO_MAT[c], out, cap);
}
static void any_matcat(char *out, int cap)
{
    uint8_t cats[WO_NMATCATS]; uint16_t it[64];
    int nc = 0, c;
    for (c = 0; c < WO_NMATCATS; c++) if (mat_live_items((uint8_t)c, it)) cats[nc++] = (uint8_t)c;
    if (!nc) { wo_strcpy(out, "your own recordings", cap); return; }
    wo_str(WO_MATCAT[cats[wo_ri(0, nc - 1)]], out, cap);
}
static void any_genre(char *out, int cap)
{
    uint8_t here = wo_live_genre(), g;
    do g = (uint8_t)wo_ri(0, WO_NGENRES - 1); while (g == here && WO_NGENRES > 1);
    wo_str(WO_GENRE[g], out, cap);
}
static void any_role(const uint8_t *avoid, int na, char *out, int cap)
{
    uint8_t live[WO_NROLES], safe[WO_NROLES], it[WO_MAXINV];
    int nl = 0, ns = 0, r, i, j, k;
    for (r = 0; r < WO_NROLES; r++) if (by_role((uint8_t)r, it)) live[nl++] = (uint8_t)r;
    for (i = 0; i < nl; i++) {
        int n = by_role(live[i], it), clash = 0;
        for (j = 0; j < n && !clash; j++) for (k = 0; k < na; k++) if (it[j] == avoid[k]) clash = 1;
        if (!clash) safe[ns++] = live[i];
    }
    if (na && ns) { for (i = 0; i < ns; i++) live[i] = safe[i]; nl = ns; }
    r = nl ? live[wo_ri(0, nl - 1)] : wo_ri(0, WO_NROLES - 1);
    wo_str(WO_ROLE[r], out, cap);
    for (i = 0; out[i]; i++) out[i] = wo_lower(out[i]);
}
static int dev_name(void)
{
    uint8_t live[WO_MAXINV]; int n = 0; uint8_t i;
    for (i = 0; i < W.ninv; i++) if (wo_inv_on(i)) live[n++] = i;
    return n ? live[wo_ri(0, n - 1)] : -1;
}
static void chain_of(char *out, int cap)
{
    uint8_t pool[WO_MAXINV], used[(WO_MAXINV + 7) / 8] = {0};
    int n = bucket_items(F_MIX, pool), k = 0, i, want;
    if (n < 2) { n = 0; for (i = 0; i < W.ninv; i++) if (wo_inv_on((uint8_t)i)) pool[n++] = (uint8_t)i; }
    out[0] = 0;
    want = n < 3 ? n : 3;
    while (k < want) {
        int c;
        do c = pool[wo_ri(0, n - 1)]; while (wo_bit(used, (uint32_t)c));
        wo_setbit(used, (uint32_t)c, 1);
        if (k) wo_strcat(out, " -> ", cap);
        wo_strcat(out, W.inv[c].name, cap);
        k++;
    }
    if (!k) wo_strcpy(out, "your chain", cap);
}
static void euclid(char *out, int cap)
{
    int n = WO_TOK_ELENS[wo_ri(0, WO_NTOK_ELENS - 1)];
    int lo = n - 1 < 3 ? n - 1 : 3, hi = n - 2;
    if (lo < 1) lo = 1;
    if (hi < 1) hi = 1;
    out[0] = 0;
    wo_strcat(out, "E(", cap); wo_catu(out, (uint32_t)wo_ri(lo, hi), cap);
    wo_strcat(out, ",", cap); wo_catu(out, (uint32_t)n, cap); wo_strcat(out, ")", cap);
}
static void poly_pair(char *out, int cap)
{
    int a = WO_TOK_PCYC[wo_ri(0, WO_NTOK_PCYC - 1)], b = a, g = 0;
    while (b == a && g++ < 20) b = WO_TOK_PCYC[wo_ri(0, WO_NTOK_PCYC - 1)];
    if (b == a) b = a + 1;
    out[0] = 0;
    wo_catu(out, (uint32_t)a, cap); wo_strcat(out, " and ", cap); wo_catu(out, (uint32_t)b, cap);
    wo_strcat(out, " beats", cap);
}

/* fill(): devices first, so a {ROLE} next to a {DEV} never bans that device's own category */
void wo_fill(const char *t, wo_line_t *l)
{
    char tok[12], v[WO_LINE];
    int i, o = 0, pass;
    uint8_t devs[4]; int nd = 0, di = 0;
    l->ndev = 0;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; t[i]; ) {
            if (t[i] == '{') {
                int j = 1, k = 0;
                while (t[i + j] && t[i + j] != '}' && k < 11) tok[k++] = t[i + j++];
                tok[k] = 0;
                if (t[i + j] == '}') {
                    if (pass == 0) {
                        if (wo_streq(tok, "DEV") && nd < 4) { int d = dev_name(); devs[nd++] = (uint8_t)(d < 0 ? 255 : d); }
                    } else {
                        v[0] = 0;
                        if (wo_streq(tok, "DEV")) {
                            uint8_t d = di < nd ? devs[di++] : 255;
                            if (d != 255) { wo_strcpy(v, W.inv[d].name, WO_LINE); if (l->ndev < 4) l->dev[l->ndev++] = d; }
                            else wo_strcpy(v, "your main instrument", WO_LINE);
                        } else if (wo_streq(tok, "ROLE")) any_role(devs, nd, v, WO_LINE);
                        else if (wo_streq(tok, "MATCAT")) any_matcat(v, WO_LINE);
                        else if (wo_streq(tok, "MAT")) any_material(v, WO_LINE);
                        else if (wo_streq(tok, "GENRE")) any_genre(v, WO_LINE);
                        else if (wo_streq(tok, "CHAIN")) chain_of(v, WO_LINE);
                        else if (wo_streq(tok, "E")) euclid(v, WO_LINE);
                        else if (wo_streq(tok, "N")) { wo_catu(v, (uint32_t)wo_ri(WO_TOK_NMIN, WO_TOK_NMAX), WO_LINE); }
                        else if (wo_streq(tok, "P")) poly_pair(v, WO_LINE);
                        else { v[0] = '{'; v[1] = 0; wo_strcat(v, tok, WO_LINE); wo_strcat(v, "}", WO_LINE); }
                        for (k = 0; v[k] && o < WO_LINE - 1; k++) l->text[o++] = v[k];
                    }
                    i += j + 1;
                    continue;
                }
            }
            if (pass == 1 && o < WO_LINE - 1) l->text[o++] = t[i];
            i++;
        }
    }
    l->text[o] = 0;
    /* a chain names devices too */
    if (wo_stristr(t, "{CHAIN}")) {
        uint8_t k;
        for (k = 0; k < W.ninv && l->ndev < 4; k++)
            if (wo_inv_on(k) && wo_stristr(l->text, W.inv[k].name)) {
                int seen = 0, q;
                for (q = 0; q < l->ndev; q++) if (l->dev[q] == k) seen = 1;
                if (!seen) l->dev[l->ndev++] = k;
            }
    }
}

/* ================================================================ pools */
static uint8_t g_active_genre = NONE8;          /* ACTIVE_GENRE: the genre pool's filter */

typedef struct { uint16_t id; } cand_t;         /* entry index, or 0x8000 | custom index */
#define C_CUSTOM 0x8000u

static uint32_t cand_tags(uint16_t c)
{
    return (c & C_CUSTOM) ? wo_tags_of(W.custom[c & 0x7FFFu].text) : WO_ENTRIES[c].tags;
}
static int cand_cool(uint16_t c)
{
    return (c & C_CUSTOM) ? wo_bit(W.coolc, c & 0x7FFFu) : wo_bit(W.cool, c);
}
static void cand_setcool(uint16_t c, int on)
{
    if (c & C_CUSTOM) wo_setbit(W.coolc, c & 0x7FFFu, on); else wo_setbit(W.cool, c, on);
}
static uint32_t cand_wk(uint16_t c) { return (c & C_CUSTOM) ? WK_CUSTOM + (c & 0x7FFFu) : WK_ENTRY + c; }

static int pool_cands(uint8_t pool, uint8_t tier, uint16_t *out, int cap, uint8_t mode, uint8_t genre)
{
    int n = 0, i, a = WO_POOLIDX[pool][tier][0], cnt = WO_POOLIDX[pool][tier][1];
    for (i = a; i < a + cnt && n < cap; i++) {
        const wo_entry_t *e = &WO_ENTRIES[i];
        if (wo_bit(W.disabled, (uint32_t)i)) continue;
        if (e->mode != NONE8 && e->mode != mode) continue;
        if (pool == P_GENRE && genre != NONE8 && e->genre != NONE8 && e->genre != genre) continue;
        out[n++] = (uint16_t)i;
    }
    for (i = 0; i < W.ncustom && n < cap; i++)
        if (W.custom[i].pool == pool && W.custom[i].tier == tier) out[n++] = (uint16_t)(C_CUSTOM | i);
    return n;
}

/* drawPool: cooldown, no wording that fights what is already on the ticket, weighted */
int wo_draw_pool(uint8_t pool, uint8_t tier, int n, wo_line_t *out, uint32_t *have, uint8_t mode, uint8_t genre)
{
    static uint16_t all[192], avail[192], safe[192];
    static float wt[192];
    uint8_t taken[24] = {0};                    /* bit per position in all[] */
    int na = pool_cands(pool, tier, all, 192, mode, genre), got = 0, i, k;
    bias_t b = bias();
    int guard = W.set.noclash && have;
    for (i = 0; i < n; i++) {
        int nv = 0, ns = 0, j;
        uint16_t c;
        for (k = 0; k < na; k++)
            if (!wo_bit(taken, (uint32_t)k) && !(W.set.cooldown && cand_cool(all[k]))) avail[nv++] = (uint16_t)k;
        if (!nv) {                              /* the pool has cycled: start it again */
            for (k = 0; k < na; k++) cand_setcool(all[k], 0);
            for (k = 0; k < na; k++) if (!wo_bit(taken, (uint32_t)k)) avail[nv++] = (uint16_t)k;
        }
        if (guard && nv > 1) {
            for (k = 0; k < nv; k++) if (!wo_clashes(cand_tags(all[avail[k]]), *have)) safe[ns++] = avail[k];
            if (ns) { for (k = 0; k < ns; k++) avail[k] = safe[k]; nv = ns; }
        }
        if (!nv) break;
        for (k = 0; k < nv; k++) {
            uint16_t ck = all[avail[k]];
            wt[k] = wo_w(cand_wk(ck)) * ((ck & C_CUSTOM) ? 1.0f : bias_rule(&b, WO_ENTRIES[ck].rule));
        }
        j = pick_w(wt, nv);
        wo_setbit(taken, avail[j], 1);
        c = all[avail[j]];
        cand_setcool(c, 1);
        if (guard) *have |= cand_tags(c);
        {
            char tmpl[WO_LINE];
            wo_line_t *l = &out[got++];
            if (c & C_CUSTOM) wo_strcpy(tmpl, W.custom[c & 0x7FFFu].text, WO_LINE);
            else wo_str(WO_ENTRIES[c].text, tmpl, WO_LINE);
            wo_fill(tmpl, l);
            l->entry = (c & C_CUSTOM) ? NONE16 : c;
            l->custom = (c & C_CUSTOM) ? (uint8_t)(c & 0x7FFFu) : NONE8;
            l->tier = tier;
            l->flags = 0;
            l->field = WO_POOL_FIELD[pool];
        }
    }
    return got;
}

/* ================================================================ the draft */
static uint32_t g_have;                         /* ROLLHAVE: tags already on the ticket */
static int g_have_on;

static uint32_t line_tags(const wo_line_t *l)
{
    return l->entry != NONE16 ? WO_ENTRIES[l->entry].tags : wo_tags_of(l->text);
}
static uint32_t draft_tags(const wo_draft_t *d)
{
    uint32_t t = 0; int i;
    for (i = 0; i < d->nlines; i++) t |= line_tags(&d->line[i]);
    return t;
}

int wo_field_lines(const wo_draft_t *d, uint8_t f, uint8_t *idx, int cap)
{
    int n = 0, i;
    for (i = 0; i < d->nlines && n < cap; i++) if (d->line[i].field == f) idx[n++] = (uint8_t)i;
    return n;
}

static void drop_field(wo_draft_t *d, uint8_t f)
{
    int i, j = 0;
    for (i = 0; i < d->nlines; i++) if (d->line[i].field != f) { if (j != i) d->line[j] = d->line[i]; j++; }
    d->nlines = (uint8_t)j;
}

static wo_line_t *add_line(wo_draft_t *d, uint8_t f, uint8_t tier, uint8_t flags, const char *text)
{
    wo_line_t *l;
    if (d->nlines >= WO_MAXLINES) return 0;
    l = &d->line[d->nlines++];
    wo_strcpy(l->text, text, WO_LINE);
    l->entry = NONE16; l->custom = NONE8; l->field = f; l->tier = tier; l->flags = flags; l->ndev = 0;
    return l;
}

static int count_for(uint8_t f, uint8_t tier)
{
    if (W.count[f]) return W.count[f];
    if (f == F_RULES || f == F_PROCESS) return tier == TIER_GENTLE ? 1 : 2;
    if (f == F_GEAR) return tier == TIER_BRUTAL ? 2 : 1;
    return tier == TIER_BRUTAL ? 2 : 1;
}

/* drawLines: n lines from one pool; a mixed parameter picks each line's severity on its own */
static int draw_lines(wo_draft_t *d, uint8_t pool, uint8_t tier, int n, int mixed)
{
    static wo_line_t tmp[8];
    int want[3] = {0, 0, 0}, i, t, got = 0;
    uint8_t f = WO_POOL_FIELD[pool];
    for (i = 0; i < n; i++) want[mixed ? wo_pick_tier(f) : tier]++;
    for (t = 0; t < 3; t++) {
        int k, m;
        if (!want[t]) continue;
        m = wo_draw_pool(pool, (uint8_t)t, want[t] > 8 ? 8 : want[t], tmp, g_have_on ? &g_have : 0,
                         W.set.mode, g_active_genre);
        for (k = 0; k < m && d->nlines < WO_MAXLINES; k++) { d->line[d->nlines++] = tmp[k]; got++; }
    }
    return got;
}

/* ---- key and tempo */
static wo_key_t roll_key(uint8_t tier)
{
    wo_key_t k;
    uint8_t cand[WO_NSCALES]; float wt[WO_NSCALES];
    int n = 0, i;
    bias_t b = bias();
    for (i = 0; i < WO_NSCALES; i++) {
        uint8_t tg = WO_SCALES[i].tag;
        if (tier == TIER_GENTLE && tg != 0) continue;
        if (tier == TIER_FIRM && tg == 2) continue;
        cand[n] = (uint8_t)i;
        wt[n] = wo_w(WK_SCALE + i) * ((b.a || b.g) ? bias_scale(&b, tg) : 1.0f);
        n++;
    }
    k.scale = cand[pick_w(wt, n)];
    k.root = (int8_t)wo_ri(0, 11);
    if (tier == TIER_GENTLE) k.root = -1;
    k.forbidden = -1; k.nomod = 0;
    if (tier == TIER_BRUTAL) { k.forbidden = (int8_t)wo_ri(0, WO_NFORBIDDEN - 1); k.nomod = 1; }
    return k;
}

static wo_tempo_t roll_tempo(uint8_t tier)
{
    wo_tempo_t t;
    float wt[WO_NBANDS]; int i;
    bias_t b = bias();
    for (i = 0; i < WO_NBANDS; i++) wt[i] = wo_w(WK_BAND + i) * bias_band(&b, i);
    t.band = (uint8_t)pick_w(wt, WO_NBANDS);
    t.meter = (uint8_t)wo_ri(0, WO_METER_N[tier] - 1);
    t.tier = tier;
    t.noauto = tier == TIER_BRUTAL;
    if (tier == TIER_GENTLE) {
        t.lo = (uint8_t)wo_ri(WO_BANDS[t.band].lo, WO_BANDS[t.band].hi - 8);
        t.hi = (uint8_t)(t.lo + 8);
        t.bpm = 0;
    } else {
        t.bpm = (uint8_t)wo_ri(WO_BANDS[t.band].lo, WO_BANDS[t.band].hi);
        t.lo = t.hi = t.bpm;
    }
    return t;
}

void wo_key_line(const wo_key_t *k, char *o, int cap)
{
    char b[64];
    o[0] = 0;
    if (k->root >= 0) { wo_str(WO_ROOT[k->root], b, 64); wo_strcat(o, b, cap); wo_strcat(o, " ", cap); }
    else wo_strcat(o, "Any root, ", cap);
    wo_str(WO_SCALES[k->scale].name, b, 64); wo_strcat(o, b, cap);
    if (k->forbidden >= 0) { wo_strcat(o, " | avoid ", cap); wo_str(WO_FORBIDDEN[k->forbidden], b, 64); wo_strcat(o, b, cap); }
    if (k->nomod) wo_strcat(o, " | no modulation", cap);
}

void wo_tempo_line(const wo_tempo_t *t, char *o, int cap)
{
    char b[48];
    o[0] = 0;
    if (t->bpm) wo_catu(o, t->bpm, cap);
    else { wo_catu(o, t->lo, cap); wo_strcat(o, "-", cap); wo_catu(o, t->hi, cap); }
    wo_strcat(o, " BPM | ", cap);
    wo_str(WO_METER[t->tier][t->meter], b, 48); wo_strcat(o, b, cap);
    if (t->noauto) wo_strcat(o, " | no tempo automation", cap);
}

/* ---- chords: real harmonic material from the key on the ticket */
static const char *const SHARP[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const FLAT[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
static const char *note_name(int pc, int flats) { pc = ((pc % 12) + 12) % 12; return flats ? FLAT[pc] : SHARP[pc]; }

/* chordSymbol(): name a chord from its intervals above its own root, or give up honestly */
static int chord_symbol(const int *rel, int n, char *out, int cap)
{
    int has[12] = {0}, used[12] = {0}, i, third, fifth, sev, ext[6], ne = 0;
    char q[16] = "", tail[24] = "";
    int bad = 0;
    for (i = 0; i < n; i++) has[rel[i] % 12] = 1;
    for (i = 0; i < n; i++) if (rel[i] >= 13) ext[ne++] = rel[i];
    used[0] = 1;
    third = has[4] ? 1 : (has[3] ? 2 : 0);              /* 1 maj, 2 min */
    fifth = has[7] ? 1 : (has[6] ? 2 : (has[8] ? 3 : 0));  /* 1 P, 2 b5, 3 #5 */
    sev = has[11] ? 1 : (has[10] ? 2 : ((has[9] && !has[10] && third == 2 && fifth == 2) ? 3 : 0)); /* maj7 b7 bb7 */
    if (!third) {
        if (has[2] && has[7] && n == 3) { wo_strcpy(out, "sus2", cap); return 1; }
        if (has[5] && has[7] && n == 3) { wo_strcpy(out, "sus4", cap); return 1; }
        return 0;
    }
    used[third == 1 ? 4 : 3] = 1;
    if (fifth == 1) used[7] = 1; else if (fifth == 2) used[6] = 1; else if (fifth == 3) used[8] = 1;
    if (sev == 1) used[11] = 1; else if (sev == 2) used[10] = 1; else if (sev == 3) used[9] = 1;
    if (third == 1 && fifth == 1) wo_strcpy(q, sev == 1 ? "maj7" : (sev == 2 ? "7" : ""), 16);
    else if (third == 2 && fifth == 1) wo_strcpy(q, sev == 1 ? "m(maj7)" : (sev == 2 ? "m7" : "m"), 16);
    else if (third == 2 && fifth == 2) wo_strcpy(q, sev == 3 ? "dim7" : (sev == 2 ? "m7b5" : (sev == 1 ? "dim(maj7)" : "dim")), 16);
    else if (third == 1 && fifth == 3) wo_strcpy(q, sev == 1 ? "maj7#5" : (sev == 2 ? "7#5" : "aug"), 16);
    else if (third == 1 && fifth == 2) wo_strcpy(q, sev == 2 ? "7b5" : "maj(b5)", 16);
    else if (third == 2 && fifth == 3) wo_strcpy(q, sev == 1 ? "m(maj7)#5" : (sev == 2 ? "m7#5" : "m(#5)"), 16);
    else return 0;
    if (has[9] && !sev && !ne) { wo_strcat(tail, "6", 24); used[9] = 1; }
    for (i = 0; i < ne; i++) {
        int e = ext[i] % 12, s7 = wo_stristr(q, "7") != 0;
        if (e == 2) { wo_strcat(tail, s7 ? "9" : "add9", 24); used[2] = 1; }
        else if (e == 1) { wo_strcat(tail, "b9", 24); used[1] = 1; }
        else if (e == 3 && third == 1) { wo_strcat(tail, "#9", 24); used[3] = 1; }
        else if (e == 5) { wo_strcat(tail, s7 ? "11" : "add11", 24); used[5] = 1; }
        else if (e == 6) { wo_strcat(tail, "#11", 24); used[6] = 1; }
        else if (e == 9) { wo_strcat(tail, s7 ? "13" : "add13", 24); used[9] = 1; }
        else if (e == 8) { wo_strcat(tail, "b13", 24); used[8] = 1; }
        else bad = 1;
    }
    for (i = 0; i < n; i++) if (!used[rel[i] % 12]) bad = 1;
    if (bad) return 0;
    {   /* only a natural ninth absorbs the seventh */
        int lq = wo_strlen(q);
        if (tail[0] == '9' && lq && q[lq - 1] == '7') q[lq - 1] = 0;
    }
    wo_strcpy(out, q, cap);
    wo_strcat(out, tail, cap);
    return 1;
}

static void build_chord(int root_pc, const wo_scale_t *sc, int degree, uint8_t shape, int flats, wo_chord_t *c)
{
    const wo_shape_t *sh = &WO_SHAPES[shape < WO_NSHAPES ? shape : SH_TRIAD];
    int tones[6] = {0}, nt = 0, i, seen[12] = {0}, n = sc->n, rel[6];
    char sym[20];
    for (i = 0; i < sh->n; i++) {
        int idx = degree + sh->steps[i];
        int t = sc->iv[((idx % n) + n) % n] + 12 * (idx / n);
        int pc = ((t % 12) + 12) % 12;
        if (seen[pc]) continue;                 /* a five-note scale wraps: drop doublings */
        seen[pc] = 1;
        tones[nt++] = t;
    }
    c->npc = (uint8_t)nt;
    c->root = (uint8_t)((root_pc + tones[0]) % 12);
    for (i = 0; i < nt; i++) {
        rel[i] = tones[i] - tones[0];
        c->tone[i] = (int8_t)tones[i];
        c->pcs[i] = (uint8_t)((root_pc + tones[i]) % 12);
    }
    c->label[0] = 0;
    if (shape != SH_QUARTAL && shape != SH_QUARTAL4 && shape != SH_CLUSTER && chord_symbol(rel, nt, sym, 20)) {
        wo_strcpy(c->label, note_name(c->root, flats), 28);
        wo_strcat(c->label, sym, 28);
    } else {
        for (i = 0; i < nt; i++) {
            if (i) wo_strcat(c->label, "-", 28);
            wo_strcat(c->label, note_name(root_pc + tones[i], flats), 28);
        }
    }
}

typedef struct { uint8_t count, max, shape; uint8_t avoid; uint8_t any; const char *from; } hd_t;
static hd_t harmony_directives(const wo_draft_t *d)
{
    hd_t o = {0, 0, NONE8, 0, 0, 0};
    int i;
    if (!d->on[F_HARMONY]) return o;
    for (i = 0; i < d->nlines; i++) {
        const wo_line_t *l = &d->line[i];
        const wo_hd_t *h;
        if (l->field != F_HARMONY || l->entry == NONE16 || !WO_ENTRIES[l->entry].hd) continue;
        h = &WO_HD[WO_ENTRIES[l->entry].hd];
        if (h->count) o.count = h->count;
        if (h->max) o.max = o.max ? (h->max < o.max ? h->max : o.max) : h->max;
        if (h->shape != NONE8) o.shape = h->shape;
        o.avoid |= h->avoid;
        if (!o.any) o.from = l->text;
        o.any = 1;
    }
    return o;
}

static int shapes_for(uint8_t tier, uint8_t *out)
{
    static const uint8_t G[] = {SH_TRIAD, SH_TRIAD, SH_SEVENTH, SH_SUS2, SH_SUS4};
    static const uint8_t F[] = {SH_SEVENTH, SH_NINTH, SH_SUS2, SH_SUS4, SH_SIXTH, SH_QUARTAL, SH_TRIAD};
    static const uint8_t B[] = {SH_NINTH, SH_QUARTAL, SH_QUARTAL4, SH_CLUSTER, SH_SHELL, SH_SEVENTH};
    const uint8_t *s = tier == TIER_GENTLE ? G : (tier == TIER_FIRM ? F : B);
    int n = tier == TIER_GENTLE ? 5 : (tier == TIER_FIRM ? 7 : 6), i;
    for (i = 0; i < n; i++) out[i] = s[i];
    return n;
}

static void roll_chords(wo_draft_t *d, uint8_t tier)
{
    wo_key_t k = (d->on[F_KEY] && d->has[F_KEY]) ? d->key : roll_key(tier);
    hd_t hd = harmony_directives(d);
    const wo_scale_t *sc = &WO_SCALES[k.scale];
    int root = k.root >= 0 ? k.root : wo_ri(0, 11);
    char rn[8], line[WO_LINE], b[64];
    int flats, n, i, na = 0, nu = 0, tries = 0;
    uint8_t degs[12], allowed[8], al[8], shape, alt;
    wo_chord_t tmp;
    wo_chords_t *cs = &d->chords;
    wo_str(WO_ROOT[root], rn, 8);
    flats = (rn[1] == 'b') || wo_streq(rn, "F");
    n = tier == TIER_BRUTAL ? wo_ri(2, 3) : wo_ri(3, 4);
    if (hd.count) n = hd.count;
    if (hd.max && hd.max < n) n = hd.max;
    if (n < 2) n = 2;
    for (i = 0; i < sc->n; i++) degs[i] = (uint8_t)i;
    shuffle8(degs, sc->n);
    if (n > sc->n) n = sc->n;
    {
        int ns = shapes_for(tier, al);
        for (i = 0; i < ns; i++) if (!(hd.avoid >> al[i] & 1u)) allowed[na++] = al[i];
        if (!na) allowed[na++] = SH_SEVENTH;
    }
    shape = hd.shape != NONE8 ? hd.shape : allowed[wo_ri(0, na - 1)];
    alt = (hd.shape == NONE8 && tier == TIER_BRUTAL && wo_frand() < 0.4f) ? allowed[wo_ri(0, na - 1)] : shape;
    cs->n = 0;
    for (i = 0; i < n; i++) {
        int j, dup = 0;
        build_chord(root, sc, degs[i], (i % 2 && alt != shape) ? alt : shape, flats, &tmp);
        for (j = 0; j < cs->n; j++) if (wo_streq(cs->c[j].label, tmp.label)) dup = 1;
        if (!dup && cs->n < WO_MAXCHORDS) cs->c[cs->n++] = tmp;
    }
    nu = cs->n;
    while (nu < 2 && tries++ < 12) {          /* two degrees collapsed: widen, then fall back to triads */
        int j, dup = 0;
        build_chord(root, sc, wo_ri(0, sc->n - 1), tries > 6 ? SH_TRIAD : allowed[wo_ri(0, na - 1)], flats, &tmp);
        for (j = 0; j < cs->n; j++) if (wo_streq(cs->c[j].label, tmp.label)) dup = 1;
        if (!dup) { cs->c[cs->n++] = tmp; nu++; }
    }
    if (cs->n < 2) {
        build_chord(root, sc, 0, SH_TRIAD, flats, &cs->c[0]);
        build_chord(root, sc, sc->n / 2, SH_TRIAD, flats, &cs->c[1]);
        cs->n = 2; shape = SH_TRIAD;
    }
    cs->shape = shape; cs->alt = alt; cs->root = (uint8_t)root; cs->scale = k.scale;
    drop_field(d, F_CHORDS);
    line[0] = 0;
    for (i = 0; i < cs->n; i++) { if (i) wo_strcat(line, "  ->  ", WO_LINE); wo_strcat(line, cs->c[i].label, WO_LINE); }
    add_line(d, F_CHORDS, tier, L_HEAD | L_GEN, line);
    wo_str(WO_PROG[tier][wo_ri(0, WO_PROG_N[tier] - 1)], line, WO_LINE);
    add_line(d, F_CHORDS, tier, L_GEN, line);
    if (hd.shape == NONE8) wo_str(WO_VOICE[tier][wo_ri(0, WO_VOICE_N[tier] - 1)], line, WO_LINE);
    else wo_strcpy(line, "Shape set by the harmony constraint.", WO_LINE);
    add_line(d, F_CHORDS, tier, L_GEN, line);
    if (tier != TIER_GENTLE) {
        wo_strcpy(line, "Everything is drawn from ", WO_LINE);
        wo_strcat(line, rn, WO_LINE); wo_strcat(line, " ", WO_LINE);
        wo_str(sc->name, b, 64); wo_strcat(line, b, WO_LINE); wo_strcat(line, ".", WO_LINE);
        add_line(d, F_CHORDS, tier, L_GEN, line);
    }
}

/* ---- gear: voices only, six ticket shapes */
static int pick_job(uint8_t *used)
{
    int cand[WO_NGEARJOB], n = 0, i;
    for (i = 0; i < WO_NGEARJOB; i++) if (!wo_bit(used, (uint32_t)i)) cand[n++] = i;
    i = n ? cand[wo_ri(0, n - 1)] : wo_ri(0, WO_NGEARJOB - 1);
    wo_setbit(used, (uint32_t)i, 1);
    return i;
}

void wo_slot_text(const wo_slot_t *s, char *o, int cap)
{
    char b[WO_LINE];
    o[0] = 0;
    if (s->text[0]) { wo_strcpy(o, s->text, cap); return; }
    if (s->item < 0) {
        int i;
        wo_catu(o, s->count, cap);
        wo_strcat(o, " x ", cap);
        wo_str(WO_ROLE[s->role], b, 64);
        for (i = 0; b[i]; i++) b[i] = wo_lower(b[i]);
        wo_strcat(o, b, cap);
        wo_strcat(o, ", your choice", cap);
        if (s->job >= 0) { wo_strcat(o, " - one of them ", cap); wo_str(WO_GEARJOB[s->job], b, WO_LINE); wo_strcat(o, b, cap); }
        return;
    }
    wo_strcpy(o, W.inv[s->item].name, cap);
    if (s->job == -2) wo_strcat(o, " is the entire piece", cap);
    else if (s->job >= 0) { wo_strcat(o, " ", cap); wo_str(WO_GEARJOB[s->job], b, WO_LINE); wo_strcat(o, b, cap); }
    if (s->limit >= 0) {
        wo_strcat(o, s->job != -1 ? ". " : " - ", cap);
        wo_str(WO_GEARLIMIT[s->limit], b, WO_LINE); wo_cap1(b); wo_strcat(o, b, cap);
    }
    if (s->job == -1 && s->limit < 0) {
        int i;
        wo_str(WO_ROLE[s->role], b, 64);
        for (i = 0; b[i]; i++) b[i] = wo_lower(b[i]);
        wo_strcat(o, " - ", cap); wo_strcat(o, b, cap);
    }
}

static wo_slot_t *add_slot(wo_gear_t *g, uint8_t role, int item, int count, int job, int limit)
{
    wo_slot_t *s;
    if (g->nslots >= WO_MAXSLOTS) return 0;
    s = &g->slot[g->nslots++];
    s->role = role; s->item = (int8_t)item; s->count = (uint8_t)count; s->job = (int8_t)job; s->limit = (int8_t)limit;
    s->text[0] = 0;
    return s;
}

static void gear_directives(wo_draft_t *d, uint8_t tier, int exclusive)
{
    int start = d->nlines, i, tries = 0;
    draw_lines(d, P_GEAR, tier, count_for(F_GEAR, tier), W.mixin[F_GEAR]);
    if (!exclusive) return;
    /* the rack is already exclusive: a directive claiming "the only" would name a second one */
    for (i = start; i < d->nlines; ) {
        if (d->line[i].entry != NONE16 && (WO_ENTRIES[d->line[i].entry].flags & E_ONLY)) {
            int keep = d->nlines - start - 1, j;
            for (j = i; j < d->nlines - 1; j++) d->line[j] = d->line[j + 1];
            d->nlines--;
            while (tries++ < 20) {
                int before = d->nlines;
                draw_lines(d, P_GEAR, tier, 1, W.mixin[F_GEAR]);
                if (d->nlines > before) {
                    wo_line_t *nl = &d->line[d->nlines - 1];
                    int dup = 0, q;
                    for (q = start; q < d->nlines - 1; q++) if (wo_streq(d->line[q].text, nl->text)) dup = 1;
                    if ((nl->entry != NONE16 && (WO_ENTRIES[nl->entry].flags & E_ONLY)) || dup) d->nlines--;
                    else break;
                }
            }
            (void)keep;
            continue;
        }
        i++;
    }
}

static int take_role(uint8_t role, const uint8_t *live, int nl, uint8_t *used, const bias_t *b)
{
    uint8_t tmp[WO_MAXINV];
    int n = by_role(role, tmp);
    return n ? pick_unused(tmp, n, used, b) : pick_unused(live, nl, used, b);
}

static void roll_gear(wo_draft_t *d, uint8_t tier)
{
    wo_gear_t *g = &d->gear;
    bias_t b = bias();
    uint8_t live[WO_MAXINV], roles[WO_NROLES], tmp[WO_MAXINV];
    uint8_t used[(WO_MAXINV + 7) / 8] = {0}, jobs[(WO_NGEARJOB + 7) / 8] = {0};
    int nl = bucket_items(F_GEAR, live), nr = 0, r, i, shape;
    g->nslots = 0; g->nbanned = 0; g->exclusive = 0; g->empty = 0;
    drop_field(d, F_GEAR);
    if (!nl) { g->empty = 1; return; }
    for (r = 0; r < WO_NROLES; r++) if (feeds((uint8_t)r, F_GEAR) && by_role((uint8_t)r, tmp)) roles[nr++] = (uint8_t)r;
    if (!nr) for (r = 0; r < WO_NROLES; r++) {
        int k, any = 0;
        for (k = 0; k < nl; k++) if (W.inv[live[k]].roles >> r & 1u) any = 1;
        if (any) roles[nr++] = (uint8_t)r;
    }
    {
        static const uint8_t SG[] = {0, 1, 2}, SF[] = {1, 3, 4, 1}, SB[] = {5, 3, 4, 5};
        /* 0 budget, 1 roster, 2 ban, 3 pairing, 4 chain, 5 hero */
        shape = tier == TIER_GENTLE ? SG[wo_ri(0, 2)] : (tier == TIER_FIRM ? SF[wo_ri(0, 3)] : SB[wo_ri(0, 3)]);
    }
#define TAKE(role) take_role((role), live, nl, used, &b)
    if (shape == 0) {                               /* BUDGET: roles and counts, one gets a job */
        uint8_t gr[WO_NROLES]; int ng, total = 0;
        float wt[WO_MAXINV]; int k;
        for (i = 0; i < nr; i++) gr[i] = roles[i];
        shuffle8(gr, nr);
        ng = wo_ri(3, 4); if (ng > nr) ng = nr;
        for (i = 0; i < ng; i++) {
            int c = wo_ri(1, 3);
            if (total + c > 8) c = 8 - total > 1 ? 8 - total : 1;
            total += c;
            add_slot(g, gr[i], -1, c, i == 0 ? pick_job(jobs) : -1, -1);
        }
        for (k = 0; k < nl; k++) wt[k] = wo_w(WK_ITEM + live[k]) * item_bias(&b, live[k]);
        g->banned[g->nbanned++] = (int8_t)live[pick_w(wt, nl)];
        gear_directives(d, tier, 0);
        return;
    }
    if (shape == 2) {                               /* BAN: everything open but one or two things */
        int b1 = pick_unused(live, nl, used, &b), b2 = wo_ri(0, 1) ? pick_unused(live, nl, used, &b) : -1, keep;
        keep = TAKE(roles[wo_ri(0, nr - 1)]);
        if (keep >= 0) add_slot(g, first_role((uint8_t)keep), keep, 1, pick_job(jobs), -1);
        if (b1 >= 0) g->banned[g->nbanned++] = (int8_t)b1;
        if (b2 >= 0) g->banned[g->nbanned++] = (int8_t)b2;
        gear_directives(d, tier, 0);
        return;
    }
    if (shape == 5) {                               /* HERO: one instrument does everything */
        int h = pick_unused(live, nl, used, &b);
        add_slot(g, first_role((uint8_t)h), h, 1, -2, wo_ri(0, WO_NGEARLIMIT - 1));   /* -2: "is the entire piece" */
        if (wo_frand() < 0.45f) {
            int s2 = pick_unused(live, nl, used, &b);
            if (s2 >= 0) add_slot(g, first_role((uint8_t)s2), s2, 1, pick_job(jobs), -1);
        }
        g->exclusive = 1;
        gear_directives(d, tier, 1);
        return;
    }
    if (shape == 3 && nl > 1) {                     /* PAIRING: two instruments in a stated relationship */
        int a = pick_unused(live, nl, used, &b), c = pick_unused(live, nl, used, &b);
        if (a >= 0 && c >= 0) {
            char t[WO_LINE], o[WO_LINE];
            int k, x = 0;
            wo_slot_t *s = add_slot(g, first_role((uint8_t)a), a, 1, -1, -1);
            wo_str(WO_GEARPAIR[wo_ri(0, WO_NGEARPAIR - 1)], t, WO_LINE);
            for (k = 0; t[k] && x < WO_LINE - 1; ) {
                if (t[k] == '{' && (t[k + 1] == 'A' || t[k + 1] == 'B') && t[k + 2] == '}') {
                    const char *nm = W.inv[t[k + 1] == 'A' ? a : c].name; int q;
                    for (q = 0; nm[q] && x < WO_LINE - 1; q++) o[x++] = nm[q];
                    k += 3;
                } else o[x++] = t[k++];
            }
            o[x] = 0;
            if (s) wo_strcpy(s->text, o, WO_LINE);
            add_slot(g, first_role((uint8_t)c), c, 1, tier == TIER_BRUTAL ? -1 : pick_job(jobs),
                     tier == TIER_BRUTAL ? wo_ri(0, WO_NGEARLIMIT - 1) : -1);
            if (tier != TIER_BRUTAL) {
                int th = pick_unused(live, nl, used, &b);
                if (th >= 0 && wo_frand() < 0.5f) add_slot(g, first_role((uint8_t)th), th, 1, pick_job(jobs), -1);
            }
            g->exclusive = tier == TIER_BRUTAL;
            gear_directives(d, tier, g->exclusive);
            return;
        }
    }
    if (shape == 4 && nl > 1) {                     /* CHAIN: each one the only source for the next */
        int n = nl < (tier == TIER_BRUTAL ? 2 : 3) ? nl : (tier == TIER_BRUTAL ? 2 : 3), got[3], ng = 0;
        for (i = 0; i < n; i++) { int c = pick_unused(live, nl, used, &b); if (c >= 0) got[ng++] = c; }
        if (ng > 1) {
            char t[WO_LINE] = "";
            wo_slot_t *s;
            for (i = 0; i < ng; i++) { if (i) wo_strcat(t, " -> ", WO_LINE); wo_strcat(t, W.inv[got[i]].name, WO_LINE); }
            wo_strcat(t, ". Each one is the only source for the next.", WO_LINE);
            s = add_slot(g, first_role((uint8_t)got[0]), got[0], 1, -1, -1);
            if (s) wo_strcpy(s->text, t, WO_LINE);
            for (i = 1; i < ng; i++)
                add_slot(g, first_role((uint8_t)got[i]), got[i], 1, -1, i == ng - 1 ? wo_ri(0, WO_NGEARLIMIT - 1) : -1);
            g->exclusive = tier == TIER_BRUTAL;
            gear_directives(d, tier, g->exclusive);
            return;
        }
    }
    {                                               /* ROSTER: named instruments, each with a job */
        int want = tier == TIER_BRUTAL ? wo_ri(1, 2) : wo_ri(3, 4), rest[WO_MAXINV], nrest = 0;
        uint8_t fr[WO_NROLES];
        for (i = 0; i < nr; i++) fr[i] = roles[i];
        shuffle8(fr, nr);
        for (i = 0; i < nr && g->nslots < want; i++) {
            int it = TAKE(fr[i]);
            if (it >= 0) add_slot(g, fr[i], it, 1, pick_job(jobs),
                                  wo_frand() < (tier == TIER_BRUTAL ? 0.8f : 0.35f) ? wo_ri(0, WO_NGEARLIMIT - 1) : -1);
        }
        while (g->nslots < want) {
            int ex = pick_unused(live, nl, used, &b);
            if (ex < 0) break;
            add_slot(g, first_role((uint8_t)ex), ex, 1, pick_job(jobs), -1);
        }
        for (i = 0; i < nl; i++) if (!wo_bit(used, live[i])) rest[nrest++] = live[i];
        if (tier != TIER_BRUTAL && nrest && wo_frand() < 0.4f) g->banned[g->nbanned++] = (int8_t)rest[wo_ri(0, nrest - 1)];
        g->exclusive = tier == TIER_BRUTAL;
        gear_directives(d, tier, g->exclusive);
    }
#undef TAKE
}

/* ---- named devices with a job, for the other parameters */
static void dev_noun(uint8_t f, int n, char *o, int cap) { wo_str(WO_DEVNOUN[f][n == 1 ? 0 : 1], o, cap); }
static uint16_t dev_list_str(const uint16_t *const *tab, const uint8_t *ns, uint8_t f)
{
    if (tab[f] && ns[f]) return tab[f][wo_ri(0, ns[f] - 1)];
    return tab[WO_NFIELDS][wo_ri(0, ns[WO_NFIELDS] - 1)];
}

static void roll_devices(wo_draft_t *d, uint8_t f, uint8_t tier)
{
    uint8_t pool[WO_MAXINV], used[(WO_MAXINV + 7) / 8] = {0};
    int np = bucket_items(f, pool);
    bias_t b = bias();
    char t[WO_LINE], x[WO_LINE], nn[24];
    wo_line_t *l;
    int a, c, i;
    if (!np) return;
    t[0] = 0;
    if (d->nlines >= WO_MAXLINES) return;
    /* the device line goes first in its parameter: make room at the front */
    for (i = d->nlines; i > 0; i--) d->line[i] = d->line[i - 1];
    d->nlines++;
    l = &d->line[0];
    l->entry = NONE16; l->custom = NONE8; l->field = f; l->tier = tier; l->flags = L_DEV; l->ndev = 0;
#define NAME(k) W.inv[(k)].name
#define DEV(k) do { if (l->ndev < 4) l->dev[l->ndev++] = (uint8_t)(k); } while (0)
    if (tier == TIER_GENTLE) {
        float g = wo_frand();
        if (g < 0.4f) {
            int n = f == F_MIX ? wo_ri(3, 4) : wo_ri(1, 2);
            wo_strcpy(t, "Up to ", WO_LINE); wo_catu(t, (uint32_t)n, WO_LINE); wo_strcat(t, " ", WO_LINE);
            dev_noun(f, n, nn, 24); wo_strcat(t, nn, WO_LINE); wo_strcat(t, ", your choice.", WO_LINE);
        } else if (g < 0.75f) {
            a = pick_unused(pool, np, used, &b);
            wo_strcpy(t, NAME(a), WO_LINE); wo_strcat(t, " ", WO_LINE);
            wo_str(dev_list_str(WO_DEVJOB, WO_DEVJOB_N, f), x, WO_LINE); wo_strcat(t, x, WO_LINE); wo_strcat(t, ".", WO_LINE);
            DEV(a);
        } else {
            a = pick_unused(pool, np, used, &b);
            wo_strcpy(t, "Everything except ", WO_LINE); wo_strcat(t, NAME(a), WO_LINE);
            wo_strcat(t, ". That one is out.", WO_LINE);
            DEV(a);
        }
    } else if (tier == TIER_FIRM) {
        float r = wo_frand();
        if (r < 0.35f && np > 1) {
            int k, o = 0;
            a = pick_unused(pool, np, used, &b); c = pick_unused(pool, np, used, &b);
            wo_str(dev_list_str(WO_DEVPAIR, WO_DEVPAIR_N, f), x, WO_LINE);
            for (k = 0; x[k] && o < WO_LINE - 1; ) {
                if (x[k] == '{' && (x[k + 1] == 'A' || x[k + 1] == 'B') && x[k + 2] == '}') {
                    const char *nm = NAME(x[k + 1] == 'A' ? a : c); int q;
                    for (q = 0; nm[q] && o < WO_LINE - 1; q++) t[o++] = nm[q];
                    k += 3;
                } else t[o++] = x[k++];
            }
            t[o] = 0;
            DEV(a); DEV(c);
        } else if (r < 0.7f) {
            a = pick_unused(pool, np, used, &b);
            wo_strcpy(t, NAME(a), WO_LINE); wo_strcat(t, " ", WO_LINE);
            wo_str(dev_list_str(WO_DEVJOB, WO_DEVJOB_N, f), x, WO_LINE); wo_strcat(t, x, WO_LINE); wo_strcat(t, ". ", WO_LINE);
            wo_str(dev_list_str(WO_DEVLIMIT, WO_DEVLIMIT_N, f), x, WO_LINE); wo_cap1(x); wo_strcat(t, x, WO_LINE);
            wo_strcat(t, ".", WO_LINE);
            DEV(a);
        } else {
            int want = f == F_MIX ? wo_ri(2, 3) : wo_ri(1, 2), got[3], ng = 0;
            for (i = 0; i < want; i++) { int k = pick_unused(pool, np, used, &b); if (k >= 0) got[ng++] = k; }
            if (f == F_RHYTHM) wo_strcpy(t, "Percussion from ", WO_LINE);
            else if (f == F_SOURCE) wo_strcpy(t, "All material from ", WO_LINE);
            else if (f == F_MIX) wo_strcpy(t, "Processing: ", WO_LINE);
            else {
                wo_strcpy(t, "Only ", WO_LINE); wo_catu(t, (uint32_t)ng, WO_LINE); wo_strcat(t, " ", WO_LINE);
                dev_noun(f, ng, nn, 24); wo_strcat(t, nn, WO_LINE); wo_strcat(t, ": ", WO_LINE);
            }
            for (i = 0; i < ng; i++) { if (i) wo_strcat(t, ", ", WO_LINE); wo_strcat(t, NAME(got[i]), WO_LINE); DEV(got[i]); }
            wo_strcat(t, ". Nothing else.", WO_LINE);
        }
    } else {
        float r = wo_frand();
        int done = 0;
        if (r < 0.35f && np > 1) {
            int q = np < (f == F_MIX ? 3 : 2) ? np : (f == F_MIX ? 3 : 2), ch[3], nc = 0;
            for (i = 0; i < q; i++) { int k = pick_unused(pool, np, used, &b); if (k >= 0) ch[nc++] = k; }
            if (nc > 1) {
                wo_strcpy(t, f == F_RHYTHM ? "Every drum sound passes " :
                             (f == F_SOURCE || f == F_TEXTURE) ? "Everything derives from " :
                             f == F_MIX ? "Signal passes " : "The chain is ", WO_LINE);
                for (i = 0; i < nc; i++) { if (i) wo_strcat(t, " -> ", WO_LINE); wo_strcat(t, NAME(ch[i]), WO_LINE); DEV(ch[i]); }
                wo_strcat(t, ", in that order. Nothing else touches it.", WO_LINE);
                done = 1;
            }
        }
        if (!done) {
            a = pick_unused(pool, np, used, &b);
            if (a < 0) a = pool[wo_ri(0, np - 1)];
            wo_strcpy(t, NAME(a), WO_LINE); wo_strcat(t, " is the only ", WO_LINE);
            dev_noun(f, 1, nn, 24); wo_strcat(t, nn, WO_LINE); wo_strcat(t, " in the piece. ", WO_LINE);
            wo_str(dev_list_str(WO_DEVLIMIT, WO_DEVLIMIT_N, f), x, WO_LINE); wo_cap1(x); wo_strcat(t, x, WO_LINE);
            wo_strcat(t, ".", WO_LINE);
            DEV(a);
        }
    }
#undef NAME
#undef DEV
    wo_strcpy(l->text, t, WO_LINE);
}

/* ---- form */
static void roll_form(wo_draft_t *d, uint8_t tier)
{
    char t[WO_LINE], b[16];
    drop_field(d, F_FORM);
    if (tier == TIER_GENTLE) {
        static const uint16_t R[4][2] = {{90, 150}, {150, 240}, {240, 360}, {300, 480}};
        int k = wo_ri(0, 3);
        wo_strcpy(t, "Between ", WO_LINE); wo_mmss(b, R[k][0], 16); wo_strcat(t, b, WO_LINE);
        wo_strcat(t, " and ", WO_LINE); wo_mmss(b, R[k][1], 16); wo_strcat(t, b, WO_LINE); wo_strcat(t, " long.", WO_LINE);
        add_line(d, F_FORM, tier, L_GEN, t);
    } else if (tier == TIER_FIRM) {
        int secs = wo_ri(100, 330);
        wo_mmss(b, (uint32_t)(((secs + 5) / 10) * 10), 16);
        wo_strcpy(t, b, WO_LINE); wo_strcat(t, " long, in ", WO_LINE); wo_catu(t, (uint32_t)wo_ri(3, 5), WO_LINE);
        wo_strcat(t, " sections.", WO_LINE);
        add_line(d, F_FORM, tier, L_GEN, t);
    } else {
        static const char *const LB[5] = {"A", "B", "A'", "C", "D"};
        static const uint8_t BL[5] = {8, 16, 24, 32, 64};
        int n = wo_ri(3, 5), i;
        t[0] = 0;
        for (i = 0; i < n; i++) { if (i) wo_strcat(t, " | ", WO_LINE); wo_catu(t, BL[wo_ri(0, 4)], WO_LINE); }
        wo_strcat(t, " bars. ", WO_LINE);
        for (i = 0; i < n; i++) { if (i) wo_strcat(t, " ", WO_LINE); wo_strcat(t, LB[i], WO_LINE); }
        wo_strcat(t, ". No deviation.", WO_LINE);
        add_line(d, F_FORM, tier, L_GEN, t);
        add_line(d, F_FORM, tier, L_GEN, "The bar count is fixed before you write a note.");
    }
}

static void roll_genre(wo_draft_t *d, uint8_t tier)
{
    uint8_t g = W.set.genrelock < WO_NGENRES ? W.set.genrelock : (uint8_t)wo_ri(0, WO_NGENRES - 1);
    char name[32];
    int start, i;
    drop_field(d, F_GENRE);
    d->genre = g;
    wo_str(WO_GENRE[g], name, 32);
    add_line(d, F_GENRE, tier, L_HEAD, name);
    start = d->nlines;
    g_active_genre = g;
    draw_lines(d, P_GENRE, tier, count_for(F_GENRE, tier), W.mixin[F_GENRE]);
    g_active_genre = NONE8;
    (void)start; (void)i;
}

static void roll_extra(wo_draft_t *d, uint8_t f, uint8_t tier)
{
    uint8_t tmp[WO_MAXINV];
    drop_field(d, f);
    draw_lines(d, WO_FIELD_POOL[f], tier, count_for(f, tier), W.mixin[f]);
    if (bucket_items(f, tmp)) roll_devices(d, f, tier);
}

static void roll_rule_set(wo_draft_t *d, uint8_t f, uint8_t tier)
{
    drop_field(d, f);
    draw_lines(d, WO_FIELD_POOL[f], tier, count_for(f, tier), W.mixin[f]);
    if (f == F_PROCESS && tier == TIER_BRUTAL) {
        char t[WO_LINE];
        wo_str(WO_META[wo_ri(0, WO_NMETA - 1)], t, WO_LINE);
        add_line(d, f, TIER_BRUTAL, L_META, t);
    }
}

/* rollField: one parameter, against what the rest of the ticket already says */
void wo_roll_field(uint8_t f)
{
    wo_draft_t *d = &W.draft;
    int i;
    if (!W.has_draft) {
        uint8_t *p = (uint8_t *)d;
        for (i = 0; i < (int)sizeof *d; i++) p[i] = 0;
        d->genre = NONE8;
        W.has_draft = 1;
    }
    drop_field(d, f);
    d->has[f] = 0;
    if (f == F_GEAR) { d->gear.nslots = 0; d->gear.nbanned = 0; d->gear.exclusive = 0; }
    g_have_on = W.set.noclash;
    g_have = g_have_on ? draft_tags(d) : 0;
    for (i = 0; i < WO_NFIELDS; i++) d->on[i] = W.on[i];
    if (f == F_KEY) d->key = roll_key(W.sev[f]);
    else if (f == F_TEMPO) d->tempo = roll_tempo(W.sev[f]);
    else if (f == F_GEAR) roll_gear(d, W.sev[f]);
    else if (f == F_RULES || f == F_PROCESS) roll_rule_set(d, f, W.sev[f]);
    else if (f == F_FORM) roll_form(d, W.sev[f]);
    else if (f == F_GENRE) roll_genre(d, W.sev[f]);
    else if (f == F_CHORDS) roll_chords(d, W.sev[f]);
    else roll_extra(d, f, W.sev[f]);
    d->has[f] = 1;
    /* chords follow the key and the harmony rule: a reroll of either brings them along */
    if (f != F_CHORDS && (f == F_KEY || f == F_HARMONY) && W.on[F_CHORDS] && d->has[F_CHORDS])
        roll_chords(d, W.sev[F_CHORDS]);
    for (i = 0; i < WO_NFIELDS; i++) { d->sev[i] = W.sev[i]; d->on[i] = W.on[i]; }
    d->skip[f] = 0;
    g_have_on = 0;
}

static int active_fields(uint8_t *out)
{
    int n = 0, i;
    for (i = 0; i < WO_NFIELDS; i++) if (W.on[WO_ORDER[i]]) out[n++] = WO_ORDER[i];
    return n;
}

int wo_draft_ready(void)
{
    uint8_t af[WO_NFIELDS];
    int n = active_fields(af), i, any = 0;
    if (!W.has_draft || !n) return 0;
    for (i = 0; i < n; i++) {
        if (W.draft.has[af[i]]) any = 1;
        else if (!W.draft.skip[af[i]]) return 0;
    }
    return any;
}

void wo_roll_all(void)
{
    uint8_t act[WO_NFIELDS], chosen[WO_NFIELDS], rest[WO_NFIELDS];
    int na = active_fields(act), nc = 0, nr = 0, i, j, n = W.set.rollsize;
    for (i = 0; i < na; i++)
        if (W.lock[act[i]] && W.has_draft && W.draft.has[act[i]]) chosen[nc++] = act[i];
    if (n > 0 && n < na) {
        for (i = 0; i < na; i++) {
            int in = 0;
            for (j = 0; j < nc; j++) if (chosen[j] == act[i]) in = 1;
            if (!in) rest[nr++] = act[i];
        }
        shuffle8(rest, nr);
        for (i = 0; nc < (n > nc ? n : nc) && i < nr; i++) chosen[nc++] = rest[i];
    } else { nc = na; for (i = 0; i < na; i++) chosen[i] = act[i]; }
    if (!W.has_draft) wo_roll_field(act[0]);     /* makes the draft */
    for (i = 0; i < WO_NFIELDS; i++) W.draft.skip[i] = 0;
    for (i = 0; i < na; i++) {
        int in = 0;
        for (j = 0; j < nc; j++) if (chosen[j] == act[i]) in = 1;
        if (!in) { drop_field(&W.draft, act[i]); W.draft.has[act[i]] = 0; W.draft.skip[act[i]] = 1; }
    }
    for (i = 0; i < nc; i++) {
        uint8_t f = chosen[i];
        if (W.lock[f] && W.draft.has[f]) continue;
        if (W.set.mixsev) W.sev[f] = wo_pick_tier(f);
        wo_roll_field(f);
    }
    wo_resolve_conflicts(16, 0);
}

void wo_mutate(void)
{
    uint8_t pool[WO_NFIELDS], af[WO_NFIELDS];
    int na = active_fields(af), np = 0, i, n;
    uint32_t only = 0;
    for (i = 0; i < na; i++) if (!W.lock[af[i]] && W.has_draft && W.draft.has[af[i]]) pool[np++] = af[i];
    if (!np) return;
    n = W.set.mutn > 0 ? W.set.mutn : wo_ri(1, 2);
    if (n > np) n = np;
    shuffle8(pool, np);
    for (i = 0; i < n; i++) {
        if (W.set.mixsev) W.sev[pool[i]] = wo_pick_tier(pool[i]);
        wo_roll_field(pool[i]);
        only |= 1u << pool[i];
    }
    wo_resolve_conflicts(8, only);
}

static uint8_t sev_roll(uint8_t policy, uint8_t f)     /* MSEV_* */
{
    if (policy == MSEV_ESCALATE) {
        float r = wo_frand();
        return r < 0.2f ? TIER_GENTLE : (r < 0.55f ? TIER_FIRM : TIER_BRUTAL);
    }
    if (policy >= MSEV_GENTLE) return (uint8_t)(policy - MSEV_GENTLE);
    return wo_pick_tier(f);
}

/* fullRandom: which parameters, how hard, maybe an anchor */
void wo_full_random(void)
{
    uint8_t held[WO_NFIELDS], chosen[WO_NFIELDS], rest[WO_NFIELDS];
    int nh = 0, nc = 0, nr = 0, i, j, chaos = W.set.chaos, respect = chaos ? 0 : W.set.holds, lo, hi, n;
    for (i = 0; i < WO_NFIELDS; i++)
        if (respect && W.lock[i] && W.has_draft && W.draft.has[i]) held[nh++] = (uint8_t)i;
    lo = chaos ? 1 : (W.set.minp < 1 ? 1 : (W.set.minp > WO_NFIELDS ? WO_NFIELDS : W.set.minp));
    hi = chaos ? WO_NFIELDS : (W.set.maxp < lo ? lo : (W.set.maxp > WO_NFIELDS ? WO_NFIELDS : W.set.maxp));
    n = wo_ri(lo, hi);
    if (n < nh) n = nh;
    for (i = 0; i < nh; i++) chosen[nc++] = held[i];
    for (i = 0; i < WO_NFIELDS; i++) {
        int in = 0;
        for (j = 0; j < nc; j++) if (chosen[j] == i) in = 1;
        if (!in) rest[nr++] = (uint8_t)i;
    }
    while (nc < n && nr) {
        float wt[WO_NFIELDS];
        int k;
        for (k = 0; k < nr; k++) wt[k] = wo_w(WK_PARAM + rest[k]) * wo_pool_bias(rest[k]);
        k = pick_w(wt, nr);
        chosen[nc++] = rest[k];
        rest[k] = rest[--nr];
    }
    for (i = 0; i < WO_NFIELDS; i++) {
        int in = 0;
        for (j = 0; j < nc; j++) if (chosen[j] == i) in = 1;
        W.on[i] = (uint8_t)in;
    }
    for (i = 0; i < nc; i++) {
        int isheld = 0;
        for (j = 0; j < nh; j++) if (held[j] == chosen[i]) isheld = 1;
        if (!isheld) W.sev[chosen[i]] = sev_roll(chaos ? MSEV_ANY : (W.set.rsev ? W.set.rsev : MSEV_ANY), chosen[i]);
    }
    if (chaos || W.set.ranchor) W.set.anchor = wo_frand() < 0.55f ? (uint8_t)wo_ri(0, WO_NANCHORS - 1) : NONE8;
    if (!W.has_draft) wo_roll_field(chosen[0]);
    for (i = 0; i < WO_NFIELDS; i++) {
        W.draft.skip[i] = 0;
        if (!W.on[i]) { drop_field(&W.draft, (uint8_t)i); W.draft.has[i] = 0; }
    }
    for (i = 0; i < WO_NFIELDS; i++) {
        uint8_t f = WO_ORDER[i];
        int isheld = 0;
        if (!W.on[f]) continue;
        for (j = 0; j < nh; j++) if (held[j] == f) isheld = 1;
        if (!isheld) wo_roll_field(f);
    }
    wo_resolve_conflicts(16, 0);
}

/* rerollLine: one line of one parameter */
int wo_reroll_line(uint8_t f, uint8_t li)
{
    wo_draft_t *d = &W.draft;
    wo_line_t *l, nl[2];
    uint8_t idx[WO_MAXLINES];
    int n = wo_field_lines(d, f, idx, WO_MAXLINES), t;
    if (li >= n) return 0;
    l = &d->line[idx[li]];
    t = l->tier < 3 ? l->tier : W.sev[f];
    if (f == F_CHORDS) { roll_chords(d, W.sev[F_CHORDS]); return 1; }
    if (f == F_GENRE && (l->flags & L_HEAD)) {
        uint8_t was = d->genre; int g = 0;
        do roll_genre(d, W.sev[F_GENRE]); while (W.set.genrelock == NONE8 && d->genre == was && g++ < 12);
        return 1;
    }
    if (f == F_FORM) { roll_form(d, (uint8_t)t); return 1; }
    if (l->flags & L_DEV) {
        int k, j = 0;
        for (k = 0; k < d->nlines; k++) if (k != idx[li]) d->line[j++] = d->line[k];
        d->nlines = (uint8_t)j;
        roll_devices(d, f, (uint8_t)t);
        return 1;
    }
    if (l->flags & L_META) { wo_str(WO_META[wo_ri(0, WO_NMETA - 1)], l->text, WO_LINE); return 1; }
    if (WO_FIELD_POOL[f] == NONE8) return 0;
    {
        uint32_t have = draft_tags(d);
        uint8_t gen = f == F_GENRE ? d->genre : NONE8;
        int m = wo_draw_pool(WO_FIELD_POOL[f], (uint8_t)t, 1, nl, W.set.noclash ? &have : 0, W.set.mode, gen);
        if (!m) return 0;
        nl[0].field = f; nl[0].tier = (uint8_t)t;
        *l = nl[0];
    }
    return 1;
}

/* the gear's lines as the ticket will read them (ticketTextLines) */
static int ban_pat(const char *t)
{
    return wo_stristr(t, "is banned") || wo_stristr(t, "Banned this session") || wo_stristr(t, "is out") ||
           wo_stristr(t, "nothing at all from your") || wo_stristr(t, "banned outright") || wo_stristr(t, "Nothing from your");
}

/* ROLEBAN_PAT: the category a rule bans outright, upper case, or "" */
static void role_ban(const char *t, char *out, int cap)
{
    static const char *const PRE[] = {"nothing at all from your ", "nothing from your ", 0};
    const char *p, *q;
    int i, k = 0;
    out[0] = 0;
    for (i = 0; PRE[i]; i++) {
        p = wo_stristr(t, PRE[i]);
        if (!p) continue;
        p += wo_strlen(PRE[i]);
        q = wo_stristr(p, " category");
        if (!q) continue;
        while (p < q && k < cap - 1) out[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p), p++;
        out[k] = 0;
        return;
    }
    q = wo_stristr(t, " category is banned outright");
    if (q) {
        p = q;
        while (p > t && !(p - t >= 5 && wo_stristr(p - 5, "your ") == p - 5)) p--;
        if (p > t) {
            while (p < q && k < cap - 1) out[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p), p++;
            out[k] = 0;
        }
    }
}

static void add_conf(char out[][WO_CONFLEN], int *n, int cap, const char *s)
{
    int i;
    for (i = 0; i < *n; i++) if (wo_streq(out[i], s)) return;
    if (*n < cap) wo_strcpy(out[(*n)++], s, WO_CONFLEN);
}

static int has_frag(const wo_draft_t *d, const char *frag)
{
    int i;
    for (i = 0; i < d->nlines; i++) {
        const wo_line_t *l = &d->line[i];
        if (!d->on[l->field]) continue;
        if (l->field == F_FORM || l->field == F_GEAR || l->field == F_INTENT || l->field == F_HARMONY ||
            l->field == F_MELODY || l->field == F_BASS || l->field == F_RHYTHM || l->field == F_SOURCE ||
            l->field == F_TEXTURE || l->field == F_MIX || l->field == F_MODULATION)
            if (wo_stristr(l->text, frag)) return 1;
    }
    return 0;
}

static int has_rule(const wo_draft_t *d, int rid)
{
    int i;
    for (i = 0; i < d->nlines; i++) {
        const wo_line_t *l = &d->line[i];
        if ((l->field == F_RULES || l->field == F_PROCESS) && d->on[l->field] && l->entry != NONE16 &&
            WO_ENTRIES[l->entry].rule == rid) return 1;
    }
    return 0;
}

static int name_banned_here(const char *t, const char *n)
{
    const char *p = t;
    int ln = wo_strlen(n);
    while ((p = wo_stristr(p, n))) {          /* n\s*(is banned|is out) */
        const char *q = p + ln;
        while (*q == ' ') q++;
        if (wo_stristr(q, "is banned") == q || wo_stristr(q, "is out") == q) return 1;
        p++;
    }
    return 0;
}

static const char *strstr_cs(const char *h, const char *n)    /* case-sensitive, as indexOf */
{
    int ln = wo_strlen(n), i;
    if (!ln) return h;
    for (; *h; h++) {
        for (i = 0; i < ln && h[i] == n[i]; i++) {}
        if (i == ln) return h;
    }
    return 0;
}

static void device_conflicts(const wo_draft_t *d, char out[][WO_CONFLEN], int *no, int cap)
{
    static char lines[WO_MAXLINES + 12][WO_LINE];
    static int8_t banitem[WO_MAXLINES + 12];
    uint8_t banned[(WO_MAXINV + 7) / 8] = {0}, wanted[(WO_MAXINV + 7) / 8] = {0};
    char rolebans[4][24]; int nrb = 0;
    struct { char noun[16]; int dev; } only[16];
    int nonly = 0, nl = 0, i, k;
    char m[WO_CONFLEN];
    if (d->on[F_GEAR] && d->has[F_GEAR]) {
        for (i = 0; i < d->gear.nbanned; i++) {
            wo_strcpy(lines[nl], W.inv[d->gear.banned[i]].name, WO_LINE);
            wo_strcat(lines[nl], " is banned.", WO_LINE);
            banitem[nl++] = d->gear.banned[i];
        }
        for (i = 0; i < d->gear.nslots; i++) {
            wo_slot_text(&d->gear.slot[i], lines[nl], WO_LINE);
            banitem[nl++] = -1;
        }
        if (d->gear.exclusive) { wo_strcpy(lines[nl], "Nothing else may be loaded.", WO_LINE); banitem[nl++] = -1; }
    }
    for (i = 0; i < d->nlines && nl < WO_MAXLINES + 12; i++) {
        const wo_line_t *l = &d->line[i];
        if (!d->on[l->field] || (l->flags & L_HEAD)) continue;
        wo_strcpy(lines[nl], l->text, WO_LINE);
        banitem[nl++] = -1;
    }
    if (!nl) return;
    for (k = 0; k < nl; k++) {
        const char *t = lines[k];
        int isban, rm = 0;
        char rb[24], noun[16];
        if (banitem[k] >= 0) { wo_setbit(banned, (uint32_t)banitem[k], 1); continue; }
        isban = ban_pat(t);
        role_ban(t, rb, 24);
        if (rb[0]) { rm = 1; if (nrb < 4) wo_strcpy(rolebans[nrb++], rb, 24); }
        for (i = 0; i < W.ninv; i++) {
            if (!wo_inv_on((uint8_t)i) || !strstr_cs(t, W.inv[i].name)) continue;
            if (isban && name_banned_here(t, W.inv[i].name)) wo_setbit(banned, (uint32_t)i, 1);
            else if (isban && !rm) wo_setbit(banned, (uint32_t)i, 1);
            else wo_setbit(wanted, (uint32_t)i, 1);
        }
        noun[0] = 0;
        if (!wo_only_claim(t, noun, 16)) {
            if (wo_stristr(t, "Nothing else may be loaded")) wo_strcpy(noun, "device", 16);
            else if (wo_stristr(t, "is the entire piece")) wo_strcpy(noun, "instrument", 16);
        }
        if (noun[0] && nonly < 16) {
            int dv = -1;
            for (i = 0; i < W.ninv && dv < 0; i++) if (wo_inv_on((uint8_t)i) && strstr_cs(t, W.inv[i].name)) dv = i;
            wo_strcpy(only[nonly].noun, noun, 16); only[nonly].dev = dv; nonly++;
        }
    }
    for (i = 0; i < W.ninv; i++)
        if (wo_bit(wanted, (uint32_t)i) && wo_bit(banned, (uint32_t)i)) {
            wo_strcpy(m, W.inv[i].name, WO_CONFLEN); wo_strcat(m, " is both banned and required by another constraint.", WO_CONFLEN);
            add_conf(out, no, cap, m);
        }
    for (k = 0; k < nrb; k++) {
        int r;
        for (r = 0; r < WO_NROLES; r++) {
            char rn[24];
            wo_str(WO_ROLE[r], rn, 24);
            if (!wo_streq(rn, rolebans[k])) continue;
            for (i = 0; i < W.ninv; i++)
                if (wo_inv_on((uint8_t)i) && (W.inv[i].roles >> r & 1u) && wo_bit(wanted, (uint32_t)i)) {
                    wo_strcpy(m, W.inv[i].name, WO_CONFLEN); wo_strcat(m, " is named, but everything in ", WO_CONFLEN);
                    wo_strcat(m, rn, WO_CONFLEN); wo_strcat(m, " is banned.", WO_CONFLEN);
                    add_conf(out, no, cap, m);
                }
        }
    }
    for (i = 0; i < nonly; i++) {
        int j, other = -1;
        if (only[i].dev < 0) continue;
        for (j = 0; j < i; j++)
            if (only[j].dev >= 0 && only[j].dev != only[i].dev && wo_streq(only[j].noun, only[i].noun)) other = only[j].dev;
        if (other >= 0) {
            wo_strcpy(m, W.inv[other].name, WO_CONFLEN); wo_strcat(m, " and ", WO_CONFLEN);
            wo_strcat(m, W.inv[only[i].dev].name, WO_CONFLEN); wo_strcat(m, " each claim to be the only ", WO_CONFLEN);
            wo_strcat(m, only[i].noun, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN);
            add_conf(out, no, cap, m);
        }
    }
}

int wo_conflicts(const wo_draft_t *d, char out[][WO_CONFLEN], int cap)
{
    int n = 0, i, slot_total = 0, exclusive = 0, pitched = 0, drum_slot = 0;
    char m[WO_CONFLEN], b[48];
    if (d->on[F_GEAR] && d->has[F_GEAR]) {
        for (i = 0; i < d->gear.nslots; i++) {
            slot_total += d->gear.slot[i].count;
            if (d->gear.slot[i].role == R_SYNTH || d->gear.slot[i].role == R_SAMPLER) pitched++;
            if (d->gear.slot[i].role == R_DRUM_SOURCE) drum_slot = 1;
        }
        exclusive = d->gear.exclusive;
    }
    for (i = 0; i < d->nlines; i++) {
        const wo_line_t *l = &d->line[i];
        const wo_entry_t *e;
        if ((l->field != F_RULES && l->field != F_PROCESS) || !d->on[l->field] || l->entry == NONE16) continue;
        e = &WO_ENTRIES[l->entry];
        if (e->cap && slot_total > e->cap) {
            wo_strcpy(m, "The gear allocation permits ", WO_CONFLEN); wo_catu(m, (uint32_t)slot_total, WO_CONFLEN);
            wo_strcat(m, " devices, but a rule caps you at ", WO_CONFLEN); wo_catu(m, e->cap, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN);
            add_conf(out, &n, cap, m);
        }
        if ((e->flags & E_DRUMS) && exclusive && !drum_slot)
            add_conf(out, &n, cap, "A rule governs drum placement, but no drum source is permitted this session.");
    }
    if (has_rule(d, 28) && has_rule(d, 29))
        add_conf(out, &n, cap, "One loop with no arrangement, and no element may repeat identically. These fight.");
    if (has_rule(d, 12) && has_rule(d, 15))
        add_conf(out, &n, cap, "No undo, and delete anything over ten minutes. Every deletion is permanent.");
    if (has_rule(d, 7) && pitched > 1)
        add_conf(out, &n, cap, "All pitched material from one source, but two pitched instruments are allocated.");
    if (has_rule(d, 8) && d->on[F_TEMPO] && d->has[F_TEMPO] && d->sev[F_TEMPO] == TIER_BRUTAL) {
        wo_strcpy(m, "No quantization, in ", WO_CONFLEN);
        wo_str(WO_METER[d->tempo.tier][d->tempo.meter], b, 48); wo_strcat(m, b, WO_CONFLEN);
        wo_strcat(m, ". Play it or don't.", WO_CONFLEN);
        add_conf(out, &n, cap, m);
    }
    if (has_rule(d, 26) && has_rule(d, 18))
        add_conf(out, &n, cap, "Under three minutes, with a section repeating three times unchanged.");
    device_conflicts(d, out, &n, cap);
    if (d->on[F_CHORDS] && d->has[F_CHORDS]) {
        hd_t hd = harmony_directives(d);
        int got = d->chords.n;
        char sn[16];
        if (hd.count && got != hd.count) {
            wo_strcpy(m, "Harmony asks for ", WO_CONFLEN); wo_catu(m, hd.count, WO_CONFLEN); wo_strcat(m, " chords, the progression has ", WO_CONFLEN);
            wo_catu(m, (uint32_t)got, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN); add_conf(out, &n, cap, m);
        } else if (hd.max && got > hd.max) {
            wo_strcpy(m, "Harmony caps it at ", WO_CONFLEN); wo_catu(m, hd.max, WO_CONFLEN); wo_strcat(m, " chords, the progression has ", WO_CONFLEN);
            wo_catu(m, (uint32_t)got, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN); add_conf(out, &n, cap, m);
        }
        wo_str(WO_SHAPES[d->chords.shape].name, sn, 16);
        if (hd.shape != NONE8 && d->chords.shape != hd.shape) {
            char hn[16];
            wo_str(WO_SHAPES[hd.shape].name, hn, 16);
            wo_strcpy(m, "Harmony asks for ", WO_CONFLEN); wo_strcat(m, hn, WO_CONFLEN); wo_strcat(m, " voicings, the chords are ", WO_CONFLEN);
            wo_strcat(m, sn, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN); add_conf(out, &n, cap, m);
        }
        if (hd.avoid >> d->chords.shape & 1u) {
            wo_strcpy(m, "Harmony rules out ", WO_CONFLEN); wo_strcat(m, sn, WO_CONFLEN); wo_strcat(m, " voicings, but the chords are ", WO_CONFLEN);
            wo_strcat(m, sn, WO_CONFLEN); wo_strcat(m, ".", WO_CONFLEN); add_conf(out, &n, cap, m);
        }
    }
    if ((has_frag(d, "nothing is quantized") || has_frag(d, "played by hand")) && has_rule(d, 8))
        add_conf(out, &n, cap, "Quantization is forbidden twice, by two different parameters. One is redundant.");
    if (has_frag(d, "entire mix is mono") && has_rule(d, 1))
        add_conf(out, &n, cap, "The mix is mono permanently, so mixing in mono until the arrangement is done is moot.");
    if ((has_frag(d, "nothing synthesized") || has_frag(d, "one sample") || has_frag(d, "derives from one recording")) &&
        d->on[F_GEAR] && d->has[F_GEAR]) {
        int syn = 0;
        for (i = 0; i < d->gear.nslots; i++) if (d->gear.slot[i].role == R_SYNTH) syn = 1;
        if (syn) add_conf(out, &n, cap, "The source constraint bans synthesis, but a synth is allocated in the gear slots.");
    }
    if (has_frag(d, "no deviation") && has_rule(d, 28))
        add_conf(out, &n, cap, "A fixed bar map, and one loop with no arrangement. Only one of these can hold.");
    if (has_frag(d, "harmony is frozen") && has_frag(d, "parallel motion"))
        add_conf(out, &n, cap, "Harmony is frozen and also moves in parallel. It cannot do both.");
    if (has_frag(d, "only two pitches") && has_frag(d, "stack of fourths"))
        add_conf(out, &n, cap, "Stacks of fourths need three voices, but only two pitches may sound at once.");
    if (has_frag(d, "no eq at all") && has_rule(d, 2))
        add_conf(out, &n, cap, "EQ is banned outright, so delaying EQ until recording is finished says nothing.");
    if (has_frag(d, "polymetric") && d->on[F_TEMPO] && d->has[F_TEMPO]) {
        wo_str(WO_METER[d->tempo.tier][d->tempo.meter], b, 48);
        if (wo_stristr(b, "alternating"))
            add_conf(out, &n, cap, "A polymetric constraint on top of an alternating meter. Map it before you start.");
    }
    return n;
}

/* resolveConflicts: reroll unlocked parameters until the ticket stops fighting itself */
int wo_resolve_conflicts(int limit, uint32_t only)
{
    static wo_draft_t best;
    static char tmp[8][WO_CONFLEN];
    uint8_t af[WO_NFIELDS], cand[WO_NFIELDS];
    int n, bestn, tries = limit, na, nc, i;
    if (!W.set.noclash || !W.has_draft) return 0;
    n = wo_conflicts(&W.draft, tmp, 8);
    if (!n) return 0;
    best = W.draft; bestn = n;
    na = active_fields(af);
    while (bestn > 0 && tries-- > 0) {
        nc = 0;
        for (i = 0; i < na; i++) if (!W.lock[af[i]] && (!only || (only >> af[i] & 1u))) cand[nc++] = af[i];
        if (!nc) break;
        wo_roll_field(cand[wo_ri(0, nc - 1)]);
        n = wo_conflicts(&W.draft, tmp, 8);
        if (n < bestn) { bestn = n; best = W.draft; }
    }
    if (wo_conflicts(&W.draft, tmp, 8) > bestn) W.draft = best;
    return bestn;
}

/* a setup from the Roll tab's presets */
void wo_apply_builtin(uint8_t bi)
{
    const wo_builtin_t *b = &WO_BUILTIN[bi];
    int i;
    for (i = 0; i < WO_NFIELDS; i++) {
        W.on[i] = (uint8_t)(b->on >> i & 1u);
        if (W.on[i]) W.sev[i] = b->sev[i];
        W.count[i] = b->count[i];
        if (b->deep && W.on[i] && i != F_KEY && i != F_TEMPO && i != F_GEAR) W.count[i] = b->deep;
        W.mixin[i] = 0; W.lock[i] = 0;
    }
    W.set.rollsize = b->rollsize;
    W.set.mixsev = b->mixsev;
    W.set.anchor = NONE8;
    W.has_draft = 0;
}

/* for tests/engine_test.c: one chord's name, as the ticket would show it */
void wo_test_chord(uint8_t scale, uint8_t shape, int degree, int root_pc, int flats, char *out, int cap)
{
    wo_chord_t c;
    build_chord(root_pc, &WO_SCALES[scale], degree, shape, flats, &c);
    wo_strcpy(out, c.label, cap);
}
