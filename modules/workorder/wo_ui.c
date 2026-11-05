/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1's screens: 240 x 240 in a 6 x 10 font (40 columns, 19 rows of content), drawn into
 * 240 x 80 strips. The bottom line names the page's three main buttons; GLO opens a menu of
 * everything the page can do, in groups, each with its shortcut. Nothing needs a held button:
 * what can't be undone is in the menu and asks first (SCL yes, HOME no).
 *
 * One kind of action per button, on every page:
 *   SCL open / tick / confirm      HOME back                 GLO the menu
 *   PLAY start the ticket (no ticket: roll and start in one press), pause / resume
 *   REC  make new: roll, deal again, add      ARP  reroll the selected one    SEQ  mutate
 *   LFO  random                               EDIT on / off, edit             SAVE hold, park
 *   FX   break / unbreak                      ENV  play / stop the sketch */
#include "hb_module.h"
#include "wo.h"
#include "wo_store.h"
#include "wo_synth.h"
#include "wo_seq.h"
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
#define NROWS 19                /* 14 + 19 * 11 = 223 */
#define FOOT_Y 228              /* the bottom line, its rule at 227 */
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
       PG_RULES, PG_GEAR, PG_BUCKETS, PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP, PG_GLOSS, PG_TEXT, PG_SEQ, PG_SOUND, PG_N };
typedef struct { uint8_t page; int16_t sel; int16_t scroll; int16_t arg; } frame_t;
static frame_t st[10];
static int depth;
#define TOP (st[depth])

static char toast_s[48];
static uint32_t toast_until;
static int need_full = 1;
static uint32_t last_clock_s = 0xFFFFFFFFu;

#define EV_HOLD 0x0A000000u    /* a page's "for good" action (retire, delete, discard): from the menu, confirmed */

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

/* ================================================================ the bottom line and the menu */
/* key_tap: what each button does here (index = button id - 2), for the LEDs */
static const char *key_tap[12];
static void keys_clear(void);
static void K(int btn, const char *tap, const char *hold) { (void)hold; if (btn >= 2 && btn < 14) key_tap[btn - 2] = tap; }

static const char *const BTN_NAME[14] = {"OCT-", "OCT+", "FX", "SCL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE",
                                         "ARP", "SEQ", "PLAY", "REC"};
/* the bottom line: up to three of the page's buttons */
static uint8_t foot_btn[3]; static const char *foot_lab[3]; static int nfoot;
static void F(int btn, const char *lab) { if (nfoot < 3) { foot_btn[nfoot] = (uint8_t)btn; foot_lab[nfoot++] = lab; } }

/* the menu: the page's actions in groups. An item runs a button (as if pressed) or its own action */
enum { A_NONE, A_FINISH, A_PARK, A_FORGOOD, A_RESETW, A_WIPE, A_HOMEPAGE, A_HELP, A_GLOSS, A_SKETCH,
       A_SQ_CLEARROW, A_SQ_CLEARPAT, A_SQ_COPY, A_SQ_CHADD, A_SQ_CHDROP, A_SQ_CHMODE, A_SQ_CHCLEAR, A_SQ_CLOCK };
typedef struct { const char *group, *label; uint8_t btn, act, confirm; } mi_t;
#define MAXMI 26
static mi_t mi[MAXMI]; static int nmi;
static int menu_open, menu_sel, conf_open;
static mi_t conf_item;
static void MI(const char *group, const char *label, int btn, int act, int confirm)
{
    if (nmi >= MAXMI) return;
    mi[nmi].group = group; mi[nmi].label = label; mi[nmi].btn = (uint8_t)btn; mi[nmi].act = (uint8_t)act;
    mi[nmi].confirm = (uint8_t)confirm; nmi++;
}
static void keys_clear(void) { int i; for (i = 0; i < 12; i++) key_tap[i] = 0; nfoot = 0; nmi = 0; }

static int page_has_menu(void);
/* "KEY label" pairs, two spaces apart when they fit, one when it's tight; GLO menu on the right */
static void draw_footer(void)
{
    int i, x = 3, len = 0, gap, right = page_has_menu() ? 9 : 0, room = COLS - 1 - right;
    d_rect(0, FOOT_Y - 1, 240, 1, C(TC_RULE2));
    d_rect(0, FOOT_Y, 240, 240 - FOOT_Y, C(TC_INK2));
    for (i = 0; i < nfoot; i++) len += wo_strlen(BTN_NAME[foot_btn[i]]) + 1 + wo_strlen(foot_lab[i]);
    gap = len + 2 * (nfoot - 1) <= room ? 2 : 1;
    for (i = 0; i < nfoot; i++) {
        int need = wo_strlen(BTN_NAME[foot_btn[i]]) + 1 + wo_strlen(foot_lab[i]);
        if ((x - 3) / CW + need > room) break;
        d_text(x, FOOT_Y + 1, BTN_NAME[foot_btn[i]], C(TC_SIGNAL));
        x += (wo_strlen(BTN_NAME[foot_btn[i]]) + 1) * CW;
        d_text(x, FOOT_Y + 1, foot_lab[i], C(TC_TEXT));
        x += (wo_strlen(foot_lab[i]) + gap) * CW;
    }
    if (right) { d_text_r(237, FOOT_Y + 1, "menu", C(TC_TEXT)); d_text_r(237 - 5 * CW, FOOT_Y + 1, "GLO", C(TC_SIGNAL)); }
}

/* the menu as a box over the page; the confirmation as a smaller one */
static void draw_menu(void)
{
    int i, y, rows = 0, x0 = 10, w = 220, h, top, sel_row = 0, first = 0, shown;
    const char *g = 0;
    if (!menu_open || conf_open) goto confirm;
    for (i = 0; i < nmi; i++) { if (!g || !wo_streq(g, mi[i].group)) { rows++; g = mi[i].group; } if (i == menu_sel) sel_row = rows; rows++; }
    shown = rows < 17 ? rows : 17;
    if (sel_row >= shown) first = sel_row - shown + 1;
    h = (shown + 1) * ROWH + 6;
    top = (Y0 + FOOT_Y - h) / 2;
    d_rect(x0 - 1, top - 1, w + 2, h + 2, C(TC_SIGNAL));
    d_rect(x0, top, w, h, C(TC_INK));
    d_rect(x0, top, w, ROWH + 1, C(TC_INK2));
    d_text(x0 + 4, top + 1, "MENU", C(TC_SIGNAL));
    d_text_r(x0 + w - 4, top + 1, "SCL runs  HOME closes", C(TC_DIM));
    y = top + ROWH + 3;
    g = 0; rows = 0;
    for (i = 0; i < nmi; i++) {
        if (!g || !wo_streq(g, mi[i].group)) {
            g = mi[i].group;
            if (rows >= first && rows < first + shown) { d_text(x0 + 4, y, g, C(TC_CYAN)); y += ROWH; }
            rows++;
        }
        if (rows >= first && rows < first + shown) {
            int on = i == menu_sel;
            if (on) { d_rect(x0, y - 1, w, ROWH, C(TC_INK3)); d_rect(x0, y - 1, 2, ROWH, C(TC_SIGNAL)); }
            d_text(x0 + 16, y, mi[i].label, on ? C(TC_SIGNAL) : C(TC_TEXT));
            if (mi[i].btn) d_text_r(x0 + w - 4, y, BTN_NAME[mi[i].btn], C(TC_DIM));
            else if (mi[i].confirm) d_text_r(x0 + w - 4, y, "asks", C(TC_DIMMER));
            y += ROWH;
        }
        rows++;
    }
confirm:
    if (conf_open) {
        int ch = 4 * ROWH, ct = (Y0 + FOOT_Y - ch) / 2;
        d_rect(29, ct - 1, 182, ch + 2, C(TC_MAGENTA));
        d_rect(30, ct, 180, ch, C(TC_INK2));
        d_text(36, ct + 4, conf_item.label, C(TC_TEXT));
        d_text(36, ct + 4 + ROWH, "Sure? This can't be undone.", C(TC_DIM));
        d_text(36, ct + 4 + 2 * ROWH, "SCL", C(TC_SIGNAL)); d_text(36 + 4 * CW, ct + 4 + 2 * ROWH, "yes", C(TC_TEXT));
        d_text(36 + 9 * CW, ct + 4 + 2 * ROWH, "HOME", C(TC_SIGNAL)); d_text(36 + 14 * CW, ct + 4 + 2 * ROWH, "no", C(TC_TEXT));
    }
}

