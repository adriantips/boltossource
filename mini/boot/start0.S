/* ===========================================================================
 *  BoltBoot  -  mini/boot/start0.S                                (AArch64)
 *
 *  boot0 as the core sees it after start32.S's warm reset: AArch64, EL3,
 *  MMU and caches off, at BOOT0_ENTRY64. The 4 KiB 32-bit blob comes first
 *  (it is what the ROM jumped into), then the 4 KiB the ROM's FEL mode keeps
 *  its stacks in, left as zeros and never written, then this.
 * ===========================================================================*/
#include "boot.h"

    .section .text.head, "ax"
    .globl _start
_start:
    .incbin START32_BIN             /* 0x20000: header + AArch32 stubs      */
    .space  0x1000                  /* 0x21000: the ROM's, while in FEL     */

entry64:                            /* 0x22000 = BOOT0_ENTRY64              */
    ldr     x0, =BOOT0_STACK_TOP
    mov     sp, x0
    msr     daifset, #0xF           /* no interrupts, no aborts taken       */
    mrs     x0, sctlr_el3
    orr     x0, x0, #(1 << 12)      /* I-cache on: fetches from SRAM, faster */
    msr     sctlr_el3, x0
    isb

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
1:  cmp     x1, x2
    b.hs    2f
    str     xzr, [x1], #8
    b       1b
2:
    bl      boot0_main
3:  wfi
    b       3b
    .ltorg

/* Back to the ROM's FEL loop: reset the core into AArch32 with the hotplug
 * mailbox pointing at back_in_32, which restores what start32 saved. Only
 * the core resets: SRAM, the DRAM controller and every other block keep
 * what boot0 did to them, so the Mac can read the results. */
    .text
    .globl return_to_fel
return_to_fel:
    ldr     x2, =HOTPLUG_FLAG
    ldr     w0, =HOTPLUG_MAGIC
    str     w0, [x2]
    ldr     x2, =HOTPLUG_ENTRY
    ldr     w0, =SRAM_BASE + OFF_BACK_IN_32
    str     w0, [x2]
    ic      iallu
    dsb     sy
    isb
    mov     x0, #2                  /* RR, AArch32                          */
    msr     rmr_el3, x0
    isb
4:  wfi
    b       4b
    .ltorg
