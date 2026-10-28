/* BoltOS Mini - mini/hid.h: HID report descriptors, and reports turned into input (mini/hid.c). */
#pragma once
#include <stdint.h>

#define HID_MAXEL 96

typedef struct {
    uint8_t  rid, flags, size, count;    /* flags: 1 variable (else an array), 2 relative */
    uint16_t page, usage, umax;          /* array: usage .. umax                          */
    uint16_t bit;                        /* where in the report, after its ID             */
    int32_t  lmin, lmax;
} hid_el_t;

typedef struct {
    hid_el_t el[HID_MAXEL];
    int      nel, has_ids;
    int      keyboard, mouse, gamepad;   /* what it has                                   */
} hid_map_t;

typedef struct {
    uint8_t  keys[16], nkeys, mods;
    int      dirs;                       /* gamepad: bit 0 up, 1 down, 2 left, 3 right      */
    uint32_t buttons;
    int      rep_key, rep_ch, rep_mods;
    uint64_t rep_at;
} hid_state_t;

int  hid_parse(hid_map_t *m, const uint8_t *desc, int len);   /* 0 if it found something usable */
/* A report as the device sent it (with its ID first if the descriptor has IDs, unless
 * `rid` names it: LE delivers each report ID on its own characteristic). */
void hid_report(const hid_map_t *m, hid_state_t *st, int rid, const uint8_t *r, int len, int source);
void hid_repeat(hid_state_t *st, int source);                  /* held keys and directions */