/* ================================================================ chrome */
static void menu_common(void);
static void chrome(const char *title, const char *right)
{
    char clk[16];
    if (page_has_menu()) menu_common();
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
    draw_footer();
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
    K(HB_BTN_SEL, "done", 0); K(HB_BTN_HOME, "cancel", 0); K(HB_BTN_EDIT, "caps", 0); K(HB_BTN_FX, "sign", 0);
    { static char sym[12]; sym[0] = 's'; sym[1] = 'i'; sym[2] = 'g'; sym[3] = 'n'; sym[4] = ' '; sym[5] = SYMS[tx.sym]; sym[6] = 0;
      F(HB_BTN_SEL, "done"); F(HB_BTN_FX, sym); F(HB_BTN_EDIT, tx.shift ? "CAPS" : "caps"); }
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
    keys_clear(); K(HB_BTN_HOME, "back", 0); F(HB_BTN_HOME, "back");
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

static void sketch_toggle(void);

static void mode_name(uint8_t m, char *o, int cap) { wo_str(WO_MODES[m < WO_NMODES ? m : 0].name, o, cap); }

/* the keys every page shares: SCL, HOME, ENV, PLAY (set before the page adds its own) */
static void keys_common(void)
{
    keys_clear();
    if (depth) K(HB_BTN_HOME, "back", 0);
    K(HB_BTN_ENV, "sketch", 0);
    K(HB_BTN_PLAY, "play", 0);
    K(HB_BTN_GLO, "menu", 0);
}
/* the items every menu ends with */
static void menu_common(void)
{
    if (W.has_cur) {
        MI("Ticket", W.cur.running ? "Pause the clock" : "Resume the clock", HB_BTN_PLAY, 0, 0);
        MI("Ticket", "Finish and rate it", 0, A_FINISH, 0);
        MI("Ticket", "Park it for later", 0, A_PARK, 0);
        if (TOP.page != PG_SEQ && TOP.page != PG_SOUND) MI("Ticket", "Open its sketch", 0, A_SKETCH, 0);
    } else MI("Ticket", "Roll and start a ticket", HB_BTN_PLAY, 0, 0);
    MI("Sound", wo_seq_playing() ? "Stop the sketch" : "Play the sketch", HB_BTN_ENV, 0, 0);
    if (depth) MI("Go", "Home", 0, A_HOMEPAGE, 0);
    MI("Go", "Help", 0, A_HELP, 0);
}

/* PLAY with no ticket: roll what is set up and start the clock, in one press */
static void quick_start(void)
{
    wo_seq_stop();
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
/* grouped: what you play, what you kept, how it is set up */
static const char *const HOME_ITEMS[] = {"Session", "Sketch", "Roll", "Modes", "Parked", "Library", "Scores", "Rules",
                                         "Gear", "Materials", "Presets", "Settings", "Help"};
static const uint8_t HOME_PAGE[] = {PG_SESSION, PG_SEQ, PG_ROLL, PG_MODES, PG_PARKED, PG_LIBRARY, PG_SCORES, PG_RULES,
                                    PG_GEAR, PG_MATS, PG_PRESETS, PG_SETTINGS, PG_HELP};
#define NHOME 13
static void draw_home(void)
{
    char sub[48];
    int i;
    lv_reset();
    for (i = 0; i < NHOME; i++) {
        int on = 0, k;
        char row[COLS + 1];
        if (i == 0) lv_add(-1, 3, 0, 0, "", "PLAY", C(TC_CYAN), C(TC_INK));
        if (i == 5) { lv_note(" ", C(TC_INK)); lv_add(-1, 3, 0, 0, "", "LIBRARY", C(TC_CYAN), C(TC_INK)); }
        if (i == 7) { lv_note(" ", C(TC_INK)); lv_add(-1, 3, 0, 0, "", "SETUP", C(TC_CYAN), C(TC_INK)); }
        sub[0] = 0;
        switch (i) {
        case 0:
            if (W.has_cur) {
                int d = 0;
                for (k = 0; k < W.cur.nitems; k++) d += !!(W.cur.item[k].flags & I_CHECKED);
                wo_catu(sub, wo_ticket_score(&W.cur), 48); wo_strcat(sub, " pts ", 48);
                wo_catu(sub, (uint32_t)d, 48); wo_strcat(sub, "/", 48); wo_catu(sub, W.cur.nitems, 48);
                wo_strcat(sub, W.cur.running ? " running" : " paused", 48);
            } else wo_strcpy(sub, "none: PLAY starts one", 48);
            break;
        case 1:
            if (W.has_cur && W.cur.song.div) {
                for (k = 0; k < SQ_PATS; k++) on += !wo_seq_pat_empty(&W.cur.song, k);
                wo_catu(sub, (uint32_t)on, 48); wo_strcat(sub, on == 1 ? " pattern" : " patterns", 48);
            } else wo_strcpy(sub, W.has_cur ? "drums, bass, chords, lead" : "with a ticket", 48);
            if (wo_seq_playing()) wo_strcat(sub, ", playing", 48);
            break;
        case 2: for (k = 0; k < WO_NFIELDS; k++) on += W.on[k];
                wo_catu(sub, (uint32_t)on, 48); wo_strcat(sub, " params", 48);
                if (wo_draft_ready()) wo_strcat(sub, ", rolled", 48);
                break;
        case 3: mode_name(W.set.mode, sub, 30); break;
        case 4: wo_catu(sub, (uint32_t)wo_parked_count(), 48); wo_strcat(sub, " of 2", 48); break;
        case 5: wo_catu(sub, (uint32_t)wo_lib_count(), 48); wo_strcat(sub, " filed", 48); break;
        case 6: wo_catu(sub, W.nscores, 48); wo_strcat(sub, " scored", 48); break;
        case 7: wo_catu(sub, WO_NENTRIES + W.ncustom, 48); wo_strcat(sub, " rules", 48); break;
        case 8: for (k = 0; k < W.ninv; k++) on += wo_inv_on((uint8_t)k); wo_catu(sub, (uint32_t)on, 48); wo_strcat(sub, " on", 48); break;
        case 9: wo_catu(sub, WO_NMAT + W.nmatadd, 48); wo_strcat(sub, " sounds", 48); break;
        case 10: wo_catu(sub, WO_NBUILTIN, 48); wo_strcat(sub, " setups", 48); break;
        default: break;
        }
        {
            int l, pad;
            wo_strcpy(row, HOME_ITEMS[i], COLS + 1);
            l = wo_strlen(row); pad = 36 - l - wo_strlen(sub);
            while (pad-- > 0 && l < COLS) row[l++] = ' ';
            row[l] = 0;
            wo_strcat(row, sub, COLS + 1);
            lv_add(i, 9, 0, 0, "", row, i == 0 && W.has_cur ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
        }
    }
    keys_common();
    K(HB_BTN_SEL, "open", 0);
    F(HB_BTN_SEL, "open"); F(HB_BTN_PLAY, W.has_cur ? (W.cur.running ? "pause" : "resume") : "start");
    MI("Go", "Open", HB_BTN_SEL, 0, 0);
    chrome("WORKORDER", WO_BUILD " FM-1");
    lv_draw(TOP.sel);
}
static void home_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL && TOP.sel >= 0 && TOP.sel < NHOME) push(HOME_PAGE[TOP.sel], 0);
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
        K(HB_BTN_SEL, "open", 0); K(HB_BTN_EDIT, "on/off", 0); K(HB_BTN_SAVE, "hold", 0); K(HB_BTN_REC, "roll", 0);
        K(HB_BTN_ARP, "reroll", 0); K(HB_BTN_SEQ, "mutate", 0); K(HB_BTN_LFO, "random", 0);
        F(HB_BTN_REC, "roll"); F(HB_BTN_ARP, "this"); F(HB_BTN_PLAY, W.has_cur ? "pause" : "start");
        MI("Roll", "Roll everything", HB_BTN_REC, 0, 0);
        MI("Roll", "Reroll this parameter", HB_BTN_ARP, 0, 0);
        MI("Roll", "Mutate one or two", HB_BTN_SEQ, 0, 0);
        MI("Roll", "Random: which and how hard", HB_BTN_LFO, 0, 0);
        MI("This parameter", "Open it", HB_BTN_SEL, 0, 0);
        MI("This parameter", W.on[WO_ORDER[TOP.sel >= 0 && TOP.sel < WO_NFIELDS ? TOP.sel : 0]] ? "Switch it off" : "Switch it on", HB_BTN_EDIT, 0, 0);
        MI("This parameter", W.lock[WO_ORDER[TOP.sel >= 0 && TOP.sel < WO_NFIELDS ? TOP.sel : 0]] ? "Release it" : "Hold it (rolls skip it)", HB_BTN_SAVE, 0, 0);
        MI("This parameter", "Explain its terms", 0, A_GLOSS, 0);
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
    case HB_BTN_REC: wo_seq_stop(); wo_roll_all(); wo_store_mark(ST_DRAFT | ST_STATE); toast("Rolled"); break;
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
    K(HB_BTN_EDIT, "on/off", 0); K(HB_BTN_SAVE, "hold", 0); K(HB_BTN_REC, "roll", 0); K(HB_BTN_ARP, "line", 0);
    F(HB_BTN_REC, "roll it"); F(HB_BTN_ARP, (f != F_KEY && f != F_TEMPO && n) ? "line" : "again"); F(HB_BTN_EDIT, W.on[f] ? "off" : "on");
    MI("Roll", "Roll this parameter", HB_BTN_REC, 0, 0);
    if (f != F_KEY && f != F_TEMPO && n) MI("Roll", "Reroll the highlighted line", HB_BTN_ARP, 0, 0);
    MI("This parameter", W.on[f] ? "Switch it off" : "Switch it on", HB_BTN_EDIT, 0, 0);
    MI("This parameter", W.lock[f] ? "Release it" : "Hold it (rolls skip it)", HB_BTN_SAVE, 0, 0);
    MI("This parameter", "Explain its terms", 0, A_GLOSS, 0);
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
        F(HB_BTN_PLAY, "start");
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
            lv_note("GLO, then Finish and rate it, files it.", C(TC_DIM));
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
    if (hand) {
        K(HB_BTN_SEL, "take", 0); K(HB_BTN_REC, "deal", 0);
        F(HB_BTN_SEL, "take"); F(HB_BTN_REC, "deal again"); F(HB_BTN_PLAY, c->running ? "pause" : "resume");
        MI("This round", "Keep this card", HB_BTN_SEL, 0, 0);
        MI("This round", "Deal again", HB_BTN_REC, 0, 0);
        MI("This round", "Explain the card's terms", 0, A_GLOSS, 0);
    } else if (TOP.sel >= 0 && TOP.sel < c->nitems) {
        int br = c->item[TOP.sel].flags & I_BROKEN, ck = c->item[TOP.sel].flags & I_CHECKED;
        K(HB_BTN_SEL, "tick", 0); K(HB_BTN_FX, "break", 0);
        F(HB_BTN_SEL, ck ? "untick" : "tick"); F(HB_BTN_FX, br ? "unbreak" : "break"); F(HB_BTN_PLAY, c->running ? "pause" : "resume");
        MI("This rule", ck ? "Untick it" : "Tick it off", HB_BTN_SEL, 0, 0);
        MI("This rule", br ? "Unbreak it" : "Break it (say why)", HB_BTN_FX, 0, 0);
        if (!br) MI("This rule", "Retire it for good", 0, A_FORGOOD, 1);
        MI("This rule", "Explain its terms", 0, A_GLOSS, 0);
    } else { F(HB_BTN_PLAY, c->running ? "pause" : "resume"); }
    K(HB_BTN_SAVE, "park", 0);
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
        int r;
        wo_seq_stop();
        r = wo_park();
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
    K(HB_BTN_HOME, "back", 0); K(HB_BTN_SEL, "file", 0); K(HB_BTN_EDIT, "note", 0);
    F(HB_BTN_SEL, "file it"); F(HB_BTN_EDIT, "note"); F(HB_BTN_HOME, "not yet");
    chrome("FINISH TICKET", " ");
    lv_draw(TOP.sel);
}
static void rate_event(uint32_t kind, uint32_t id, int32_t arg)
{
    (void)arg;
    if (kind != HB_EV_BTN_DOWN) return;
    if (id == HB_BTN_EDIT) { text_begin(TX_NOTE, 0, 0, "ONE LINE OF OUTCOME", rate_note, WO_WHY); return; }
    if (id == HB_BTN_SEL && TOP.sel >= 0 && TOP.sel < 4) {
        wo_seq_stop();
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
    F(HB_BTN_SEL, "start it"); F(HB_BTN_EDIT, "its rules");
    MI("This mode", "Start it", HB_BTN_SEL, 0, 0);
    MI("This mode", "Its own rules", HB_BTN_EDIT, 0, 0);
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
        wo_seq_stop();
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
    if (n) {
        K(HB_BTN_SEL, "resume", 0);
        F(HB_BTN_SEL, "resume");
        MI("This ticket", "Resume it", HB_BTN_SEL, 0, 0);
        MI("This ticket", "Discard it", 0, A_FORGOOD, 1);
    }
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
        int r;
        wo_seq_stop();
        r = wo_unpark(s);
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
    if (n) {
        K(HB_BTN_SEL, "view", 0); K(HB_BTN_REC, "again", 0);
        F(HB_BTN_SEL, "view"); F(HB_BTN_REC, "play again");
        MI("This ticket", "View it", HB_BTN_SEL, 0, 0);
        MI("This ticket", "Play it again", HB_BTN_REC, 0, 0);
        MI("This ticket", "Delete it", 0, A_FORGOOD, 1);
    }
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
        wo_seq_stop();
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
    if (t->song.div) {
        int p, np = 0;
        for (p = 0; p < SQ_PATS; p++) np += !wo_seq_pat_empty(&t->song, p);
        wo_strcpy(b, "Sketch: ", 80); wo_catu(b, (uint32_t)np, 80); wo_strcat(b, np == 1 ? " pattern. ENV plays it." : " patterns. ENV plays it.", 80);
        lv_note(b, C(TC_CYAN));
    } else lv_note("No notes kept with it (the newest 4 keep theirs). ENV plays its chords.", C(TC_DIMMER));
    for (i = 0; i < t->nitems; i++) {
        const wo_item_t *it = &t->item[i];
        lv_add(i, 4, it->tier < 3 ? TIERCH[it->tier] : 0, tier_col(it->tier),
               (it->flags & I_BROKEN) ? "[/] " : (it->flags & I_CHECKED) ? "[x] " : "[ ] ", it->text,
               (it->flags & I_BROKEN) ? C(TC_DIMMER) : C(TC_TEXT), C(TC_INK));
        if (it->flags & I_BROKEN) { wo_strcpy(b, "broke it: ", 80); wo_strcat(b, it->why, 80); lv_add(i, 37, 0, 0, "", b, C(TC_MAGENTA), C(TC_INK)); }
    }
    F(HB_BTN_HOME, "back");
    MI("This rule", "Explain its terms", 0, A_GLOSS, 0);
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

/* ================================================================ SKETCH: the ticket's patterns */
/* eight drum lanes, bass, chords, lead; one bar a pattern, in the ticket's meter. The keys are the
 * steps of the highlighted row; with REC on they play it and write what you play */
static const char *const ROWNAME[WO_SEQ_ROWS] = {"KICK", "SNARE", "C.HAT", "O.HAT", "CLAP", "LO TOM", "HI TOM", "PERC",
                                                 "BASS", "CHORDS", "LEAD"};
static const char *const TRNAME[TR_N] = {"DRUMS", "BASS", "CHORDS", "LEAD"};
static const char *const ENGNAME[2][ENG_N] = {{"FM", "saw + filter", "square + filter"}, {"FM kit", "analog kit", "analog kit"}};
static const char *const PNAME[2][4] = {{"Ratio", "Depth", "Mod decay", "Decay"}, {"Cutoff", "Resonance", "Filter env", "Decay"}};
static const char *const PSHORT[2][4] = {{"ratio", "depth", "mod", "dec"}, {"cut", "res", "env", "dec"}};
static const char *const RATIOS[10] = {"0.5", "1", "1.5", "2", "3", "4", "5", "6", "7", "8"};
static const char PATCH[] = "ABCDEFGH";
#define SK_GX 42                /* the first step's x */
#define SK_CW 12                /* a step's width */
#define SK_YPAT (Y0 + ROWH)
#define SK_YRUL (Y0 + 2 * ROWH)
#define SK_YGRID (Y0 + 3 * ROWH)
#define SK_YINFO (SK_YGRID + WO_SEQ_ROWS * ROWH + 2)

static struct {
    int pat, cur, rec, shown;   /* the pattern edited, the step cursor, REC, the play position drawn */
    int8_t kpat[27], krow[27], kstep[27];   /* live: where each held key wrote */
    uint8_t knotes[27][6], kn[27];          /* what each held key sounds */
} sk;
static wo_basics_t skb;

static wo_song_t *sk_song(void)
{
    if (!W.has_cur) return 0;
    wo_basics_ticket(&W.cur, &skb);
    wo_song_ready(&W.cur.song, &skb);
    wo_synth_song(&W.cur.song);
    if (sk.pat < 0 || sk.pat >= SQ_PATS) sk.pat = 0;
    if (sk.cur >= wo_seq_pat_len(&W.cur.song, sk.pat)) sk.cur = 0;
    return &W.cur.song;
}
static void sk_changed(void) { W.cur.song.made = 1; wo_store_mark(ST_CUR); }
static int sk_row(void) { return TOP.sel < 0 ? 0 : TOP.sel >= WO_SEQ_ROWS ? WO_SEQ_ROWS - 1 : TOP.sel; }

/* the short form of the basics: "C minor  112 bpm  7/8 into 4/4" */
static void sk_basics_line(char *o, int cap)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char b[48];
    o[0] = 0;
    if (W.cur.has_key) {
        if (W.cur.key.root >= 0) { wo_strcat(o, NN[W.cur.key.root % 12], cap); wo_strcat(o, " ", cap); }
        wo_str(WO_SCALES[W.cur.key.scale].name, b, 48); wo_strcat(o, b, cap);
    } else if (W.cur.has_chords) { wo_strcat(o, NN[W.cur.chords.root % 12], cap); wo_strcat(o, " (chords' key)", cap); }
    else wo_strcat(o, "any key", cap);
    wo_strcat(o, "  ", cap); wo_catu(o, (uint32_t)skb.bpm, cap); wo_strcat(o, " bpm", cap);
    if (W.cur.has_tempo) { wo_strcat(o, "  ", cap); wo_str(WO_METER[W.cur.tempo.tier][W.cur.tempo.meter], b, 48); wo_strcat(o, b, cap); }
}

static void clip_text(int x, int y, const char *s, uint16_t c, int cols)
{
    char b[COLS + 1];
    wo_strcpy(b, s, cols + 1 > COLS + 1 ? COLS + 1 : cols + 1);
    if (wo_strlen(s) > cols && cols > 1) b[cols - 1] = '.';
    d_text(x, y, b, c);
}

static void sk_track_line(const wo_song_t *s, int row, char *o, int cap)
{
    int tr = wo_seq_row_track(row), i;
    const sq_track_t *t = &s->tr[tr];
    o[0] = 0;
    if (tr == TR_DRUMS) {
        const sq_drum_t *d = &s->dr[row];
        wo_strcat(o, ENGNAME[1][t->eng], cap);
        wo_strcat(o, "  ", cap); wo_strcat(o, ROWNAME[row], cap);
        wo_strcat(o, "  tune ", cap); wo_catu(o, d->tune, cap);
        wo_strcat(o, " dec ", cap); wo_catu(o, d->decay, cap);
        wo_strcat(o, " lvl ", cap); wo_catu(o, d->level, cap);
        return;
    } else {
        int sub = t->eng != ENG_FM;
        wo_strcat(o, sub ? (t->eng == ENG_SAW ? "saw" : "square") : "FM", cap);
        for (i = 0; i < 4; i++) {
            wo_strcat(o, " ", cap); wo_strcat(o, PSHORT[sub][i], cap); wo_strcat(o, " ", cap);
            if (!sub && i == 0) wo_strcat(o, RATIOS[t->p[0] % 10], cap); else wo_catu(o, t->p[i], cap);
        }
    }
}

static void draw_seq(void)
{
    wo_song_t *s;
    char b[96], nm[12];
    int r, st, len, row = sk_row(), ppat = -1, pstep = -1, playing, i;
    lv_reset();
    keys_common();
    s = sk_song();
    if (!s) {
        lv_note("No ticket in play. A sketch is kept with its ticket: PLAY starts one.", C(TC_TEXT));
        lv_note("ENV still plays what you rolled.", C(TC_DIM));
        F(HB_BTN_PLAY, "start"); F(HB_BTN_ENV, wo_seq_playing() ? "stop" : "play");
        chrome("SKETCH", " ");
        lv_draw(-1);
        return;
    }
    playing = wo_seq_pos(&ppat, &pstep, 0, 0);
    sk.shown = playing ? ppat * 32 + pstep : -1;
    len = wo_seq_pat_len(s, sk.pat);
    /* the basics */
    sk_basics_line(b, 96);
    clip_text(3, Y0, b, C(TC_DIM), 39);
    /* the patterns */
    d_text(3, SK_YPAT, "PAT", C(TC_DIM));
    for (i = 0; i < SQ_PATS; i++) {
        char c2[2] = {PATCH[i], 0};
        int x = SK_GX + i * SK_CW, empty = wo_seq_pat_empty(s, i);
        if (i == sk.pat) d_rect(x, SK_YPAT - 1, SK_CW - 1, ROWH, C(TC_INK3));
        d_text(x + 3, SK_YPAT, c2, i == sk.pat ? C(TC_SIGNAL) : empty ? C(TC_DIMMER) : C(TC_TEXT));
        if (playing && i == ppat) d_rect(x + 1, SK_YPAT + 9, SK_CW - 3, 2, C(TC_CYAN));
    }
    if (s->chain_on && s->nchain) { wo_strcpy(b, "chain of ", 32); wo_catu(b, s->nchain, 32); }
    else { wo_strcpy(b, "loops ", 32); b[6] = PATCH[sk.pat]; b[7] = 0; }
    d_text_r(237, SK_YPAT, b, C(TC_DIM));
    /* the ruler: the beats, and where it plays */
    for (st = 0; st < len; st++) {
        int x = SK_GX + st * SK_CW;
        if (st % s->div == 0) { b[0] = 0; wo_catu(b, (uint32_t)(st / s->div + 1), 8); d_text(x + 1, SK_YRUL, b, C(TC_DIMMER)); }
        if (playing && ppat == sk.pat && pstep == st) d_rect(x + 1, SK_YRUL + 8, SK_CW - 2, 2, C(TC_SIGNAL));
    }
    if (sk.rec) d_text_r(237, SK_YRUL, "REC", C(TC_MAGENTA));
    /* the grid */
    for (r = 0; r < WO_SEQ_ROWS; r++) {
        int y = SK_YGRID + r * ROWH, tr = wo_seq_row_track(r), sel = r == row;
        int muted = s->tr[tr].mute || (r < SQ_LANES && (s->lanemute >> r & 1u));
        const sq_note_t *n = r < SQ_LANES ? 0 : s->pat[sk.pat].n[r - SQ_LANES];
        int tail = 0;
        if (sel) { d_rect(0, y - 1, 240, ROWH, C(TC_INK3)); d_rect(0, y - 1, 2, ROWH, C(TC_SIGNAL)); }
        if (r == SQ_LANES) d_rect(3, y - 2, 234, 1, C(TC_RULE));
        d_text(4, y, ROWNAME[r], muted ? C(TC_DIMMER) : sel ? C(TC_SIGNAL) : C(TC_DIM));
        for (st = 0; st < len; st++) {
            int x = SK_GX + st * SK_CW + 1, on = 0, held = 0;
            uint16_t bg = st % s->div == 0 ? C(TC_RULE) : C(TC_INK2), fg = C(TC_INK);
            char lab[2] = {0, 0};
            if (r < SQ_LANES) {
                on = s->pat[sk.pat].hit[r] >> st & 1u;
                if (on) bg = (s->pat[sk.pat].acc[r] >> st & 1u) ? C(TC_SIGNAL) : C(TC_TEXT);
            } else if (n[st].note) {
                int dg;
                on = 1;
                bg = tr == TR_BASS ? C(TC_CYAN) : tr == TR_CHORDS ? C(TC_MAGENTA) : C(TC_TEXT);
                if (tr == TR_CHORDS) lab[0] = (char)('0' + (n[st].note > 9 ? 9 : n[st].note));
                else { dg = wo_seq_degree(&skb, n[st].note); lab[0] = dg ? (char)('0' + (dg > 9 ? 9 : dg)) : '*'; }
                tail = n[st].len > 1 ? n[st].len - 1 : 0;
            } else if (tail > 0) { held = 1; tail--; }
            if (held) d_rect(x - 1, y + 3, SK_CW, 3, muted ? C(TC_DIMMER) : C(TC_DIM));
            else {
                if (on && muted) bg = C(TC_DIMMER);
                d_rect(x, y, SK_CW - 2, 9, bg);
                if (lab[0]) d_text(x + 2, y, lab, fg);
            }
            if (sel && st == sk.cur) {
                d_rect(x - 1, y - 1, SK_CW, 1, C(TC_SIGNAL)); d_rect(x - 1, y + 9, SK_CW, 1, C(TC_SIGNAL));
                d_rect(x - 1, y - 1, 1, 11, C(TC_SIGNAL)); d_rect(x + SK_CW - 2, y - 1, 1, 11, C(TC_SIGNAL));
            }
        }
    }
    /* the step under the cursor */
    {
        int y = SK_YINFO, tr = wo_seq_row_track(row);
        wo_strcpy(b, "Step ", 96); wo_catu(b, (uint32_t)(sk.cur + 1), 96); wo_strcat(b, " ", 96);
        wo_strcat(b, ROWNAME[row], 96); wo_strcat(b, ": ", 96);
        if (row < SQ_LANES) {
            uint16_t bit = (uint16_t)(1u << sk.cur);
            wo_strcat(b, (s->pat[sk.pat].hit[row] & bit) ? ((s->pat[sk.pat].acc[row] & bit) ? "hit, accent" : "hit") : "off", 96);
        } else {
            const sq_note_t *n = &s->pat[sk.pat].n[row - SQ_LANES][sk.cur];
            if (!n->note) wo_strcat(b, "off", 96);
            else {
                if (tr == TR_CHORDS) wo_strcat(b, n->note - 1 < skb.ch.n ? skb.ch.c[n->note - 1].label : "?", 96);
                else { wo_seq_note_name(n->note, nm, 12); wo_strcat(b, nm, 96); }
                wo_strcat(b, "  ", 96); wo_catu(b, n->len, 96); wo_strcat(b, n->len == 1 ? " step" : " steps", 96);
                wo_strcat(b, "  vel ", 96); wo_catu(b, n->vel, 96);
            }
        }
        clip_text(3, y, b, C(TC_TEXT), 39);
        sk_track_line(s, row, b, 96);
        clip_text(3, y + ROWH, b, C(TC_DIM), 39);
        if (s->nchain) {
            wo_strcpy(b, s->chain_on ? "CHAIN " : "chain (off) ", 96);
            for (i = 0; i < s->nchain && wo_strlen(b) < 37; i++) { char c2[3] = {PATCH[s->chain[i] % SQ_PATS], ' ', 0}; wo_strcat(b, c2, 96); }
        } else wo_strcpy(b, "No chain: it loops the pattern (menu)", 96);
        clip_text(3, y + 2 * ROWH, b, s->chain_on ? C(TC_CYAN) : C(TC_DIMMER), 39);
        if (!sk.rec) wo_strcpy(b, "Keys: steps, PRESETS: cursor, ALGO: A-H", 96);
        else if (row < SQ_LANES) wo_strcpy(b, "REC: the first 8 keys play the drums", 96);
        else if (tr == TR_CHORDS) { wo_strcpy(b, "REC: the first ", 96); wo_catu(b, skb.ch.n, 96); wo_strcat(b, " keys play the chords", 96); }
        else { wo_strcpy(b, "REC: the keys play ", 96); wo_strcat(b, ROWNAME[row], 96); wo_strcat(b, playing ? ", it writes" : ", step by step", 96); }
        clip_text(3, y + 3 * ROWH, b, sk.rec ? C(TC_MAGENTA) : C(TC_DIMMER), 39);
        if (row < SQ_LANES) wo_strcpy(b, "K3 accent  K4 swing ", 96);
        else wo_strcpy(b, "K1 note  K2 len  K3 vel  K4 swing ", 96);
        wo_catu(b, s->swing, 96);
        clip_text(3, y + 4 * ROWH, b, C(TC_DIMMER), 39);
    }
    K(HB_BTN_SEL, "sound", 0); K(HB_BTN_REC, "rec", 0); K(HB_BTN_ARP, "roll", 0); K(HB_BTN_SEQ, "mutate", 0);
    K(HB_BTN_LFO, "roll all", 0); K(HB_BTN_EDIT, "mute", 0); K(HB_BTN_ENV, "play", 0);
    F(HB_BTN_ENV, wo_seq_playing() ? "stop" : "play"); F(HB_BTN_REC, sk.rec ? "rec off" : "rec"); F(HB_BTN_ARP, "roll row");
    MI("This row", "Roll it", HB_BTN_ARP, 0, 0);
    MI("This row", "Mutate it", HB_BTN_SEQ, 0, 0);
    MI("This row", (s->tr[wo_seq_row_track(row)].mute || (row < SQ_LANES && (s->lanemute >> row & 1u))) ? "Unmute it" : "Mute it", HB_BTN_EDIT, 0, 0);
    MI("This row", "Its sound", HB_BTN_SEL, 0, 0);
    MI("This row", "Clear it", 0, A_SQ_CLEARROW, 0);
    MI("Pattern", "Roll the whole pattern", HB_BTN_LFO, 0, 0);
    MI("Pattern", "Copy it to the next", 0, A_SQ_COPY, 0);
    MI("Pattern", "Clear it", 0, A_SQ_CLEARPAT, 1);
    MI("Chain", "Add this pattern to it", 0, A_SQ_CHADD, 0);
    if (s->nchain) {
        MI("Chain", "Take the last one off", 0, A_SQ_CHDROP, 0);
        MI("Chain", s->chain_on ? "Loop this pattern instead" : "Play the chain", 0, A_SQ_CHMODE, 0);
        MI("Chain", "Clear it", 0, A_SQ_CHCLEAR, 0);
    }
    MI("Record", sk.rec ? "Stop recording" : "Record live (REC)", HB_BTN_REC, 0, 0);
    MI("MIDI", s->clock ? "MIDI clock out: on" : "MIDI clock out: off", 0, A_SQ_CLOCK, 0);
    { char c2[2] = {PATCH[sk.pat], 0}; wo_strcpy(b, sk.rec ? "SKETCH REC  " : "SKETCH  ", 32); wo_strcat(b, c2, 32); }
    chrome(b, "");
}

/* the knobs and buttons on the sketch */
static void sk_preview(int k, int tr, int note, int vel)
{
    sk.kn[k] = 0;
    if (tr == TR_CHORDS) {
        int ci = note - 1, i;
        if (ci < 0 || ci >= skb.ch.n) return;
        for (i = 0; i < skb.ch.c[ci].npc && i < 6; i++) {
            int m = 48 + skb.ch.root + skb.ch.c[ci].tone[i];
            wo_synth_track_note(TR_CHORDS, m, vel);
            sk.knotes[k][sk.kn[k]++] = (uint8_t)m;
        }
    } else { wo_synth_track_note(tr, note, vel); sk.knotes[k][0] = (uint8_t)note; sk.kn[k] = 1; }
}
static void sk_release(int k)
{
    int i;
    for (i = 0; i < sk.kn[k]; i++) wo_synth_track_note(TR_BASS, sk.knotes[k][i], 0);
    sk.kn[k] = 0;
}

static void seq_key(int k, int down, int vel)
{
    wo_song_t *s = sk_song();
    int row = sk_row(), tr = wo_seq_row_track(row), len, pat, st, pp, ps, fr, playing;
    if (!s || k < 0 || k > 26) return;
    if (!vel) vel = 100;
    if (!down) {
        if (sk.rec && sk.kstep[k] >= 0 && sk.krow[k] >= SQ_LANES && wo_seq_pos(&pp, &ps, &fr, 0) && pp == sk.kpat[k]) {
            int l = wo_seq_pat_len(s, pp), held = (ps - sk.kstep[k] + l) % l + (fr >= 128 ? 1 : 0);
            wo_seq_set_len(s, sk.kpat[k], sk.krow[k], sk.kstep[k], held < 1 ? 1 : held);
        }
        sk.kstep[k] = -1;
        sk_release(k);
        return;
    }
    if (!W.set.sound) { toast("Sound is off (Settings)"); return; }
    if (!sk.rec) {                                  /* the key is a step */
        len = wo_seq_pat_len(s, sk.pat);
        if (k >= len) return;
        sk.cur = k;
        if (wo_seq_toggle(s, &skb, sk.pat, row, k)) {
            if (row < SQ_LANES) wo_synth_drum(row, 100);
            else sk_preview(k, tr, s->pat[sk.pat].n[row - SQ_LANES][k].note, 100);
        }
        sk_changed();
        return;
    }
    /* REC: play it, and write it where it is playing (or at the cursor, when stopped) */
    {
        int lane = -1, note = 0, wrow = row;
        if (row < SQ_LANES) { if (k >= SQ_LANES) return; lane = k; wrow = k; }
        else if (tr == TR_CHORDS) { if (k >= skb.ch.n) return; note = k + 1; }
        else { note = wo_synth_snap(k) - (tr == TR_BASS ? 24 : 0); if (note < 24) note += 12; }
        playing = wo_seq_pos(&pp, &ps, &fr, 0);
        if (playing) {
            pat = pp; len = wo_seq_pat_len(s, pat);
            st = fr < 128 ? ps : (ps + 1) % len;
            if (st != ps) { if (lane >= 0) wo_seq_skip_drum(lane, st); else wo_seq_skip(tr, st); }
        } else { pat = sk.pat; len = wo_seq_pat_len(s, pat); st = sk.cur; sk.cur = (sk.cur + 1) % len; }
        wo_seq_write(s, pat, wrow, st, note, vel);
        sk.kpat[k] = (int8_t)pat; sk.krow[k] = (int8_t)wrow; sk.kstep[k] = (int8_t)st;
        if (lane >= 0) wo_synth_drum(lane, vel);
        else sk_preview(k, tr, note, vel);
        sk_changed();
    }
}

static void seq_event(uint32_t kind, uint32_t id, int32_t arg)
{
    wo_song_t *s = sk_song();
    int row = sk_row(), tr = wo_seq_row_track(row), len;
    if (!s) return;
    len = wo_seq_pat_len(s, sk.pat);
    if (kind == HB_EV_ENC) {
        int d = arg > 0 ? 1 : arg < 0 ? -1 : 0;
        switch (id) {
        case HB_ENC_SELECT: { int r = row + arg; TOP.sel = (int16_t)(r < 0 ? 0 : r >= WO_SEQ_ROWS ? WO_SEQ_ROWS - 1 : r); break; }
        case HB_ENC_PRESETS: sk.cur = ((sk.cur + arg) % len + len) % len; break;
        case HB_ENC_ALGORITHM: {
            int p = sk.pat + d;
            sk.pat = p < 0 ? 0 : p >= SQ_PATS ? SQ_PATS - 1 : p;
            wo_seq_edit_pat(sk.pat);
            if (sk.cur >= wo_seq_pat_len(s, sk.pat)) sk.cur = 0;
            break; }
        case HB_ENC_KNOB1: wo_seq_adjust(s, &skb, sk.pat, row, sk.cur, 0, arg); sk_changed();
            if (row >= SQ_LANES && s->pat[sk.pat].n[row - SQ_LANES][sk.cur].note && W.set.sound) {
                sk_release(26); sk_preview(26, tr, s->pat[sk.pat].n[row - SQ_LANES][sk.cur].note, 90);
            }
            break;
        case HB_ENC_KNOB2: wo_seq_adjust(s, &skb, sk.pat, row, sk.cur, 1, d); sk_changed(); break;
        case HB_ENC_KNOB3: wo_seq_adjust(s, &skb, sk.pat, row, sk.cur, 2, d); sk_changed(); break;
        case HB_ENC_KNOB4: { int w = s->swing + d; s->swing = (uint8_t)(w < 0 ? 0 : w > 10 ? 10 : w); sk_changed(); break; }
        default: break;
        }
        return;
    }
    if (kind != HB_EV_BTN_DOWN) return;
    switch (id) {
    case HB_BTN_SEL: push(PG_SOUND, (int16_t)row); break;
    case HB_BTN_REC: sk.rec = !sk.rec; toast(sk.rec ? (wo_seq_playing() ? "Recording: play along" : "Step recording: a key a step") : "Recording off"); break;
    case HB_BTN_ARP: wo_seq_roll_row(s, &skb, sk.pat, row); sk_changed(); toast("Rolled"); break;
    case HB_BTN_SEQ: wo_seq_mutate_row(s, &skb, sk.pat, row); sk_changed(); toast("Mutated"); break;
    case HB_BTN_LFO: wo_seq_roll_pat(s, &skb, sk.pat); sk_changed(); toast("Rolled the pattern"); break;
    case HB_BTN_EDIT:
        if (row < SQ_LANES) s->lanemute ^= (uint8_t)(1u << row);
        else s->tr[tr].mute = !s->tr[tr].mute;
        sk_changed();
        break;
    default: break;
    }
}

static void sketch_action(int act)
{
    wo_song_t *s = sk_song();
    int row = sk_row();
    if (!s) return;
    switch (act) {
    case A_SQ_CLEARROW: wo_seq_clear_row(s, sk.pat, row); toast("Row cleared"); break;
    case A_SQ_CLEARPAT: wo_seq_clear_pat(s, sk.pat); toast("Pattern cleared"); break;
    case A_SQ_COPY: {
        int to = wo_seq_copy_next(s, sk.pat);
        if (to < 0) { toast("H is the last pattern"); return; }
        sk.pat = to; wo_seq_edit_pat(sk.pat); toast("Copied");
        break; }
    case A_SQ_CHADD: if (wo_seq_chain_add(s, sk.pat)) { toast("16 in the chain is the most"); return; } toast("Added to the chain"); break;
    case A_SQ_CHDROP: wo_seq_chain_drop(s); break;
    case A_SQ_CHMODE: s->chain_on = !s->chain_on && s->nchain; toast(s->chain_on ? "Plays the chain" : "Loops the pattern"); break;
    case A_SQ_CHCLEAR: wo_seq_chain_clear(s); toast("Chain cleared"); break;
    case A_SQ_CLOCK: s->clock = !s->clock; toast(s->clock ? "MIDI clock goes out" : "No MIDI clock"); break;
    default: return;
    }
    sk_changed();
}

/* ================================================================ SOUND: one track's voice */
static int sound_rows(int tr) { return tr == TR_DRUMS ? 4 + SQ_LANES : 8; }
static void draw_sound(void)
{
    wo_song_t *s = sk_song();
    int row = TOP.arg, tr = wo_seq_row_track(row < 0 ? 0 : row), i, n;
    char b[64], v[24], title[24];
    lv_reset();
    keys_common();
    if (!s) { lv_note("No ticket in play.", C(TC_TEXT)); chrome("SOUND", " "); lv_draw(-1); return; }
    n = sound_rows(tr);
    for (i = 0; i < n; i++) {
        const sq_track_t *t = &s->tr[tr];
        int sub = t->eng != ENG_FM;
        v[0] = 0;
        if (i == 0) { wo_strcpy(b, tr == TR_DRUMS ? "Kit" : "Engine", 64); wo_strcpy(v, ENGNAME[tr == TR_DRUMS][t->eng % ENG_N], 24); }
        else if (tr != TR_DRUMS && i <= 4) {
            wo_strcpy(b, PNAME[sub][i - 1], 64);
            if (!sub && i == 1) wo_strcpy(v, RATIOS[t->p[0] % 10], 24); else wo_catu(v, t->p[i - 1], 24);
            wo_strcat(b, i == 1 ? " (K1)" : i == 2 ? " (K2)" : i == 3 ? " (K3)" : " (K4)", 64);
        } else {
            int k = tr == TR_DRUMS ? i : i - 4;
            if (k == 1) { wo_strcpy(b, "Volume", 64); wo_catu(v, t->vol, 24); }
            else if (k == 2) { wo_strcpy(b, "MIDI channel", 64); wo_catu(v, (uint32_t)(t->ch & 15u) + 1u, 24); }
            else if (k == 3) { wo_strcpy(b, "Mute", 64); wo_strcpy(v, t->mute ? "muted" : "plays", 24); }
            else {
                const sq_drum_t *d = &s->dr[k - 4];
                wo_strcpy(b, ROWNAME[k - 4], 64);
                wo_strcat(v, "t ", 24); wo_catu(v, d->tune, 24); wo_strcat(v, " d ", 24); wo_catu(v, d->decay, 24);
                wo_strcat(v, " l ", 24); wo_catu(v, d->level, 24);
            }
        }
        while (wo_strlen(b) + wo_strlen(v) < 37) wo_strcat(b, " ", 64);
        wo_strcat(b, v, 64);
        lv_add(i, 6, 0, 0, "", b, TOP.sel == i ? C(TC_SIGNAL) : C(TC_TEXT), C(TC_INK));
    }
    lv_note(" ", C(TC_INK));
    if (tr == TR_DRUMS) lv_note("ALGO changes the highlighted. On a drum: K1 tune, K2 decay, K3 level. The first 8 keys play the drums.", C(TC_DIM));
    else lv_note("ALGO changes the highlighted. K1 to K4 set the four sound knobs. The keys play it.", C(TC_DIM));
    {
        int last, peak;
        wo_synth_load(&last, &peak);
        wo_strcpy(b, "Audio load ", 64); wo_catu(b, (uint32_t)last, 64); wo_strcat(b, "%, peak ", 64); wo_catu(b, (uint32_t)peak, 64);
        wo_strcat(b, "% of the time", 64);
        lv_note(b, peak > 80 ? C(TC_MAGENTA) : C(TC_DIMMER));
    }
    K(HB_BTN_SEL, "hear", 0);
    F(HB_BTN_SEL, "hear it"); F(HB_BTN_ENV, wo_seq_playing() ? "stop" : "play");
    wo_strcpy(title, TRNAME[tr], 24); wo_strcat(title, " SOUND", 24);
    chrome(title, " ");
    lv_draw(TOP.sel);
}

static void sound_change(wo_song_t *s, int tr, int i, int d)
{
    sq_track_t *t = &s->tr[tr];
#define CL(v, lo, hi) v = (uint8_t)((int)(v) + d < (lo) ? (lo) : (int)(v) + d > (hi) ? (hi) : (int)(v) + d)
    if (i == 0) t->eng = (uint8_t)((t->eng + (tr == TR_DRUMS ? 1 : (d > 0 ? 1 : ENG_N - 1))) % (tr == TR_DRUMS ? 2 : ENG_N));
    else if (tr != TR_DRUMS && i <= 4) { if (i == 1 && t->eng == ENG_FM) CL(t->p[0], 0, 9); else CL(t->p[i - 1], 0, 31); }
    else {
        int k = tr == TR_DRUMS ? i : i - 4;
        if (k == 1) CL(t->vol, 0, 31);
        else if (k == 2) CL(t->ch, 0, 15);
        else if (k == 3) t->mute = !t->mute;
    }
#undef CL
    sk_changed();
}
static void sound_hear(wo_song_t *s, int tr, int lane)
{
    if (!W.set.sound) { toast("Sound is off (Settings)"); return; }
    if (tr == TR_DRUMS) { wo_synth_drum(lane < 0 ? 0 : lane, 110); return; }
    sk_release(25);
    sk_preview(25, tr, tr == TR_CHORDS ? 1 : tr == TR_BASS ? 36 + skb.root : wo_seq_snap(&skb, 60 + skb.root + 7), 100);
    (void)s;
}
static void sound_event(uint32_t kind, uint32_t id, int32_t arg)
{
    wo_song_t *s = sk_song();
    int tr = wo_seq_row_track(TOP.arg < 0 ? 0 : TOP.arg), d = arg > 0 ? 1 : arg < 0 ? -1 : 0, n;
    if (!s) return;
    n = sound_rows(tr);
    if (kind == HB_EV_ENC) {
        if (id == HB_ENC_ALGORITHM) { sound_change(s, tr, TOP.sel, d); return; }
        if (tr == TR_DRUMS && TOP.sel >= 4 && TOP.sel < n && id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB3) {
            sq_drum_t *dr = &s->dr[TOP.sel - 4];
            uint8_t *f = id == HB_ENC_KNOB1 ? &dr->tune : id == HB_ENC_KNOB2 ? &dr->decay : &dr->level;
            int x = *f + d;
            *f = (uint8_t)(x < 0 ? 0 : x > 31 ? 31 : x);
            sk_changed();
            return;
        }
        if (tr != TR_DRUMS && id >= HB_ENC_KNOB1 && id <= HB_ENC_KNOB4) { sound_change(s, tr, 1 + (int)(id - HB_ENC_KNOB1), d); return; }
        return;
    }
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) sound_hear(s, tr, tr == TR_DRUMS && TOP.sel >= 4 ? TOP.sel - 4 : TOP.arg);
    if (kind == HB_EV_BTN_UP && id == HB_BTN_SEL) { sk_release(25); }
}
static void sound_key(int k, int down, int vel)
{
    wo_song_t *s = sk_song();
    int tr = wo_seq_row_track(TOP.arg < 0 ? 0 : TOP.arg);
    if (!s || k < 0 || k > 26) return;
    if (!down) { sk_release(k); return; }
    if (!W.set.sound) return;
    if (!vel) vel = 100;
    if (tr == TR_DRUMS) { if (k < SQ_LANES) wo_synth_drum(k, vel); return; }
    if (tr == TR_CHORDS) { if (k < skb.ch.n) sk_preview(k, tr, k + 1, vel); return; }
    sk_preview(k, tr, wo_synth_snap(k) - (tr == TR_BASS ? 24 : 0), vel);
}

/* ================================================================ ENV: play the sketch */
static wo_song_t scratch;
static wo_basics_t scratch_b;
static void sketch_toggle(void)
{
    wo_song_t *s;
    if (!W.set.sound) { toast("Sound is off (Settings)"); return; }
    if (wo_seq_playing()) { wo_seq_stop(); toast("Stopped"); return; }
    if (TOP.page == PG_VIEW) {                              /* a filed ticket's sketch */
        const wo_ticket_t *t = lib_ticket(TOP.arg);
        if (!t) return;
        wo_basics_ticket(t, &scratch_b);
        scratch = t->song;
        wo_song_ready(&scratch, &scratch_b);
        s = &scratch;
    } else if ((s = sk_song()) != 0) scratch_b = skb;
    else { wo_basics_draft(&scratch_b); wo_song_seed(&scratch, &scratch_b); s = &scratch; }
    {
        int p, any = 0;
        for (p = 0; p < SQ_PATS; p++) if (!wo_seq_pat_empty(s, p)) any = 1;
        if (!any) { toast(s == &scratch && !W.has_cur ? "Nothing to play: roll Chords" : "Empty: the keys or ARP put notes in"); return; }
    }
    wo_seq_play(s, &scratch_b, s == &scratch ? 0 : sk.pat);
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
    F(HB_BTN_HOME, "back");
    if (W.nscores) MI("Scores", "Clear the leaderboard", 0, A_FORGOOD, 1);
    chrome("SCORES", "ALGO: mode");
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
    if (TOP.sel >= 0 && TOP.sel < nrule_ids) {
        if (rule_ids[TOP.sel] >= 10000) {
            K(HB_BTN_EDIT, "edit", 0); F(HB_BTN_EDIT, "edit");
            MI("This rule", "Edit it", HB_BTN_EDIT, 0, 0);
            MI("This rule", "Delete it", 0, A_FORGOOD, 1);
        } else {
            int dis = wo_bit(W.disabled, (uint32_t)rule_ids[TOP.sel]);
            K(HB_BTN_EDIT, "on/off", 0); F(HB_BTN_EDIT, dis ? "on" : "off");
            MI("This rule", dis ? "Switch it on" : "Switch it off", HB_BTN_EDIT, 0, 0);
        }
        MI("This rule", "Explain its terms", 0, A_GLOSS, 0);
    }
    if (mode < 0) { K(HB_BTN_REC, "add", 0); F(HB_BTN_REC, "add yours"); MI("Pool", "Add a rule of your own", HB_BTN_REC, 0, 0); }
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
    if (TOP.sel == 0) { K(HB_BTN_SEL, "open", 0); F(HB_BTN_SEL, "open"); }
    else if (TOP.sel > 0 && TOP.sel <= W.ninv) {
        K(HB_BTN_SEL, "rename", 0); K(HB_BTN_EDIT, "on/off", 0);
        F(HB_BTN_EDIT, W.inv[TOP.sel - 1].on ? "off" : "on"); F(HB_BTN_SEL, "rename");
        MI("This item", W.inv[TOP.sel - 1].on ? "Switch it off" : "Switch it on", HB_BTN_EDIT, 0, 0);
        MI("This item", "Rename it", HB_BTN_SEL, 0, 0);
        MI("This item", "Delete it", 0, A_FORGOOD, 1);
    }
    F(HB_BTN_REC, "add");
    MI("Gear", "Add an item", HB_BTN_REC, 0, 0);
    chrome("GEAR", "ALGO: category");
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
    F(HB_BTN_EDIT, "switch it");
    chrome("CATEGORIES", "KNOB1: param");
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
        if (mat_ids[TOP.sel] >= 1000) MI("This sound", "Delete it", 0, A_FORGOOD, 1);
        else {
            int off = wo_bit(W.matoff, (uint32_t)mat_ids[TOP.sel]);
            K(HB_BTN_EDIT, "on/off", 0); F(HB_BTN_EDIT, off ? "on" : "off");
            MI("This sound", off ? "Switch it on" : "Switch it off", HB_BTN_EDIT, 0, 0);
        }
    }
    F(HB_BTN_REC, "add");
    MI("Materials", "Add a sound to this category", HB_BTN_REC, 0, 0);
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
    F(HB_BTN_SEL, "use it");
    MI("This setup", "Use it", HB_BTN_SEL, 0, 0);
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
    case S_RESETW: case S_WIPE: wo_strcpy(o, "SCL, asks", cap); break;
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
    if (TOP.sel == S_RESETW || TOP.sel == S_WIPE) { K(HB_BTN_SEL, "reset", 0); F(HB_BTN_SEL, "reset"); }
    else if (TOP.sel != S_ABOUT) { K(HB_BTN_SEL, "change", 0); F(HB_BTN_SEL, "change"); }
    MI("Reset", "Reset the learning", 0, A_RESETW, 1);
    MI("Reset", "Reset everything", 0, A_WIPE, 1);
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
    case S_SOUND: W.set.sound = !W.set.sound; if (!W.set.sound) { wo_seq_stop(); wo_synth_all_off(); } break;
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
        else if (TOP.sel == S_WIPE) { wo_seq_stop(); wo_store_wipe(); go_root(PG_HOME); toast("Everything reset"); }
        return;
    }
    if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) {
        if (TOP.sel == S_RESETW || TOP.sel == S_WIPE) {
            conf_item.label = TOP.sel == S_RESETW ? "Reset the learning" : "Reset everything";
            conf_item.btn = 0; conf_item.act = TOP.sel == S_RESETW ? A_RESETW : A_WIPE; conf_item.confirm = 1;
            conf_open = 1;
        } else settings_change(TOP.sel, 1);
    }
}

