/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1's screens: 240 x 240, the 16 px Terminus font (30 columns), drawn into 240 x 80 strips.
 * Every page lists what you can do with it on its bottom line. */
#include "hb_module.h"
#include "wo.h"
#include "wo_store.h"
#include "wo_synth.h"
#include "wo_ui.h"

static const hb_host_t *H;

/* ================================================================ colour */
static uint16_t C(int k) { return WO_THEMES[W.set.theme < WO_NTHEMES ? W.set.theme : 0].c[k]; }
static uint16_t tier_col(uint8_t t)
{
    return t == TIER_GENTLE ? C(TC_CYAN) : t == TIER_FIRM ? C(TC_LITE) : t == TIER_BRUTAL ? C(TC_MAGENTA) : C(TC_DIMMER);
}
static const char TIERCH[3] = {'G', 'F', 'B'};

/* ================================================================ the draw list */
#define NOPS 300
#define TPOOL 9000
typedef struct { int16_t x, y, w, h; uint16_t c; int16_t s; uint8_t big; } op_t;
static op_t ops[NOPS];
static int nops, tpos;
static char tpool[TPOOL];

static void d_rect(int x, int y, int w, int h, uint16_t c)
{
    if (nops >= NOPS) return;
    ops[nops].x = (int16_t)x; ops[nops].y = (int16_t)y; ops[nops].w = (int16_t)w; ops[nops].h = (int16_t)h;
    ops[nops].c = c; ops[nops].s = -1; ops[nops].big = 0; nops++;
}
static void d_text(int x, int y, const char *s, uint16_t c, int big)
{
    int n = wo_strlen(s);
    if (nops >= NOPS || tpos + n + 1 > TPOOL || !n) return;
    wo_strcpy(tpool + tpos, s, n + 1);
    ops[nops].x = (int16_t)x; ops[nops].y = (int16_t)y; ops[nops].w = (int16_t)(n * (big ? 16 : 8));
    ops[nops].h = (int16_t)(big ? 32 : 16);
    ops[nops].c = c; ops[nops].s = (int16_t)tpos; ops[nops].big = (uint8_t)big; nops++;
    tpos += n + 1;
}
static void d_text_r(int xr, int y, const char *s, uint16_t c) { d_text(xr - wo_strlen(s) * 8, y, s, c, 0); }

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
            else H->cv_text(o->x, o->y - sy, o->big, tpool + o->s, o->c);
        }
        H->cv_blit(0, sy);
    }
}

/* ================================================================ pages */
enum { PG_HOME, PG_ROLL, PG_FIELD, PG_SESSION, PG_RATE, PG_MODES, PG_PARKED, PG_LIBRARY, PG_VIEW, PG_SCORES,
       PG_RULES, PG_GEAR, PG_BUCKETS, PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP, PG_GLOSS, PG_TEXT, PG_CONF };
typedef struct { uint8_t page; int16_t sel; int16_t scroll; int16_t arg; } frame_t;
static frame_t st[10];
static int depth;
#define TOP (st[depth])

static char toast_s[48];
static uint32_t toast_until;
static uint32_t confirm_at; static int confirm_what = -1;
static uint32_t home_down;
static int need_full = 1;
static uint32_t last_clock_s = 0xFFFFFFFFu;

static void toast(const char *s) { wo_strcpy(toast_s, s, 48); toast_until = H->ms() + 1800u; need_full = 1; }
static void push(uint8_t p, int16_t arg) { if (depth < 9) depth++; st[depth].page = p; st[depth].sel = 0; st[depth].scroll = 0; st[depth].arg = arg; need_full = 1; }
static void pop(void) { if (depth > 0) depth--; need_full = 1; }
static void go_root(uint8_t p) { depth = 0; st[0].page = PG_HOME; st[0].sel = 0; st[0].scroll = 0; if (p != PG_HOME) push(p, 0); }
static int confirm(int what)          /* the same destructive press twice within 2 s */
{
    uint32_t t = H->ms();
    if (confirm_what == what && t - confirm_at < 2000u) { confirm_what = -1; return 1; }
    confirm_what = what; confirm_at = t;
    toast("Press again to confirm");
    return 0;
}

/* ================================================================ the list view */
#define ROWH 17
#define Y0 19
#define NROWS 12
#define MAXROWS 220
typedef struct { char t[31]; uint16_t fg, bg; int16_t entry; uint8_t x; char badge; uint16_t bcol; } row_t;
static row_t rows[MAXROWS];
static int nrows;
static int lv_nowrap;                   /* the next lv_add: one row, cut short with a '.' */

static void lv_reset(void) { nrows = 0; }

/* wrapped into rows of up to 30 columns; continuation rows hang under the text, past the prefix */
static void lv_add(int entry, int indent, char badge, uint16_t bcol, const char *pre, const char *text, uint16_t fg, uint16_t bg)
{
    int x0 = indent + (badge ? 12 : 0), cols = (236 - x0) / 8, plen = wo_strlen(pre), pos = 0, first = 1;
    int tl = wo_strlen(text);
    if (cols < 4) cols = 4;
    do {
        row_t *r;
        int room = cols - (first ? plen : (plen > 4 ? 2 : plen)), n, cut, k = 0;
        if (nrows >= MAXROWS) return;
        r = &rows[nrows++];
        r->fg = fg; r->bg = bg; r->entry = (int16_t)entry; r->x = (uint8_t)x0;
        r->badge = first ? badge : 0; r->bcol = bcol;
        if (first) { for (k = 0; k < plen && k < 30; k++) r->t[k] = pre[k]; }
        else { int pad = plen > 4 ? 2 : plen; for (k = 0; k < pad; k++) r->t[k] = ' '; }
        while (text[pos] == ' ' && !first) pos++;
        n = tl - pos;
        if (n > room && lv_nowrap) { n = room; }
        else if (n > room) {
            cut = room;
            while (cut > 0 && text[pos + cut] != ' ' && text[pos + cut - 1] != '_') cut--;
            if (cut <= room / 3) cut = room;
            n = cut;
        }
        if (n < 0) n = 0;
        {
            int q;
            for (q = 0; q < n && k < 30; q++) r->t[k++] = text[pos + q];
        }
        r->t[k] = 0;
        pos += n;
        first = 0;
        if (lv_nowrap) { if (pos < tl && k > 0) r->t[k - 1] = '.'; break; }
    } while (pos < tl);
    lv_nowrap = 0;
}
static void lv_note(const char *text, uint16_t fg) { lv_add(-1, 4, 0, 0, "", text, fg, C(TC_INK)); }

/* draw the rows so the selected entry is in view */
static void lv_draw(int sel)
{
    int first = -1, last = -1, i, s = TOP.scroll, y;
    for (i = 0; i < nrows; i++) if (rows[i].entry == sel && sel >= 0) { if (first < 0) first = i; last = i; }
    if (first >= 0) {
        if (first < s) s = first;
        if (last >= s + NROWS) s = last - NROWS + 1;
        if (first < s) s = first;
        if (first == 0 || (s > 0 && rows[s - 1].entry < 0 && first == s)) {   /* show notes above the first entry */
            while (s > 0 && rows[s - 1].entry < 0 && last - (s - 1) < NROWS) s--;
        }
    }
    if (s > nrows - NROWS) s = nrows - NROWS;
    if (s < 0) s = 0;
    TOP.scroll = (int16_t)s;
    for (i = s, y = Y0; i < nrows && i < s + NROWS; i++, y += ROWH) {
        row_t *r = &rows[i];
        int on = sel >= 0 && r->entry == sel;
        uint16_t bg = on ? C(TC_INK3) : r->bg;
        if (bg != C(TC_INK)) d_rect(0, y - 1, 240, ROWH, bg);
        if (on) d_rect(0, y - 1, 3, ROWH, C(TC_SIGNAL));
        if (r->badge) { d_rect(r->x - 12, y + 1, 10, 13, r->bcol); { char b[2] = {r->badge, 0}; d_text(r->x - 11, y, b, C(TC_ONACC), 0); } }
        d_text(r->x, y, r->t, r->fg, 0);
    }
    if (nrows > NROWS) {                    /* a scroll mark on the right */
        int h = 204 * NROWS / nrows, top = 204 * s / nrows;
        d_rect(237, Y0 + top, 3, h < 6 ? 6 : h, C(TC_RULE2));
    }
}

/* how many entries the page's list holds, for moving the selection */
static int lv_entries(void)
{
    int i, m = -1;
    for (i = 0; i < nrows; i++) if (rows[i].entry > m) m = rows[i].entry;
    return m + 1;
}
static int lv_has(int e) { int i; for (i = 0; i < nrows; i++) if (rows[i].entry == e) return 1; return 0; }

