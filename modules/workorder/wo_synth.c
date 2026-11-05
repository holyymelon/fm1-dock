/* SPDX-License-Identifier: GPL-3.0-only */
/* The sound: the sketch's four tracks (drums, bass, chords, lead) and the keys. A melodic track is
 * 2-operator FM, or a saw or square through a resonant filter; the drums are eight voices in two kits
 * (FM, analog). The sequencer's clock runs here, in the audio interrupt, so its steps land on the
 * sample; its MIDI goes out through a queue the main loop empties (wo_synth_midi). The main loop
 * reaches the voices only through a queue the other way: they are render()'s alone.
 * Floats in the general registers (module.cfg's flags); no double, no division by a variable in
 * the inner loops. */
#include "wo.h"
#include "wo_synth.h"

#define FS 44100.0f
#define SUBN 16u                       /* frames per control block */
#define TR_KEYS 4u                     /* the keys' own sound, away from the sketch */

/* ================================================================ small maths */
static float exp2f_(float x)            /* 2^x, good to ~1e-4 */
{
    union { float f; uint32_t u; } s;
    int i;
    float f, r;
    if (x < -60.0f) return 0.0f;
    if (x > 60.0f) x = 60.0f;
    i = (int)x;
    if ((float)i > x) i--;
    f = x - (float)i;
    r = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    s.u = (uint32_t)(i + 127) << 23;
    return r * s.f;
}
static float kdecay(float tau) { return tau <= 0.0f ? 0.0f : exp2f_(-(float)SUBN * 1.442695f / (FS * tau)); }

static int32_t sine(uint32_t ph)        /* sine.c's refined parabola, -32767..32767 */
{
    int32_t x = (int32_t)ph >> 16, ax = x < 0 ? -x : x, y, ay;
    y = (x * (32768 - ax)) >> 13;
    ay = y < 0 ? -y : y;
    y += ((((y * ay) >> 15) - y) * 7373) >> 15;
    return y > 32767 ? 32767 : y < -32767 ? -32767 : y;
}