/* ================================================================ HELP */
static const char *const HELP[] = {
    "The menu", "GLO opens a menu of everything the page can do, in groups, with the button that does it straight away on the right. SELECT moves, SCL runs it, HOME closes. The bottom line names the page's three main buttons.",
    "Everywhere", "SELECT moves, PRESETS moves faster. SCL opens or ticks. HOME goes back (hold it to leave WORKORDER). ENV plays or stops the sketch. The keys play the rolled scale; OCT- / OCT+ shift them.",
    "The sketch", "Drums (8), bass, chords and lead, a bar a pattern, in the ticket's key, tempo, meter and chords. SELECT picks a row, the keys are its steps, PRESETS moves the cursor, ALGO the pattern A to H. K1 note, K2 length, K3 velocity (accent on a drum), K4 swing. REC plays along and writes it. ARP rolls a row, LFO the pattern, SEQ mutates. SCL: the row's sound.",
    "Chain and MIDI", "The menu adds patterns to the chain, which ENV plays in order; without one, the pattern loops. Notes go out over USB MIDI: bass 1, chords 2, lead 3, drums 10 (change it on the sound page), with MIDI clock. The newest 4 filed tickets keep their sketch.",
    "Starting", "PLAY with no ticket rolls your setup and starts the clock. To tweak first, open Roll: REC rolls everything, ARP rerolls one parameter, SEQ mutates, LFO rolls at random, EDIT switches a parameter, SAVE holds it. Then PLAY.",
    "Working", "SCL ticks a rule off. FX breaks it (say why) or unbreaks it. PLAY pauses the clock, SAVE parks the ticket. Finishing, rating and retiring a rule for good are in the menu.",
    "For good", "Anything that can't be undone (finishing, retiring, deleting, resets) is in the menu, and asks first: SCL yes, HOME no.",
    "Modes and the Run", "Modes: SCL starts one, and from then on PLAY starts that mode. Free play goes back to your own setup. In a Run each round deals cards: SCL keeps one, REC deals again. A broken brutal card costs two lives.",
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
    F(HB_BTN_HOME, "back");
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
                                   draw_presets, draw_settings, draw_help, draw_gloss, draw_text_page, draw_seq, draw_sound};
