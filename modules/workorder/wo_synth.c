/* SPDX-License-Identifier: GPL-3.0-only */
/* Audition: hear the roll. The keys play only the rolled scale (a key outside it falls to the
 * scale note below), PLAY loops the rolled chords at the rolled tempo, one bar each, with the
 * chord's root in the bass. A soft two-partial tone with a decay: a sketch, not an instrument.
 * render() runs in the audio interrupt; everything else in the main loop. Integers only. */
#include "wo.h"
#include "wo_synth.h"

#define NV 10u
static struct {
    volatile uint32_t inc;
    volatile int32_t gate;          /* target level, Q15; 0 = released */
    volatile uint32_t trig;         /* bumped by the main loop: start the attack */
    uint32_t seen, ph;
    int32_t amp, peak;
    uint8_t note, held;
    uint32_t age;
} v[NV];
static volatile int32_t master = 9;       /* 0..10 */
static uint32_t ages;

static int32_t sine(uint32_t ph)          /* sine.c's refined parabola, -32767..32767 */
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
    int oct = midi / 12;
    if (midi < 0 || midi > 127) return 0;
    if (oct > 10) oct = 10;
    return INC_C9[midi % 12] >> (10 - oct);
}

void wo_synth_render(int32_t *out, uint32_t n)
{
    uint32_t i, k, blk;
    int32_t gm = master * 3276;             /* Q15 */
    for (k = 0; k < NV; k++) {
        uint32_t ph = v[k].ph, step = v[k].inc;
        int32_t amp = v[k].amp, g = v[k].gate, peak = v[k].peak;
        if (v[k].trig != v[k].seen) { v[k].seen = v[k].trig; peak = g; }
        if (!amp && !g) { v[k].amp = 0; continue; }
        for (blk = 0; blk < n; blk += 16u) {
            /* envelope once per 16 frames: quick attack to the peak, decay towards half, release */
            if (g) {
                if (peak) { amp += 2400; if (amp >= peak) { amp = peak; peak = 0; } }
                else { int32_t floor_ = g >> 1; if (amp > floor_) amp -= (amp - floor_) >> 7; }
            } else {
                amp -= (amp >> 5) + 8;
                if (amp < 0) amp = 0;
            }
            for (i = blk; i < blk + 16u && i < n; i++) {
                int32_t s;
                ph += step;
                s = sine(ph) + (sine(ph << 1) >> 2) + (sine(ph * 3u) >> 4);
                s = (s * amp) >> 15;
                s = (s * gm) >> 15;
                s >>= 2;                    /* headroom for ten voices */
                out[2u * i] += s;
                out[2u * i + 1u] += s;
            }
        }
        v[k].ph = ph; v[k].amp = amp; v[k].peak = peak;
    }
}

void wo_synth_volume(int vol) { master = vol < 0 ? 0 : vol > 10 ? 10 : vol; }

/* a voice: the same note again, a free one, or the oldest */
static int voice_for(int note)
{
    uint32_t best = 0xFFFFFFFFu; int k, pick = 0;
    for (k = 0; k < (int)NV; k++) if (v[k].held && v[k].note == note) return k;
    for (k = 0; k < (int)NV; k++) if (!v[k].gate && !v[k].amp) return k;
    for (k = 0; k < (int)NV; k++) if (v[k].age < best) { best = v[k].age; pick = k; }
    return pick;
}

void wo_synth_note(int note, int vel)
{
    int k;
    if (vel <= 0) {
        for (k = 0; k < (int)NV; k++) if (v[k].held && v[k].note == note) { v[k].gate = 0; v[k].held = 0; }
        return;
    }
    k = voice_for(note);
    v[k].inc = inc_of(note);                /* the pitch before the gate */
    v[k].note = (uint8_t)note;
    v[k].held = 1;
    v[k].age = ++ages;
    v[k].gate = 12000 + vel * 150;
    v[k].trig++;
}

void wo_synth_all_off(void)
{
    int k;
    for (k = 0; k < (int)NV; k++) { v[k].gate = 0; v[k].held = 0; }
}

int wo_synth_busy(void)
{
    int k;
    for (k = 0; k < (int)NV; k++) if (v[k].gate || v[k].amp > 64) return 1;
    return 0;
}

/* ---------------------------------------------------------------- the keys, in the rolled scale */
static int key_root = 0;
static uint16_t key_mask = 0x0FFFu;        /* bit pc above the root */
static int oct_shift = 0;
static int8_t key_note[27];                 /* what each key is playing now */

void wo_synth_scale(int root, uint16_t mask) { key_root = root < 0 ? 0 : root; key_mask = mask ? mask : 0x0FFFu; }
void wo_synth_octave(int d) { oct_shift += d; if (oct_shift < -2) oct_shift = -2; if (oct_shift > 2) oct_shift = 2; }
int wo_synth_get_octave(void) { return oct_shift; }

int wo_synth_in_scale(int key)
{
    int midi = 53 + key, rel = ((midi - key_root) % 12 + 12) % 12;
    return (key_mask >> rel) & 1;
}

int wo_synth_snap(int key)                 /* key 0 = F3 */
{
    int midi = 53 + key, k;
    for (k = 0; k < 12; k++) {
        int rel = ((midi - k - key_root) % 12 + 12) % 12;
        if (key_mask >> rel & 1) return midi - k + 12 * oct_shift;
    }
    return midi + 12 * oct_shift;
}

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

/* ---------------------------------------------------------------- the progression */
static wo_chords_t prog;
static int prog_on, prog_step, prog_bpm = 90, prog_notes[8], prog_n;
static uint32_t prog_next;

static void prog_release(void)
{
    int i;
    for (i = 0; i < prog_n; i++) wo_synth_note(prog_notes[i], 0);
    prog_n = 0;
}

static void prog_sound(uint32_t now)
{
    const wo_chord_t *c = &prog.c[prog_step % prog.n];
    int i, base = 48 + prog.root;
    prog_release();
    prog_notes[prog_n++] = 36 + c->root;               /* the chord's root, low */
    for (i = 0; i < c->npc && prog_n < 8; i++) prog_notes[prog_n++] = base + c->tone[i];
    for (i = 0; i < prog_n; i++) wo_synth_note(prog_notes[i], i == 0 ? 80 : 64);
    prog_next = now + 4u * 60000u / (uint32_t)prog_bpm;
}

int wo_synth_playing(void) { return prog_on; }
int wo_synth_step(void) { return prog_on ? prog_step % (prog.n ? prog.n : 1) : -1; }

void wo_synth_play(const wo_chords_t *c, int bpm, uint32_t now)
{
    if (prog_on || !c || c->n < 1) { prog_on = 0; prog_release(); return; }
    prog = *c;
    prog_bpm = bpm < 40 ? 90 : bpm;
    prog_step = 0;
    prog_on = 1;
    prog_sound(now);
}

void wo_synth_stop(void) { if (prog_on) { prog_on = 0; prog_release(); } }

void wo_synth_poll(uint32_t now)
{
    if (!prog_on || (int32_t)(now - prog_next) < 0) return;
    prog_step++;
    prog_sound(now);
}

int wo_synth_chord_note(int pc)            /* is pc in the chord sounding now (for the key LEDs) */
{
    const wo_chord_t *c;
    int i;
    if (!prog_on) return 0;
    c = &prog.c[prog_step % prog.n];
    for (i = 0; i < c->npc; i++) if (c->pcs[i] == pc) return 1;
    return 0;
}