static const uint32_t INC_C9[12] = {
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
static uint32_t inc_of(int midi)
{
    int oct;
    if (midi < 0 || midi > 127) return 0;
    oct = midi / 12;
    if (oct > 10) oct = 10;
    return INC_C9[midi % 12] >> (10 - oct);
}

/* the tables of times: decay 30 ms .. 6.8 s, release a third of it, the modulator's 10 ms .. 2.3 s */
static float DK[32], RK[32], MK[32];
static const uint16_t RATIO_Q8[10] = {128, 256, 384, 512, 768, 1024, 1280, 1536, 1792, 2048};

/* ================================================================ the sounds a fresh sketch starts with */
const sq_track_t WO_TRACK_DEF[TR_N] = {
    {ENG_FM, {16, 16, 16, 16}, 26, 9, 0},          /* drums: the FM kit, MIDI 10 */
    {ENG_SAW, {9, 14, 18, 13}, 26, 0, 0},          /* bass: saw, low cutoff, MIDI 1 */
    {ENG_FM, {1, 7, 13, 21}, 17, 1, 0},            /* chords: soft FM, MIDI 2 */
    {ENG_SQUARE, {15, 9, 12, 15}, 21, 2, 0},       /* lead: square, MIDI 3 */
};
static const sq_track_t AUDITION = {ENG_FM, {1, 6, 14, 18}, 24, 0, 0};
const sq_drum_t WO_DRUM_DEF = {16, 16, 24};
const uint8_t WO_DRUM_GM[SQ_LANES] = {36, 38, 42, 46, 39, 45, 50, 37};

/* ================================================================ queues */
enum { CM_ON, CM_OFF, CM_ALLOFF, CM_DRUM, CM_START, CM_STOP };
#define NQ 64u
static volatile uint8_t q_op[NQ], q_tr[NQ], q_a[NQ], q_b[NQ];
static volatile uint32_t qh, qt;        /* the main loop moves qh, render() qt */
static void put(int op, int tr, int a, int b)
{
    uint32_t h = qh;
    if (h - qt >= NQ) return;
    q_op[h % NQ] = (uint8_t)op; q_tr[h % NQ] = (uint8_t)tr; q_a[h % NQ] = (uint8_t)a; q_b[h % NQ] = (uint8_t)b;
    qh = h + 1u;
}

#define NM 256u
static volatile uint32_t mq[NM];
static volatile uint32_t mh, mt;        /* render() moves mh, the main loop mt */
static void mout(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t h = mh;
    if (h - mt >= NM) return;
    mq[h % NM] = st | (d1 & 127u) << 8 | (d2 & 127u) << 16;
    mh = h + 1u;
}
void wo_synth_midi(void (*send)(uint32_t, uint32_t, uint32_t))
{
    while (mt != mh) {
        uint32_t m = mq[mt % NM];
        mt = mt + 1u;
        if (send) send(m & 255u, m >> 8 & 127u, m >> 16 & 127u);
    }
}

/* ================================================================ state the two sides share */
static volatile int32_t master = 9;                   /* 0..10 */
static const wo_song_t *volatile snd_song;            /* where the tracks' sounds come from */
static const wo_song_t *volatile next_song;           /* what CM_START plays */
static wo_basics_t next_b;                            /* written before CM_START, read by it */
static volatile int sq_on, sq_edit, sq_ppat, sq_pstep, sq_pfrac, sq_ci_out;
static volatile uint8_t sq_skip[TR_N];                /* 1 + a step not to sound once (just played live) */
static volatile uint8_t sq_dskip[SQ_LANES];           /* the same, per drum */
static int play_req;                                  /* the main loop's view: playing, or asked to */

/* ================================================================ melodic voices */
typedef struct {
    uint32_t ph, ph2, inc;
    float amp, menv, lp, bp, vel;
    uint8_t on, gate, att, tr, note, steps, seq, midi;
    uint32_t age;
} mv_t;
#define V_BASS 0
#define V_LEAD 1
#define V_CH0 2
#define V_CHN 6
#define V_KEY0 8
#define NMV 16
static mv_t mv[NMV];
static uint32_t ages;

/* render() only: a key's or the sequencer's song */
static const wo_song_t *cur_song(void) { return sq_on ? next_song : snd_song; }
static const sq_track_t *patch(int tr)
{
    const wo_song_t *s = cur_song();
    if (tr >= (int)TR_N) return &AUDITION;
    return s && s->div ? &s->tr[tr] : &WO_TRACK_DEF[tr];
}
static uint32_t chan_of(int tr) { const sq_track_t *p = patch(tr); return p->ch & 15u; }

static void mv_on(mv_t *v, int tr, int note, int vel, int steps, int seq)
{
    if (!v->on) { v->ph = 0; v->ph2 = 0; v->amp = 0.0f; v->lp = v->bp = 0.0f; }
    v->inc = inc_of(note);
    v->note = (uint8_t)note; v->tr = (uint8_t)tr;
    v->vel = (float)(vel > 127 ? 127 : vel) * (1.0f / 127.0f);
    v->menv = 1.0f;
    v->gate = 1; v->att = 1; v->on = 1;
    v->steps = (uint8_t)steps; v->seq = (uint8_t)seq;
    v->age = ++ages;
    v->midi = 0;
    if (tr < (int)TR_N && (seq || tr != TR_DRUMS)) { mout(0x90u | chan_of(tr), (uint32_t)note, (uint32_t)vel); v->midi = 1; }
}
static void mv_off(mv_t *v)
{
    if (!v->gate) return;
    v->gate = 0;
    if (v->midi) { mout(0x80u | chan_of(v->tr), v->note, 0); v->midi = 0; }
}

static void mv_run(mv_t *v, float *mix)
{
    const sq_track_t *p = patch(v->tr);
    uint32_t i, ph = v->ph, ph2 = v->ph2, inc = v->inc;
    float amp = v->amp, g, sus = 0.38f;
    int dec = p->p[3] & 31;
    /* the envelope, once a block: a quick attack, a decay to the sustain, a release */
    if (v->gate) {
        if (v->att) { amp += 0.3f; if (amp >= 1.0f) { amp = 1.0f; v->att = 0; } }
        else amp = sus + (amp - sus) * DK[dec];
    } else {
        amp *= RK[dec];
        if (amp < 0.0004f) { v->on = 0; v->amp = 0.0f; return; }
    }
    v->menv *= MK[p->p[2] & 31];
    g = amp * v->vel * (float)(p->vol & 31) * (0.30f / 31.0f);
    if (p->eng == ENG_FM) {
        uint32_t inc2 = (uint32_t)(((uint64_t)inc * RATIO_Q8[p->p[0] % 10u]) >> 8);
        uint32_t dq = (uint32_t)((float)(p->p[1] & 31) * v->menv * (170000.0f / 31.0f));
        for (i = 0; i < SUBN; i++) {
            uint32_t off;
            ph += inc; ph2 += inc2;
            off = (uint32_t)sine(ph2) * dq;
            mix[i] += (float)sine(ph + off) * (1.0f / 32767.0f) * g;
        }
    } else {
        /* the filter: cutoff in octaves above 30 Hz, the envelope opens it further */
        float oct = (float)(p->p[0] & 31) * 0.27f + (float)(p->p[2] & 31) * v->menv * 0.22f;
        float fc = 30.0f * exp2f_(oct), f, qd, lp = v->lp, bp = v->bp;
        if (fc > 7000.0f) fc = 7000.0f;
        f = fc * (6.2831853f / FS);
        qd = 2.0f - (float)(p->p[1] & 31) * 0.06f;
        for (i = 0; i < SUBN; i++) {
            float x, hp;
            ph += inc;
            if (p->eng == ENG_SAW) x = (float)((int32_t)ph >> 16) * (1.0f / 32768.0f);
            else x = (ph & 0x80000000u) ? 0.7f : -0.7f;
            lp += f * bp;
            hp = x - lp - qd * bp;
            bp += f * hp;
            if (lp > 4.0f) lp = 4.0f; else if (lp < -4.0f) lp = -4.0f;
            if (bp > 4.0f) bp = 4.0f; else if (bp < -4.0f) bp = -4.0f;
            mix[i] += lp * g;
        }
        v->lp = lp; v->bp = bp;
    }
    v->ph = ph; v->ph2 = ph2; v->amp = amp;
}

/* ================================================================ drums */
typedef struct {
    uint16_t f0; uint8_t pamt10, pdec_ms, kind, tl, nl, nkind, nq10; uint16_t nfc, dec_ms; uint8_t bursts; uint16_t mr100; uint8_t md10;
} dp_t;
enum { DT_SINE, DT_FM, DT_SQ, DT_BELL };
enum { DN_NONE, DN_LP, DN_HP, DN_BP };
/* kick, snare, closed hat, open hat, clap, low tom, high tom, perc */
static const dp_t KIT[2][SQ_LANES] = {
    {   /* FM */
        {52, 50, 28, DT_SINE, 100, 6, DN_LP, 10, 1800, 320, 0, 0, 0},
        {185, 16, 14, DT_FM, 55, 75, DN_HP, 10, 1700, 170, 0, 160, 6},
        {440, 10, 10, DT_FM, 25, 80, DN_HP, 12, 7200, 45, 0, 343, 25},
        {440, 10, 10, DT_FM, 25, 80, DN_HP, 12, 7200, 330, 0, 343, 25},
        {1000, 10, 10, DT_SINE, 0, 100, DN_BP, 18, 1200, 190, 3, 0, 0},
        {98, 18, 60, DT_FM, 100, 8, DN_LP, 10, 900, 260, 0, 100, 3},
        {158, 18, 55, DT_FM, 100, 8, DN_LP, 10, 1200, 220, 0, 100, 3},
        {820, 12, 8, DT_FM, 90, 0, DN_NONE, 10, 1000, 80, 0, 148, 12},
    },
    {   /* analog */
        {48, 40, 40, DT_SINE, 100, 10, DN_LP, 10, 400, 420, 0, 0, 0},
        {175, 13, 20, DT_SQ, 45, 85, DN_BP, 8, 2600, 190, 0, 0, 0},
        {400, 10, 10, DT_SQ, 0, 90, DN_HP, 30, 8500, 50, 0, 0, 0},
        {400, 10, 10, DT_SQ, 0, 90, DN_HP, 30, 8500, 360, 0, 0, 0},
        {1000, 10, 10, DT_SINE, 0, 100, DN_BP, 25, 1000, 210, 4, 0, 0},
        {92, 15, 80, DT_SINE, 100, 12, DN_LP, 10, 600, 300, 0, 0, 0},
        {146, 15, 70, DT_SINE, 100, 12, DN_LP, 10, 800, 260, 0, 0, 0},
        {560, 10, 10, DT_BELL, 80, 0, DN_NONE, 10, 1000, 140, 0, 150, 0},
    }};

typedef struct {
    uint32_t ph, ph2, rnd;
    float inc0, amp, ak, penv, pk, pamt, tl, nl, lp, bp, f, qd, lp1, lev, mratio, md;
    uint8_t on, kind, nkind, bursts, burst_left;
} dv_t;
static dv_t dv[SQ_LANES];

static void drum_on(int lane, float vel)
{
    const wo_song_t *s = cur_song();
    const sq_drum_t *u = s && s->div ? &s->dr[lane] : &WO_DRUM_DEF;
    int kit = s && s->div && s->tr[TR_DRUMS].eng ? 1 : 0;
    const dp_t *pr = &KIT[kit][lane];
    dv_t *d = &dv[lane];
    float tune = exp2f_(((float)(u->tune & 31) - 16.0f) * (1.0f / 16.0f));
    float decm = exp2f_(((float)(u->decay & 31) - 16.0f) * 0.125f);
    float fc;
    d->inc0 = (float)pr->f0 * tune * (4294967296.0f / FS);
    d->ph = 0; d->ph2 = 0;
    if (!d->rnd) d->rnd = 22222u + (uint32_t)lane * 7919u;
    d->amp = 1.0f;
    d->ak = kdecay((float)pr->dec_ms * 0.001f * decm * 0.5f);
    d->pk = kdecay((float)pr->pdec_ms * 0.001f * 0.5f);
    d->penv = 1.0f;
    d->pamt = (float)pr->pamt10 * 0.1f - 1.0f;
    d->tl = (float)pr->tl * 0.01f;
    d->nl = (float)pr->nl * 0.01f;
    d->lp = d->bp = d->lp1 = 0.0f;
    fc = (float)pr->nfc * tune;
    if (fc > 9000.0f) fc = 9000.0f;
    d->f = fc * (6.2831853f / FS);
    if (d->f > 1.2f) d->f = 1.2f;
    d->qd = 10.0f / (float)(pr->nq10 ? pr->nq10 : 10);
    if (d->qd < 0.15f) d->qd = 0.15f;
    d->lev = vel * (float)(u->level & 31) * (0.42f / 31.0f);
    d->kind = pr->kind; d->nkind = pr->nkind;
    d->mratio = (float)pr->mr100 * 0.01f;
    d->md = (float)pr->md10 * 0.1f * (4294967296.0f / 6.2831853f / 32767.0f);
    d->bursts = pr->bursts;
    d->burst_left = d->bursts ? 30u : 0u;
    d->on = 1;
    if (lane == 2) dv[3].on = 0;                           /* the closed hat chokes the open one */
}

static void drum_run(dv_t *d, float *mix, float g)
{
    uint32_t i, ph = d->ph, ph2 = d->ph2, rnd = d->rnd, inc, inc2;
    float lp = d->lp, bp = d->bp, lp1 = d->lp1, f = d->f, qd = d->qd, amp, tl = d->tl, nl = d->nl;
    float da;
    if (d->burst_left && !--d->burst_left && d->bursts) { d->bursts--; d->amp = 1.0f; d->burst_left = d->bursts ? 30u : 0u; }
    amp = d->amp;
    d->amp *= d->ak;
    if (d->burst_left) d->amp = amp * 0.86f;               /* a clap's quick bursts */
    d->penv *= d->pk;
    if (amp < 0.0003f) { d->on = 0; return; }
    inc = (uint32_t)(d->inc0 * (1.0f + d->pamt * d->penv));
    inc2 = (uint32_t)((float)inc * d->mratio);
    da = (d->amp - amp) * (1.0f / (float)SUBN);
    g *= d->lev;
    for (i = 0; i < SUBN; i++) {
        float s = 0.0f, nz, hp, n = 0.0f;
        ph += inc;
        switch (d->kind) {
        case DT_SINE: s = (float)sine(ph) * (1.0f / 32767.0f); break;
        case DT_FM: {
            uint32_t off;
            ph2 += inc2;
            off = (uint32_t)sine(ph2) * (uint32_t)(d->md * amp);
            s = (float)sine(ph + off) * (1.0f / 32767.0f);
            break; }
        case DT_SQ: lp1 += (((ph & 0x80000000u) ? 0.8f : -0.8f) - lp1) * 0.3f; s = lp1; break;
        default:
            ph2 += inc2;
            lp1 += (((ph & 0x80000000u) ? 0.4f : -0.4f) + ((ph2 & 0x80000000u) ? 0.4f : -0.4f) - lp1) * 0.35f;
            s = lp1;
            break;
        }
        if (d->nkind) {
            rnd = rnd * 1664525u + 1013904223u;
            nz = (float)((int32_t)rnd >> 8) * (1.0f / 8388608.0f);
            lp += f * bp;
            hp = nz - lp - qd * bp;
            bp += f * hp;
            if (lp > 4.0f) lp = 4.0f; else if (lp < -4.0f) lp = -4.0f;
            if (bp > 4.0f) bp = 4.0f; else if (bp < -4.0f) bp = -4.0f;
            n = d->nkind == DN_LP ? lp : d->nkind == DN_HP ? hp : bp;
        }
        mix[i] += (s * tl + n * nl) * amp * g;
        amp += da;
    }
    d->ph = ph; d->ph2 = ph2; d->rnd = rnd; d->lp = lp; d->bp = bp; d->lp1 = lp1;
}

/* ================================================================ the sequencer, in render() */
static wo_basics_t sb;                  /* what CM_START copied */
static int sq_pat, sq_step, sq_ci;
static float sq_t, sq_dur, sq_spp, sq_clk_t, sq_clk_step;
static uint8_t drum_midi;               /* lanes with a MIDI note out, sent off at the next step */

static int pat_len(const wo_song_t *s, int p)
{
    int l = s->pat[p].len;
    return l < 1 ? 16 : l > SQ_STEPS ? SQ_STEPS : l;
}

static void seq_release(int midi_too)
{
    int k, l;
    for (k = 0; k < V_KEY0; k++) if (mv[k].seq) { mv_off(&mv[k]); mv[k].seq = 0; }
    if (midi_too) for (l = 0; l < SQ_LANES; l++) if (drum_midi >> l & 1u) mout(0x80u | chan_of(TR_DRUMS), WO_DRUM_GM[l], 0);
    drum_midi = 0;
}

static void fire(void)
{
    const wo_song_t *s = next_song;
    const sq_pat_t *p;
    int st, l, t, k;
    float sw;
    if (!s) return;
    p = &s->pat[sq_pat];
    st = sq_step;
    /* the notes whose length ran out */
    for (k = 0; k < V_KEY0; k++) if (mv[k].on && mv[k].seq && mv[k].gate && mv[k].steps && !--mv[k].steps) mv_off(&mv[k]);
    for (l = 0; l < SQ_LANES; l++) if (drum_midi >> l & 1u) mout(0x80u | chan_of(TR_DRUMS), WO_DRUM_GM[l], 0);
    drum_midi = 0;
    if (!s->tr[TR_DRUMS].mute)
        for (l = 0; l < SQ_LANES; l++) {
            int acc;
            if (!(p->hit[l] >> st & 1u) || (s->lanemute >> l & 1u)) continue;
            if (sq_dskip[l] == st + 1) { sq_dskip[l] = 0; continue; }
            acc = p->acc[l] >> st & 1u;
            drum_on(l, acc ? 1.0f : 0.68f);
            mout(0x90u | chan_of(TR_DRUMS), WO_DRUM_GM[l], acc ? 120u : 90u);
            drum_midi |= (uint8_t)(1u << l);
        }
    for (t = 0; t < 3; t++) {
        const sq_note_t *n = &p->n[t][st];
        int tr = t + 1;
        if (!n->note || s->tr[tr].mute) continue;
        if (sq_skip[tr] == st + 1) { sq_skip[tr] = 0; continue; }
        if (tr == TR_CHORDS) {
            int ci = n->note - 1, i, base = 48 + sb.ch.root, nv = 0;
            if (ci >= sb.ch.n) continue;
            for (k = V_CH0; k < V_CH0 + V_CHN; k++) if (mv[k].seq) { mv_off(&mv[k]); mv[k].seq = 0; }
            for (i = 0; i < sb.ch.c[ci].npc && nv < V_CHN; i++, nv++)
                mv_on(&mv[V_CH0 + nv], TR_CHORDS, base + sb.ch.c[ci].tone[i], n->vel, n->len ? n->len : 1, 1);
        } else {
            mv_t *v = &mv[tr == TR_BASS ? V_BASS : V_LEAD];
            if (v->seq) mv_off(v);
            mv_on(v, tr, n->note, n->vel, n->len ? n->len : 1, 1);
        }
    }
    sq_ppat = sq_pat; sq_pstep = st; sq_ci_out = sq_ci;
    /* how long this step lasts: swing lengthens the on-beats and shortens the off-beats */
    sw = (float)(s->swing > 10 ? 10 : s->swing) * 0.033f;
    sq_dur = sq_spp * ((st & 1) ? 1.0f - sw : 1.0f + sw);
    if (++sq_step >= pat_len(s, sq_pat)) {
        sq_step = 0;
        if (s->chain_on && s->nchain) { sq_ci = (sq_ci + 1) % s->nchain; sq_pat = s->chain[sq_ci] % SQ_PATS; }
        else sq_pat = sq_edit % SQ_PATS;
    }
}

static void seq_start(void)
{
    const wo_song_t *s = next_song;
    int bpm;
    if (!s) return;
    sb = next_b;
    bpm = sb.bpm < 30 ? 90 : sb.bpm > 300 ? 300 : sb.bpm;
    sq_spp = FS * 60.0f / ((float)bpm * (float)(s->div ? s->div : 4));
    sq_clk_step = FS * 60.0f / ((float)bpm * 24.0f);
    sq_step = 0; sq_ci = 0;
    sq_pat = s->chain_on && s->nchain ? s->chain[0] % SQ_PATS : sq_edit % SQ_PATS;
    sq_t = 0.0f; sq_clk_t = 0.0f;
    sq_skip[0] = sq_skip[1] = sq_skip[2] = sq_skip[3] = 0;
    { int l; for (l = 0; l < SQ_LANES; l++) sq_dskip[l] = 0; }
    if (s->clock) mout(0xFAu, 0, 0);
    sq_on = 1;
}
static void seq_stop(void)
{
    const wo_song_t *s = next_song;
    if (!sq_on) return;
    seq_release(1);
    if (s && s->clock) mout(0xFCu, 0, 0);
    sq_on = 0;
    sq_pstep = -1;
}

/* ================================================================ render() */
static void commands(void)
{
    while (qt != qh) {
        uint32_t i = qt % NQ;
        int op = q_op[i], tr = q_tr[i], a = q_a[i], b = q_b[i], k, pick = -1;
        qt = qt + 1u;
        switch (op) {
        case CM_ON: {
            uint32_t best = 0xFFFFFFFFu;
            for (k = V_KEY0; k < NMV && pick < 0; k++) if (mv[k].on && mv[k].gate && !mv[k].seq && mv[k].note == a && mv[k].tr == tr) pick = k;
            for (k = V_KEY0; k < NMV && pick < 0; k++) if (!mv[k].on) pick = k;
            if (pick < 0) for (k = V_KEY0; k < NMV; k++) if (mv[k].age < best) { best = mv[k].age; pick = k; }
            if (mv[pick].gate) mv_off(&mv[pick]);
            mv_on(&mv[pick], tr, a, b, 0, 0);
            break; }
        case CM_OFF:
            for (k = V_KEY0; k < NMV; k++) if (mv[k].on && !mv[k].seq && mv[k].note == a) mv_off(&mv[k]);
            break;
        case CM_ALLOFF: for (k = V_KEY0; k < NMV; k++) mv_off(&mv[k]); break;
        case CM_DRUM:
            if (a >= SQ_LANES) break;
            drum_on(a, (float)b * (1.0f / 127.0f));
            mout(0x90u | chan_of(TR_DRUMS), WO_DRUM_GM[a], (uint32_t)b);      /* a drum is a hit: off straight after */
            mout(0x80u | chan_of(TR_DRUMS), WO_DRUM_GM[a], 0);
            break;
        case CM_START: seq_stop(); seq_start(); break;
        case CM_STOP: seq_stop(); break;
        default: break;
        }
    }
}

/* how long render() takes, against the 5.8 ms of a 256-frame block (the host's 24 MHz counter) */
static uint32_t (*volatile tick_fn)(void);
static volatile uint32_t load_last, load_peak;
void wo_synth_ticks(uint32_t (*ticks)(void)) { tick_fn = ticks; }
void wo_synth_load(int *last, int *peak)
{
    if (last) *last = (int)load_last;
    if (peak) { *peak = (int)load_peak; load_peak = load_last; }
}

void wo_synth_render(int32_t *out, uint32_t n)
{
    uint32_t blk, i, k, t0 = tick_fn ? tick_fn() : 0u;
    float mix[SUBN], gm = (float)master * 0.1f;
    commands();
    for (blk = 0; blk < n; blk += SUBN) {
        const wo_song_t *s = next_song;
        float dg;
        for (i = 0; i < SUBN; i++) mix[i] = 0.0f;
        if (sq_on && s) {
            sq_t -= (float)SUBN;
            while (sq_t <= 0.0f) { fire(); sq_t += sq_dur > 1.0f ? sq_dur : 1.0f; }
            sq_pfrac = (int)(256.0f * (1.0f - sq_t / (sq_dur > 1.0f ? sq_dur : 1.0f)));
            if (s->clock) { sq_clk_t -= (float)SUBN; while (sq_clk_t <= 0.0f) { mout(0xF8u, 0, 0); sq_clk_t += sq_clk_step; } }
        }
        for (k = 0; k < NMV; k++) if (mv[k].on) mv_run(&mv[k], mix);
        s = cur_song();
        dg = s && s->div ? (s->tr[TR_DRUMS].mute && sq_on ? 0.0f : (float)(s->tr[TR_DRUMS].vol & 31) * (1.0f / 26.0f)) : 1.0f;
        for (k = 0; k < SQ_LANES; k++) if (dv[k].on) drum_run(&dv[k], mix, dg);
        for (i = 0; i < SUBN && blk + i < n; i++) {
            float y = mix[i] * gm * 0.62f;
            int32_t v;
            /* a soft knee instead of a hard clip */
            if (y > 1.0f) y = 1.0f; else if (y < -1.0f) y = -1.0f;
            y = y * (1.5f - 0.5f * y * y);
            v = (int32_t)(y * 30000.0f);
            out[2u * (blk + i)] += v;
            out[2u * (blk + i) + 1u] += v;
        }
    }
    if (tick_fn && n) {                 /* percent of the block's time: dt / (n frames at 44.1 kHz) */
        uint32_t dt = tick_fn() - t0, pc = dt / (n * 544u / 100u + 1u);
        load_last = pc;
        if (pc > load_peak) load_peak = pc;
    }
}

/* ================================================================ the main loop's side */
void wo_synth_init(void)
{
    int d;
    for (d = 0; d < 32; d++) {
        float t = 0.03f * exp2f_((float)d * 0.25f), r = t * 0.33f;
        if (r > 0.9f) r = 0.9f;
        if (r < 0.012f) r = 0.012f;
        DK[d] = kdecay(t);
        RK[d] = kdecay(r);
        MK[d] = kdecay(0.01f * exp2f_((float)d * 0.25f));
    }
}

void wo_synth_volume(int vol) { master = vol < 0 ? 0 : vol > 10 ? 10 : vol; }
void wo_synth_song(const wo_song_t *s) { snd_song = s; }

static int keys_tr = TR_KEYS;
void wo_synth_keys_track(int tr) { keys_tr = tr < 0 || tr >= (int)TR_N ? (int)TR_KEYS : tr; }
int wo_synth_get_keys_track(void) { return keys_tr == (int)TR_KEYS ? -1 : keys_tr; }

void wo_synth_note(int note, int vel)
{
    if (note < 0 || note > 127) return;
    if (vel <= 0) put(CM_OFF, keys_tr, note, 0);
    else put(CM_ON, keys_tr, note, vel > 127 ? 127 : vel);
}
void wo_synth_track_note(int tr, int note, int vel)
{
    if (note < 0 || note > 127) return;
    if (vel <= 0) put(CM_OFF, tr, note, 0);
    else put(CM_ON, tr, note, vel > 127 ? 127 : vel);
}
void wo_synth_drum(int lane, int vel) { if (lane >= 0 && lane < SQ_LANES) put(CM_DRUM, 0, lane, vel > 127 ? 127 : vel < 1 ? 1 : vel); }
void wo_synth_all_off(void) { put(CM_ALLOFF, 0, 0, 0); }

int wo_synth_busy(void)
{
    int k;
    if (sq_on) return 1;
    for (k = 0; k < NMV; k++) if (mv[k].on) return 1;
    for (k = 0; k < SQ_LANES; k++) if (dv[k].on) return 1;
    return 0;
}

/* the transport */
void wo_seq_play(const wo_song_t *s, const wo_basics_t *b, int pat)
{
    if (!s || !b) return;
    next_b = *b;
    sq_edit = pat;
    next_song = s;
    put(CM_START, 0, 0, 0);
    play_req = 1;
}

void wo_seq_stop(void) { if (play_req) put(CM_STOP, 0, 0, 0); play_req = 0; }
int wo_seq_playing(void) { return play_req; }
void wo_seq_edit_pat(int pat) { sq_edit = pat; }

/* where it is: the pattern and step it last sounded, how far into the step (0..255), the chain slot */
int wo_seq_pos(int *pat, int *step, int *frac, int *slot)
{
    if (!play_req || !sq_on || sq_pstep < 0) return 0;
    if (pat) *pat = sq_ppat;
    if (step) *step = sq_pstep;
    if (frac) *frac = sq_pfrac < 0 ? 0 : sq_pfrac > 255 ? 255 : sq_pfrac;
    if (slot) *slot = sq_ci_out;
    return 1;
}
void wo_seq_skip(int tr, int step) { if (tr > 0 && tr < (int)TR_N) sq_skip[tr] = (uint8_t)(step + 1); }
void wo_seq_skip_drum(int lane, int step) { if (lane >= 0 && lane < SQ_LANES) sq_dskip[lane] = (uint8_t)(step + 1); }

/* ================================================================ the keys, in the scale */
static int key_root = 0;
static uint16_t key_mask = 0x0FFFu;        /* bit pc above the root */
static int oct_shift = 0;
static int8_t key_note[27];                 /* what each key is playing now */

void wo_synth_scale(int root, uint16_t mask) { key_root = root < 0 ? 0 : root % 12; key_mask = mask ? mask : 0x0FFFu; }
void wo_synth_octave(int d) { oct_shift += d; if (oct_shift < -2) oct_shift = -2; if (oct_shift > 2) oct_shift = 2; }
int wo_synth_get_octave(void) { return oct_shift; }

int wo_synth_in_scale(int key)
{
    int midi = 53 + key, rel = ((midi - key_root) % 12 + 12) % 12;
    return (key_mask >> rel) & 1;
}
int wo_synth_snap_midi(int midi)           /* to the scale note at or below */
{
    int k;
    for (k = 0; k < 12; k++) {
        int rel = ((midi - k - key_root) % 12 + 12) % 12;
        if (key_mask >> rel & 1) return midi - k;
    }
    return midi;
}
int wo_synth_snap(int key) { return wo_synth_snap_midi(53 + key) + 12 * oct_shift; }

void wo_synth_key(int key, int down, int vel)
{
    if (key < 0 || key > 26) return;
    if (down) {
        if (key_note[key]) wo_synth_note(key_note[key], 0);
        key_note[key] = (int8_t)wo_synth_snap(key);
        wo_synth_note(key_note[key], vel ? vel : 90);
    } else if (key_note[key]) {
        int n = key_note[key], k, still = 0;
        key_note[key] = 0;
        for (k = 0; k < 27; k++) if (key_note[k] == n) still = 1;   /* two keys snapped to one note */
        if (!still) wo_synth_note(n, 0);
    }
}
