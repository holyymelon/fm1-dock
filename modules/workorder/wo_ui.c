/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1's screens: 240 x 240 in a 6 x 10 font (40 columns, 18 rows of content), drawn into
 * 240 x 80 strips. The bottom of every page is a grid laid out like the panel's two rows of
 * buttons (FX SCL ENV LFO EDIT GLO / HOME SAVE ARP SEQ PLAY REC), naming what each one does here.
 *
 * One kind of action per button, on every page:
 *   SCL open / tick / confirm      HOME back                 GLO explain the terms
 *   PLAY start the ticket (no ticket: roll and start in one press), pause / resume; hold: finish
 *   REC  make new: roll, deal again, add      ARP  reroll the selected one    SEQ  mutate
 *   LFO  random                               EDIT on / off, edit             SAVE hold, park
 *   FX   break; hold: retire / delete         ENV  hear the chords
 * Anything that can't be undone needs a hold (0.8 s), and the grid shows it while you hold. */
#include "hb_module.h"
#include "wo.h"
#include "wo_store.h"
#include "wo_synth.h"
#include "wo_ui.h"
#include "wo_font.h"

static const hb_host_t *H;

/* ================================================================ colour */
static uint16_t C(int k) { return WO_THEMES[W.set.theme < WO_NTHEMES ? W.set.theme : 0].c[k]; }
static uint16_t tier_col(uint8_t t)
{
    return t == TIER_GENTLE ? C(TC_CYAN) : t == TIER_FIRM ? C(TC_LITE) : t == TIER_BRUTAL ? C(TC_MAGENTA) : C(TC_DIMMER);
}
static const char TIERCH[3] = {'G', 'F', 'B'};

/* ================================================================ geometry */
#define CW 6                    /* a character */
#define CH 10
#define HDR_H 12                /* the header: 0..11, its rule at 12 */
#define Y0 14                   /* the first content row */
#define ROWH 11
#define NROWS 18                /* 14 + 18 * 11 = 212 */
#define GRID_Y 214              /* the button grid: two rows of 13 px, its rule at 213 */
#define GRID_RH 13
#define COLS 40

/* ================================================================ the draw list */
#define NOPS 320
#define TPOOL 9000
typedef struct { int16_t x, y, w, h; uint16_t c; int16_t s; } op_t;
static op_t ops[NOPS];
static int nops, tpos;
static char tpool[TPOOL];

static void d_rect(int x, int y, int w, int h, uint16_t c)
{
    if (nops >= NOPS) return;
    ops[nops].x = (int16_t)x; ops[nops].y = (int16_t)y; ops[nops].w = (int16_t)w; ops[nops].h = (int16_t)h;
    ops[nops].c = c; ops[nops].s = -1; nops++;
}
static void d_text(int x, int y, const char *s, uint16_t c)
{
    int n = wo_strlen(s);
    if (nops >= NOPS || tpos + n + 1 > TPOOL || !n) return;
    wo_strcpy(tpool + tpos, s, n + 1);
    ops[nops].x = (int16_t)x; ops[nops].y = (int16_t)y; ops[nops].w = (int16_t)(n * CW); ops[nops].h = CH;
    ops[nops].c = c; ops[nops].s = (int16_t)tpos; nops++;
    tpos += n + 1;
}
static void d_text_r(int xr, int y, const char *s, uint16_t c) { d_text(xr - wo_strlen(s) * CW, y, s, c); }

/* the font's pixels into the host's canvas */
static void glyphs(int x, int y, const char *s, uint16_t c, int y0, int h)
{
    for (; *s; s++, x += CW) {
        uint8_t ch = (uint8_t)*s;
        const uint8_t *g;
        int r;
        if (ch < 32 || ch > 126) ch = '?';
        if (ch == ' ') continue;
        g = WO_FONT + (ch - 32) * CH;
        for (r = 0; r < CH; r++) {
            int yy = y + r, b;
            uint8_t bits = g[r];
            if (yy < y0 || yy >= y0 + h || !bits) continue;
            for (b = 0; b < CW; b++) if (bits & (0x80u >> b)) H->cv_pixel(x + b, yy - y0, c);
        }
    }
}

#define STRIP 80
static void flush_rows(int y0, int y1)
{
    int sy, i;
    for (sy = y0; sy < y1; sy += STRIP) {
        int h = y1 - sy < STRIP ? y1 - sy : STRIP;
        H->cv_begin(240, h, C(TC_INK));
        for (i = 0; i < nops; i++) {
            op_t *o = &ops[i];
            if (o->y + o->h <= sy || o->y >= sy + h) continue;
            if (o->s < 0) H->cv_rect(o->x, o->y - sy, o->w, o->h, o->c);
            else glyphs(o->x, o->y, tpool + o->s, o->c, sy, h);
        }
        H->cv_blit(0, sy);
    }
}

/* ================================================================ pages */
enum { PG_HOME, PG_ROLL, PG_FIELD, PG_SESSION, PG_RATE, PG_MODES, PG_PARKED, PG_LIBRARY, PG_VIEW, PG_SCORES,
       PG_RULES, PG_GEAR, PG_BUCKETS, PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP, PG_GLOSS, PG_TEXT, PG_N };
typedef struct { uint8_t page; int16_t sel; int16_t scroll; int16_t arg; } frame_t;
static frame_t st[10];
static int depth;
#define TOP (st[depth])

static char toast_s[48];
static uint32_t toast_until;
static int need_full = 1;
static uint32_t last_clock_s = 0xFFFFFFFFu;

/* a button held down: tap on release, the hold action once it has been down 0.8 s */
#define HOLD_MS 800u
#define EV_HOLD 0x0A000000u
static int held_btn = -1, held_fired;
static uint32_t held_t0;

static void toast(const char *s) { wo_strcpy(toast_s, s, 48); toast_until = H->ms() + 1800u; need_full = 1; }
static void push(uint8_t p, int16_t arg) { if (depth < 9) depth++; st[depth].page = p; st[depth].sel = 0; st[depth].scroll = 0; st[depth].arg = arg; need_full = 1; }
static void pop(void) { if (depth > 0) depth--; need_full = 1; }
static void go_root(uint8_t p) { depth = 0; st[0].page = PG_HOME; st[0].sel = 0; st[0].scroll = 0; if (p != PG_HOME) push(p, 0); }

/* ================================================================ the list view */
#define MAXROWS 240
typedef struct { char t[COLS + 1]; uint16_t fg, bg; int16_t entry; uint8_t x; char badge; uint16_t bcol; } row_t;
static row_t rows[MAXROWS];
static int nrows;
static int lv_nowrap;                   /* the next lv_add: one row, cut short with a '.' */

static void lv_reset(void) { nrows = 0; }

/* wrapped into rows; continuation rows hang under the text, past the prefix */
static void lv_add(int entry, int indent, char badge, uint16_t bcol, const char *pre, const char *text, uint16_t fg, uint16_t bg)
{
    int x0 = indent + (badge ? 9 : 0), cols = (238 - x0) / CW, plen = wo_strlen(pre), pos = 0, first = 1;
    int tl = wo_strlen(text);
    if (cols < 4) cols = 4;
    if (cols > COLS) cols = COLS;
    do {
        row_t *r;
        int room = cols - (first ? plen : (plen > 4 ? 2 : plen)), n, cut, k = 0;
        if (nrows >= MAXROWS) break;
        r = &rows[nrows++];
        r->fg = fg; r->bg = bg; r->entry = (int16_t)entry; r->x = (uint8_t)x0;
        r->badge = first ? badge : 0; r->bcol = bcol;
        if (first) { for (k = 0; k < plen && k < COLS; k++) r->t[k] = pre[k]; }
        else { int pad = plen > 4 ? 2 : plen; for (k = 0; k < pad; k++) r->t[k] = ' '; }
        while (text[pos] == ' ' && !first) pos++;
        n = tl - pos;
        if (n > room && lv_nowrap) n = room;
        else if (n > room) {
            cut = room;
            while (cut > 0 && text[pos + cut] != ' ' && text[pos + cut - 1] != '_') cut--;
            if (cut <= room / 3) cut = room;
            n = cut;
        }
        if (n < 0) n = 0;
        { int q; for (q = 0; q < n && k < COLS; q++) r->t[k++] = text[pos + q]; }
        r->t[k] = 0;
        pos += n;
        first = 0;
        if (lv_nowrap) { if (pos < tl && k > 0) r->t[k - 1] = '.'; break; }
    } while (pos < tl);
    lv_nowrap = 0;
}
static void lv_note(const char *text, uint16_t fg) { lv_add(-1, 3, 0, 0, "", text, fg, C(TC_INK)); }

/* draw the rows so the selected entry is in view */
static void lv_draw(int sel)
{
    int first = -1, last = -1, i, s = TOP.scroll, y;
    for (i = 0; i < nrows; i++) if (rows[i].entry == sel && sel >= 0) { if (first < 0) first = i; last = i; }
    if (first >= 0) {
        if (first < s) s = first;
        if (last >= s + NROWS) s = last - NROWS + 1;
        if (first < s) s = first;
        while (s > 0 && rows[s - 1].entry < 0 && last - (s - 1) < NROWS) s--;   /* notes above the entry, if they fit */
    }
    if (s > nrows - NROWS) s = nrows - NROWS;
    if (s < 0) s = 0;
    TOP.scroll = (int16_t)s;
    for (i = s, y = Y0; i < nrows && i < s + NROWS; i++, y += ROWH) {
        row_t *r = &rows[i];
        int on = sel >= 0 && r->entry == sel;
        uint16_t bg = on ? C(TC_INK3) : r->bg;
        if (bg != C(TC_INK)) d_rect(0, y - 1, 240, ROWH, bg);
        if (on) d_rect(0, y - 1, 2, ROWH, C(TC_SIGNAL));
        if (r->badge) { char b[2] = {r->badge, 0}; d_rect(r->x - 9, y, 7, CH, r->bcol); d_text(r->x - 9 + 1, y, b, C(TC_ONACC)); }
        d_text(r->x, y, r->t, r->fg);
    }
    if (nrows > NROWS) {                    /* a scroll mark on the right */
        int hh = NROWS * ROWH, bh = hh * NROWS / nrows, top = hh * s / nrows;
        d_rect(238, Y0 - 1 + top, 2, bh < 5 ? 5 : bh, C(TC_RULE2));
    }
}

static int lv_entries(void)
{
    int i, m = -1;
    for (i = 0; i < nrows; i++) if (rows[i].entry > m) m = rows[i].entry;
    return m + 1;
}
static int lv_has(int e) { int i; for (i = 0; i < nrows; i++) if (rows[i].entry == e) return 1; return 0; }

/* ================================================================ the button grid */
/* index = button id - 2: FX SCL ENV LFO EDIT GLO, then HOME SAVE ARP SEQ PLAY REC */
static const char *key_tap[12], *key_hold[12];
static void keys_clear(void) { int i; for (i = 0; i < 12; i++) key_tap[i] = key_hold[i] = 0; }
static void K(int btn, const char *tap, const char *hold) { key_tap[btn - 2] = tap; key_hold[btn - 2] = hold; }

static void draw_grid(void)
{
    int i;
    d_rect(0, GRID_Y - 1, 240, 1, C(TC_RULE2));
    d_rect(0, GRID_Y, 240, 240 - GRID_Y, C(TC_INK2));
    for (i = 0; i < 12; i++) {
        int x = (i % 6) * 40, y = GRID_Y + (i / 6) * GRID_RH;
        int holding = held_btn == i + 2;
        const char *t = holding && key_hold[i] ? key_hold[i] : key_tap[i];
        if (!t || !t[0]) continue;
        d_rect(x + 1, y + 1, 38, GRID_RH - 1, holding ? C(TC_SIGNAL) : C(TC_INK3));
        if (key_hold[i] && !holding) d_rect(x + 36, y + 2, 2, 2, C(TC_SIGNAL));   /* a hold does more */
        d_text(x + 20 - wo_strlen(t) * CW / 2, y + 2, t, holding ? C(TC_ONACC) : C(TC_TEXT));
    }
}

/* ================================================================ chrome */
static void chrome(const char *title, const char *right)
{
    char clk[16];
    d_rect(0, 0, 240, HDR_H, C(TC_INK2));
    d_rect(0, HDR_H, 240, 1, C(TC_RULE2));
    if (toast_until && H->ms() < toast_until) d_text(3, 1, toast_s, C(TC_SIGNAL));
    else d_text(3, 1, title, C(TC_SIGNAL));
    if (right && right[0]) d_text_r(237, 1, right, C(TC_DIM));
    else if (W.has_cur) {
        uint32_t e = wo_elapsed(&W.cur);
        if (W.cur.limit_ms) wo_hhmmss(clk, W.cur.limit_ms > e ? W.cur.limit_ms - e : 0, 16);
        else wo_hhmmss(clk, e, 16);
        if (!W.cur.running) wo_strcat(clk, " ||", 16);
        d_text_r(237, 1, clk, W.cur.running ? C(TC_TEXT) : C(TC_DIMMER));
    }
    draw_grid();
}