/* ================================================================ chrome */
static void chrome(const char *title, const char *right, const char *hint)
{
    char clk[16];
    d_rect(0, 0, 240, 18, C(TC_INK2));
    d_rect(0, 18, 240, 1, C(TC_RULE2));
    d_text(4, 1, title, C(TC_SIGNAL), 0);
    if (right && right[0]) d_text_r(236, 1, right, C(TC_DIM));
    else if (W.has_cur) {
        uint32_t e = wo_elapsed(&W.cur);
        if (W.cur.limit_ms) wo_hhmmss(clk, W.cur.limit_ms > e ? W.cur.limit_ms - e : 0, 16);
        else wo_hhmmss(clk, e, 16);
        d_text_r(236, 1, clk, W.cur.running ? C(TC_TEXT) : C(TC_DIMMER));
    }
    d_rect(0, 222, 240, 1, C(TC_RULE));
    d_rect(0, 223, 240, 17, C(TC_INK2));
    if (toast_until && H->ms() < toast_until) d_text(4, 223, toast_s, C(TC_SIGNAL), 0);
    else d_text(4, 223, hint, C(TC_DIM), 0);
}

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
    int n = wo_strlen(tx.s), i;
    if (n >= tx.cap - 1) return;
    if (ch >= 'a' && ch <= 'z') {
        int auto_cap = tx.cur == 0 || (tx.cur >= 2 && tx.s[tx.cur - 1] == ' ' && (tx.s[tx.cur - 2] == '.' || tx.s[tx.cur - 2] == '?' || tx.s[tx.cur - 2] == '!'));
        if (tx.shift || (auto_cap && tx.purpose != TX_INV_NEW && tx.purpose != TX_INV_RENAME)) ch = (char)(ch - 32);
        if ((tx.purpose == TX_INV_NEW || tx.purpose == TX_INV_RENAME) && tx.cur == 0) ch = (char)(ch >= 'a' ? ch - 32 : ch);
        tx.shift = 0;
    }
    for (i = n + 1; i > tx.cur; i--) tx.s[i] = tx.s[i - 1];
    tx.s[tx.cur++] = ch;
}

static void text_done(void);

static void draw_text_page(void)
{
    int i, x, row = 0, col = 0, y;
    chrome(tx.prompt, "", "SCL done OCT- del OCT+ space");
    /* the text, wrapped by character, with the cursor */
    for (i = 0; i <= wo_strlen(tx.s) && row < 5; i++) {
        y = Y0 + 4 + row * 18;
        if (i == tx.cur) d_rect(4 + col * 8, y + 15, 8, 2, C(TC_SIGNAL));
        if (tx.s[i]) { char b[2] = {tx.s[i], 0}; d_text(4 + col * 8, y, b, C(TC_TEXT), 0); }
        if (++col >= 29) { col = 0; row++; }
    }
    y = 118;
    {
        char b[40];
        wo_strcpy(b, "FX inserts: ", 40);
        { char c2[2] = {SYMS[tx.sym], 0}; wo_strcat(b, c2, 40); }
        wo_strcat(b, "  (KNOB1)", 40);
        if (tx.shift) wo_strcat(b, "  EDIT:CAPS", 40);
        d_text(4, y, b, C(TC_DIM), 0);
    }
    /* the keyboard as letters */
    y = 140;
    for (i = 0; i < 16; i++) {
        char b[2] = {(char)(key_char(WHITE[i]) - 32), 0};
        x = i * 15;
        d_rect(x, y, 14, 80, C(TC_TEXT));
        d_text(x + 3, y + 62, b, C(TC_INK), 0);
    }
    for (i = 0; i < 27; i++) {
        int w = 0, j, wi = 0;
        for (j = 0; j < 16; j++) { if (WHITE[j] == i) w = 1; if (WHITE[j] < i) wi = j + 1; }
        if (w) continue;
        x = wi * 15 - 5;
        d_rect(x, y, 10, 46, C(TC_INK2));
        { char b[2] = {key_char(i) == '.' ? '.' : (char)(key_char(i) - 32), 0}; d_text(x + 1, y + 28, b, C(TC_SIGNAL), 0); }
    }
}

static void text_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_KEY_DOWN) { text_insert(key_char((int)id)); return; }
    if (kind == HB_EV_ENC) {
        if (id == HB_ENC_SELECT) { tx.cur += arg; if (tx.cur < 0) tx.cur = 0; if (tx.cur > wo_strlen(tx.s)) tx.cur = wo_strlen(tx.s); }
        else if (id == HB_ENC_KNOB1 || id == HB_ENC_ALGORITHM) {
            int n = (int)sizeof SYMS - 1;
            tx.sym = ((tx.sym + arg) % n + n) % n;
        }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_OCT_DOWN) {
        int n = wo_strlen(tx.s), i;
        if (tx.cur > 0) { for (i = tx.cur - 1; i < n; i++) tx.s[i] = tx.s[i + 1]; tx.cur--; }
    } else if (id == HB_BTN_OCT_UP) text_insert(' ');
    else if (id == HB_BTN_EDIT) tx.shift = !tx.shift;
    else if (id == HB_BTN_FX) text_insert(SYMS[tx.sym]);
    else if (id == HB_BTN_SEL || id == HB_BTN_SAVE) text_done();
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
        lv_add(i, 4, 0, 0, "", term, C(TC_SIGNAL), C(TC_INK));
        lv_add(i, 12, 0, 0, "", def, C(TC_TEXT), C(TC_INK));
    }
    chrome("GLOSSARY", "", "SELECT scroll  HOME back");
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

static void play_toggle(void)
{
    int bpm;
    const wo_chords_t *c = chords_now(&bpm);
    if (wo_synth_playing()) { wo_synth_stop(); toast("Stopped"); return; }
    if (!c || !c->n) { toast("No chords rolled: switch Chords on"); return; }
    wo_synth_play(c, bpm, H->ms());
}

static void mode_name(uint8_t m, char *o, int cap) { wo_str(WO_MODES[m < WO_NMODES ? m : 0].name, o, cap); }

/* ================================================================ HOME */
static const char *const HOME_ITEMS[] = {"Roll", "Session", "Modes", "Parked", "Library", "Scores", "Rules", "Gear",
                                         "Materials", "Presets", "Settings", "Help"};
