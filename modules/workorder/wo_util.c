/* SPDX-License-Identifier: GPL-3.0-only */
/* Strings, numbers and randomness: a module links against no libc. */
#include "wo.h"

static uint32_t rs = 0x9E3779B9u;

void wo_seed(uint32_t s) { rs = s ? s : 0x9E3779B9u; }

uint32_t wo_rand(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

int wo_ri(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)(wo_rand() % (uint32_t)(hi - lo + 1));
}

float wo_frand(void) { return (float)(wo_rand() >> 8) * (1.0f / 16777216.0f); }

int wo_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

void wo_strcpy(char *d, const char *s, int cap)
{
    int i = 0;
    if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void wo_strcat(char *d, const char *s, int cap)
{
    int n = wo_strlen(d);
    if (n < cap) wo_strcpy(d + n, s, cap - n);
}

void wo_catu(char *d, uint32_t v, int cap)
{
    char b[12]; int i = 11;
    b[11] = 0;
    do { b[--i] = (char)('0' + v % 10u); v /= 10u; } while (v && i > 0);
    wo_strcat(d, b + i, cap);
}

int wo_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

char wo_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

const char *wo_stristr(const char *hay, const char *needle)
{
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && wo_lower(*h) == wo_lower(*n)) { h++; n++; }
        if (!*n) return hay;
    }
    return 0;
}

void wo_cap1(char *s) { if (s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32); }

void wo_mmss(char *d, uint32_t sec, int cap)
{
    d[0] = 0;
    wo_catu(d, sec / 60u, cap);
    wo_strcat(d, (sec % 60u) < 10u ? ":0" : ":", cap);
    wo_catu(d, sec % 60u, cap);
}

void wo_hhmmss(char *d, uint32_t ms, int cap)
{
    uint32_t t = ms / 1000u, h = t / 3600u, m = (t % 3600u) / 60u, s = t % 60u;
    d[0] = 0;
    if (h) { wo_catu(d, h, cap); wo_strcat(d, ":", cap); }
    if (m < 10u) wo_strcat(d, "0", cap);
    wo_catu(d, m, cap);
    wo_strcat(d, s < 10u ? ":0" : ":", cap);
    wo_catu(d, s, cap);
}

/* ---- the string blob: byte pairs, expanded on a small stack */
static int expand(uint8_t c, char *out, int pos, int cap)
{
    uint8_t st[48]; int sp = 0;
    st[sp++] = c;
    while (sp) {
        uint8_t x = st[--sp];
        if (x >= 32u && x <= 126u) {
            if (pos < cap - 1) out[pos++] = (char)x;
        } else if (sp < 46) {
            st[sp++] = WO_BPE[x][1];
            st[sp++] = WO_BPE[x][0];
        }
    }
    return pos;
}

int wo_bpe_decode(const uint8_t *in, char *out, int cap)
{
    int pos = 0, i = 0;
    while (in[i]) { pos = expand(in[i], out, pos, cap); i++; }
    out[pos] = 0;
    return i + 1;
}

void wo_str(uint16_t idx, char *out, int cap)
{
    if (idx >= WO_NSTR) { out[0] = 0; return; }
    wo_bpe_decode(WO_STRBLOB + WO_STROFF[idx], out, cap);
}

/* encode with the same pairs, in the order they were learnt (codes 1..31, then 127..255) */
int wo_bpe_encode(const char *s, uint8_t *out, int cap)
{
    uint8_t buf[WO_LINE + 8];
    int n = 0, c, i, j;
    while (s[n] && n < WO_LINE + 7) { buf[n] = (uint8_t)s[n]; n++; }
    for (c = 1; c < 256; c++) {
        uint8_t a, b;
        if (c == 32) c = 127;
        a = WO_BPE[c][0]; b = WO_BPE[c][1];
        if (!a) continue;
        for (i = 0, j = 0; i < n; ) {
            if (i + 1 < n && buf[i] == a && buf[i + 1] == b) { buf[j++] = (uint8_t)c; i += 2; }
            else buf[j++] = buf[i++];
        }
        n = j;
    }
    if (n + 1 > cap) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = buf[i];
    out[n] = 0;
    return n + 1;
}
