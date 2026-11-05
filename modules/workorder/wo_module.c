/* SPDX-License-Identifier: GPL-3.0-only */
/* WORKORDER as an fm1-dock module: the whole screen and panel are its own (HB_F_SCREEN, HB_F_PANEL).
 * Built on its own as a solo firmware (tools/build.py --modules ... --name workorder), or added to
 * fm1-dock beside the synths. */
#include "hb_module.h"
#include "wo.h"
#include "wo_store.h"
#include "wo_synth.h"
#include "wo_ui.h"

static const hb_host_t *H;

static uint32_t ms(void) { return H->ms(); }

static int bank_load(uint32_t idx, void *p, uint32_t n)
{
    if (idx == 0) return n >= HB_BANK_MAX && H->bank_load(p, HB_BANK_MAX) == 0 ? (int)HB_BANK_MAX : -1;
    if (!HB_HOST_HAS(H, bank_load_n)) return -1;
    return H->bank_load_n(idx, p, n);
}

static int bank_save(uint32_t idx, const void *p, uint32_t n)
{
    if (idx == 0) {                     /* the first bank always the same size, so a load of that size fits */
        static uint8_t pad[HB_BANK_MAX];
        const uint8_t *q = p; uint32_t i;
        if (n > HB_BANK_MAX) return -1;
        for (i = 0; i < HB_BANK_MAX; i++) pad[i] = i < n ? q[i] : 0u;
        return H->bank_save(pad, HB_BANK_MAX);
    }
    if (!HB_HOST_HAS(H, bank_save_n)) return -1;
    return H->bank_save_n(idx, p, n) < 0 ? -1 : 0;
}

static int init(const hb_host_t *h)
{
    if (h->abi < 2u) return 1;
    H = h;
    wo_now = ms;
    WB.load = bank_load;
    WB.save = bank_save;
    wo_seed(h->rand() ^ (h->ticks() << 7));
    wo_defaults();
    wo_synth_init();
    wo_synth_ticks(h->ticks);
    wo_store_load_all();
    wo_ui_init(h);
    return 0;
}

static void release(void) { wo_ui_release(); wo_synth_midi(H->midi_send); }

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    if (W.set.sound) wo_synth_note((int)key, (int)vel);   /* MIDI in plays as it comes */
}

static void cc(uint32_t ch, uint32_t num, uint32_t val) { (void)ch; (void)num; (void)val; }

static void render(int32_t *out, uint32_t n) { wo_synth_render(out, n); }

static void ui(uint32_t ev, int32_t arg) { wo_ui_event(ev, arg); }

static void draw(uint32_t full) { wo_ui_draw(full); }

HB_DESC = {
    HB_ABI, "WORKORDER", init, release, note, cc, render, ui, draw, 0, 0,
    HB_F_SCREEN | HB_F_PANEL, 0, 0
};