#define NHOME 12
static void draw_home(void)
{
    char sub[40];
    int i;
    lv_reset();
    for (i = 0; i < NHOME; i++) {
        int on = 0, k;
        sub[0] = 0;
        switch (i) {
        case 0: for (k = 0; k < WO_NFIELDS; k++) on += W.on[k]; wo_catu(sub, (uint32_t)on, 40); wo_strcat(sub, " params", 40);
                if (wo_draft_ready()) wo_strcat(sub, ", ready", 40); break;
        case 1: if (W.has_cur) { wo_strcpy(sub, W.cur.name, 19); } else wo_strcpy(sub, "no ticket", 40); break;
        case 2: mode_name(W.set.mode, sub, 19); break;
        case 3: wo_catu(sub, (uint32_t)wo_parked_count(), 40); wo_strcat(sub, " of 2", 40); break;
        case 4: wo_catu(sub, (uint32_t)wo_lib_count(), 40); wo_strcat(sub, " filed", 40); break;
        case 5: wo_catu(sub, W.nscores, 40); wo_strcat(sub, " scored", 40); break;
        case 6: wo_catu(sub, WO_NENTRIES + W.ncustom, 40); wo_strcat(sub, " rules", 40); break;
        case 7: for (k = 0; k < W.ninv; k++) on += wo_inv_on((uint8_t)k); wo_catu(sub, (uint32_t)on, 40); wo_strcat(sub, " on", 40); break;
        case 8: wo_catu(sub, WO_NMAT + W.nmatadd, 40); wo_strcat(sub, " sounds", 40); break;
        case 9: wo_catu(sub, WO_NBUILTIN, 40); wo_strcat(sub, " setups", 40); break;
        default: break;
        }
        {
            char row[32];
            int l, pad;
            wo_strcpy(row, HOME_ITEMS[i], 32);
            l = wo_strlen(row); pad = 28 - l - wo_strlen(sub);
            while (pad-- > 0 && l < 30) row[l++] = ' ';
            row[l] = 0;
            wo_strcat(row, sub, 31);
            lv_add(i, 8, 0, 0, "", row, i == 1 && W.has_cur ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        }
    }
    chrome("WORKORDER", WO_BUILD " FM-1", "SELECT move  SCL open");
    lv_draw(TOP.sel);
}
static void open_home(int i)
{
    static const uint8_t P[NHOME] = {PG_ROLL, PG_SESSION, PG_MODES, PG_PARKED, PG_LIBRARY, PG_SCORES, PG_RULES, PG_GEAR,
                                     PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP};
    if (i >= 0 && i < NHOME) push(P[i], 0);
}

/* ================================================================ ROLL */
static void draw_roll(void)
{
    char conf[WO_NCONF][WO_CONFLEN], pre[20], lab[24];
    uint8_t idx[WO_MAXLINES];
    int nc = W.has_draft ? wo_conflicts(&W.draft, conf, WO_NCONF) : 0, i, j;
    lv_reset();
    for (i = 0; i < nc; i++) { lv_add(-1, 4, 0, 0, "! ", conf[i], C(TC_SIGNAL), C(TC_INK)); }
    for (i = 0; i < WO_NFIELDS; i++) {
        uint8_t f = WO_ORDER[i];
        int on = W.on[f];
        uint16_t fg = on ? C(TC_TEXT) : C(TC_DIMMER);
        wo_field_name(f, lab, 24);
        pre[0] = 0;
        wo_strcat(pre, on ? "" : "-", 20);
        wo_strcat(pre, lab, 20);
        if (W.lock[f]) wo_strcat(pre, "*", 20);
        if (W.count[f]) { wo_strcat(pre, " x", 20); wo_catu(pre, W.count[f], 20); }
        if (W.mixin[f]) wo_strcat(pre, " mix", 20);
        while (wo_strlen(pre) < 12) wo_strcat(pre, " ", 20);
        wo_strcat(pre, " ", 20);
        if (TOP.sel == i && on && W.has_draft && W.draft.has[f]) {
            lv_add(i, 16, TIERCH[W.sev[f]], tier_col(W.sev[f]), "", pre, C(TC_SIGNAL), C(TC_INK));
            if (f == F_KEY || f == F_TEMPO || f == F_GEAR) { field_preview(f, tb, WO_LINE); lv_add(i, 20, 0, 0, "", tb, C(TC_TEXT), C(TC_INK)); }
            if (f == F_GEAR) for (j = 1; j < W.draft.gear.nslots; j++) { wo_slot_text(&W.draft.gear.slot[j], tb, WO_LINE); lv_add(i, 20, 0, 0, "", tb, C(TC_TEXT), C(TC_INK)); }
            {
                int n = wo_field_lines(&W.draft, f, idx, WO_MAXLINES);
                for (j = 0; j < n; j++) lv_add(i, 20, 0, 0, (W.draft.line[idx[j]].flags & L_HEAD) ? "" : "> ", W.draft.line[idx[j]].text,
                                               (W.draft.line[idx[j]].flags & L_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            }
        } else {
            char row[64];
            wo_strcpy(row, pre, 64);
            if (on) { field_preview(f, tb, WO_LINE); wo_strcat(row, tb, 64); }
            lv_nowrap = 1;
            lv_add(i, 16, TIERCH[W.sev[f]], on ? tier_col(W.sev[f]) : C(TC_DIMMER), "", row, fg, C(TC_INK));
        }
    }
    {
        char right[24]; int on = 0, k;
        for (k = 0; k < WO_NFIELDS; k++) on += W.on[k];
        right[0] = 0;
        if (nc) { wo_strcat(right, "!", 24); wo_catu(right, (uint32_t)nc, 24); wo_strcat(right, " ", 24); }
        wo_strcat(right, wo_draft_ready() ? "READY" : "", 24);
        chrome("ROLL", right[0] ? right : " ", "REC roll  ARP one  LFO issue");
    }
    lv_draw(TOP.sel);
}

static void roll_event(uint32_t kind, uint32_t id, int32_t arg)
{
    uint8_t f = WO_ORDER[TOP.sel < WO_NFIELDS ? TOP.sel : 0];
    if (kind == HB_EV_ENC) {
        if (id == HB_ENC_ALGORITHM) { int s = W.sev[f] + (arg > 0 ? 1 : -1); W.sev[f] = (uint8_t)(s < 0 ? 0 : s > 2 ? 2 : s); wo_store_mark(ST_STATE); }
        else if (id == HB_ENC_KNOB1) { int c = W.count[f] + (arg > 0 ? 1 : -1); W.count[f] = (uint8_t)(c < 0 ? 0 : c > 6 ? 6 : c); wo_store_mark(ST_STATE); }
        else if (id == HB_ENC_KNOB2) { W.mixin[f] = arg > 0; wo_store_mark(ST_STATE); }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: W.on[f] = !W.on[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_SAVE: W.lock[f] = !W.lock[f]; toast(W.lock[f] ? "Held: rolls leave it alone" : "Released"); wo_store_mark(ST_STATE); break;
    case HB_BTN_REC: wo_synth_stop(); wo_roll_all(); wo_store_mark(ST_DRAFT | ST_STATE); toast("Rolled"); break;
    case HB_BTN_ARP: if (W.on[f]) { wo_roll_field(f); wo_store_mark(ST_DRAFT | ST_STATE); } else toast("Switch it on first (EDIT)"); break;
    case HB_BTN_SEQ: wo_mutate(); wo_store_mark(ST_DRAFT | ST_STATE); toast("Mutated"); break;
    case HB_BTN_FX: wo_full_random(); wo_store_mark(ST_DRAFT | ST_STATE); toast(W.set.chaos ? "Chaos roll" : "Random roll"); break;
    case HB_BTN_SEL: push(PG_FIELD, f); break;
    case HB_BTN_LFO:
        if (!wo_draft_ready()) { toast("Roll every parameter first"); break; }
        if (W.has_cur && wo_park()) { toast("Park slots full: close a ticket"); break; }
        wo_issue(); wo_store_mark(ST_CUR | ST_STATE | ST_DRAFT);
        go_root(PG_SESSION); toast("Ticket issued");
        break;
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
    if (W.has_draft && d->has[f]) {
        if (f == F_KEY) {
            wo_key_line(&d->key, tb, WO_LINE); lv_add(0, 4, 0, 0, "", tb, C(TC_TEXT), C(TC_INK));
            wo_str(WO_SCALES[d->key.scale].chr, tb, WO_LINE); lv_add(0, 12, 0, 0, "", tb, C(TC_DIM), C(TC_INK));
        } else if (f == F_TEMPO) {
            wo_tempo_line(&d->tempo, tb, WO_LINE); lv_add(0, 4, 0, 0, "", tb, C(TC_TEXT), C(TC_INK));
            wo_str(WO_BANDS[d->tempo.band].name, tb, WO_LINE); lv_add(0, 12, 0, 0, "Band: ", tb, C(TC_DIM), C(TC_INK));
        } else if (f == F_GEAR) {
            if (d->gear.empty) lv_note("No category feeds GEAR. Fix it on the Gear page (SEQ).", C(TC_SIGNAL));
            if (d->gear.nbanned) {
                wo_strcpy(tb, "Banned this session: ", WO_LINE);
                for (i = 0; i < d->gear.nbanned; i++) { if (i) wo_strcat(tb, ", ", WO_LINE); wo_strcat(tb, W.inv[d->gear.banned[i]].name, WO_LINE); }
                lv_note(tb, C(TC_TEXT));
            }
            for (i = 0; i < d->gear.nslots; i++) { wo_slot_text(&d->gear.slot[i], tb, WO_LINE); lv_add(-1, 4, 0, 0, "- ", tb, C(TC_TEXT), C(TC_INK)); }
            if (d->gear.exclusive) lv_note("Nothing else may be loaded.", C(TC_TEXT));
        }
        n = wo_field_lines(d, f, idx, WO_MAXLINES);
        for (i = 0; i < n; i++) {
            const wo_line_t *l = &d->line[idx[i]];
            int e = (f == F_KEY || f == F_TEMPO) ? 0 : i;
            lv_add(e, 16, l->tier < 3 ? TIERCH[l->tier] : 0, tier_col(l->tier), "", l->text,
                   (l->flags & L_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        }
        if (f == F_GEAR && !n) lv_note("(no directives)", C(TC_DIMMER));
    } else lv_note(W.on[f] ? "Not rolled yet. REC rolls it." : "Switched off. EDIT switches it on.", C(TC_DIM));
    lv_note(" ", C(TC_DIM));
    {
        char b[64];
        wo_strcpy(b, "Severity ", 64); wo_strcat(b, wo_tier_name(W.sev[f]), 64);
        wo_strcat(b, "  lines ", 64);
        if (W.count[f]) wo_catu(b, W.count[f], 64); else wo_strcat(b, "auto", 64);
        if (W.mixin[f]) wo_strcat(b, "  mixed", 64);
        lv_note(b, C(TC_DIM));
        lv_note("ALGO severity KNOB1 lines KNOB2 mixed  SAVE hold  EDIT on/off", C(TC_DIMMER));
    }
    wo_field_name(f, title, 40);
    { int k; for (k = 0; title[k]; k++) if (title[k] >= 'a' && title[k] <= 'z') title[k] = (char)(title[k] - 32); }
    wo_strcpy(right, W.on[f] ? "" : "OFF ", 24);
    if (W.lock[f]) wo_strcat(right, "HELD", 24);
    chrome(title, right[0] ? right : " ", "ARP line  REC all  GLO terms");
    lv_draw(TOP.sel);
}

static void field_event(uint32_t kind, uint32_t id, int32_t arg)
{
    uint8_t f = (uint8_t)TOP.arg, idx[WO_MAXLINES];
    if (kind == HB_EV_ENC) {
        if (id == HB_ENC_ALGORITHM) { int s = W.sev[f] + (arg > 0 ? 1 : -1); W.sev[f] = (uint8_t)(s < 0 ? 0 : s > 2 ? 2 : s); }
        else if (id == HB_ENC_KNOB1) { int c = W.count[f] + (arg > 0 ? 1 : -1); W.count[f] = (uint8_t)(c < 0 ? 0 : c > 6 ? 6 : c); }
        else if (id == HB_ENC_KNOB2) W.mixin[f] = arg > 0;
        wo_store_mark(ST_STATE);
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: W.on[f] = !W.on[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_SAVE: W.lock[f] = !W.lock[f]; wo_store_mark(ST_STATE); break;
    case HB_BTN_REC: if (W.on[f]) { wo_roll_field(f); wo_store_mark(ST_DRAFT | ST_STATE); } break;
    case HB_BTN_ARP:
        if (!W.on[f]) break;
        if (f == F_KEY || f == F_TEMPO || !W.has_draft || !W.draft.has[f]) wo_roll_field(f);
        else if (!wo_reroll_line(f, (uint8_t)TOP.sel)) wo_roll_field(f);
        wo_store_mark(ST_DRAFT | ST_STATE);
        break;
    case HB_BTN_GLO: {
        int n = W.has_draft ? wo_field_lines(&W.draft, f, idx, WO_MAXLINES) : 0;
        if (n && TOP.sel < n) gloss_open(W.draft.line[idx[TOP.sel]].text, f);
        else { field_preview(f, tb, WO_LINE); gloss_open(tb, f); }
        break; }
    case HB_BTN_LFO: roll_event(kind, id, arg); break;
    default: break;
    }
}

/* ================================================================ SESSION */
static uint8_t item_skey(const wo_item_t *it) { return it->label >= 100 ? WO_POOL_SKEY[it->label - 100] : it->pkey; }

static void draw_session(void)
{
    wo_ticket_t *c = &W.cur;
    char b[80], lab[24];
    int i, hand = 0;
    lv_reset();
    if (!W.has_cur) {
        lv_note("No ticket in play.", C(TC_TEXT));
        lv_note("Roll one on the Roll page (LFO issues it), start a game mode, or resume a parked one.", C(TC_DIM));
        chrome("SESSION", " ", "HOME back");
        lv_draw(-1);
        return;
    }
    b[0] = 0;
    wo_strcat(b, "pts ", 80); wo_catu(b, wo_ticket_score(c), 80); wo_strcat(b, " / ", 80); wo_catu(b, wo_ticket_max(c), 80);
    mode_name(c->mode, lab, 24);
    wo_strcat(b, "  ", 80); wo_strcat(b, c->running ? lab : "PAUSED", 80);
    lv_note(b, c->running ? C(TC_DIM) : C(TC_SIGNAL));
    lv_note(c->name, C(TC_DIMMER));
    if (c->powered_off) lv_note("The clock stopped when the FM-1 was switched off. LFO starts it again.", C(TC_SIGNAL));
    if (c->limit_ms && wo_elapsed(c) > c->limit_ms) lv_note("Time is up: nothing ticked now scores.", C(TC_SIGNAL));
    else if (c->target_min && wo_elapsed(c) > c->target_min * 60000u) lv_note("Past your target time.", C(TC_SIGNAL));
    for (i = 0; i < c->nconf; i++) lv_add(-1, 4, 0, 0, "! ", c->conf[i], C(TC_SIGNAL), C(TC_INK));
    if (c->is_run) {
        const wo_run_t *r = &c->run;
        wo_strcpy(b, "ROUND ", 80); wo_catu(b, r->round, 80); wo_strcat(b, "/", 80); wo_catu(b, r->nladder, 80);
        wo_strcat(b, "  lives ", 80);
        for (i = 0; i < r->maxlives; i++) wo_strcat(b, i < r->lives ? "#" : ".", 80);
        if (r->round >= 1 && r->round <= r->nladder) { wo_strcat(b, "  ", 80); wo_strcat(b, wo_tier_name(r->ladder[r->round - 1]), 80); }
        if (r->genre != NONE8) { wo_str(WO_GENRE[r->genre], lab, 24); wo_strcat(b, "  ", 80); wo_strcat(b, lab, 80); }
        lv_note(b, C(TC_CYAN));
        if (r->ended) {
            wo_strcpy(b, r->won ? "RUN COMPLETE  +" : (r->reason == 1 ? "RUN OVER: a brutal card was broken" : "RUN OVER: out of lives"), 80);
            if (r->won) wo_catu(b, r->bonus, 80);
            lv_note(b, r->won ? C(TC_CYAN) : C(TC_MAGENTA));
            lv_note("EDIT closes and files it.", C(TC_DIM));
        } else if (r->nhand) {
            hand = 1;
            lv_note("Keep one card. SCL takes it, ARP deals again.", C(TC_TEXT));
            for (i = 0; i < r->nhand; i++) {
                const wo_card_t *k = &r->hand[i];
                wo_card_label(k, lab, 24);
                wo_strcpy(b, lab, 80); wo_strcat(b, "  +", 80); wo_catu(b, k->pts, 80);
                if (k->risk == RISK_FIGHTS) wo_strcat(b, "  FIGHTS YOUR DECK", 80);
                else if (k->risk == RISK_DOUBLES) wo_strcat(b, "  DOUBLES UP", 80);
                else if (k->risk == RISK_PILES) wo_strcat(b, "  PILES ON", 80);
                lv_add(i, 16, TIERCH[k->tier], tier_col(k->tier), "", b, k->risk ? C(TC_SIGNAL) : C(TC_DIM), C(TC_INK2));
                lv_add(i, 16, 0, 0, "", k->text, C(TC_TEXT), C(TC_INK2));
            }
            if (c->nitems) lv_note("Your deck:", C(TC_DIM));
        } else if (c->nitems) lv_note("Tick the card off to reach the next round.", C(TC_DIM));
    }
    for (i = 0; i < c->nitems; i++) {
        const wo_item_t *it = &c->item[i];
        int e = hand ? -1 : i;
        const char *mark = (it->flags & I_BROKEN) ? "[/] " : (it->flags & I_CHECKED) ? "[x] " : "[ ] ";
        uint16_t fg = (it->flags & I_BROKEN) ? C(TC_DIMMER) : (it->flags & I_CHECKED) ? C(TC_DIM) : (it->flags & I_HEAD) ? C(TC_SIGNAL) : C(TC_TEXT);
        lv_add(e, 16, it->tier < 3 ? TIERCH[it->tier] : 0, tier_col(it->tier), mark, it->text, fg, C(TC_INK));
        if (it->flags & I_BROKEN) { wo_strcpy(b, "broke it: ", 80); wo_strcat(b, it->why, 80); lv_add(e, 48, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK)); }
        if (!hand && TOP.sel == i) {
            wo_item_label(it, lab, 24);
            wo_strcpy(b, lab, 80);
            if (it->tier < 3) { wo_strcat(b, " | ", 80); wo_strcat(b, wo_tier_name(it->tier), 80); }
            if (it->flags & I_CHECKED) { wo_strcat(b, " | +", 80); wo_catu(b, it->pts, 80); }
            else if (!(it->flags & I_BROKEN)) { wo_strcat(b, " | now +", 80); wo_catu(b, (uint32_t)wo_item_points(c, it, wo_elapsed(c)), 80); }
            if (it->flags & I_WASBROKEN) wo_strcat(b, " | broken earlier", 80);
            lv_add(e, 48, 0, 0, "", b, C(TC_DIMMER), C(TC_INK));
        }
    }
    {
        char t[24];
        wo_strcpy(t, c->is_run ? "RUN" : "SESSION", 24);
        chrome(t, "", hand ? "SCL take  ARP deal again" : (c->is_run && c->run.ended) ? "EDIT close & file" : "SCL tick REC break LFO pause");
    }
    lv_draw(TOP.sel);
}

static void session_event(uint32_t kind, uint32_t id, int32_t arg)
{
    wo_ticket_t *c = &W.cur;
    int hand;
    (void)arg;
    if (kind != HB_EV_BTN_DOWN || !W.has_cur) return;
    hand = c->is_run && !c->run.ended && c->run.nhand;
    switch (id) {
    case HB_BTN_SEL:
        if (hand) { wo_take_card((uint8_t)TOP.sel); TOP.sel = (int16_t)(c->nitems - 1); wo_store_mark(ST_CUR); break; }
        if (TOP.sel < c->nitems) {
            int was = c->item[TOP.sel].flags & I_CHECKED;
            if (!c->running && !was) { toast("Resume the clock to tick (LFO)"); break; }
            wo_tick((uint8_t)TOP.sel);
            if (!was) {
                char b[24]; wo_strcpy(b, "+", 24); wo_catu(b, c->item[TOP.sel].pts, 24);
                if (c->item[TOP.sel].flags & I_WASBROKEN) wo_strcpy(b, "Broken earlier: no points", 24);
                toast(b);
            }
            if (c->is_run && c->run.nhand) TOP.sel = 0;
            wo_store_mark(ST_CUR);
        }
        break;
    case HB_BTN_REC: if (!hand && TOP.sel < c->nitems) text_begin(TX_BREAK, TOP.sel, 0, "WHY DID YOU BREAK IT?", "", WO_WHY); break;
    case HB_BTN_FX: if (!hand && TOP.sel < c->nitems) text_begin(TX_RETIRE, TOP.sel, 0, "RETIRE: WHAT WAS WRONG?", "", WO_WHY); break;
    case HB_BTN_ARP:
        if (hand) { wo_redeal(); wo_store_mark(ST_CUR); toast(c->run.nhand ? "Dealt again" : "Nothing left to deal"); }
        else if (TOP.sel < c->nitems && (c->item[TOP.sel].flags & I_BROKEN)) {
            wo_unbreak((uint8_t)TOP.sel); wo_store_mark(ST_CUR);
            toast((c->item[TOP.sel].flags & I_WASBROKEN) ? "Unbroken, but it can no longer score" : "Unbroken");
        }
        break;
    case HB_BTN_LFO: wo_pause(); c->powered_off = 0; wo_store_mark(ST_CUR); toast(c->running ? "Clock running" : "Paused"); break;
    case HB_BTN_SAVE: {
        int r = wo_park();
        toast(r == 0 ? "Ticket parked" : r == -1 ? "Both park slots are taken" : "Could not save it");
        if (!r) go_root(PG_HOME);
        break; }
    case HB_BTN_EDIT: push(PG_RATE, 0); TOP.sel = 1; break;
    case HB_BTN_GLO:
        if (hand && TOP.sel < c->run.nhand) gloss_open(c->run.hand[TOP.sel].text, WO_POOL_SKEY[c->run.hand[TOP.sel].pool]);
        else if (TOP.sel < c->nitems) gloss_open(c->item[TOP.sel].text, item_skey(&c->item[TOP.sel]));
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
    for (i = 0; i < 4; i++) lv_add(i, 16, 0, 0, TOP.sel == i ? "> " : "  ", RATES[i], i == 3 ? C(TC_MAGENTA) : C(TC_TEXT), C(TC_INK));
    lv_add(4, 16, 0, 0, "Note: ", rate_note[0] ? rate_note : "(REC to write one)", C(TC_DIM), C(TC_INK));
    chrome("CLOSE TICKET", " ", "SCL file it  REC note  HOME back");
    lv_draw(TOP.sel);
}
static void rate_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_REC || (id == HB_BTN_SEL && TOP.sel == 4)) { text_begin(TX_NOTE, 0, 0, "ONE LINE OF OUTCOME", rate_note, WO_WHY); return; }
    if (id == HB_BTN_SEL && TOP.sel < 4) {
        wo_synth_stop();
        wo_close((uint8_t)TOP.sel, rate_note);
        rate_note[0] = 0;
        wo_store_mark(ST_ALL);
        go_root(PG_LIBRARY);
        toast("Filed");
    }
}

/* ================================================================ MODES */
static int mode_order[WO_NMODES + WO_NMODECATS];
static int build_mode_rows(void)
{
    char b[80], nm[32];
    int c, m, n = 0, e = 0;
    lv_reset();
    for (c = 0; c < WO_NMODECATS; c++) {
        wo_str(WO_MODECAT[c], nm, 32);
        { int k; for (k = 0; nm[k]; k++) if (nm[k] >= 'a' && nm[k] <= 'z') nm[k] = (char)(nm[k] - 32); }
        lv_add(-1, 4, 0, 0, "", nm, C(TC_CYAN), C(TC_INK));
        for (m = 0; m < WO_NMODES; m++) {
            const wo_mode_t *md = &WO_MODES[m];
            int mult10, rules = 0, i;
            if (md->cat != c) continue;
            mode_order[e] = m;
            wo_str(md->name, b, 80);
            mult10 = (int)(md->mult * 10.0f + 0.5f);
            while (wo_strlen(b) < 20) wo_strcat(b, " ", 80);
            wo_strcat(b, "x", 80); wo_catu(b, (uint32_t)(mult10 / 10), 80);
            if (mult10 % 10) { wo_strcat(b, ".", 80); wo_catu(b, (uint32_t)(mult10 % 10), 80); }
            if (md->limit) { wo_strcat(b, " ", 80); wo_catu(b, md->limit, 80); wo_strcat(b, "m", 80); }
            lv_add(e, 12, 0, 0, W.set.mode == m ? "* " : "  ", b, TOP.sel == e ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            if (TOP.sel == e) {
                wo_str(md->desc, tb, WO_LINE);
                lv_add(e, 28, 0, 0, "", tb, C(TC_DIM), C(TC_INK));
                for (i = 0; i < WO_NENTRIES; i++) if (WO_ENTRIES[i].mode == m) rules++;
                if (rules) { wo_strcpy(b, "", 80); wo_catu(b, (uint32_t)rules, 80); wo_strcat(b, " rules only this mode deals (EDIT)", 80); lv_add(e, 28, 0, 0, "", b, C(TC_DIMMER), C(TC_INK)); }
            }
            e++; n++;
        }
    }
    return n;
}
static void draw_modes(void)
{
    build_mode_rows();
    chrome("MODES", " ", "SCL start  EDIT its rules");
    lv_draw(TOP.sel);
}
static void modes_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int m;
    (void)arg;
    if (kind != HB_EV_BTN_DOWN) return;
    build_mode_rows();
    m = mode_order[TOP.sel];
    if (id == HB_BTN_SEL) {
        if (W.has_cur && wo_park()) { toast("Park slots full: close a ticket first"); return; }
        wo_synth_stop();
        if (m == M_FREE) { W.set.mode = M_FREE; wo_store_mark(ST_STATE); go_root(PG_ROLL); toast("Free play: your own setup"); return; }
        wo_start_mode((uint8_t)m);
        wo_store_mark(ST_ALL);
        go_root(PG_SESSION);
        { char b[40]; mode_name((uint8_t)m, b, 32); wo_strcat(b, " started", 40); toast(b); }
    } else if (id == HB_BTN_EDIT) {
        push(PG_RULES, (int16_t)(1000 + m));
    }
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
        lv_add(k, 8, 0, 0, "", wo_parked_name(s), C(TC_TEXT), C(TC_INK));
        wo_strcpy(b, "on the clock ", 64); wo_strcat(b, tb2, 64);
        lv_add(k, 24, 0, 0, "", b, C(TC_DIM), C(TC_INK));
    }
    chrome("PARKED", " ", "SCL resume  FX discard");
    lv_draw(TOP.sel);
}
static void parked_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int s;
    (void)arg;
    if (kind != HB_EV_BTN_DOWN || TOP.sel >= wo_parked_count()) return;
    s = wo_parked_slot(TOP.sel);
    if (id == HB_BTN_SEL) {
        int r = wo_unpark(s);
        if (r) { toast(r == -2 ? "No slot free for the ticket in play" : "Could not read it"); return; }
        go_root(PG_SESSION); toast("Resumed");
    } else if (id == HB_BTN_FX && confirm(100 + s)) { wo_discard_parked(s); toast("Discarded"); }
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
    if (!n) lv_note("No filed tickets yet. Close a ticket (EDIT on the Session page) to file it.", C(TC_DIM));
    for (i = 0; i < n; i++) {
        const wo_ticket_t *t = lib_ticket(i);
        if (!t) continue;
        lv_add(i, 8, 0, 0, "", t->name, C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i || n <= 6) {
            wo_strcpy(b, RATES[t->rating & 3], 80); wo_strcat(b, "  ", 80); wo_catu(b, t->score, 80); wo_strcat(b, " pts  ", 80);
            wo_hhmmss(tb2, t->elapsed, 24); wo_strcat(b, tb2, 80);
            if (t->is_run) wo_strcat(b, t->run.won ? "  run won" : "  run lost", 80);
            lv_add(i, 24, 0, 0, "", b, C(TC_DIM), C(TC_INK));
            if (t->note[0] && TOP.sel == i) lv_add(i, 24, 0, 0, "\"", t->note, C(TC_DIMMER), C(TC_INK));
        }
    }
    chrome("LIBRARY", " ", "SCL view  ARP reissue  FX delete");
    lv_draw(TOP.sel);
}
static void library_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind != HB_EV_BTN_DOWN || TOP.sel >= wo_lib_count()) return;
    if (id == HB_BTN_SEL) push(PG_VIEW, TOP.sel);
    else if (id == HB_BTN_FX && confirm(200 + TOP.sel)) { wo_lib_delete(TOP.sel); libt_idx = -1; toast("Deleted"); }
    else if (id == HB_BTN_ARP) {
        const wo_ticket_t *t = lib_ticket(TOP.sel);
        int i;
        if (!t || t->is_run) { toast("A run can't be reissued"); return; }
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
        go_root(PG_SESSION); toast("Reissued");
    }
}
static void draw_view(void)
{
    const wo_ticket_t *t = lib_ticket(TOP.arg);
    char b[80];
    int i;
    lv_reset();
    if (!t) { lv_note("Could not read this ticket.", C(TC_SIGNAL)); chrome("TICKET", " ", "HOME back"); lv_draw(-1); return; }
    lv_note(t->name, C(TC_DIM));
    wo_strcpy(b, RATES[t->rating & 3], 80); wo_strcat(b, "  ", 80); wo_catu(b, t->score, 80); wo_strcat(b, " pts", 80);
    lv_note(b, C(TC_TEXT));
    if (t->note[0]) lv_add(-1, 4, 0, 0, "\"", t->note, C(TC_DIM), C(TC_INK));
    for (i = 0; i < t->nitems; i++) {
        const wo_item_t *it = &t->item[i];
        lv_add(i, 16, it->tier < 3 ? TIERCH[it->tier] : 0, tier_col(it->tier),
               (it->flags & I_BROKEN) ? "[/] " : (it->flags & I_CHECKED) ? "[x] " : "[ ] ", it->text,
               (it->flags & I_BROKEN) ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        if (it->flags & I_BROKEN) { wo_strcpy(b, "broke it: ", 80); wo_strcat(b, it->why, 80); lv_add(i, 48, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK)); }
    }
    chrome("TICKET", " ", "SELECT move  GLO terms");
    lv_draw(TOP.sel);
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
    wo_strcpy(b, mn, 80); wo_strcat(b, ": ", 80); wo_catu(b, (uint32_t)n, 80); wo_strcat(b, " | best ", 80);
    wo_catu(b, best, 80); wo_strcat(b, " | total ", 80); wo_catu(b, total, 80);
    lv_note(b, C(TC_CYAN));
    for (i = 0; i < n; i++) {
        const wo_score_t *s = &W.score[ord[i]];
        b[0] = 0;
        wo_catu(b, s->score, 80);
        while (wo_strlen(b) < 6) wo_strcat(b, " ", 80);
        mode_name(s->mode, mn, 32); mn[12] = 0; wo_strcat(b, mn, 80);
        wo_strcat(b, " #", 80); wo_catu(b, s->id, 80);
        lv_add(i, 8, 0, 0, "", b, i == 0 ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i) {
            wo_strcpy(b, RATES[s->rating & 3], 80); wo_strcat(b, "  ", 80); wo_catu(b, s->done, 80); wo_strcat(b, "/", 80); wo_catu(b, s->total, 80);
            wo_strcat(b, " ticked  ", 80); wo_hhmmss(tb2, s->ms, 24); wo_strcat(b, tb2, 80);
            lv_add(i, 24, 0, 0, "", b, C(TC_DIM), C(TC_INK));
            b[0] = 0;
            if (s->run_total) { wo_strcat(b, s->run_won ? "run won  " : "run lost at round ", 80); if (!s->run_won) { wo_catu(b, s->run_reached, 80); wo_strcat(b, "  ", 80); } }
            if (s->burst) wo_strcat(b, "ticked in a burst  ", 80);
            if (s->pauses) { wo_catu(b, s->pauses, 80); wo_strcat(b, " pauses", 80); }
            if (b[0]) lv_add(i, 24, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK));
            lv_add(i, 24, 0, 0, "", s->name, C(TC_DIMMER), C(TC_INK));
        }
    }
    chrome("SCORES", " ", "ALGO mode  FX clear");
    lv_draw(TOP.sel);
}
static void scores_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_ENC && (id == HB_ENC_ALGORITHM || id == HB_ENC_KNOB1)) {
        int a = TOP.arg + (arg > 0 ? 1 : -1);
        TOP.arg = (int16_t)((a + WO_NMODES + 1) % (WO_NMODES + 1));
        TOP.sel = 0; TOP.scroll = 0;
    } else if (kind == HB_EV_BTN_DOWN && id == HB_BTN_FX && confirm(300)) {
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
        wo_strcat(b, " off, ", 80); wo_catu(b, (uint32_t)yours, 80); wo_strcat(b, " yours", 80);
        lv_note(b, C(TC_DIM));
    }
    for (i = 0; i < nrule_ids; i++) {
        int id = rule_ids[i];
        if (id >= 10000) {
            lv_add(i, 16, TIERCH[rtier], tier_col(rtier), "+ ", W.custom[id - 10000].text, C(TC_CYAN), C(TC_INK));
        } else {
            const wo_entry_t *e = &WO_ENTRIES[id];
            int dis = wo_bit(W.disabled, (uint32_t)id);
            char pre[28] = "";
            wo_str(e->text, tb, WO_LINE);
            if (mode >= 0) { wo_pool_name(e->pool, pl, 24); wo_strcat(pre, pl, 28); wo_strcat(pre, ": ", 28); }
            else if (e->mode != NONE8) { mode_name(e->mode, pl, 24); pl[10] = 0; wo_strcat(pre, pl, 28); wo_strcat(pre, ": ", 28); }
            if (e->genre != NONE8) { wo_str(WO_GENRE[e->genre], pl, 24); wo_strcat(pre, pl, 28); wo_strcat(pre, ": ", 28); }
            if (dis) wo_strcat(pre, "off ", 28);
            lv_add(i, 16, TIERCH[e->tier], dis ? C(TC_DIMMER) : tier_col(e->tier), pre, tb, dis ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        }
    }
    if (mode >= 0) chrome("MODE RULES", " ", "EDIT on/off  GLO terms");
    else {
        char t[40];
        wo_pool_name(rpool, t, 24);
        { int k; for (k = 0; t[k]; k++) if (t[k] >= 'a' && t[k] <= 'z') t[k] = (char)(t[k] - 32); }
        wo_strcat(t, " ", 40); wo_strcat(t, wo_tier_name(rtier), 40);
        chrome(t, " ", "ALGO pool KNOB2 sev REC add");
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
    if (kind != HB_EV_BTN_DOWN) return;
    rid = TOP.sel < nrule_ids ? rule_ids[TOP.sel] : -1;
    switch (id) {
    case HB_BTN_EDIT:
        if (rid >= 0 && rid < 10000) { wo_setbit(W.disabled, (uint32_t)rid, !wo_bit(W.disabled, (uint32_t)rid)); wo_store_mark(ST_STATE); }
        else if (rid >= 10000) toast("Yours: FX deletes it");
        break;
    case HB_BTN_REC:
        if (mode >= 0) break;
        if (W.ncustom >= WO_MAXCUSTOM) { toast("48 rules of your own is the most"); break; }
        text_begin(TX_CUSTOM_NEW, rpool, rtier, "YOUR RULE ({MAT} etc. work)", "", WO_CUSTOMLEN);
        break;
    case HB_BTN_SEL:
        if (rid >= 10000) text_begin(TX_CUSTOM_EDIT, rid - 10000, 0, "EDIT YOUR RULE", W.custom[rid - 10000].text, WO_CUSTOMLEN);
        else toast("Stock rules switch on and off (EDIT)");
        break;
    case HB_BTN_FX:
        if (rid >= 10000 && confirm(400 + rid)) {
            int k = rid - 10000;
            for (; k < W.ncustom - 1; k++) W.custom[k] = W.custom[k + 1];
            W.ncustom--;
            wo_store_mark(ST_CUSTOM); toast("Deleted");
        }
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
static void draw_gear(void)
{
    char b[64], rl[40];
    int i, on = 0;
    for (i = 0; i < W.ninv; i++) on += wo_inv_on((uint8_t)i);
    lv_reset();
    wo_strcpy(b, "", 64); wo_catu(b, (uint32_t)on, 64); wo_strcat(b, " of ", 64); wo_catu(b, W.ninv, 64);
    wo_strcat(b, " on. SEQ: what each category feeds.", 64);
    lv_note(b, C(TC_DIM));
    for (i = 0; i < W.ninv; i++) {
        int l;
        wo_strcpy(b, W.inv[i].name, 64);
        role_of(W.inv[i].roles, rl, 40);
        l = 28 - wo_strlen(rl) - 1;
        if (l < 4) l = 4;
        while (wo_strlen(b) < l) wo_strcat(b, " ", 64);
        b[l] = 0;
        wo_strcat(b, " ", 64); wo_strcat(b, rl, 64);
        lv_nowrap = 1;
        lv_add(i, 8, 0, 0, W.inv[i].on ? "" : "", b, W.inv[i].on ? C(TC_TEXT) : C(TC_DIMMER), C(TC_INK));
    }
    chrome("GEAR", " ", "EDIT on/off ALGO role SCL name");
    lv_draw(TOP.sel);
}
static void gear_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int i = TOP.sel;
    if (kind == HB_EV_ENC && id == HB_ENC_ALGORITHM && i < W.ninv) {
        int r = 0, k;
        for (k = 0; k < WO_NROLES; k++) if (W.inv[i].roles >> k & 1u) { r = k; break; }
        r = (r + (arg > 0 ? 1 : WO_NROLES - 1)) % WO_NROLES;
        W.inv[i].roles = (uint8_t)(1u << r);
        wo_store_mark(ST_STATE);
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_EDIT: if (i < W.ninv) { W.inv[i].on = !W.inv[i].on; wo_store_mark(ST_STATE); } break;
    case HB_BTN_SEL: if (i < W.ninv) text_begin(TX_INV_RENAME, i, 0, "RENAME", W.inv[i].name, WO_NAMELEN); break;
    case HB_BTN_REC:
        if (W.ninv >= WO_MAXINV) { toast("64 items is the most"); break; }
        text_begin(TX_INV_NEW, 0, 0, "NEW GEAR (ALGO sets its role)", "", WO_NAMELEN);
        break;
    case HB_BTN_FX:
        if (i < W.ninv && confirm(500 + i)) {
            int k;
            if (W.has_draft) W.has_draft = 0;          /* the draft names gear by number */
            for (k = i; k < W.ninv - 1; k++) W.inv[k] = W.inv[k + 1];
            W.ninv--;
            for (k = i; k < WO_MAXINV - 1; k++) W.weight[WK_ITEM + k] = W.weight[WK_ITEM + k + 1];
            wo_store_mark(ST_STATE | ST_DRAFT); toast("Deleted");
        }
        break;
    case HB_BTN_SEQ: push(PG_BUCKETS, 0); break;
    default: break;
    }
}
static int bucket_cur;
static void draw_buckets(void)
{
    char b[80], fn[24];
    int r, f;
    lv_reset();
    lv_note("Which parameters each category of gear can be named in.", C(TC_DIM));
    for (r = 0; r < WO_NROLES; r++) {
        wo_str(WO_ROLE[r], b, 80);
        wo_strcat(b, ": ", 80);
        if (TOP.sel != r) {
            int any = 0;
            for (f = 0; f < WO_NFIELDS; f++) if (W.bucket[r] >> f & 1u) { if (any) wo_strcat(b, ", ", 80); wo_field_name((uint8_t)f, fn, 24); wo_strcat(b, fn, 80); any = 1; }
            if (!any) wo_strcat(b, "nothing", 80);
            lv_add(r, 8, 0, 0, "", b, C(TC_TEXT), C(TC_INK));
        } else {
            lv_add(r, 8, 0, 0, "", b, C(TC_SIGNAL), C(TC_INK));
            for (f = 0; f < WO_NFIELDS; f++) {
                if (f == F_KEY || f == F_TEMPO) continue;
                wo_field_name((uint8_t)f, fn, 24);
                wo_strcpy(b, (W.bucket[r] >> f & 1u) ? "[x] " : "[ ] ", 80); wo_strcat(b, fn, 80);
                lv_add(r, 24, 0, 0, bucket_cur == f ? "> " : "  ", b, bucket_cur == f ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
            }
        }
    }
    chrome("CATEGORIES", " ", "KNOB1 param  EDIT feeds it");
    lv_draw(TOP.sel);
}
static void buckets_event(uint32_t kind, uint32_t id, int32_t arg)
{
    if (kind == HB_EV_ENC && id == HB_ENC_KNOB1) {
        do bucket_cur = (bucket_cur + (arg > 0 ? 1 : WO_NFIELDS - 1)) % WO_NFIELDS; while (bucket_cur == F_KEY || bucket_cur == F_TEMPO);
    } else if (kind == HB_EV_BTN_DOWN && id == HB_BTN_EDIT && TOP.sel < WO_NROLES) {
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
        { int k; for (k = 0; b[k]; k++) if (b[k] >= 'a' && b[k] <= 'z') b[k] = (char)(b[k] - 32); }
        lv_add(-1, 4, 0, 0, "", b, C(TC_CYAN), C(TC_INK));
        for (i = WO_MAT_START[c]; i < WO_MAT_START[c + 1]; i++) {
            int off = wo_bit(W.matoff, (uint32_t)i);
            wo_str(WO_MAT[i], b, 64);
            mat_ids[nmat_ids] = (int16_t)i; mat_cat[nmat_ids] = (uint8_t)c;
            lv_add(nmat_ids++, 16, 0, 0, off ? "off " : "", b, off ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        }
        for (i = 0; i < W.nmatadd; i++) if (W.matadd[i].cat == c) {
            mat_ids[nmat_ids] = (int16_t)(1000 + i); mat_cat[nmat_ids] = (uint8_t)c;
            lv_add(nmat_ids++, 16, 0, 0, "+ ", W.matadd[i].text, C(TC_CYAN), C(TC_INK));
        }
    }
    chrome("MATERIALS", " ", "EDIT on/off REC add FX del");
    lv_draw(TOP.sel);
}
static void mats_event(uint32_t kind, uint32_t id, int32_t arg)
{
    int m;
    (void)arg;
    if (kind != HB_EV_BTN_DOWN || TOP.sel >= nmat_ids) return;
    m = mat_ids[TOP.sel];
    if (id == HB_BTN_EDIT && m < 1000) { wo_setbit(W.matoff, (uint32_t)m, !wo_bit(W.matoff, (uint32_t)m)); wo_store_mark(ST_STATE); }
    else if (id == HB_BTN_REC) {
        if (W.nmatadd >= WO_MAXMATADD) toast("32 of your own is the most");
        else text_begin(TX_MAT_NEW, mat_cat[TOP.sel], 0, "A SOUND TO SAMPLE", "", WO_MATLEN);
    } else if (id == HB_BTN_FX && m >= 1000 && confirm(600 + m)) {
        int k;
        for (k = m - 1000; k < W.nmatadd - 1; k++) W.matadd[k] = W.matadd[k + 1];
        W.nmatadd--; wo_store_mark(ST_CUSTOM); toast("Deleted");
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
        lv_add(i, 8, 0, 0, "", tb2, TOP.sel == i ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == i) { wo_str(WO_BUILTIN[i].desc, tb, WO_LINE); lv_add(i, 24, 0, 0, "", tb, C(TC_DIM), C(TC_INK)); }
    }
    chrome("PRESETS", " ", "SCL use it");
    lv_draw(TOP.sel);
}
static void presets_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL && TOP.sel < WO_NBUILTIN) {
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
                                       "Target time", "Random: min", "Random: max", "Random: sev", "Random: chaos",
                                       "Random: holds", "Random: anchor", "Sound", "Volume", "Reset learning",
                                       "Reset everything", "About"};
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
    case S_RESETW: case S_WIPE: wo_strcpy(o, "SCL x2", cap); break;
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
        l = 28 - wo_strlen(v);
        while (wo_strlen(b) < l) wo_strcat(b, " ", 64);
        wo_strcat(b, v, 64);
        lv_add(s, 8, 0, 0, "", b, s >= S_RESETW && s <= S_WIPE ? C(TC_MAGENTA) : C(TC_TEXT), C(TC_INK));
        if (TOP.sel == s && s == S_ABOUT) {
            wo_strcpy(b, "", 64); wo_catu(b, WO_NENTRIES, 64); wo_strcat(b, " rules, ", 64); wo_catu(b, WO_NGLOSS, 64);
            wo_strcat(b, " glossary terms, ", 64); wo_catu(b, WO_NMODES, 64); wo_strcat(b, " modes. From workorder.html.", 64);
            lv_add(s, 24, 0, 0, "", b, C(TC_DIM), C(TC_INK));
        }
    }
    chrome("SETTINGS", " ", "ALGO change  SCL toggle");
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
    case S_RSEV: { static const uint8_t R[6] = {0, MSEV_ESCALATE, MSEV_GENTLE, MSEV_GENTLE + 1, MSEV_GENTLE + 2, 0}; int k = 0, j;
                   for (j = 0; j < 5; j++) if (R[j] == W.set.rsev) k = j; k = (k + d + 5) % 5; W.set.rsev = R[k]; break; }
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
    if (kind != HB_EV_BTN_DOWN || id != HB_BTN_SEL) return;
    if (TOP.sel == S_RESETW) { if (confirm(700)) { wo_reset_weights(); wo_store_mark(ST_STATE); toast("Learning reset"); } }
    else if (TOP.sel == S_WIPE) { if (confirm(701)) { wo_synth_stop(); wo_store_wipe(); go_root(PG_HOME); toast("Everything reset"); } }
    else settings_change(TOP.sel, 1);
}

/* ================================================================ HELP */
static const char *const HELP[] = {
    "Everywhere", "HOME goes back. Hold it to leave WORKORDER. SELECT moves, SCL opens. GLO explains the terms in a line. PLAY loops the rolled chords. The keys play the rolled scale; OCT-/OCT+ shift them.",
    "Roll", "ALGO severity, KNOB1 lines, KNOB2 mixed severity. EDIT switches a parameter on/off, SAVE holds it. REC rolls all, ARP rolls one, SEQ mutates, FX rolls at random. LFO issues the ticket.",
    "Session", "SCL ticks a rule off, REC breaks it (say why), ARP unbreaks it, FX retires a rule from the pool for good. LFO pauses the clock, SAVE parks the ticket, EDIT closes and files it.",
    "The Run", "Each round deals cards: SCL keeps one, ARP deals again. Tick it off to reach the next round. A broken brutal card costs two lives.",
    "Typing", "White keys are A to P, black keys Q to Z, the top black key is a full stop. OCT- deletes, OCT+ is a space, EDIT capitals, KNOB1 and FX add a sign. SCL finishes.",
    "Scores", "Points drop the longer a ticket takes. Brutal counts 3.5x, firm 2x. Modes multiply. Keeper tickets teach the next rolls.",
};
static void draw_help(void)
{
    int i;
    lv_reset();
    for (i = 0; i < (int)(sizeof HELP / sizeof HELP[0]); i += 2) {
        lv_add(i / 2, 4, 0, 0, "", HELP[i], C(TC_SIGNAL), C(TC_INK));
        lv_add(i / 2, 12, 0, 0, "", HELP[i + 1], C(TC_TEXT), C(TC_INK));
    }
    chrome("HELP", " ", "SELECT scroll");
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
        TOP.sel = (int16_t)(W.ninv - 1);
        wo_store_mark(ST_STATE); toast("Added. ALGO sets its role");
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
static void home_event(uint32_t kind, uint32_t id, int32_t arg) { (void)arg; if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) open_home(TOP.sel); }
static void view_event(uint32_t kind, uint32_t id, int32_t arg)
{
    const wo_ticket_t *t = lib_ticket(TOP.arg);
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_GLO && t && TOP.sel < t->nitems) gloss_open(t->item[TOP.sel].text, item_skey(&t->item[TOP.sel]));
}