static void upcase(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }

/* ================================================================ text entry */
enum { TX_BREAK, TX_RETIRE, TX_NOTE, TX_CUSTOM_NEW, TX_CUSTOM_EDIT, TX_INV_NEW, TX_INV_RENAME, TX_MAT_NEW };
static struct { uint8_t purpose; int16_t arg, arg2; char prompt[40]; char s[WO_CUSTOMLEN]; int cur, cap, shift; int sym; } tx;
static const char SYMS[] = ".,'-?!:;()/&+0123456789";
/* white keys spell A..P left to right, black keys Q..Z, the top black key is a full stop */
static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static char key_char(int k)
{
    int i, b = 0;
    for (i = 0; i < 16; i++) if (WHITE[i] == k) return (char)('a' + i);
    for (i = 0; i < k; i++) { int w = 0, j; for (j = 0; j < 16; j++) if (WHITE[j] == i) w = 1; if (!w) b++; }
    return b < 10 ? (char)('q' + b) : '.';
}

static void text_begin(uint8_t purpose, int arg, int arg2, const char *prompt, const char *init, int cap)
{
    tx.purpose = purpose; tx.arg = (int16_t)arg; tx.arg2 = (int16_t)arg2;
    wo_strcpy(tx.prompt, prompt, 40);
    wo_strcpy(tx.s, init ? init : "", WO_CUSTOMLEN);
    tx.cap = cap > WO_CUSTOMLEN ? WO_CUSTOMLEN : cap;
    tx.cur = wo_strlen(tx.s);
    tx.shift = 0; tx.sym = 0;
    push(PG_TEXT, 0);
}

static void text_insert(char ch)
{
    int n = wo_strlen(tx.s), i, name = tx.purpose == TX_INV_NEW || tx.purpose == TX_INV_RENAME;
    if (n >= tx.cap - 1) return;
    if (ch >= 'a' && ch <= 'z') {
        int auto_cap = tx.cur == 0 || (tx.cur >= 2 && tx.s[tx.cur - 1] == ' ' && (tx.s[tx.cur - 2] == '.' || tx.s[tx.cur - 2] == '?' || tx.s[tx.cur - 2] == '!'));
        if (tx.shift || auto_cap || (name && tx.cur && tx.s[tx.cur - 1] == ' ')) ch = (char)(ch - 32);
        tx.shift = 0;
    }
    for (i = n + 1; i > tx.cur; i--) tx.s[i] = tx.s[i - 1];
    tx.s[tx.cur++] = ch;
}

static void text_done(void);

static void draw_text_page(void)
{
    int i, x, row = 0, col = 0, y;
    keys_clear();
    K(HB_BTN_SEL, "done", 0); K(HB_BTN_HOME, "cancel", 0); K(HB_BTN_EDIT, tx.shift ? "CAPS" : "caps", 0);
    { static char sym[8]; sym[0] = '"'; sym[1] = SYMS[tx.sym]; sym[2] = '"'; sym[3] = 0; K(HB_BTN_FX, sym, 0); }
    chrome("TYPING", " ");
    d_text(3, Y0, tx.prompt, C(TC_SIGNAL));
    for (i = 0; i <= wo_strlen(tx.s) && row < 7; i++) {
        y = Y0 + 2 + (row + 1) * ROWH;
        if (i == tx.cur) d_rect(3 + col * CW, y + CH, CW, 1, C(TC_SIGNAL));
        if (tx.s[i]) { char b[2] = {tx.s[i], 0}; d_text(3 + col * CW, y, b, C(TC_TEXT)); }
        if (++col >= 39) { col = 0; row++; }
    }
    d_text(3, 104, "OCT- delete  OCT+ space  KNOB1 sign", C(TC_DIMMER));
    y = 120;
    for (i = 0; i < 16; i++) {
        char b[2] = {(char)(key_char(WHITE[i]) - 32), 0};
        x = i * 15;
        d_rect(x, y, 14, 90, C(TC_TEXT));
        d_text(x + 4, y + 78, b, C(TC_INK));
    }
    for (i = 0; i < 27; i++) {
        int w = 0, j, wi = 0;
        for (j = 0; j < 16; j++) { if (WHITE[j] == i) w = 1; if (WHITE[j] < i) wi = j + 1; }
        if (w) continue;
        x = wi * 15 - 5;
        d_rect(x, y, 10, 52, C(TC_INK2));
        { char b[2] = {key_char(i) == '.' ? '.' : (char)(key_char(i) - 32), 0}; d_text(x + 2, y + 38, b, C(TC_SIGNAL)); }
    }
}

static void text_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_KEY_DOWN) { text_insert(key_char((int)id)); return; }
    if (kind == HB_EV_ENC) {
        if (id == HB_ENC_SELECT) { tx.cur += arg; if (tx.cur < 0) tx.cur = 0; if (tx.cur > wo_strlen(tx.s)) tx.cur = wo_strlen(tx.s); }
        else if (id == HB_ENC_KNOB1 || id == HB_ENC_ALGORITHM) { int n = (int)sizeof SYMS - 1; tx.sym = ((tx.sym + arg) % n + n) % n; }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_OCT_DOWN) {
        int n = wo_strlen(tx.s), i;
        if (tx.cur > 0) { for (i = tx.cur - 1; i < n; i++) tx.s[i] = tx.s[i + 1]; tx.cur--; }
    } else if (id == HB_BTN_OCT_UP) text_insert(' ');
    else if (id == HB_BTN_EDIT) tx.shift = !tx.shift;
    else if (id == HB_BTN_FX) text_insert(SYMS[tx.sym]);
    else if (id == HB_BTN_SEL) text_done();
    else if (id == HB_BTN_HOME) pop();
}

/* ================================================================ glossary */
static struct { char text[WO_LINE]; uint8_t skey; } gl;
static uint16_t gl_found[24]; static int gl_n;

static int isw(char c) { c = wo_lower(c); return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

/* glossy(): the terms in a line, longest first at each word, only where the term belongs */
static void gloss_scan(const char *t, uint8_t skey)
{
    int p = 0, n = wo_strlen(t), i, k;
    gl_n = 0;
    while (p < n && gl_n < 24) {
        int hit = 0;
        if (p > 0 && isw(t[p - 1])) { p++; continue; }
        for (i = 0; i < WO_NGLOSS; i++) {
            uint16_t g = WO_GLOSS_BYLEN[i];
            const char *term = WO_GLOSS_TERMS + WO_GLOSS_TOFF[g];
            int l = wo_strlen(term), q;
            if (p + l > n) continue;
            for (q = 0; q < l && wo_lower(t[p + q]) == term[q]; q++) {}
            if (q < l || (p + l < n && isw(t[p + l]))) continue;
            if (!WO_GLOSS[g].scope || (skey < 32 && (WO_GLOSS[g].scope >> skey & 1u))) {
                int dup = 0;
                for (k = 0; k < gl_n; k++) if (gl_found[k] == g) dup = 1;
                if (!dup) gl_found[gl_n++] = g;
            }
            p += l; hit = 1;
            break;
        }
        if (!hit) p++;
    }
}

static void gloss_open(const char *text, uint8_t skey)
{
    wo_strcpy(gl.text, text, WO_LINE);
    gl.skey = skey;
    gloss_scan(text, skey);
    push(PG_GLOSS, 0);
}

static void draw_gloss(void)
{
    char def[WO_LINE];
    int i;
    lv_reset();
    lv_note(gl.text, C(TC_DIM));
    if (!gl_n) lv_note("No glossary terms in this line.", C(TC_TEXT));
    for (i = 0; i < gl_n; i++) {
        const char *term = WO_GLOSS_TERMS + WO_GLOSS_TOFF[gl_found[i]];
        wo_str(WO_GLOSS[gl_found[i]].def, def, WO_LINE);
        lv_add(i, 3, 0, 0, "", term, C(TC_SIGNAL), C(TC_INK));
        lv_add(i, 9, 0, 0, "", def, C(TC_TEXT), C(TC_INK));
    }
    keys_clear(); K(HB_BTN_HOME, "back", 0);
    chrome("GLOSSARY", " ");
    lv_draw(TOP.sel);
}

/* ================================================================ shared bits */
static char tb[WO_LINE * 2], tb2[WO_LINE];

static void field_preview(uint8_t f, char *o, int cap)
{
    const wo_draft_t *d = &W.draft;
    uint8_t idx[WO_MAXLINES];
    o[0] = 0;
    if (!W.has_draft || !d->has[f]) { wo_strcpy(o, d->skip[f] && W.has_draft ? "(left out of this roll)" : "-", cap); return; }
    if (f == F_KEY) { wo_key_line(&d->key, o, cap); return; }
    if (f == F_TEMPO) { wo_tempo_line(&d->tempo, o, cap); return; }
    if (f == F_GEAR) {
        if (d->gear.empty) { wo_strcpy(o, "No category feeds GEAR", cap); return; }
        if (d->gear.nslots) { wo_slot_text(&d->gear.slot[0], o, cap); return; }
    }
    if (wo_field_lines(d, f, idx, WO_MAXLINES)) wo_strcpy(o, d->line[idx[0]].text, cap);
    else wo_strcpy(o, "-", cap);
}

static const wo_chords_t *chords_now(int *bpm)
{
    const wo_tempo_t *t = 0;
    const wo_chords_t *c = 0;
    if (W.has_cur && W.cur.has_chords) { c = &W.cur.chords; if (W.cur.has_tempo) t = &W.cur.tempo; }
    else if (W.has_draft && W.draft.has[F_CHORDS]) { c = &W.draft.chords; if (W.draft.has[F_TEMPO]) t = &W.draft.tempo; }
    *bpm = t ? (t->bpm ? t->bpm : (t->lo + t->hi) / 2) : 90;
    return c;
}

/* the keys play the key on the ticket, else the draft's, else the chords' scale */
static void sync_scale(void)
{
    const wo_key_t *k = 0;
    const wo_chords_t *c;
    int bpm;
    if (W.has_cur && W.cur.has_key) k = &W.cur.key;
    else if (W.has_draft && W.draft.has[F_KEY] && W.on[F_KEY]) k = &W.draft.key;
    if (k) { wo_synth_scale(k->root >= 0 ? k->root : 0, WO_SCALES[k->scale].mask); return; }
    c = chords_now(&bpm);
    if (c && c->n) { wo_synth_scale(c->root, WO_SCALES[c->scale].mask); return; }
    wo_synth_scale(0, 0x0FFF);
}

static int have_chords(void) { int bpm; const wo_chords_t *c = chords_now(&bpm); return c && c->n; }

static void chords_toggle(void)
{
    int bpm;
    const wo_chords_t *c = chords_now(&bpm);
    if (!W.set.sound) { toast("Sound is off (Settings)"); return; }
    if (wo_synth_playing()) { wo_synth_stop(); toast("Chords stopped"); return; }
    if (!c || !c->n) { toast("No chords rolled: switch Chords on"); return; }
    wo_synth_play(c, bpm, H->ms());
}

static void mode_name(uint8_t m, char *o, int cap) { wo_str(WO_MODES[m < WO_NMODES ? m : 0].name, o, cap); }

/* the keys every page shares: SCL, HOME, ENV, PLAY (set before the page adds its own) */
static void keys_common(void)
{
    keys_clear();
    if (depth) K(HB_BTN_HOME, "back", 0);
    if (have_chords()) K(HB_BTN_ENV, wo_synth_playing() ? "stop" : "chords", 0);
    if (!W.has_cur) K(HB_BTN_PLAY, "start", 0);
    else K(HB_BTN_PLAY, W.cur.running ? "pause" : "resume", "finish");
}

/* PLAY with no ticket: roll what is set up and start the clock, in one press */
static void quick_start(void)
{
    wo_synth_stop();
    if (W.set.mode != M_FREE && W.set.mode < WO_NMODES) {
        char b[40];
        wo_start_mode(W.set.mode);
        wo_store_mark(ST_ALL);
        mode_name(W.set.mode, b, 32); wo_strcat(b, " started", 40);
        go_root(PG_SESSION); toast(b);
        return;
    }
    if (!(TOP.page == PG_ROLL || TOP.page == PG_FIELD) || !wo_draft_ready()) {
        int i, on = 0;
        for (i = 0; i < WO_NFIELDS; i++) on += W.on[i];
        if (!on) { toast("Switch a parameter on first (Roll)"); return; }
        wo_roll_all();
    }
    wo_issue();
    wo_store_mark(ST_CUR | ST_STATE | ST_DRAFT);
    go_root(PG_SESSION); toast("Ticket started");
}

/* ================================================================ HOME */
static const char *const HOME_ITEMS[] = {"Roll", "Session", "Modes", "Parked", "Library", "Scores", "Rules", "Gear",
                                         "Materials", "Presets", "Settings", "Help"};
#define NHOME 12
static void draw_home(void)
{
    char sub[48];
    int i;
    lv_reset();
    if (W.has_cur) {
        lv_note(W.cur.name, C(TC_SIGNAL));
        wo_strcpy(sub, "", 48); wo_catu(sub, wo_ticket_score(&W.cur), 48); wo_strcat(sub, " pts, ", 48);
        { int k, d = 0; for (k = 0; k < W.cur.nitems; k++) d += !!(W.cur.item[k].flags & I_CHECKED);
          wo_catu(sub, (uint32_t)d, 48); wo_strcat(sub, " of ", 48); wo_catu(sub, W.cur.nitems, 48); wo_strcat(sub, " ticked", 48); }
        lv_note(sub, C(TC_DIM));
    } else lv_note("No ticket. PLAY rolls one and starts the clock.", C(TC_DIM));
    for (i = 0; i < NHOME; i++) {
        int on = 0, k;
        char row[COLS + 1];
        sub[0] = 0;
        switch (i) {
        case 0: for (k = 0; k < WO_NFIELDS; k++) on += W.on[k];
                wo_catu(sub, (uint32_t)on, 48); wo_strcat(sub, " params", 48);
                if (wo_draft_ready()) wo_strcat(sub, ", rolled", 48);
                break;
        case 1: wo_strcpy(sub, W.has_cur ? (W.cur.running ? "running" : "paused") : "no ticket", 48); break;
        case 2: mode_name(W.set.mode, sub, 30); break;
        case 3: wo_catu(sub, (uint32_t)wo_parked_count(), 48); wo_strcat(sub, " of 2", 48); break;
        case 4: wo_catu(sub, (uint32_t)wo_lib_count(), 48); wo_strcat(sub, " filed", 48); break;
        case 5: wo_catu(sub, W.nscores, 48); wo_strcat(sub, " scored", 48); break;
        case 6: wo_catu(sub, WO_NENTRIES + W.ncustom, 48); wo_strcat(sub, " rules", 48); break;
        case 7: for (k = 0; k < W.ninv; k++) on += wo_inv_on((uint8_t)k); wo_catu(sub, (uint32_t)on, 48); wo_strcat(sub, " on", 48); break;
        case 8: wo_catu(sub, WO_NMAT + W.nmatadd, 48); wo_strcat(sub, " sounds", 48); break;
        case 9: wo_catu(sub, WO_NBUILTIN, 48); wo_strcat(sub, " setups", 48); break;
        default: break;
        }
        {
            int l, pad;
            wo_strcpy(row, HOME_ITEMS[i], COLS + 1);
            l = wo_strlen(row); pad = 37 - l - wo_strlen(sub);
            while (pad-- > 0 && l < COLS) row[l++] = ' ';
            row[l] = 0;
            wo_strcat(row, sub, COLS + 1);
            lv_add(i, 6, 0, 0, "", row, C(TC_TEXT), C(TC_INK));
        }
    }
    keys_common();
    K(HB_BTN_SEL, "open", 0);
    chrome("WORKORDER", WO_BUILD " FM-1");
    lv_draw(TOP.sel);
}
static void home_event(uint32_t kind, uint32_t id, int32_t arg)
{
    static const uint8_t P[NHOME] = {PG_ROLL, PG_SESSION, PG_MODES, PG_PARKED, PG_LIBRARY, PG_SCORES, PG_RULES, PG_GEAR,
                                     PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP};
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL && TOP.sel >= 0 && TOP.sel < NHOME) push(P[TOP.sel], 0);
}

