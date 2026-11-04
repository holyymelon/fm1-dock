/* SPDX-License-Identifier: GPL-3.0-only */
/* Reading a constraint's meaning from its wording: workorder.html's TAGGERS, as plain phrase lists
 * (the four patterns with a gap or a number in them are written out by hand). Stock rules carry
 * their tags from the export; this runs on generated wording and on rules you type. */
#include "wo.h"

typedef struct { uint8_t tag; const char *const *alt; } tagger_t;

#define A(...) (const char *const[]){__VA_ARGS__, 0}
static const tagger_t TG[] = {
    {T_MONO_FINAL,   A("entire mix is mono", "mix is mono. final", "every instrument is mono", "all of them.")},
    {T_MONO_LATER,   A("mix in mono until")},
    {T_NO_EQ,        A("no eq at all", "no eq anywhere")},
    {T_EQ_LATER,     A("no eq until")},
    {T_NO_QUANT,     A("no quantiz", "no quantis", "nothing is quantiz", "played by hand", "never quantis",
                       "never corrected", "tempo drifts")},
    {T_GRID_STRICT,  A("euclidean", "quantized to triplets", "quantised to triplets", "follows e(")},
    {T_ONE_LOOP,     A("one loop, no arrangement")},
    {T_NO_REPEAT,    A("may not repeat identically", "no element may repeat")},
    {T_FIXED_FORM,   A("no deviation", "bar count is fixed", "exactly as long as")},
    {T_NO_SECTIONS,  A("single unbroken gesture", "no sections")},
    {T_FROZEN_HARM,  A("harmony is frozen", "one chord, the entire way", "a mistake you commit")},
    {T_MOVING_HARM,  A("parallel motion", "may only move by", "common tone or semitone", "chromatic mediants",
                       "same direction")},
    {T_TWO_PITCH,    A("only two pitches")},
    {T_QUARTAL,      A("stack of fourths", "quartal")},
    {T_NO_SYNTH,     A("nothing synthesized", "one sample.", "derives from one recording", "nothing digital at the source")},
    {T_NO_UNDO,      A("no undo")},
    {T_DELETE_WORK,  A("delete anything you've spent", "cannot be deleted")},   /* + delete .{0,24} and rebuild */
    {T_ONE_TAKE,     A("one take per element", "recorded in one take", "in one continuous take")},
    {T_MANY_TAKES,   A("\x01")},                                               /* by hand, below */
    {T_NO_COPY,      A("no copy and paste")},
    {T_REPEAT_3X,    A("repeat unchanged at least three times")},
    {T_VERY_SHORT,   A("under three minutes")},
    {T_MIX_LOCKED,   A("faders are set once", "mix is finished in twenty minutes", "no mix stage", "one fader move")},
    {T_ADD_ONLY,     A("density only increases", "you must add, never remove", "everything you load stays loaded")},
    {T_REMOVE_REQ,   A("must disappear and never come back", "enters once and never returns", "is removed in the")},
    {T_NO_PERC,      A("no percussion at all", "there is no kick drum", "has no percussion")},
    {T_PERC_REQ,     A("percussion from", "all percussion", "every drum sound", "the kick may not", "hats carry")},
    {T_NO_HEADPHONES,A("no headphones")},
    {T_ALL_WET,      A("printed wet", "everything is printed wet")},
};

static int isword(char c)
{
    c = wo_lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* A then, within gap characters, B */
static int gap(const char *t, const char *a, int maxgap, const char *b)
{
    const char *p = t;
    int la = wo_strlen(a);
    while ((p = wo_stristr(p, a))) {
        const char *q = p + la;
        int k;
        for (k = 0; k <= maxgap && q[k]; k++)
            if (wo_stristr(q + k, b) == q + k) return 1;
        p++;
    }
    return 0;
}

static int many_takes(const char *t)
{
    const char *p = t;
    while ((p = wo_stristr(p, "you get "))) {          /* you get \d+ attempts */
        const char *q = p + 8;
        if (*q >= '0' && *q <= '9') {
            while (*q >= '0' && *q <= '9') q++;
            if (wo_stristr(q, " attempts") == q) return 1;
        }
        p++;
    }
    p = t;
    while ((p = wo_stristr(p, "attempts at"))) {       /* \battempts at\b */
        if ((p == t || !isword(p[-1])) && !isword(p[11])) return 1;
        p++;
    }
    return 0;
}

uint32_t wo_tags_of(const char *text)
{
    uint32_t out = 0;
    unsigned i;
    for (i = 0; i < sizeof TG / sizeof TG[0]; i++) {
        const char *const *a;
        int hit = 0;
        for (a = TG[i].alt; *a && !hit; a++)
            if ((*a)[0] != '\x01' && wo_stristr(text, *a)) hit = 1;
        if (!hit && TG[i].tag == T_DELETE_WORK) hit = gap(text, "delete ", 24, " and rebuild");
        if (!hit && TG[i].tag == T_MIX_LOCKED) hit = gap(text, "mixed once,", 30, "locked");
        if (!hit && TG[i].tag == T_MANY_TAKES) hit = many_takes(text);
        if (hit) out |= 1u << TG[i].tag;
    }
    return out;
}

int wo_clashes(uint32_t tags, uint32_t have)
{
    int t;
    for (t = 0; t < WO_NTAGS; t++)
        if ((tags >> t & 1u) && (WO_INCOMPAT[t] & have)) return 1;
    return 0;
}

/* ONLY_RE: "is the only (device|instrument|processor|sound source|drum source|voice|bass source|sound)\b",
 * not followed by " for the next" */
int wo_only_claim(const char *t, char *noun, int cap)
{
    static const char *const N[] = {"device", "instrument", "processor", "sound source", "drum source", "voice",
                                    "bass source", "sound", 0};
    const char *p = t;
    while ((p = wo_stristr(p, "is the only "))) {
        const char *q = p + 12;
        int i;
        if (p > t && isword(p[-1])) { p++; continue; }
        for (i = 0; N[i]; i++) {
            int n = wo_strlen(N[i]);
            if (wo_stristr(q, N[i]) == q && !isword(q[n]) && wo_stristr(q + n, " for the next") != q + n) {
                wo_strcpy(noun, N[i], cap);
                return 1;
            }
        }
        p++;
    }
    return 0;
}