static const draw_fn DRAW[] = {draw_home, draw_roll, draw_field, draw_session, draw_rate, draw_modes, draw_parked,
                               draw_library, draw_view, draw_scores, draw_rules, draw_gear, draw_buckets, draw_mats,
                               draw_presets, draw_settings, draw_help, draw_gloss, draw_text_page, draw_help};
static const event_fn EVENT[] = {home_event, roll_event, field_event, session_event, rate_event, modes_event,
                                 parked_event, library_event, view_event, scores_event, rules_event, gear_event,
                                 buckets_event, mats_event, presets_event, settings_event, ev_none, ev_none, ev_none, ev_none};

/* button LEDs: what does something on this page */
static uint32_t page_buttons(uint8_t p)
{
#define B(x) (1u << HB_BTN_##x)
    switch (p) {
    case PG_ROLL: return B(REC) | B(ARP) | B(SEQ) | B(FX) | B(LFO) | B(SEL) | B(EDIT) | B(SAVE) | B(GLO);
    case PG_FIELD: return B(REC) | B(ARP) | B(EDIT) | B(SAVE) | B(GLO) | B(LFO);
    case PG_SESSION: return W.has_cur ? B(SEL) | B(REC) | B(ARP) | B(FX) | B(LFO) | B(SAVE) | B(EDIT) | B(GLO) : 0;
    case PG_TEXT: return B(SEL) | B(EDIT) | B(FX) | B(OCT_DOWN) | B(OCT_UP);
    case PG_RULES: return B(EDIT) | B(REC) | B(SEL) | B(FX) | B(GLO);
    case PG_GEAR: return B(EDIT) | B(SEL) | B(REC) | B(FX) | B(SEQ);
    default: return B(SEL);
    }
#undef B
}