/* ================================================================ ROLL */
static void draw_roll(void)
{
    char conf[WO_NCONF][WO_CONFLEN], pre[24], lab[24];
    uint8_t idx[WO_MAXLINES];
    int nc = W.has_draft ? wo_conflicts(&W.draft, conf, WO_NCONF) : 0, i, j;
    lv_reset();
    for (i = 0; i < nc; i++) lv_add(-1, 3, 0, 0, "! ", conf[i], C(TC_SIGNAL), C(TC_INK));
    for (i = 0; i < WO_NFIELDS; i++) {
        uint8_t f = WO_ORDER[i];
        int on = W.on[f], sel = TOP.sel == i;
        wo_field_name(f, lab, 24);
        lab[11] = 0;
        wo_strcpy(pre, lab, 24);
        if (W.lock[f]) wo_strcat(pre, "*", 24);
        if (W.count[f]) { wo_strcat(pre, "x", 24); wo_catu(pre, W.count[f], 24); }
        while (wo_strlen(pre) < 13) wo_strcat(pre, " ", 24);
        if (sel && on && W.has_draft && W.draft.has[f]) {
            lv_add(i, 4, TIERCH[W.sev[f]], tier_col(W.sev[f]), "", pre, C(TC_SIGNAL), C(TC_INK));
            if (f == F_KEY || f == F_TEMPO || f == F_GEAR) { field_preview(f, tb, WO_LINE); lv_add(i, 21, 0, 0, "", tb, C(TC_TEXT), C(TC_INK)); }
            if (f == F_GEAR) for (j = 1; j < W.draft.gear.nslots; j++) { wo_slot_text(&W.draft.gear.slot[j], tb, WO_LINE); lv_add(i, 21, 0, 0, "", tb, C(TC_TEXT), C(TC_INK)); }
            {
                int n = wo_field_lines(&W.draft, f, idx, WO_MAXLINES);
                for (j = 0; j < n; j++) lv_add(i, 21, 0, 0, (W.draft.line[idx[j]].flags & L_HEAD) ? "" : "> ", W.draft.line[idx[j]].text,
                                               (W.draft.line[idx[j]].flags & L_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            }
        } else {
            char row[96];
            wo_strcpy(row, pre, 96);
            if (on) { field_preview(f, tb, WO_LINE); wo_strcat(row, tb, 96); }
            else wo_strcat(row, "off", 96);
            lv_nowrap = 1;
            lv_add(i, 4, TIERCH[W.sev[f]], on ? tier_col(W.sev[f]) : C(TC_DIMMER), "", row, on ? C(TC_TEXT) : C(TC_DIMMER), C(TC_INK));
        }
    }
    {
        char right[24]; int on = 0, k;
        for (k = 0; k < WO_NFIELDS; k++) on += W.on[k];
        right[0] = 0;
        if (nc) { wo_strcat(right, "!", 24); wo_catu(right, (uint32_t)nc, 24); wo_strcat(right, " ", 24); }
        wo_catu(right, (uint32_t)on, 24); wo_strcat(right, " on", 24);
        if (wo_draft_ready()) wo_strcat(right, " READY", 24);
        keys_common();
        K(HB_BTN_SEL, "open", 0); K(HB_BTN_EDIT, "on/off", 0); K(HB_BTN_SAVE, "hold", 0); K(HB_BTN_REC, "ROLL", 0);
        K(HB_BTN_ARP, "reroll", 0); K(HB_BTN_SEQ, "mutate", 0); K(HB_BTN_LFO, "random", 0); K(HB_BTN_GLO, "terms", 0);
        if (!W.has_cur && wo_draft_ready()) K(HB_BTN_PLAY, "START", 0);
        chrome("ROLL", right);
    }
    lv_draw(TOP.sel);
}

static void roll_knobs(uint8_t f, uint32_t id, int32_t arg)
{
    if (id == HB_ENC_ALGORITHM) { int s = W.sev[f] + (arg > 0 ? 1 : -1); W.sev[f] = (uint8_t)(s < 0 ? 0 : s > 2 ? 2 : s); }
    else if (id == HB_ENC_KNOB1) { int c = W.count[f] + (arg > 0 ? 1 : -1); W.count[f] = (uint8_t)(c < 0 ? 0 : c > 6 ? 6 : c); }
    else if (id == HB_ENC_KNOB2) W.mixin[f] = arg > 0;
    else return;
    wo_store_mark(ST_STATE);
}

static void roll_event(uint32_t kind, uint32_t id, int32_t arg)
{
    uint8_t f = WO_ORDER[TOP.sel >= 0 && TOP.sel < WO_NFIELDS ? TOP.sel : 0];
    if (kind == HB_EV_ENC) { roll_knobs(f, id, arg); return; }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: W.on[f] = !W.on[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_SAVE: W.lock[f] = !W.lock[f]; toast(W.lock[f] ? "Held: rolls leave it alone" : "Released"); wo_store_mark(ST_STATE); break;
    case HB_BTN_REC: wo_synth_stop(); wo_roll_all(); wo_store_mark(ST_DRAFT | ST_STATE); toast("Rolled"); break;
    case HB_BTN_ARP: if (W.on[f]) { wo_roll_field(f); wo_store_mark(ST_DRAFT | ST_STATE); } else toast("It is off: EDIT switches it on"); break;
    case HB_BTN_SEQ: wo_mutate(); wo_store_mark(ST_DRAFT | ST_STATE); toast("Mutated"); break;
    case HB_BTN_LFO: wo_full_random(); wo_store_mark(ST_DRAFT | ST_STATE); toast(W.set.chaos ? "Chaos roll" : "Random roll"); break;
    case HB_BTN_SEL: push(PG_FIELD, f); break;
    case HB_BTN_GLO: {
        uint8_t idx[WO_MAXLINES]; int n = W.has_draft ? wo_field_lines(&W.draft, f, idx, WO_MAXLINES) : 0, j;
        tb[0] = 0;
        for (j = 0; j < n; j++) { wo_strcat(tb, W.draft.line[idx[j]].text, (int)sizeof tb); wo_strcat(tb, " ", (int)sizeof tb); }
        if (!n) field_preview(f, tb, WO_LINE);
        gloss_open(tb, f);
        break; }
    default: break;
    }
}

/* ================================================================ FIELD: one parameter up close */
static void draw_field(void)
{
    uint8_t f = (uint8_t)TOP.arg, idx[WO_MAXLINES];
    const wo_draft_t *d = &W.draft;
    char title[40], right[24];
    int n = 0, i;
    lv_reset();
    {
        char b[64];
        wo_strcpy(b, "Severity ", 64); wo_strcat(b, wo_tier_name(W.sev[f]), 64);
        wo_strcat(b, " (ALGO)  lines ", 64);
        if (W.count[f]) wo_catu(b, W.count[f], 64); else wo_strcat(b, "auto", 64);
        wo_strcat(b, " (KNOB1)", 64);
        lv_note(b, C(TC_DIM));
        lv_note(W.mixin[f] ? "Mixed severity per line (KNOB2)" : "One severity for all lines (KNOB2: mixed)", C(TC_DIMMER));
    }
    if (W.has_draft && d->has[f]) {
        if (f == F_KEY) {
            wo_key_line(&d->key, tb, WO_LINE); lv_add(0, 3, 0, 0, "", tb, C(TC_TEXT), C(TC_INK));
            wo_str(WO_SCALES[d->key.scale].chr, tb, WO_LINE); lv_add(0, 9, 0, 0, "", tb, C(TC_DIM), C(TC_INK));
        } else if (f == F_TEMPO) {
            wo_tempo_line(&d->tempo, tb, WO_LINE); lv_add(0, 3, 0, 0, "", tb, C(TC_TEXT), C(TC_INK));
            wo_str(WO_BANDS[d->tempo.band].name, tb, WO_LINE); lv_add(0, 9, 0, 0, "Band: ", tb, C(TC_DIM), C(TC_INK));
        } else if (f == F_GEAR) {
            if (d->gear.empty) lv_note("No category feeds GEAR. Fix it on the Gear page.", C(TC_SIGNAL));
            if (d->gear.nbanned) {
                wo_strcpy(tb, "Banned this session: ", WO_LINE);
                for (i = 0; i < d->gear.nbanned; i++) { if (i) wo_strcat(tb, ", ", WO_LINE); wo_strcat(tb, W.inv[d->gear.banned[i]].name, WO_LINE); }
                lv_note(tb, C(TC_TEXT));
            }
            for (i = 0; i < d->gear.nslots; i++) { wo_slot_text(&d->gear.slot[i], tb, WO_LINE); lv_add(-1, 3, 0, 0, "- ", tb, C(TC_TEXT), C(TC_INK)); }
            if (d->gear.exclusive) lv_note("Nothing else may be loaded.", C(TC_TEXT));
        }
        n = wo_field_lines(d, f, idx, WO_MAXLINES);
        for (i = 0; i < n; i++) {
            const wo_line_t *l = &d->line[idx[i]];
            int e = (f == F_KEY || f == F_TEMPO) ? 0 : i;
            lv_add(e, 4, l->tier < 3 ? TIERCH[l->tier] : 0, tier_col(l->tier), "", l->text,
                   (l->flags & L_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        }
        if (f == F_GEAR && !n) lv_note("(no directives)", C(TC_DIMMER));
    } else lv_note(W.on[f] ? "Not rolled yet. REC rolls it." : "Switched off. EDIT switches it on.", C(TC_DIM));
    wo_field_name(f, title, 40);
    upcase(title);
    wo_strcpy(right, W.on[f] ? "" : "OFF ", 24);
    if (W.lock[f]) wo_strcat(right, "HELD", 24);
    keys_common();
    K(HB_BTN_EDIT, "on/off", 0); K(HB_BTN_SAVE, "hold", 0); K(HB_BTN_REC, "ROLL", 0); K(HB_BTN_GLO, "terms", 0);
    if (f != F_KEY && f != F_TEMPO && n) K(HB_BTN_ARP, "line", 0); else K(HB_BTN_ARP, "reroll", 0);
    if (!W.has_cur && wo_draft_ready()) K(HB_BTN_PLAY, "START", 0);
    chrome(title, right[0] ? right : " ");
    lv_draw(TOP.sel);
}

static void field_event(uint32_t kind, uint32_t id, int32_t arg)
{
    uint8_t f = (uint8_t)TOP.arg, idx[WO_MAXLINES];
    if (kind == HB_EV_ENC) { roll_knobs(f, id, arg); return; }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: W.on[f] = !W.on[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_SAVE: W.lock[f] = !W.lock[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_REC: if (W.on[f]) { wo_roll_field(f); wo_store_mark(ST_DRAFT | ST_STATE); } else toast("It is off: EDIT switches it on"); break;
    case HB_BTN_ARP:
        if (!W.on[f]) { toast("It is off: EDIT switches it on"); break; }
        if (f == F_KEY || f == F_TEMPO || !W.has_draft || !W.draft.has[f]) wo_roll_field(f);
        else if (!wo_reroll_line(f, (uint8_t)TOP.sel)) wo_roll_field(f);
        wo_store_mark(ST_DRAFT | ST_STATE);
        break;
    case HB_BTN_GLO: {
        int n = W.has_draft ? wo_field_lines(&W.draft, f, idx, WO_MAXLINES) : 0;
        if (n && TOP.sel < n) gloss_open(W.draft.line[idx[TOP.sel]].text, f);
        else { field_preview(f, tb, WO_LINE); gloss_open(tb, f); }
        break; }
    default: break;
    }
}

/* ================================================================ SESSION */
static uint8_t item_skey(const wo_item_t *it) { return it->label >= 100 ? WO_POOL_SKEY[it->label - 100] : it->pkey; }
static int session_hand(void) { return W.has_cur && W.cur.is_run && !W.cur.run.ended && W.cur.run.nhand; }

static void draw_session(void)
{
    wo_ticket_t *c = &W.cur;
    char b[96], lab[24];
    int i, hand = session_hand();
    lv_reset();
    keys_common();
    if (!W.has_cur) {
        lv_note("No ticket in play.", C(TC_TEXT));
        lv_note("PLAY rolls your setup and starts the clock. Or tweak it on the Roll page first, start a game mode, or resume a parked ticket.", C(TC_DIM));
        chrome("SESSION", " ");
        lv_draw(-1);
        return;
    }
    b[0] = 0;
    wo_strcat(b, "pts ", 96); wo_catu(b, wo_ticket_score(c), 96); wo_strcat(b, " / ", 96); wo_catu(b, wo_ticket_max(c), 96);
    mode_name(c->mode, lab, 24);
    wo_strcat(b, "   ", 96); wo_strcat(b, c->running ? lab : "PAUSED: PLAY resumes", 96);
    lv_note(b, c->running ? C(TC_DIM) : C(TC_SIGNAL));
    lv_note(c->name, C(TC_DIMMER));
    if (c->powered_off) lv_note("The clock stopped when the FM-1 was switched off. PLAY starts it again.", C(TC_SIGNAL));
    if (c->limit_ms && wo_elapsed(c) > c->limit_ms) lv_note("Time is up: nothing ticked now scores.", C(TC_SIGNAL));
    else if (c->target_min && wo_elapsed(c) > c->target_min * 60000u) lv_note("Past your target time.", C(TC_SIGNAL));
    for (i = 0; i < c->nconf; i++) lv_add(-1, 3, 0, 0, "! ", c->conf[i], C(TC_SIGNAL), C(TC_INK));
    if (c->is_run) {
        const wo_run_t *r = &c->run;
        wo_strcpy(b, "ROUND ", 96); wo_catu(b, r->round, 96); wo_strcat(b, "/", 96); wo_catu(b, r->nladder, 96);
        wo_strcat(b, "  lives ", 96);
        for (i = 0; i < r->maxlives; i++) wo_strcat(b, i < r->lives ? "#" : ".", 96);
        if (r->round >= 1 && r->round <= r->nladder) { wo_strcat(b, "  ", 96); wo_strcat(b, wo_tier_name(r->ladder[r->round - 1]), 96); }
        if (r->genre != NONE8) { wo_str(WO_GENRE[r->genre], lab, 24); wo_strcat(b, "  ", 96); wo_strcat(b, lab, 96); }
        lv_note(b, C(TC_CYAN));
        if (r->ended) {
            wo_strcpy(b, r->won ? "RUN COMPLETE  +" : (r->reason == 1 ? "RUN OVER: a brutal card was broken" : "RUN OVER: out of lives"), 96);
            if (r->won) wo_catu(b, r->bonus, 96);
            lv_note(b, r->won ? C(TC_CYAN) : C(TC_MAGENTA));
            lv_note("Hold PLAY to finish and file it.", C(TC_DIM));
        } else if (r->nhand) {
            lv_note("Keep one card: SCL takes it, REC deals again.", C(TC_TEXT));
            for (i = 0; i < r->nhand; i++) {
                const wo_card_t *k = &r->hand[i];
                wo_card_label(k, lab, 24);
                wo_strcpy(b, lab, 96); wo_strcat(b, "  +", 96); wo_catu(b, k->pts, 96);
                if (k->risk == RISK_FIGHTS) wo_strcat(b, "  FIGHTS YOUR DECK", 96);
                else if (k->risk == RISK_DOUBLES) wo_strcat(b, "  DOUBLES UP", 96);
                else if (k->risk == RISK_PILES) wo_strcat(b, "  PILES ON", 96);
                lv_add(i, 4, TIERCH[k->tier], tier_col(k->tier), "", b, k->risk ? C(TC_SIGNAL) : C(TC_DIM), C(TC_INK2));
                lv_add(i, 13, 0, 0, "", k->text, C(TC_TEXT), C(TC_INK2));
            }
            if (c->nitems) lv_note("Your deck:", C(TC_DIM));
        } else if (c->nitems) lv_note("Tick the card off (SCL) to reach the next round.", C(TC_DIM));
    }
    for (i = 0; i < c->nitems; i++) {
        const wo_item_t *it = &c->item[i];
        int e = hand ? -1 : i;
        const char *mark = (it->flags & I_BROKEN) ? "[/] " : (it->flags & I_CHECKED) ? "[x] " : "[ ] ";
        uint16_t fg = (it->flags & I_BROKEN) ? C(TC_DIMMER) : (it->flags & I_CHECKED) ? C(TC_DIM) : (it->flags & I_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT);
        lv_add(e, 4, it->tier < 3 ? TIERCH[it->tier] : 0, tier_col(it->tier), mark, it->text, fg, C(TC_INK));
        if (it->flags & I_BROKEN) { wo_strcpy(b, "broke it: ", 96); wo_strcat(b, it->why, 96); lv_add(e, 37, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK)); }
        if (!hand && TOP.sel == i) {
            wo_item_label(it, lab, 24);
            wo_strcpy(b, lab, 96);
            if (it->tier < 3) { wo_strcat(b, " | ", 96); wo_strcat(b, wo_tier_name(it->tier), 96); }
            if (it->flags & I_CHECKED) { wo_strcat(b, " | +", 96); wo_catu(b, it->pts, 96); }
            else if (!(it->flags & I_BROKEN)) { wo_strcat(b, " | now +", 96); wo_catu(b, (uint32_t)wo_item_points(c, it, wo_elapsed(c)), 96); }
            if (it->flags & I_WASBROKEN) wo_strcat(b, " | broken earlier", 96);
            lv_add(e, 37, 0, 0, "", b, C(TC_DIMMER), C(TC_INK));
        }
    }
    if (hand) { K(HB_BTN_SEL, "take", 0); K(HB_BTN_REC, "deal", 0); }
    else if (TOP.sel >= 0 && TOP.sel < c->nitems) {
        int br = c->item[TOP.sel].flags & I_BROKEN;
        K(HB_BTN_SEL, (c->item[TOP.sel].flags & I_CHECKED) ? "untick" : "tick", 0);
        K(HB_BTN_FX, br ? "unbrk" : "break", br ? 0 : "retire");
    }
    K(HB_BTN_SAVE, "park", 0); K(HB_BTN_GLO, "terms", 0);
    chrome(c->is_run ? "RUN" : "SESSION", "");
    lv_draw(TOP.sel);
}

static void session_event(uint32_t kind, uint32_t id, int32_t arg)
{
    wo_ticket_t *c = &W.cur;
    int hand;
    (void)arg;
    if (!W.has_cur) return;
    hand = session_hand();
    if (kind == EV_HOLD) {
        if (id == HB_BTN_FX && !hand && TOP.sel < c->nitems && !(c->item[TOP.sel].flags & I_BROKEN))
            text_begin(TX_RETIRE, TOP.sel, 0, "RETIRE FOR GOOD: WHAT WAS WRONG?", "", WO_WHY);
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_SEL:
        if (hand) { wo_take_card((uint8_t)TOP.sel); TOP.sel = (int16_t)(c->nitems - 1); wo_store_mark(ST_CUR); break; }
        if (TOP.sel >= 0 && TOP.sel < c->nitems) {
            int was = c->item[TOP.sel].flags & I_CHECKED;
            if (!c->running && !was) { toast("Paused: PLAY resumes the clock"); break; }
            wo_tick((uint8_t)TOP.sel);
            if (!was) {
                char b[32]; wo_strcpy(b, "+", 32); wo_catu(b, c->item[TOP.sel].pts, 32);
                if (c->item[TOP.sel].flags & I_WASBROKEN) wo_strcpy(b, "Broken earlier: no points", 32);
                toast(b);
            }
            if (c->is_run && c->run.nhand) TOP.sel = 0;
            wo_store_mark(ST_CUR);
        }
        break;
    case HB_BTN_FX:
        if (hand || TOP.sel < 0 || TOP.sel >= c->nitems) break;
        if (c->item[TOP.sel].flags & I_BROKEN) {
            wo_unbreak((uint8_t)TOP.sel); wo_store_mark(ST_CUR);
            toast((c->item[TOP.sel].flags & I_WASBROKEN) ? "Unbroken, but it can no longer score" : "Unbroken");
        } else text_begin(TX_BREAK, TOP.sel, 0, "WHY DID YOU BREAK IT?", "", WO_WHY);
        break;
    case HB_BTN_REC:
        if (hand) { wo_redeal(); wo_store_mark(ST_CUR); toast(c->run.nhand ? "Dealt again" : "Nothing left to deal"); }
        break;
    case HB_BTN_SAVE: {
        int r = wo_park();
        toast(r == 0 ? "Ticket parked" : r == -1 ? "Both park slots are taken" : "Could not save it");
        if (!r) go_root(PG_HOME);
        break; }
    case HB_BTN_GLO:
        if (hand && TOP.sel < c->run.nhand) gloss_open(c->run.hand[TOP.sel].text, WO_POOL_SKEY[c->run.hand[TOP.sel].pool]);
        else if (TOP.sel >= 0 && TOP.sel < c->nitems) gloss_open(c->item[TOP.sel].text, item_skey(&c->item[TOP.sel]));
        break;
    default: break;
    }
}

/* ================================================================ RATE */
static char rate_note[WO_WHY];
static const char *const RATES[4] = {"Keeper", "Neutral", "Dead end", "Abandoned"};
static void draw_rate(void)
{
    char b[64];
    int i, done = 0, broke = 0;
    lv_reset();
    for (i = 0; i < W.cur.nitems; i++) { done += !!(W.cur.item[i].flags & I_CHECKED); broke += !!(W.cur.item[i].flags & I_BROKEN); }
    wo_strcpy(b, "Score ", 64); wo_catu(b, wo_ticket_score(&W.cur), 64);
    wo_strcat(b, "  ticked ", 64); wo_catu(b, (uint32_t)done, 64); wo_strcat(b, "/", 64); wo_catu(b, W.cur.nitems, 64);
    wo_strcat(b, "  broke ", 64); wo_catu(b, (uint32_t)broke, 64);
    lv_note(b, C(TC_TEXT));
    wo_hhmmss(tb2, wo_elapsed(&W.cur), 24); wo_strcpy(b, "Time ", 64); wo_strcat(b, tb2, 64);
    if (wo_burst(&W.cur)) wo_strcat(b, "  ticked in a burst", 64);
    lv_note(b, C(TC_DIM));
    lv_note("How did it go? Keepers teach the next rolls.", C(TC_DIM));
    for (i = 0; i < 4; i++) lv_add(i, 9, 0, 0, TOP.sel == i ? "> " : "  ", RATES[i], i == 3 ? C(TC_MAGENTA) : C(TC_TEXT), C(TC_INK));
    lv_add(-1, 9, 0, 0, "Note: ", rate_note[0] ? rate_note : "(EDIT writes one)", C(TC_DIM), C(TC_INK));
    keys_clear();
    K(HB_BTN_HOME, "back", 0); K(HB_BTN_SEL, "FILE", 0); K(HB_BTN_EDIT, "note", 0);
    chrome("FINISH TICKET", " ");
    lv_draw(TOP.sel);
}
static void rate_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_EDIT) { text_begin(TX_NOTE, 0, 0, "ONE LINE OF OUTCOME", rate_note, WO_WHY); return; }
    if (id == HB_BTN_SEL && TOP.sel >= 0 && TOP.sel < 4) {
        wo_synth_stop();
        wo_close((uint8_t)TOP.sel, rate_note);
        rate_note[0] = 0;
        wo_store_mark(ST_ALL);
        go_root(PG_LIBRARY);
        toast("Filed");
    }
}

/* ================================================================ MODES */
static int mode_order[WO_NMODES];
static void build_mode_rows(void)
{
    char b[80], nm[32];
    int c, m, e = 0;
    lv_reset();
    lv_note("SCL starts a mode. Once picked, PLAY on any page starts it again.", C(TC_DIMMER));
    for (c = 0; c < WO_NMODECATS; c++) {
        wo_str(WO_MODECAT[c], nm, 32);
        upcase(nm);
        lv_add(-1, 3, 0, 0, "", nm, C(TC_CYAN), C(TC_INK));
        for (m = 0; m < WO_NMODES; m++) {
            const wo_mode_t *md = &WO_MODES[m];
            int mult10, rules = 0, i;
            if (md->cat != c) continue;
            mode_order[e] = m;
            wo_str(md->name, b, 80);
            mult10 = (int)(md->mult * 10.0f + 0.5f);
            while (wo_strlen(b) < 22) wo_strcat(b, " ", 80);
            wo_strcat(b, "x", 80); wo_catu(b, (uint32_t)(mult10 / 10), 80);
            if (mult10 % 10) { wo_strcat(b, ".", 80); wo_catu(b, (uint32_t)(mult10 % 10), 80); }
            if (md->limit) { wo_strcat(b, "  ", 80); wo_catu(b, md->limit, 80); wo_strcat(b, " min", 80); }
            lv_add(e, 9, 0, 0, W.set.mode == m ? "* " : "  ", b, TOP.sel == e ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            if (TOP.sel == e) {
                wo_str(md->desc, tb, WO_LINE);
                lv_add(e, 21, 0, 0, "", tb, C(TC_DIM), C(TC_INK));
                for (i = 0; i < WO_NENTRIES; i++) if (WO_ENTRIES[i].mode == m) rules++;
                if (rules) { b[0] = 0; wo_catu(b, (uint32_t)rules, 80); wo_strcat(b, " rules only this mode deals (EDIT)", 80); lv_add(e, 21, 0, 0, "", b, C(TC_DIMMER), C(TC_INK)); }
            }
            e++;
        }
    }
}
static void draw_modes(void)
{
    build_mode_rows();
    keys_common();
    K(HB_BTN_SEL, "start", 0); K(HB_BTN_EDIT, "rules", 0);
    chrome("MODES", " ");
    lv_draw(TOP.sel);
}
static void modes_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int m;
    (void)arg;
    if (kind != HB_EV_BTN_DOWN || TOP.sel < 0) return;
    build_mode_rows();
    m = mode_order[TOP.sel % WO_NMODES];
    if (id == HB_BTN_SEL) {
        if (W.has_cur && wo_park()) { toast("Park slots full: finish a ticket first"); return; }
        wo_synth_stop();
        if (m == M_FREE) { W.set.mode = M_FREE; wo_store_mark(ST_STATE); go_root(PG_ROLL); toast("Free play: your own setup"); return; }
        wo_start_mode((uint8_t)m);
        wo_store_mark(ST_ALL);
        go_root(PG_SESSION);
        { char b[40]; mode_name((uint8_t)m, b, 32); wo_strcat(b, " started", 40); toast(b); }
    } else if (id == HB_BTN_EDIT) push(PG_RULES, (int16_t)(1000 + m));
}

/* ================================================================ PARKED */
static void draw_parked(void)
{
    char b[64];
    int k, n = wo_parked_count();
    lv_reset();
    if (!n) lv_note("Nothing parked. SAVE on a ticket sets it aside with its clock stopped.", C(TC_DIM));
    for (k = 0; k < n; k++) {
        int s = wo_parked_slot(k);
        wo_hhmmss(tb2, wo_parked_elapsed(s), 24);
        lv_add(k, 6, 0, 0, "", wo_parked_name(s), C(TC_TEXT), C(TC_INK));
        wo_strcpy(b, "on the clock ", 64); wo_strcat(b, tb2, 64);
        lv_add(k, 18, 0, 0, "", b, C(TC_DIM), C(TC_INK));
    }
    keys_common();
    if (n) { K(HB_BTN_SEL, "resume", 0); K(HB_BTN_FX, "", "discrd"); }
    chrome("PARKED", " ");
    lv_draw(TOP.sel);
}
static void parked_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int s;
    (void)arg;
    if (TOP.sel < 0 || TOP.sel >= wo_parked_count()) return;
    s = wo_parked_slot(TOP.sel);
    if (kind == EV_HOLD && id == HB_BTN_FX) { wo_discard_parked(s); toast("Discarded"); return; }
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) {
        int r = wo_unpark(s);
        if (r) { toast(r == -2 ? "No slot free for the ticket in play" : "Could not read it"); return; }
        go_root(PG_SESSION); toast("Resumed");
    }
}