static const event_fn EVENT[PG_N] = {home_event, roll_event, field_event, session_event, rate_event, modes_event,
                                     parked_event, library_event, view_event, scores_event, rules_event, gear_event,
                                     buckets_event, mats_event, presets_event, settings_event, ev_none, ev_none, ev_none,
                                     seq_event, sound_event};

/* LEDs: the buttons that do something on this page glow, the keys of the rolled scale glow */
static uint32_t leds_shown = 0xFFFFFFFFu, keys_shown = 0xFFFFFFFFu, keys_lit_shown = 0xFFFFFFFFu;
/* the sketch's keys: the steps (lit: on, glowing: off; the step playing turns over), or what REC plays */
static void sk_leds(uint32_t *lit, uint32_t *glow)
{
    const wo_song_t *s = &W.cur.song;
    int row = sk_row(), tr = wo_seq_row_track(row), len, st, pp, ps, i;
    if (!s->div) return;
    if (sk.rec) {
        if (row < SQ_LANES) *glow = 0xFFu;
        else if (tr == TR_CHORDS) *glow = (1u << skb.ch.n) - 1u;
        else for (i = 0; i < 27; i++) if (wo_synth_in_scale(i)) *glow |= 1u << i;
        return;
    }
    len = wo_seq_pat_len(s, sk.pat);
    for (st = 0; st < len; st++) {
        int on = row < SQ_LANES ? (s->pat[sk.pat].hit[row] >> st & 1u) : !!s->pat[sk.pat].n[row - SQ_LANES][st].note;
        if (on) *lit |= 1u << st; else *glow |= 1u << st;
    }
    if (wo_seq_pos(&pp, &ps, 0, 0) && pp == sk.pat && ps < len) { *lit ^= 1u << ps; *glow ^= 1u << ps; }
}
static void leds(void)
{
    uint32_t b = 0, k = 0, lit = 0, i, extra;
    for (i = 0; i < 12; i++) if (key_tap[i] && key_tap[i][0]) b |= 1u << (i + 2);
    if (menu_open || conf_open) b = (1u << HB_BTN_SEL) | (1u << HB_BTN_HOME) | (1u << HB_BTN_GLO);
    sync_scale();
    if (TOP.page == PG_TEXT) { b |= 3u; k = 0x7FFFFFFu; }
    else if (TOP.page == PG_SEQ && W.has_cur) sk_leds(&lit, &k);
    else if (TOP.page == PG_SOUND && W.has_cur) {
        int tr = wo_seq_row_track(TOP.arg < 0 ? 0 : TOP.arg);
        if (tr == TR_DRUMS) k = 0xFFu;
        else if (tr == TR_CHORDS) k = (1u << skb.ch.n) - 1u;
        else for (i = 0; i < 27; i++) if (wo_synth_in_scale((int)i)) k |= 1u << i;
    } else for (i = 0; i < 27; i++) if (wo_synth_in_scale((int)i)) k |= 1u << i;
    extra = (wo_seq_playing() ? 1u << 20 : 0u) | (TOP.page == PG_SEQ && sk.rec ? 1u << 21 : 0u);
    if ((b | extra) != leds_shown) {
        for (i = 0; i < 14; i++) {
            uint32_t on = (b >> i & 1u) ? 2u : 0u;
            if (on && ((i == HB_BTN_ENV && (extra >> 20 & 1u)) || (i == HB_BTN_REC && (extra >> 21 & 1u)))) on = 1u;
            H->led(i, on);
        }
        leds_shown = b | extra;
    }
    if (k != keys_shown || lit != keys_lit_shown) {
        for (i = 0; i < 27; i++) H->led(HB_LED_KEY(i), (lit >> i & 1u) ? 1u : (k >> i & 1u) ? 2u : 0u);
        keys_shown = k; keys_lit_shown = lit;
    }
}