static uint32_t leds_shown = 0xFFFFFFFFu; static uint32_t keys_shown = 0xFFFFFFFFu;
static void leds(void)
{
    uint32_t b = page_buttons(TOP.page), k = 0, i;
    if (wo_synth_playing()) b |= 1u << HB_BTN_PLAY;
    if (TOP.page == PG_TEXT) k = 0x7FFFFFFu;
    else {
        sync_scale();
        for (i = 0; i < 27; i++) if (wo_synth_in_scale((int)i)) k |= 1u << i;
    }
    if (b != leds_shown) {
        for (i = 0; i < 14; i++) H->led(i, (b >> i & 1u) ? ((i == HB_BTN_PLAY && wo_synth_playing()) ? 1u : 2u) : 0u);
        leds_shown = b;
    }
    if (k != keys_shown) { for (i = 0; i < 27; i++) H->led(HB_LED_KEY(i), (k >> i & 1u) ? 2u : 0u); keys_shown = k; }
}

/* ================================================================ the module's calls */
void wo_ui_init(const hb_host_t *h)
{
    H = h;
    depth = 0; st[0].page = PG_HOME; st[0].sel = W.has_cur ? 1 : 0; st[0].scroll = 0;
    need_full = 1; leds_shown = keys_shown = 0xFFFFFFFFu;
    wo_synth_volume(W.set.volume);
    sync_scale();
}