/* ================================================================ LIBRARY and VIEW */
static wo_ticket_t libt;
static int libt_idx = -1;
static const wo_ticket_t *lib_ticket(int i)
{
    if (libt_idx != i) { if (wo_lib_get(i, &libt)) { libt_idx = -1; return 0; } libt_idx = i; }
    return &libt;
}
static void draw_library(void)
{
    char b[80];
    int i, n = wo_lib_count();
    lv_reset();
    if (!n) lv_note("No filed tickets yet. Hold PLAY on a ticket to finish and file it.", C(TC_DIM));
    for (i = 0; i < n; i++) {
        const wo_ticket_t *t = lib_ticket(i);
        if (!t) continue;
        b[0] = 0;
        wo_catu(b, t->score, 80);
        while (wo_strlen(b) < 6) wo_strcat(b, " ", 80);
        wo_strcat(b, t->name, 80);
        lv_nowrap = 1;
        lv_add(i, 6, 0, 0, "", b, TOP.sel == i ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i) {
            wo_strcpy(b, RATES[t->rating & 3], 80); wo_strcat(b, "  ", 80);
            wo_hhmmss(tb2, t->elapsed, 24); wo_strcat(b, tb2, 80);
            if (t->is_run) wo_strcat(b, t->run.won ? "  run won" : "  run lost", 80);
            lv_add(i, 42, 0, 0, "", b, C(TC_DIM), C(TC_INK));
            if (t->note[0]) lv_add(i, 42, 0, 0, "\"", t->note, C(TC_DIMMER), C(TC_INK));
        }
    }
    keys_common();
    if (n) { K(HB_BTN_SEL, "view", 0); K(HB_BTN_REC, "again", 0); K(HB_BTN_FX, "", "delete"); }
    chrome("LIBRARY", " ");
    lv_draw(TOP.sel);
}
static void library_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (TOP.sel < 0 || TOP.sel >= wo_lib_count()) return;
    if (kind == EV_HOLD && id == HB_BTN_FX) { wo_lib_delete(TOP.sel); libt_idx = -1; toast("Deleted"); return; }
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_SEL) push(PG_VIEW, TOP.sel);
    else if (id == HB_BTN_REC) {                    /* the same ticket again, every box empty */
        const wo_ticket_t *t = lib_ticket(TOP.sel);
        int i;
        if (!t || t->is_run) { toast("A run can't be started again"); return; }
        if (W.has_cur && wo_park()) { toast("Park slots full"); return; }
        W.cur = *t;
        W.cur.id = W.next_id++;
        W.cur.elapsed = 0; W.cur.started = H->ms(); W.cur.running = 1; W.cur.score = 0;
        W.cur.pauses = W.cur.parks = 0; W.cur.paused_ms = W.cur.parked_ms = W.cur.paused_at = 0; W.cur.powered_off = 0;
        W.cur.note[0] = 0;
        for (i = 0; i < W.cur.nitems; i++) {
            wo_item_t *it = &W.cur.item[i];
            it->flags &= (uint8_t)(I_HEAD | I_META);
            it->why[0] = 0; it->pts = 0; it->at = it->wall = it->broke_at = 0;
        }
        W.has_cur = 1; W.set.mode = W.cur.mode;
        wo_store_mark(ST_CUR | ST_STATE);
        go_root(PG_SESSION); toast("Started again");
    }
}
static void draw_view(void)
{
    const wo_ticket_t *t = lib_ticket(TOP.arg);
    char b[80];
    int i;
    lv_reset();
    keys_common();
    if (!t) { lv_note("Could not read this ticket.", C(TC_SIGNAL)); chrome("TICKET", " "); lv_draw(-1); return; }
    lv_note(t->name, C(TC_DIM));
    wo_strcpy(b, RATES[t->rating & 3], 80); wo_strcat(b, "  ", 80); wo_catu(b, t->score, 80); wo_strcat(b, " pts", 80);
    lv_note(b, C(TC_TEXT));
    if (t->note[0]) lv_add(-1, 3, 0, 0, "\"", t->note, C(TC_DIM), C(TC_INK));
    for (i = 0; i < t->nitems; i++) {
        const wo_item_t *it = &t->item[i];
        lv_add(i, 4, it->tier < 3 ? TIERCH[it->tier] : 0, tier_col(it->tier),
               (it->flags & I_BROKEN) ? "[/] " : (it->flags & I_CHECKED) ? "[x] " : "[ ] ", it->text,
               (it->flags & I_BROKEN) ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        if (it->flags & I_BROKEN) { wo_strcpy(b, "broke it: ", 80); wo_strcat(b, it->why, 80); lv_add(i, 37, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK)); }
    }
    K(HB_BTN_GLO, "terms", 0);
    chrome("TICKET", " ");
    lv_draw(TOP.sel);
}
static void view_event(uint32_t kind, uint32_t id, int32_t arg)
{
    const wo_ticket_t *t = lib_ticket(TOP.arg);
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_GLO && t && TOP.sel >= 0 && TOP.sel < t->nitems)
        gloss_open(t->item[TOP.sel].text, item_skey(&t->item[TOP.sel]));
}

