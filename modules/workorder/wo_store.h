/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WO_STORE_H
#define WO_STORE_H
#include "wo.h"
typedef struct {
    int (*load)(uint32_t idx, void *p, uint32_t n);   /* bytes loaded, or -1 */
    int (*save)(uint32_t idx, const void *p, uint32_t n);  /* 0 saved */
} wo_banks_t;
extern wo_banks_t WB;
#define ST_STATE  1u
#define ST_CUSTOM 2u
#define ST_CUR    4u
#define ST_DRAFT  8u
#define ST_SCORES 16u
#define ST_LIB    32u
#define ST_ALL    63u
void wo_store_load_all(void);
void wo_store_mark(uint32_t what);
int wo_store_flush(void);
int wo_store_poll(int quiet);
uint32_t wo_store_failed(void);
void wo_store_wipe(void);
int wo_park(void);
int wo_unpark(int slot);
void wo_discard_parked(int slot);
int wo_parked_count(void);
int wo_parked_slot(int k);
const char *wo_parked_name(int slot);
uint32_t wo_parked_elapsed(int slot);
int wo_lib_count(void);
int wo_lib_get(int idx, wo_ticket_t *out);
void wo_lib_delete(int idx);
#endif
