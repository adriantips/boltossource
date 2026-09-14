#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/sleep.h
 *  Suspend to RAM (ACPI S3), and coming back from it.
 *
 *  S3 is the one power state that is genuinely different from the others.
 *  Shutting down (S5) is easy because nothing has to work afterwards. In S3
 *  the RAM is refreshed and everything else is not: when the machine wakes,
 *  the CPU is in 16-bit real mode with paging off, long mode off, no usable
 *  GDT, every device back at its power-on defaults, and a kernel image sitting
 *  in memory that believes none of that happened.
 *
 *  So the work splits in three:
 *
 *    getting out   -- save what the C world needs to resume (a jmp_buf is
 *                     exactly the right shape for this), leave a real-mode
 *                     address in the FACS for the firmware to jump to, quiesce
 *                     anything doing DMA, and write SLP_TYP|SLP_EN.
 *    getting back  -- kernel/s3_wake.asm climbs real -> protected -> long mode
 *                     using the page tables that survived in RAM, and calls
 *                     into C.
 *    putting it right -- re-run the initialisation of everything that forgot:
 *                     the GDT and IDT, the interrupt controllers, the timers,
 *                     the syscall MSRs, the PS/2 controller, the video mode.
 *
 *  That last one is re-initialisation rather than register restoration on
 *  purpose. Every subsystem here already has an init() that puts it in a known
 *  state from nothing, which is exactly the situation after a wake, and a
 *  second path that restores a saved copy of the same registers is a second
 *  thing to keep correct.
 *
 *  --- what does NOT come back, stated rather than discovered ---------------
 *  The application processors. They were asleep too and need the full
 *  INIT-SIPI-SIPI again; nothing here restarts them, so a resumed machine runs
 *  on the boot processor alone until it is rebooted. On this kernel that costs
 *  nothing functional -- the APs idle in a hlt loop and no thread is ever
 *  scheduled onto one -- and it would be dishonest not to say so.
 * ========================================================================= */

/* 1 if this machine declares S3 and there is somewhere to leave a waking
 * vector. When it returns 0, sleep_why() says which of those is missing. */
int         sleep_s3_ok(void);
const char *sleep_why(void);

/* Go to sleep. Returns 0 once the machine has woken and been put back
 * together, or -1 without sleeping if it could not. It does not return
 * "later" -- from the caller's point of view this is one slow function. */
int sleep_suspend(void);

/* How many times this boot has been through the above. `power` prints it,
 * which is the cheapest way to tell a resumed machine from a rebooted one. */
uint32_t sleep_count(void);