/* ================================================================ SCORES */
static void draw_scores(void)
{
    uint8_t ord[WO_MAXSCORES];
    char b[80], mn[32];
    int n = 0, i, j, filt = TOP.arg - 1;           /* arg 0: every mode */
    uint32_t best = 0, total = 0;
    for (i = 0; i < W.nscores; i++) if (filt < 0 || W.score[i].mode == filt) ord[n++] = (uint8_t)i;
    for (i = 1; i < n; i++) { uint8_t x = ord[i]; j = i; while (j > 0 && W.score[ord[j - 1]].score < W.score[x].score) { ord[j] = ord[j - 1]; j--; } ord[j] = x; }
    for (i = 0; i < n; i++) { total += W.score[ord[i]].score; if (W.score[ord[i]].score > best) best = W.score[ord[i]].score; }
    lv_reset();
    if (filt < 0) wo_strcpy(mn, "All modes", 32); else mode_name((uint8_t)filt, mn, 32);
    wo_strcpy(b, mn, 80); wo_strcat(b, " (ALGO): ", 80); wo_catu(b, (uint32_t)n, 80); wo_strcat(b, ", best ", 80);
    wo_catu(b, best, 80); wo_strcat(b, ", total ", 80); wo_catu(b, total, 80);
    lv_note(b, C(TC_CYAN));
    for (i = 0; i < n; i++) {
        const wo_score_t *s = &W.score[ord[i]];
        b[0] = 0;
        wo_catu(b, s->score, 80);
        while (wo_strlen(b) < 6) wo_strcat(b, " ", 80);
        mode_name(s->mode, mn, 32); mn[16] = 0; wo_strcat(b, mn, 80);
        while (wo_strlen(b) < 24) wo_strcat(b, " ", 80);
        wo_strcat(b, RATES[s->rating & 3], 80);
        lv_add(i, 6, 0, 0, "", b, i == 0 ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i) {
            b[0] = 0; wo_catu(b, s->done, 80); wo_strcat(b, "/", 80); wo_catu(b, s->total, 80);
            wo_strcat(b, " ticked  ", 80); wo_hhmmss(tb2, s->ms, 24); wo_strcat(b, tb2, 80);
            if (s->run_total) { wo_strcat(b, s->run_won ? "  run won" : "  run lost at round ", 80); if (!s->run_won) wo_catu(b, s->run_reached, 80); }
            lv_add(i, 42, 0, 0, "", b, C(TC_DIM), C(TC_INK));
            b[0] = 0;
            if (s->burst) wo_strcat(b, "ticked in a burst  ", 80);
            if (s->pauses) { wo_catu(b, s->pauses, 80); wo_strcat(b, " pauses", 80); }
            if (b[0]) lv_add(i, 42, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK));
            lv_add(i, 42, 0, 0, "", s->name, C(TC_DIMMER), C(TC_INK));
        }
    }
    keys_common();
    if (W.nscores) K(HB_BTN_FX, "", "clear");
    chrome("SCORES", " ");
    lv_draw(TOP.sel);
}
static void scores_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_ENC && (id == HB_ENC_ALGORITHM || id == HB_ENC_KNOB1)) {
        int a = TOP.arg + (arg > 0 ? 1 : -1);
        TOP.arg = (int16_t)((a + WO_NMODES + 1) % (WO_NMODES + 1));
        TOP.sel = 0; TOP.scroll = 0;
    } else if (kind == EV_HOLD && id == HB_BTN_FX) {
        W.nscores = 0; wo_store_mark(ST_SCORES); toast("Scores cleared. Filed tickets stay.");
    }
}

