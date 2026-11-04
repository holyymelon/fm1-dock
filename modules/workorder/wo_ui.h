/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WO_UI_H
#define WO_UI_H
#include "hb_module.h"
void wo_ui_init(const hb_host_t *h);
void wo_ui_event(uint32_t ev, int32_t arg);
void wo_ui_draw(uint32_t full);
void wo_ui_release(void);
#endif