static int page_has_menu(void) { return TOP.page != PG_TEXT && TOP.page != PG_GLOSS && TOP.page != PG_RATE; }

/* the keys the module handles itself, before the page: ENV, PLAY, OCT-, OCT+, HOME */
static int global_tap(uint32_t id)
{
    if (id == HB_BTN_ENV) { sketch_toggle(); return 1; }
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

static void tap(uint32_t id)
{
    uint8_t p = TOP.page;
    if (p != PG_TEXT && global_tap(id)) return;
    EVENT[p](HB_EV_BTN_DOWN, id, 0);
}

/* a menu item: its button, as if pressed, or its own action; "for good" asks first */
static void run_item(mi_t m, int confirmed)
{
    uint8_t p = TOP.page;
    if (m.confirm && !confirmed) { conf_item = m; conf_open = 1; menu_open = 0; return; }   /* "no" goes back to the page */
    menu_open = 0; conf_open = 0;
    if (m.btn) { tap(m.btn); return; }
    switch (m.act) {
    case A_FINISH: if (W.has_cur) { push(PG_RATE, 0); TOP.sel = 1; } break;
    case A_PARK: {
        int r;
        wo_seq_stop();
        r = wo_park();
        toast(r == 0 ? "Ticket parked" : r == -1 ? "Both park slots are taken" : "Could not save it");
        if (!r) go_root(PG_HOME);
        break; }
    case A_FORGOOD: EVENT[p](EV_HOLD, HB_BTN_FX, 0); break;
    case A_RESETW: wo_reset_weights(); wo_store_mark(ST_STATE); toast("Learning reset"); break;
    case A_WIPE: wo_seq_stop(); wo_store_wipe(); go_root(PG_HOME); toast("Everything reset"); break;
    case A_HOMEPAGE: go_root(PG_HOME); break;
    case A_HELP: push(PG_HELP, 0); break;
    case A_GLOSS: EVENT[p](HB_EV_BTN_DOWN, HB_BTN_GLO, 0); break;
    case A_SKETCH: push(PG_SEQ, 0); break;
    default: if (m.act >= A_SQ_CLEARROW) sketch_action(m.act); break;
    }
}

/* ================================================================ the module's calls */
void wo_ui_init(const hb_host_t *h)
{
    H = h;
    depth = 0; st[0].page = PG_HOME; st[0].sel = 0; st[0].scroll = 0;
    if (W.has_cur) push(PG_SESSION, 0);
    need_full = 1; leds_shown = keys_shown = 0xFFFFFFFFu;
    menu_open = conf_open = 0;
    wo_synth_volume(W.set.volume);
    { int k; for (k = 0; k < 27; k++) sk.kstep[k] = -1; }
    sk.pat = 0; sk.cur = 0; sk.rec = 0; sk.shown = -1;
    wo_synth_song(W.has_cur ? &W.cur.song : 0);
    sync_scale();
}

void wo_ui_event(uint32_t ev, int32_t arg)
{
    uint32_t kind = HB_EV_KIND(ev), id = HB_EV_ID(ev);
    uint8_t p = TOP.page;
    need_full = 1;
    if (p == PG_TEXT) {                         /* typing: every key is a letter */
        if (kind == HB_EV_BTN_DOWN && id == HB_BTN_HOME) { pop(); return; }
        text_event(kind, id, arg);
        return;
    }
    if (kind == HB_EV_KEY_DOWN || kind == HB_EV_KEY_UP) {
        sync_scale();
        if (p == PG_SEQ && W.has_cur && !menu_open && !conf_open) { seq_key((int)id, kind == HB_EV_KEY_DOWN, arg); return; }
        need_full = 0;
        if (p == PG_SOUND && W.has_cur) { sound_key((int)id, kind == HB_EV_KEY_DOWN, arg); return; }
        if (W.set.sound) wo_synth_key((int)id, kind == HB_EV_KEY_DOWN, arg);
        return;
    }
    if (conf_open) {                            /* "Sure?": SCL yes, HOME (or GLO) no */
        if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) run_item(conf_item, 1);
        else if ((kind == HB_EV_BTN_UP && id == HB_BTN_HOME) || (kind == HB_EV_BTN_DOWN && id == HB_BTN_GLO)) conf_open = 0;
        return;
    }
    if (menu_open) {
        if (kind == HB_EV_ENC && (id == HB_ENC_SELECT || id == HB_ENC_PRESETS)) {
            int s2 = menu_sel + (id == HB_ENC_PRESETS ? arg * 4 : arg);
            menu_sel = s2 < 0 ? 0 : s2 >= nmi ? nmi - 1 : s2;
            return;
        }
        if (kind == HB_EV_BTN_DOWN && id == HB_BTN_SEL) { if (menu_sel >= 0 && menu_sel < nmi) run_item(mi[menu_sel], 0); return; }
        if (kind == HB_EV_BTN_DOWN && id == HB_BTN_GLO) { menu_open = 0; return; }
        if (kind == HB_EV_BTN_UP && id == HB_BTN_HOME) { menu_open = 0; return; }
        if (kind == HB_EV_BTN_DOWN && id != HB_BTN_HOME) menu_open = 0;   /* another button: its shortcut, as usual */
        else return;
    }
    if (p == PG_SEQ && kind == HB_EV_ENC) { EVENT[p](kind, id, arg); return; }   /* the sketch's knobs are its own */
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
        if (id == HB_BTN_HOME) return;          /* on release: a long hold is the host's picker */
        if (id == HB_BTN_GLO) { if (page_has_menu()) { menu_open = 1; menu_sel = 0; } else EVENT[p](kind, id, arg); return; }
        tap(id);
        return;
    }
    if (kind == HB_EV_BTN_UP) {
        if (id == HB_BTN_HOME) tap(id);
        return;
    }
    EVENT[p](kind, id, arg);
}