/* ================================================================ RULES */
static uint8_t rpool, rtier = TIER_FIRM;
static int16_t rule_ids[260];
static int nrule_ids;
static void draw_rules(void)
{
    char b[80], pl[24];
    int i, off = 0, yours = 0, mode = TOP.arg >= 1000 ? TOP.arg - 1000 : -1;
    lv_reset();
    nrule_ids = 0;
    if (mode >= 0) {
        mode_name((uint8_t)mode, pl, 24);
        wo_strcpy(b, "Rules only ", 80); wo_strcat(b, pl, 80); wo_strcat(b, " deals:", 80);
        lv_note(b, C(TC_CYAN));
        for (i = 0; i < WO_NENTRIES && nrule_ids < 260; i++) if (WO_ENTRIES[i].mode == mode) rule_ids[nrule_ids++] = (int16_t)i;
    } else {
        int a = WO_POOLIDX[rpool][rtier][0], n = WO_POOLIDX[rpool][rtier][1];
        for (i = a; i < a + n && nrule_ids < 260; i++) { rule_ids[nrule_ids++] = (int16_t)i; off += wo_bit(W.disabled, (uint32_t)i); }
        for (i = 0; i < W.ncustom && nrule_ids < 260; i++) if (W.custom[i].pool == rpool && W.custom[i].tier == rtier) { rule_ids[nrule_ids++] = (int16_t)(10000 + i); yours++; }
        b[0] = 0; wo_catu(b, (uint32_t)nrule_ids, 80); wo_strcat(b, " rules, ", 80); wo_catu(b, (uint32_t)off, 80);
        wo_strcat(b, " off, ", 80); wo_catu(b, (uint32_t)yours, 80); wo_strcat(b, " yours. ALGO pool, KNOB2 severity", 80);
        lv_note(b, C(TC_DIM));
    }
    for (i = 0; i < nrule_ids; i++) {
        int id = rule_ids[i];
        if (id >= 10000) {
            lv_add(i, 4, TIERCH[rtier], tier_col(rtier), "+ ", W.custom[id - 10000].text, C(TC_CYAN), C(TC_INK));
        } else {
            const wo_entry_t *e = &WO_ENTRIES[id];
            int dis = wo_bit(W.disabled, (uint32_t)id);
            char pre[32] = "";
            wo_str(e->text, tb, WO_LINE);
            if (mode >= 0) { wo_pool_name(e->pool, pl, 24); wo_strcat(pre, pl, 32); wo_strcat(pre, ": ", 32); }
            else if (e->mode != NONE8) { mode_name(e->mode, pl, 24); pl[12] = 0; wo_strcat(pre, pl, 32); wo_strcat(pre, ": ", 32); }
            if (e->genre != NONE8) { wo_str(WO_GENRE[e->genre], pl, 24); wo_strcat(pre, pl, 32); wo_strcat(pre, ": ", 32); }
            if (dis) wo_strcat(pre, "off ", 32);
            lv_add(i, 4, TIERCH[e->tier], dis ? C(TC_DIMMER) : tier_col(e->tier), pre, tb, dis ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        }
    }
    keys_common();
    K(HB_BTN_GLO, "terms", 0);
    if (TOP.sel >= 0 && TOP.sel < nrule_ids) {
        if (rule_ids[TOP.sel] >= 10000) { K(HB_BTN_EDIT, "edit", 0); K(HB_BTN_FX, "", "delete"); }
        else K(HB_BTN_EDIT, "on/off", 0);
    }
    if (mode < 0) K(HB_BTN_REC, "add", 0);
    if (mode >= 0) chrome("MODE RULES", " ");
    else {
        char t[40];
        wo_pool_name(rpool, t, 24);
        upcase(t);
        wo_strcat(t, " ", 40); wo_strcat(t, wo_tier_name(rtier), 40);
        chrome(t, " ");
    }
    lv_draw(TOP.sel);
}
static void rules_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int mode = TOP.arg >= 1000 ? TOP.arg - 1000 : -1, rid;
    if (kind == HB_EV_ENC && mode < 0) {
        if (id == HB_ENC_ALGORITHM || id == HB_ENC_KNOB1) { rpool = (uint8_t)((rpool + (arg > 0 ? 1 : WO_NPOOLS - 1)) % WO_NPOOLS); TOP.sel = 0; TOP.scroll = 0; }
        else if (id == HB_ENC_KNOB2) { rtier = (uint8_t)((rtier + (arg > 0 ? 1 : 2)) % 3); TOP.sel = 0; TOP.scroll = 0; }
        return;
    }
    rid = TOP.sel >= 0 && TOP.sel < nrule_ids ? rule_ids[TOP.sel] : -1;
    if (kind == EV_HOLD) {
        if (id == HB_BTN_FX && rid >= 10000) {
            int k = rid - 10000;
            for (; k < W.ncustom - 1; k++) W.custom[k] = W.custom[k + 1];
            W.ncustom--;
            wo_store_mark(ST_CUSTOM); toast("Deleted");
        }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT:
        if (rid >= 10000) text_begin(TX_CUSTOM_EDIT, rid - 10000, 0, "EDIT YOUR RULE", W.custom[rid - 10000].text, WO_CUSTOMLEN);
        else if (rid >= 0) { wo_setbit(W.disabled, (uint32_t)rid, !wo_bit(W.disabled, (uint32_t)rid)); wo_store_mark(ST_STATE); }
        break;
    case HB_BTN_REC:
        if (mode >= 0) break;
        if (W.ncustom >= WO_MAXCUSTOM) { toast("48 rules of your own is the most"); break; }
        text_begin(TX_CUSTOM_NEW, rpool, rtier, "YOUR RULE ({MAT}, {DEV}... work)", "", WO_CUSTOMLEN);
        break;
    case HB_BTN_GLO:
        if (rid >= 10000) gloss_open(W.custom[rid - 10000].text, WO_POOL_SKEY[rpool]);
        else if (rid >= 0) { wo_str(WO_ENTRIES[rid].text, tb, WO_LINE); gloss_open(tb, WO_POOL_SKEY[WO_ENTRIES[rid].pool]); }
        break;
    default: break;
    }
}

/* ================================================================ GEAR and BUCKETS */
static void role_of(uint8_t roles, char *o, int cap)
{
    int r;
    o[0] = 0;
    for (r = 0; r < WO_NROLES; r++) if (roles >> r & 1u) { char b[24]; if (o[0]) wo_strcat(o, "+", cap); wo_str(WO_ROLE[r], b, 24); wo_strcat(o, b, cap); }
}
/* entry 0 opens the categories page; the items follow from 1 */
static void draw_gear(void)
{
    char b[80], rl[40];
    int i, on = 0;
    for (i = 0; i < W.ninv; i++) on += wo_inv_on((uint8_t)i);
    lv_reset();
    b[0] = 0; wo_catu(b, (uint32_t)on, 80); wo_strcat(b, " of ", 80); wo_catu(b, W.ninv, 80); wo_strcat(b, " on. ALGO sets an item's category.", 80);
    lv_note(b, C(TC_DIM));
    lv_add(0, 6, 0, 0, "", "> What each category can be named in", TOP.sel == 0 ? C(TC_SIGNAL) : C(TC_CYAN), C(TC_INK));
    for (i = 0; i < W.ninv; i++) {
        int l;
        wo_strcpy(b, W.inv[i].name, 80);
        role_of(W.inv[i].roles, rl, 40);
        l = 37 - wo_strlen(rl) - 1;
        if (l < 4) l = 4;
        while (wo_strlen(b) < l) wo_strcat(b, " ", 80);
        b[l] = 0;
        wo_strcat(b, " ", 80); wo_strcat(b, rl, 80);
        lv_nowrap = 1;
        lv_add(i + 1, 6, 0, 0, "", b, W.inv[i].on ? C(TC_TEXT) : C(TC_DIMMER), C(TC_INK));
    }
    keys_common();
    K(HB_BTN_REC, "add", 0);
    if (TOP.sel == 0) K(HB_BTN_SEL, "open", 0);
    else if (TOP.sel > 0 && TOP.sel <= W.ninv) { K(HB_BTN_SEL, "rename", 0); K(HB_BTN_EDIT, "on/off", 0); K(HB_BTN_FX, "", "delete"); }
    chrome("GEAR", " ");
    lv_draw(TOP.sel);
}
static void gear_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int i = TOP.sel - 1;
    if (kind == HB_EV_ENC && id == HB_ENC_ALGORITHM && i >= 0 && i < W.ninv) {
        int r = 0, k;
        for (k = 0; k < WO_NROLES; k++) if (W.inv[i].roles >> k & 1u) { r = k; break; }
        r = (r + (arg > 0 ? 1 : WO_NROLES - 1)) % WO_NROLES;
        W.inv[i].roles = (uint8_t)(1u << r);
        wo_store_mark(ST_STATE);
        return;
    }
    if (kind == EV_HOLD) {
        if (id == HB_BTN_FX && i >= 0 && i < W.ninv) {
            int k;
            W.has_draft = 0;                    /* the draft names gear by number */
            for (k = i; k < W.ninv - 1; k++) W.inv[k] = W.inv[k + 1];
            W.ninv--;
            for (k = i; k < WO_MAXINV - 1; k++) W.weight[WK_ITEM + k] = W.weight[WK_ITEM + k + 1];
            wo_store_mark(ST_STATE | ST_DRAFT); toast("Deleted");
        }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: if (i >= 0 && i < W.ninv) { W.inv[i].on = !W.inv[i].on; wo_store_mark(ST_STATE); } break;
    case HB_BTN_SEL:
        if (TOP.sel == 0) push(PG_BUCKETS, 0);
        else if (i >= 0 && i < W.ninv) text_begin(TX_INV_RENAME, i, 0, "RENAME", W.inv[i].name, WO_NAMELEN);
        break;
    case HB_BTN_REC:
        if (W.ninv >= WO_MAXINV) { toast("64 items is the most"); break; }
        text_begin(TX_INV_NEW, 0, 0, "NEW GEAR (then ALGO sets its category)", "", WO_NAMELEN);
        break;
    default: break;
    }
}
static int bucket_cur = F_GEAR;
static void draw_buckets(void)
{
    char b[96], fn[24];
    int r, f;
    lv_reset();
    lv_note("Which parameters each category of gear can be named in. KNOB1 picks one, EDIT switches it.", C(TC_DIM));
    for (r = 0; r < WO_NROLES; r++) {
        wo_str(WO_ROLE[r], b, 96);
        wo_strcat(b, ": ", 96);
        if (TOP.sel != r) {
            int any = 0;
            for (f = 0; f < WO_NFIELDS; f++) if (W.bucket[r] >> f & 1u) { if (any) wo_strcat(b, ", ", 96); wo_field_name((uint8_t)f, fn, 24); wo_strcat(b, fn, 96); any = 1; }
            if (!any) wo_strcat(b, "nothing", 96);
            lv_add(r, 6, 0, 0, "", b, C(TC_TEXT), C(TC_INK));
        } else {
            lv_add(r, 6, 0, 0, "", b, C(TC_SIGNAL), C(TC_INK));
            for (f = 0; f < WO_NFIELDS; f++) {
                if (f == F_KEY || f == F_TEMPO) continue;
                wo_field_name((uint8_t)f, fn, 24);
                wo_strcpy(b, (W.bucket[r] >> f & 1u) ? "[x] " : "[ ] ", 96); wo_strcat(b, fn, 96);
                lv_add(r, 18, 0, 0, bucket_cur == f ? "> " : "  ", b, bucket_cur == f ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            }
        }
    }
    keys_common();
    K(HB_BTN_EDIT, "switch", 0);
    chrome("CATEGORIES", " ");
    lv_draw(TOP.sel);
}
static void buckets_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_ENC && id == HB_ENC_KNOB1) {
        do bucket_cur = (bucket_cur + (arg > 0 ? 1 : WO_NFIELDS - 1)) % WO_NFIELDS; while (bucket_cur == F_KEY || bucket_cur == F_TEMPO);
    } else if (kind == HB_EV_BTN_DOWN && id == HB_BTN_EDIT && TOP.sel >= 0 && TOP.sel < WO_NROLES) {
        W.bucket[TOP.sel] ^= 1u << bucket_cur; wo_store_mark(ST_STATE);
    }
}