void wo_ui_event(uint32_t ev, int32_t arg)
{
    uint32_t kind = HB_EV_KIND(ev), id = HB_EV_ID(ev);
    uint8_t p = TOP.page;
    need_full = 1;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_HOME) { home_down = H->ms() | 1u; return; }
    if (kind == HB_EV_BTN_UP && id == HB_BTN_HOME) {
        if (home_down && H->ms() - home_down < 1200u) { if (p == PG_TEXT) pop(); else if (depth) pop(); }
        home_down = 0;
        return;
    }
    if (p == PG_TEXT) { text_event(kind, id, arg); return; }
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
        if (id == HB_BTN_PLAY) { if (W.set.sound) play_toggle(); else toast("Sound is off (Settings)"); return; }
        if (id == HB_BTN_OCT_DOWN) { wo_synth_octave(-1); toast("Keys an octave down"); return; }
        if (id == HB_BTN_OCT_UP) { wo_synth_octave(1); toast("Keys an octave up"); return; }
    }
    EVENT[p](kind, id, arg);
    if (TOP.page != p) need_full = 1;
}

/* the main loop's: redraw what changed, keep the clock, save when quiet */
void wo_ui_draw(uint32_t full)
{
    uint32_t t = H->ms();
    wo_synth_poll(t);
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
        for (i = 0; i < nops; i++) if (ops[i].y < 18) ops[keep++] = ops[i];
        nops = keep;
        flush_rows(0, 18);
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
