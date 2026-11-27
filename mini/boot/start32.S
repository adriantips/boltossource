/* ===========================================================================
 *  BoltBoot  -  mini/boot/start32.S          (ARM32: --target=armv7a-none-eabi)
 *
 *  The first 4 KiB of boot0, which the H618's boot ROM runs in AArch32 at
 *  0x20000. Assembled on its own into a flat 4 KiB blob that start0.S puts
 *  at the front of the AArch64 image, so these addresses are fixed.
 *
 *    0x000  the eGON header the ROM checks (mkboot.py fills in the sum and
 *           the length)
 *    0x060  start32: save what FEL mode needs to carry on later, point
 *           RVBAR at boot0's AArch64 entry and warm-reset the core into
 *           AArch64. The core comes back at EL3, MMU and caches off.
 *    0x400  back_in_32: where the ROM jumps after boot0 resets the core
 *           back into AArch32 (the "hotplug" mailbox): put FEL's registers
 *           back and return to it, as if boot0 were an ordinary function
 *           the ROM had called.
 *    0x800  the FEL stash those registers are kept in
 * ===========================================================================*/
#include "boot.h"

    .syntax unified
    .arm
    .text
    .globl _head
_head:
    b       start32                 /* the ROM jumps to the first word        */
    .ascii  "eGON.BT0"
    .long   0x5F0A6C39              /* check_sum (mkboot.py)                  */
    .long   0                       /* length    (mkboot.py)                  */
    .ascii  "BOLT"                  /* 0x14: ours                             */
    .long   0                       /* 0x18: flags (pi.py fel)                */
    .long   0                       /* 0x1C                                   */
    .long   0                       /* 0x20                                   */
    .long   0                       /* 0x24: dram size in MiB (boot0 fills)   */
    .long   0                       /* 0x28: boot media (the ROM fills)       */

    .org    OFF_START32
start32:
    ldr     r0, =SRAM_BASE + OFF_FEL_STASH
    str     sp, [r0]
    str     lr, [r0, #4]
    mrs     r1, cpsr
    str     r1, [r0, #8]
    mrc     p15, 0, r1, c1, c0, 0   /* SCTLR                                  */
    str     r1, [r0, #12]
    mrc     p15, 0, r1, c12, c0, 0  /* VBAR                                   */
    str     r1, [r0, #16]
    mrs     r1, SP_irq
    str     r1, [r0, #20]

    ldr     r1, =RVBAR_A            /* which die: RVBAR is at one of two places */
    ldr     r0, =SRAM_VER_REG
    ldr     r0, [r0]
    ands    r0, r0, #0xFF
    ldrne   r1, =RVBAR_B
    ldr     r0, =BOOT0_ENTRY64
    str     r0, [r1]
    dsb     sy
    isb     sy
    mrc     p15, 0, r0, c12, c0, 2  /* RMR                                    */
    orr     r0, r0, #3              /* AA64 + RR: warm reset into AArch64     */
    mcr     p15, 0, r0, c12, c0, 2
    isb     sy
1:  wfi
    b       1b
    .ltorg

    .org    OFF_BACK_IN_32
back_in_32:
    ldr     r2, =HOTPLUG_FLAG       /* the mailbox has done its job           */
    mov     r1, #0
    str     r1, [r2]
    ldr     r0, =SRAM_BASE + OFF_FEL_STASH
    ldr     r1, [r0, #8]
    msr     cpsr_fc, r1
    isb     sy
    ldr     sp, [r0]
    ldr     lr, [r0, #4]
    ldr     r1, [r0, #20]
    msr     SP_irq, r1
    ldr     r1, [r0, #16]
    mcr     p15, 0, r1, c12, c0, 0  /* VBAR                                   */
    ldr     r1, [r0, #12]
    mcr     p15, 0, r1, c1, c0, 0   /* SCTLR                                  */
    isb     sy
    bx      lr                      /* back into the ROM's FEL loop           */
    .ltorg

    .org    OFF_FEL_STASH
fel_stash:
    .space  32

    .org    0x1000