/* ================================================================ MATERIALS */
static int16_t mat_ids[WO_NMAT + WO_MAXMATADD]; static uint8_t mat_cat[WO_NMAT + WO_MAXMATADD]; static int nmat_ids;
static void draw_mats(void)
{
    char b[64];
    int c, i;
    lv_reset();
    nmat_ids = 0;
    lv_note("What {MAT} and {MATCAT} can name.", C(TC_DIM));
    for (c = 0; c < WO_NMATCATS; c++) {
        wo_str(WO_MATCAT[c], b, 64);
        upcase(b);
        lv_add(-1, 3, 0, 0, "", b, C(TC_CYAN), C(TC_INK));
        for (i = WO_MAT_START[c]; i < WO_MAT_START[c + 1]; i++) {
            int off = wo_bit(W.matoff, (uint32_t)i);
            wo_str(WO_MAT[i], b, 64);
            mat_ids[nmat_ids] = (int16_t)i; mat_cat[nmat_ids] = (uint8_t)c;
            lv_add(nmat_ids++, 12, 0, 0, off ? "off " : "", b, off ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        }
        for (i = 0; i < W.nmatadd; i++) if (W.matadd[i].cat == c) {
            mat_ids[nmat_ids] = (int16_t)(1000 + i); mat_cat[nmat_ids] = (uint8_t)c;
            lv_add(nmat_ids++, 12, 0, 0, "+ ", W.matadd[i].text, C(TC_CYAN), C(TC_INK));
        }
    }
    keys_common();
    K(HB_BTN_REC, "add", 0);
    if (TOP.sel >= 0 && TOP.sel < nmat_ids) {
        if (mat_ids[TOP.sel] >= 1000) K(HB_BTN_FX, "", "delete"); else K(HB_BTN_EDIT, "on/off", 0);
    }
    chrome("MATERIALS", " ");
    lv_draw(TOP.sel);
}
static void mats_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int m;
    (void)arg;
    if (TOP.sel < 0 || TOP.sel >= nmat_ids) return;
    m = mat_ids[TOP.sel];
    if (kind == EV_HOLD && id == HB_BTN_FX && m >= 1000) {
        int k;
        for (k = m - 1000; k < W.nmatadd - 1; k++) W.matadd[k] = W.matadd[k + 1];
        W.nmatadd--; wo_store_mark(ST_CUSTOM); toast("Deleted");
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_EDIT && m < 1000) { wo_setbit(W.matoff, (uint32_t)m, !wo_bit(W.matoff, (uint32_t)m)); wo_store_mark(ST_STATE); }
    else if (id == HB_BTN_REC) {
        if (W.nmatadd >= WO_MAXMATADD) toast("32 of your own is the most");
        else text_begin(TX_MAT_NEW, mat_cat[TOP.sel], 0, "A SOUND TO SAMPLE", "", WO_MATLEN);
    }
}

/* ================================================================ PRESETS */
static void draw_presets(void)
{
    int i;
    lv_reset();
    lv_note("Starting points for the Roll page.", C(TC_DIM));
    for (i = 0; i < WO_NBUILTIN; i++) {
        wo_str(WO_BUILTIN[i].name, tb2, 64);
        lv_add(i, 6, 0, 0, "", tb2, TOP.sel == i ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i) { wo_str(WO_BUILTIN[i].desc, tb, WO_LINE); lv_add(i, 18, 0, 0, "", tb, C(TC_DIM), C(TC_INK)); }
    }
    keys_common();
    K(HB_BTN_SEL, "use", 0);
    chrome("PRESETS", " ");
    lv_draw(TOP.sel);
}
static void presets_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL && TOP.sel >= 0 && TOP.sel < WO_NBUILTIN) {
        wo_apply_builtin((uint8_t)TOP.sel);
        W.set.mode = M_FREE;
        wo_store_mark(ST_STATE | ST_DRAFT);
        wo_str(WO_BUILTIN[TOP.sel].name, tb2, 64);
        go_root(PG_ROLL); toast(tb2);
    }
}

/* ================================================================ SETTINGS */
enum { S_THEME, S_ANCHOR, S_GENRE, S_COOL, S_CLASH, S_ROLLSIZE, S_MUTN, S_TARGET, S_RMIN, S_RMAX, S_RSEV, S_CHAOS,
       S_HOLDS, S_RANCHOR, S_SOUND, S_VOL, S_RESETW, S_WIPE, S_ABOUT, S_N };
static const char *const SNAME[S_N] = {"Theme", "Anchor", "Genre lock", "Cooldown", "No clashes", "Roll size", "Mutate",
                                       "Target time", "Random: fewest params", "Random: most params", "Random: severity",
                                       "Random: chaos", "Random: held params", "Random: anchor", "Sound", "Volume",
                                       "Reset learning", "Reset everything", "About"};
static void setting_value(int s, char *o, int cap)
{
    static const char *const RS[7] = {"any", "any", "escalate", "ladder", "gentle", "firm", "brutal"};
    o[0] = 0;
    switch (s) {
    case S_THEME: wo_str(WO_THEMES[W.set.theme].name, o, cap); break;
    case S_ANCHOR: if (W.set.anchor < WO_NANCHORS) wo_str(WO_ANCHOR[W.set.anchor], o, cap); else wo_strcpy(o, "none", cap); break;
    case S_GENRE: if (W.set.genrelock < WO_NGENRES) wo_str(WO_GENRE[W.set.genrelock], o, cap); else wo_strcpy(o, "random", cap); break;
    case S_COOL: wo_strcpy(o, W.set.cooldown ? "on" : "off", cap); break;
    case S_CLASH: wo_strcpy(o, W.set.noclash ? "on" : "off", cap); break;
    case S_ROLLSIZE: if (W.set.rollsize) wo_catu(o, W.set.rollsize, cap); else wo_strcpy(o, "all", cap); break;
    case S_MUTN: if (W.set.mutn) wo_catu(o, W.set.mutn, cap); else wo_strcpy(o, "1-2", cap); break;
    case S_TARGET: if (W.set.target_min) { wo_catu(o, W.set.target_min, cap); wo_strcat(o, " min", cap); } else wo_strcpy(o, "off", cap); break;
    case S_RMIN: wo_catu(o, W.set.minp, cap); break;
    case S_RMAX: wo_catu(o, W.set.maxp, cap); break;
    case S_RSEV: wo_strcpy(o, RS[W.set.rsev < 7 ? W.set.rsev : 0], cap); break;
    case S_CHAOS: wo_strcpy(o, W.set.chaos ? "on" : "off", cap); break;
    case S_HOLDS: wo_strcpy(o, W.set.holds ? "kept" : "ignored", cap); break;
    case S_RANCHOR: wo_strcpy(o, W.set.ranchor ? "rolled" : "kept", cap); break;
    case S_SOUND: wo_strcpy(o, W.set.sound ? "on" : "off", cap); break;
    case S_VOL: wo_catu(o, W.set.volume, cap); break;
    case S_RESETW: case S_WIPE: wo_strcpy(o, "hold FX", cap); break;
    case S_ABOUT: wo_strcpy(o, WO_BUILD, cap); break;
    default: break;
    }
}
static void draw_settings(void)
{
    char b[64], v[32];
    int s;
    lv_reset();
    for (s = 0; s < S_N; s++) {
        int l;
        wo_strcpy(b, SNAME[s], 64);
        setting_value(s, v, 32);
        l = 37 - wo_strlen(v);
        while (wo_strlen(b) < l) wo_strcat(b, " ", 64);
        wo_strcat(b, v, 64);
        lv_add(s, 6, 0, 0, "", b, s >= S_RESETW && s <= S_WIPE ? C(TC_MAGENTA) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == s && s == S_ABOUT) {
            b[0] = 0; wo_catu(b, WO_NENTRIES, 64); wo_strcat(b, " rules, ", 64); wo_catu(b, WO_NGLOSS, 64);
            wo_strcat(b, " glossary terms, ", 64); wo_catu(b, WO_NMODES, 64); wo_strcat(b, " modes. From workorder.html.", 64);
            lv_add(s, 18, 0, 0, "", b, C(TC_DIM), C(TC_INK));
        }
    }
    keys_common();
    if (TOP.sel == S_RESETW || TOP.sel == S_WIPE) K(HB_BTN_FX, "", "reset");
    else if (TOP.sel != S_ABOUT) K(HB_BTN_SEL, "change", 0);
    chrome("SETTINGS", "ALGO changes");
    lv_draw(TOP.sel);
}
static void settings_change(int s, int d)
{
#define CYC(v, n) v = (uint8_t)(((int)(v) + d + (n)) % (n))
    switch (s) {
    case S_THEME: CYC(W.set.theme, WO_NTHEMES); break;
    case S_ANCHOR: { int a = W.set.anchor == NONE8 ? WO_NANCHORS : W.set.anchor; a = (a + d + WO_NANCHORS + 1) % (WO_NANCHORS + 1); W.set.anchor = a == WO_NANCHORS ? NONE8 : (uint8_t)a; break; }
    case S_GENRE: { int a = W.set.genrelock == NONE8 ? WO_NGENRES : W.set.genrelock; a = (a + d + WO_NGENRES + 1) % (WO_NGENRES + 1); W.set.genrelock = a == WO_NGENRES ? NONE8 : (uint8_t)a; break; }
    case S_COOL: W.set.cooldown = !W.set.cooldown; break;
    case S_CLASH: W.set.noclash = !W.set.noclash; break;
    case S_ROLLSIZE: CYC(W.set.rollsize, WO_NFIELDS + 1); break;
    case S_MUTN: CYC(W.set.mutn, 7); break;
    case S_TARGET: { int t = W.set.target_min + d * 5; W.set.target_min = (uint8_t)(t < 0 ? 0 : t > 240 ? 240 : t); break; }
    case S_RMIN: { int t = W.set.minp + d; W.set.minp = (uint8_t)(t < 1 ? 1 : t > WO_NFIELDS ? WO_NFIELDS : t); if (W.set.maxp < W.set.minp) W.set.maxp = W.set.minp; break; }
    case S_RMAX: { int t = W.set.maxp + d; W.set.maxp = (uint8_t)(t < 1 ? 1 : t > WO_NFIELDS ? WO_NFIELDS : t); if (W.set.minp > W.set.maxp) W.set.minp = W.set.maxp; break; }
    case S_RSEV: {
        static const uint8_t R[5] = {0, MSEV_ESCALATE, MSEV_GENTLE, MSEV_GENTLE + 1, MSEV_GENTLE + 2};
        int k = 0, j;
        for (j = 0; j < 5; j++) if (R[j] == W.set.rsev) k = j;
        k = (k + d + 5) % 5;
        W.set.rsev = R[k];
        break; }
    case S_CHAOS: W.set.chaos = !W.set.chaos; break;
    case S_HOLDS: W.set.holds = !W.set.holds; break;
    case S_RANCHOR: W.set.ranchor = !W.set.ranchor; break;
    case S_SOUND: W.set.sound = !W.set.sound; if (!W.set.sound) { wo_synth_stop(); wo_synth_all_off(); } break;
    case S_VOL: { int t = W.set.volume + d; W.set.volume = (uint8_t)(t < 0 ? 0 : t > 10 ? 10 : t); wo_synth_volume(W.set.volume); break; }
    default: return;
    }
#undef CYC
    wo_store_mark(ST_STATE);
}
static void settings_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_ENC && (id == HB_ENC_ALGORITHM || id == HB_ENC_KNOB1)) { settings_change(TOP.sel, arg > 0 ? 1 : -1); return; }
    if (kind == EV_HOLD && id == HB_BTN_FX) {
        if (TOP.sel == S_RESETW) { wo_reset_weights(); wo_store_mark(ST_STATE); toast("Learning reset"); }
        else if (TOP.sel == S_WIPE) { wo_synth_stop(); wo_store_wipe(); go_root(PG_HOME); toast("Everything reset"); }
        return;
    }
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL && TOP.sel != S_RESETW && TOP.sel != S_WIPE) settings_change(TOP.sel, 1);
}

