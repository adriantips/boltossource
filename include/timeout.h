#pragma once
#include <stdint.h>

/* A monotonic microsecond clock, for drivers that have to wait on hardware.
 *
 * Every wait in this kernel used to be a spin count -- `for (t = 0; t < 5000000;
 * t++) if (ready) break;`. Under QEMU that is free, because the emulated device
 * is ready on the first read and the loop never runs a second time. On real
 * hardware an MMIO or port-I/O read costs around a microsecond, so the same
 * loop is five seconds of nothing, and a machine with two dozen root ports and
 * a handful of absent drives spends minutes looking dead. Worse, a spin count
 * says nothing about what it is willing to wait for, so the number can never be
 * checked against the spec that set the deadline.
 *
 * So: waits are expressed in microseconds and bounded by a real clock. The
 * clock is the TSC, calibrated once against PIT channel 2, which needs no
 * interrupts, no ACPI and no HPET and so works everywhere and this early. */

void     timeout_init(void);    /* calibrate; call once, right after pit_init */
uint64_t now_us(void);          /* microseconds since timeout_init()          */
void     delay_us(uint64_t us); /* busy-wait                                  */
uint64_t tsc_hz(void);          /* 0 if the TSC could not be calibrated       */

/* Spin until `cond` holds or `us` microseconds pass. Evaluates to 1 if the
 * condition came true, 0 if it timed out -- so a caller can tell "the device
 * answered" from "the device never did", which a bare spin count cannot.
 *
 * `cond` is re-read every pass, so it must be a volatile read or a call.
 *
 * The clock is sampled *before* the condition, and that order is the whole
 * point rather than an accident. Whoever is polling can lose the CPU between
 * the two -- a vCPU thread the host deschedules, a core an SMI takes away for
 * seconds -- and if the condition were read first, a device that went ready
 * during the stall would be failed by a deadline that expired after it. Read
 * the clock first and the last look at the device is always newer than the
 * deadline that condemned it. Getting this backwards made an 18,599-sector
 * disk write fail once in every few runs, on a disk that was fine. */
#define wait_until(cond, us)                                        \
    ({ uint64_t _deadline = now_us() + (uint64_t)(us);              \
       int _ok = 0;                                                 \
       for (;;) {                                                   \
           int _expired = now_us() >= _deadline;                    \
           if (cond) { _ok = 1; break; }                            \
           if (_expired) break;                                     \
           __asm__ volatile("pause");                               \
       }                                                            \
       _ok; })
