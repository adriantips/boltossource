#pragma once
#include <stdint.h>

/* BoltOS Mini: the desktop's PIT is a 1 kHz tick; Mini's timer is too. */
uint64_t now_ms(void);
static inline uint64_t pit_ticks(void) { return now_ms(); }
static inline uint32_t pit_hz(void) { return 1000; }