/* ================================================================ HELP */
static const char *const HELP[] = {
    "The grid", "The bottom of the screen is laid out like the two rows of buttons. Each box names what that button does on this page. A dot in a box: holding the button does more (it shows while you hold).",
    "Everywhere", "SELECT moves, PRESETS moves faster. SCL opens or ticks. HOME goes back (hold it to leave WORKORDER). GLO explains the terms in a line. ENV plays the rolled chords. The keys play the rolled scale; OCT- / OCT+ shift them.",
    "Starting", "PLAY with no ticket rolls your setup and starts the clock. To tweak first, open Roll: REC rolls everything, ARP rerolls one parameter, SEQ mutates, LFO rolls at random, EDIT switches a parameter, SAVE holds it. Then PLAY.",
    "Working", "SCL ticks a rule off. FX breaks it (say why) or unbreaks it. Hold FX to retire a rule from the pool for good. PLAY pauses the clock, SAVE parks the ticket, hold PLAY to finish, rate and file it.",
    "Modes and the Run", "Modes: SCL starts one, and from then on PLAY starts that mode. In a Run each round deals cards: SCL keeps one, REC deals again. A broken brutal card costs two lives.",
    "Typing", "White keys are A to P, black keys Q to Z, the top black key is a full stop. OCT- deletes, OCT+ is a space, EDIT gives a capital, KNOB1 picks a sign and FX puts it in. SCL finishes, HOME cancels.",
    "Scores", "Points drop the longer a ticket takes. Brutal counts 3.5x, firm 2x. Modes multiply. Keeper tickets teach the next rolls.",
};
static void draw_help(void)
{
    int i;
    lv_reset();
    for (i = 0; i < (int)(sizeof HELP / sizeof HELP[0]); i += 2) {
        lv_add(i / 2, 3, 0, 0, "", HELP[i], C(TC_SIGNAL), C(TC_INK));
        lv_add(i / 2, 9, 0, 0, "", HELP[i + 1], C(TC_TEXT), C(TC_INK));
    }
    keys_common();
    chrome("HELP", " ");
    lv_draw(TOP.sel);
}

/* ================================================================ text entry done */
static void text_done(void)
{
    char *s = tx.s;
    int n = wo_strlen(s), i;
    while (n && s[n - 1] == ' ') s[--n] = 0;
    pop();
    switch (tx.purpose) {
    case TX_BREAK: wo_break((uint8_t)tx.arg, s); wo_store_mark(ST_CUR); toast("Break logged"); break;
    case TX_RETIRE: wo_retire((uint8_t)tx.arg, s); wo_store_mark(ST_CUR | ST_STATE); toast("Retired from the pool"); break;
    case TX_NOTE: wo_strcpy(rate_note, s, WO_WHY); break;
    case TX_CUSTOM_NEW:
        if (!n || W.ncustom >= WO_MAXCUSTOM) break;
        W.custom[W.ncustom].pool = (uint8_t)tx.arg; W.custom[W.ncustom].tier = (uint8_t)tx.arg2;
        wo_strcpy(W.custom[W.ncustom].text, s, WO_CUSTOMLEN);
        W.weight[WK_CUSTOM + W.ncustom] = WQ_ONE;
        wo_setbit(W.coolc, W.ncustom, 0);
        W.ncustom++;
        wo_store_mark(ST_CUSTOM | ST_STATE); toast("Added to the pool");
        break;
    case TX_CUSTOM_EDIT: if (n && tx.arg < W.ncustom) { wo_strcpy(W.custom[tx.arg].text, s, WO_CUSTOMLEN); wo_store_mark(ST_CUSTOM); } break;
    case TX_INV_NEW:
        if (!n || W.ninv >= WO_MAXINV) break;
        wo_strcpy(W.inv[W.ninv].name, s, WO_NAMELEN);
        W.inv[W.ninv].roles = 1u << R_SYNTH; W.inv[W.ninv].on = 1;
        W.weight[WK_ITEM + W.ninv] = WQ_ONE;
        W.ninv++;
        TOP.sel = (int16_t)W.ninv;
        wo_store_mark(ST_STATE); toast("Added. ALGO sets its category");
        break;
    case TX_INV_RENAME: if (n && tx.arg < W.ninv) { wo_strcpy(W.inv[tx.arg].name, s, WO_NAMELEN); wo_store_mark(ST_STATE); } break;
    case TX_MAT_NEW:
        if (!n || W.nmatadd >= WO_MAXMATADD) break;
        for (i = 0; s[i]; i++) if (i == 0 && s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);   /* it reads mid-sentence */
        W.matadd[W.nmatadd].cat = (uint8_t)tx.arg; wo_strcpy(W.matadd[W.nmatadd].text, s, WO_MATLEN);
        W.nmatadd++;
        wo_store_mark(ST_CUSTOM); toast("Added");
        break;
    default: break;
    }
}

/* ================================================================ dispatch */
typedef void (*draw_fn)(void);
typedef void (*event_fn)(uint32_t, uint32_t, int32_t);
static void ev_none(uint32_t k, uint32_t i, int32_t a) { (void)k; (void)i; (void)a; }

static const draw_fn DRAW[PG_N] = {draw_home, draw_roll, draw_field, draw_session, draw_rate, draw_modes, draw_parked,
                                   draw_library, draw_view, draw_scores, draw_rules, draw_gear, draw_buckets, draw_mats,
                                   draw_presets, draw_settings, draw_help, draw_gloss, draw_text_page};
static const event_fn EVENT[PG_N] = {home_event, roll_event, field_event, session_event, rate_event, modes_event,
                                     parked_event, library_event, view_event, scores_event, rules_event, gear_event,
                                     buckets_event, mats_event, presets_event, settings_event, ev_none, ev_none, ev_none};

/* LEDs: the buttons that do something on this page glow, the keys of the rolled scale glow */
static uint32_t leds_shown = 0xFFFFFFFFu, keys_shown = 0xFFFFFFFFu;
static void leds(void)
{
    uint32_t b = 0, k = 0, i;
    for (i = 0; i < 12; i++) if ((key_tap[i] && key_tap[i][0]) || key_hold[i]) b |= 1u << (i + 2);
    if (TOP.page == PG_TEXT) { b |= 3u; k = 0x7FFFFFFu; }
    else {
        sync_scale();
        for (i = 0; i < 27; i++) if (wo_synth_in_scale((int)i)) k |= 1u << i;
    }
    if (b != leds_shown) {
        for (i = 0; i < 14; i++) H->led(i, (b >> i & 1u) ? ((i == HB_BTN_ENV && wo_synth_playing()) ? 1u : 2u) : 0u);
        leds_shown = b;
    }
    if (k != keys_shown) { for (i = 0; i < 27; i++) H->led(HB_LED_KEY(i), (k >> i & 1u) ? 2u : 0u); keys_shown = k; }
}

/* the keys the module handles itself, before the page: ENV, PLAY, OCT-, OCT+ */
static int global_tap(uint32_t id)
{
    if (id == HB_BTN_ENV) { chords_toggle(); return 1; }
    if (id == HB_BTN_PLAY) {
        if (!W.has_cur) quick_start();
        else { wo_pause(); W.cur.powered_off = 0; wo_store_mark(ST_CUR); toast(W.cur.running ? "Clock running" : "Paused"); }
        return 1;
    }
    if (id == HB_BTN_OCT_DOWN) { wo_synth_octave(-1); toast("Keys an octave down"); return 1; }
    if (id == HB_BTN_OCT_UP) { wo_synth_octave(1); toast("Keys an octave up"); return 1; }
    if (id == HB_BTN_HOME) { if (depth) pop(); return 1; }
    return 0;
}
static int global_hold(uint32_t id)
{
    if (id == HB_BTN_PLAY && W.has_cur) { push(PG_RATE, 0); TOP.sel = 1; return 1; }
    return 0;
}

/* ================================================================ the module's calls */
void wo_ui_init(const hb_host_t *h)
{
    H = h;
    depth = 0; st[0].page = PG_HOME; st[0].sel = 0; st[0].scroll = 0;
    if (W.has_cur) push(PG_SESSION, 0);
    need_full = 1; leds_shown = keys_shown = 0xFFFFFFFFu;
    held_btn = -1;
    wo_synth_volume(W.set.volume);
    sync_scale();
}

static void tap(uint32_t id)
{
    uint8_t p = TOP.page;
    if (p != PG_TEXT && global_tap(id)) return;
    EVENT[p](HB_EV_BTN_DOWN, id, 0);
}

void wo_ui_event(uint32_t ev, int32_t arg)
{
    uint32_t kind = HB_EV_KIND(ev), id = HB_EV_ID(ev);
    uint8_t p = TOP.page;
    need_full = 1;
    if (p == PG_TEXT) {                         /* typing: every key is a letter, nothing is held */
        if (kind == HB_EV_BTN_DOWN && id == HB_BTN_HOME) { pop(); return; }
        text_event(kind, id, arg);
        return;
    }
    if (kind == HB_EV_KEY_DOWN || kind == HB_EV_KEY_UP) {
        if (W.set.sound) { sync_scale(); wo_synth_key((int)id, kind == HB_EV_KEY_DOWN, arg); }
        need_full = 0;
        return;
    }
    if (kind == HB_EV_ENC && (id == HB_ENC_SELECT || id == HB_ENC_PRESETS)) {
        int n = lv_entries(), s = TOP.sel + (id == HB_ENC_PRESETS ? arg * 4 : arg);
        if (n <= 0) { TOP.scroll = (int16_t)(TOP.scroll + arg); if (TOP.scroll < 0) TOP.scroll = 0; return; }
        if (s < 0) s = 0;
        if (s >= n) s = n - 1;
        while (s > 0 && !lv_has(s)) s--;
        TOP.sel = (int16_t)s;
        return;
    }
    if (kind == HB_EV_BTN_DOWN) {
        int gi = (int)id - 2;
        if (id == HB_BTN_HOME) return;          /* on release: a long hold is the host's picker */
        if (gi >= 0 && gi < 12 && key_hold[gi]) { held_btn = (int)id; held_fired = 0; held_t0 = H->ms(); return; }
        tap(id);
        return;
    }
    if (kind == HB_EV_BTN_UP) {
        if (id == HB_BTN_HOME) { tap(id); return; }
        if ((int)id == held_btn) {
            held_btn = -1;
            if (!held_fired) tap(id);
        }
        return;
    }
    EVENT[p](kind, id, arg);
}

/* the main loop's: redraw what changed, fire holds, keep the clock, save when quiet */
void wo_ui_draw(uint32_t full)
{
    uint32_t t = H->ms();
    wo_synth_poll(t);
    if (held_btn >= 0 && !held_fired && t - held_t0 >= HOLD_MS) {
        uint8_t p = TOP.page;
        held_fired = 1;
        if (!global_hold((uint32_t)held_btn)) EVENT[p](EV_HOLD, (uint32_t)held_btn, 0);
        need_full = 1;
    }
    if (toast_until && t >= toast_until) { toast_until = 0; need_full = 1; }
    if (full) need_full = 1;
    if (need_full) {
        int guard;
        nops = 0; tpos = 0;
        DRAW[TOP.page]();
        /* the selection may have landed on a row with no entry: settle it */
        for (guard = 0; guard < 3 && TOP.sel >= 0 && lv_entries() > 0 && !lv_has(TOP.sel); guard++) {
            TOP.sel = (int16_t)(TOP.sel >= lv_entries() ? lv_entries() - 1 : 0);
            nops = 0; tpos = 0; DRAW[TOP.page]();
        }
        flush_rows(0, 240);
        need_full = 0;
        last_clock_s = W.has_cur ? wo_elapsed(&W.cur) / 1000u : 0xFFFFFFFEu;
        leds();
    } else if (W.has_cur && W.cur.running && wo_elapsed(&W.cur) / 1000u != last_clock_s) {
        /* once a second: the header only */
        int i, keep = 0;
        last_clock_s = wo_elapsed(&W.cur) / 1000u;
        nops = 0; tpos = 0;
        DRAW[TOP.page]();
        for (i = 0; i < nops; i++) if (ops[i].y < HDR_H) ops[keep++] = ops[i];
        nops = keep;
        flush_rows(0, HDR_H);
        if (last_clock_s % 300u == 0u) wo_store_mark(ST_CUR);     /* the clock, every five minutes */
    }
    wo_store_poll(!wo_synth_busy());
    if (wo_store_failed()) toast("Storage full: some of it was not saved");
}

void wo_ui_release(void)
{
    wo_synth_stop();
    wo_synth_all_off();
    wo_store_flush();
}