/* the main loop's: redraw what changed, keep the clock, save when quiet */
void wo_ui_draw(uint32_t full)
{
    uint32_t t = H->ms();
    wo_synth_midi(H->midi_send);
    wo_synth_song(W.has_cur ? &W.cur.song : 0);
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
        if (menu_open && !page_has_menu()) menu_open = 0;
        if (menu_sel >= nmi) menu_sel = nmi ? nmi - 1 : 0;
        if (menu_open || conf_open) draw_menu();
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
    if (TOP.page == PG_SEQ && W.has_cur && !need_full && !menu_open && !conf_open) {
        int pp, ps, pos = wo_seq_pos(&pp, &ps, 0, 0) ? pp * 32 + ps : -1;
        if (pos != sk.shown) {                  /* the step moved on: the pattern row, the ruler, the keys */
            int i, keep = 0;
            nops = 0; tpos = 0;
            DRAW[TOP.page]();
            for (i = 0; i < nops; i++) if (ops[i].y >= SK_YPAT - 1 && ops[i].y + ops[i].h <= SK_YGRID - 1) ops[keep++] = ops[i];
            nops = keep;
            flush_rows(SK_YPAT - 1, SK_YGRID - 1);
            leds();
        }
    }
    wo_store_poll(!wo_synth_busy(), wo_seq_playing() ? 120000u : 20000u);
    if (wo_store_song_dropped()) toast("The sketch didn't fit: the ticket was saved without it");
    if (wo_store_failed()) toast("Storage full: some of it was not saved");
}

void wo_ui_release(void)
{
    if (wo_seq_playing() && W.has_cur && W.cur.song.div && H->midi_send) {    /* render() stops now: say so to MIDI */
        int t;
        for (t = 0; t < TR_N; t++) H->midi_send(0xB0u | (W.cur.song.tr[t].ch & 15u), 123, 0);
        if (W.cur.song.clock) H->midi_send(0xFCu, 0, 0);
    }
    wo_seq_stop();
    wo_synth_all_off();
    wo_store_flush();
}
