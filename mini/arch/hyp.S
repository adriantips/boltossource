/* ===========================================================================
 *  BoltOS Mini  -  mini/arch/hyp.S
 *
 *  The hypervisor's own code: all of what runs at EL2. U-Boot starts Mini
 *  at EL2 and Mini drops itself to EL1 (start.S), but first it points EL2's
 *  vectors here. BoltOS stays an EL1 kernel; when it wants to run the
 *  guest (vm.c) it asks this code with `hvc`, the way Linux's KVM does on
 *  a CPU with no VHE, like the A53:
 *
 *    HYP_RUN    BoltOS's EL1 registers out, the guest's in, stage 2 on,
 *               eret into the guest. When the guest traps -- an MMIO
 *               access, a hypercall, a physical interrupt -- its registers
 *               out, BoltOS's back, and the hvc returns why.
 *    HYP_INIT   EL2's own identity map with caches on, so this code and
 *               BoltOS see the same memory through the same caches.
 *    HYP_TLBI   forget a VM's stage-2 translations.
 *    HYP_PING   is there a hypervisor here at all.
 *
 *  It never uses a stack. On the way in, SP_EL2 holds x0 for a moment while
 *  TPIDR_EL2 says which way: 0 is BoltOS calling, anything else is the
 *  context of the guest that was running.
 * ===========================================================================*/
#include "vmctx.h"

.macro SAVE_SYS base
    mrs     x2, sctlr_el1
    mrs     x3, ttbr0_el1
    stp     x2, x3, [\base, #0]
    mrs     x2, ttbr1_el1
    mrs     x3, tcr_el1
    stp     x2, x3, [\base, #16]
    mrs     x2, mair_el1
    mrs     x3, amair_el1
    stp     x2, x3, [\base, #32]
    mrs     x2, vbar_el1
    mrs     x3, contextidr_el1
    stp     x2, x3, [\base, #48]
    mrs     x2, tpidr_el0
    mrs     x3, tpidrro_el0
    stp     x2, x3, [\base, #64]
    mrs     x2, tpidr_el1
    mrs     x3, cpacr_el1
    stp     x2, x3, [\base, #80]
    mrs     x2, esr_el1
    mrs     x3, far_el1
    stp     x2, x3, [\base, #96]
    mrs     x2, afsr0_el1
    mrs     x3, afsr1_el1
    stp     x2, x3, [\base, #112]
    mrs     x2, par_el1
    mrs     x3, cntkctl_el1
    stp     x2, x3, [\base, #128]
    mrs     x2, csselr_el1
    mrs     x3, mdscr_el1
    stp     x2, x3, [\base, #144]
.endm

.macro LOAD_SYS base
    ldp     x2, x3, [\base, #0]
    msr     sctlr_el1, x2
    msr     ttbr0_el1, x3
    ldp     x2, x3, [\base, #16]
    msr     ttbr1_el1, x2
    msr     tcr_el1, x3
    ldp     x2, x3, [\base, #32]
    msr     mair_el1, x2
    msr     amair_el1, x3
    ldp     x2, x3, [\base, #48]
    msr     vbar_el1, x2
    msr     contextidr_el1, x3
    ldp     x2, x3, [\base, #64]
    msr     tpidr_el0, x2
    msr     tpidrro_el0, x3
    ldp     x2, x3, [\base, #80]
    msr     tpidr_el1, x2
    msr     cpacr_el1, x3
    ldp     x2, x3, [\base, #96]
    msr     esr_el1, x2
    msr     far_el1, x3
    ldp     x2, x3, [\base, #112]
    msr     afsr0_el1, x2
    msr     afsr1_el1, x3
    ldp     x2, x3, [\base, #128]
    msr     par_el1, x2
    msr     cntkctl_el1, x3
    ldp     x2, x3, [\base, #144]
    msr     csselr_el1, x2
    msr     mdscr_el1, x3
.endm

.macro SAVE_FP base
    stp     q0, q1, [\base, #0]
    stp     q2, q3, [\base, #32]
    stp     q4, q5, [\base, #64]
    stp     q6, q7, [\base, #96]
    stp     q8, q9, [\base, #128]
    stp     q10, q11, [\base, #160]
    stp     q12, q13, [\base, #192]
    stp     q14, q15, [\base, #224]
    stp     q16, q17, [\base, #256]
    stp     q18, q19, [\base, #288]
    stp     q20, q21, [\base, #320]
    stp     q22, q23, [\base, #352]
    stp     q24, q25, [\base, #384]
    stp     q26, q27, [\base, #416]
    stp     q28, q29, [\base, #448]
    stp     q30, q31, [\base, #480]
.endm

.macro LOAD_FP base
    ldp     q0, q1, [\base, #0]
    ldp     q2, q3, [\base, #32]
    ldp     q4, q5, [\base, #64]
    ldp     q6, q7, [\base, #96]
    ldp     q8, q9, [\base, #128]
    ldp     q10, q11, [\base, #160]
    ldp     q12, q13, [\base, #192]
    ldp     q14, q15, [\base, #224]
    ldp     q16, q17, [\base, #256]
    ldp     q18, q19, [\base, #288]
    ldp     q20, q21, [\base, #320]
    ldp     q22, q23, [\base, #352]
    ldp     q24, q25, [\base, #384]
    ldp     q26, q27, [\base, #416]
    ldp     q28, q29, [\base, #448]
    ldp     q30, q31, [\base, #480]
.endm

/* An exception from a lower EL. x0 goes into SP for a moment; x1 = kind. */
.macro LOWER kind
    .balign 0x80
    mov     sp, x0
    mrs     x0, tpidr_el2
    cbz     x0, host_call
    str     x1, [x0, #(CTX_X + 1) * 8]
    mov     x1, sp
    str     x1, [x0, #CTX_X * 8]
    mov     x1, #\kind
    b       guest_exit
.endm

.macro BAD
    .balign 0x80
    b       hyp_bad
.endm

    .section .text
    .balign 0x800
    .globl hyp_vectors
hyp_vectors:
    BAD                         /* EL2 with SP_EL0                         */
    BAD
    BAD
    BAD
    BAD                         /* EL2 with SP_EL2: this code never faults */
    BAD
    BAD
    BAD
    LOWER EXIT_SYNC             /* EL1/EL0, AArch64                        */
    LOWER EXIT_IRQ
    LOWER EXIT_FIQ
    LOWER EXIT_SERROR
    BAD                         /* AArch32                                 */
    BAD
    BAD
    BAD

/* Something at EL2 went wrong: leave what and where for vm.c to report, and
 * stop this core. */
hyp_bad:
    mrs     x0, esr_el2
    mrs     x1, elr_el2
    mrs     x2, far_el2
    ldr     x3, =hyp_fault
    stp     x0, x1, [x3]
    str     x2, [x3, #16]
1:  wfe
    b       1b

/* ---- BoltOS calling (x0 = function, in SP) ------------------------------- */
host_call:
    mov     x0, sp
    movz    x2, #0x0000
    movk    x2, #0x4B56, lsl #16
    cmp     x0, x2
    b.eq    1f
    add     x2, x2, #1
    cmp     x0, x2
    b.eq    run_guest
    add     x2, x2, #1
    cmp     x0, x2
    b.eq    2f
    add     x2, x2, #1
    cmp     x0, x2
    b.eq    3f
    mov     x0, #-1
    eret
1:  mov     x0, #0x4B56                     /* HYP_PING                       */
    eret
2:  msr     vttbr_el2, x1                   /* HYP_TLBI                       */
    isb
    tlbi    vmalls12e1is
    dsb     ish
    isb
    msr     vttbr_el2, xzr
    isb
    mov     x0, #0
    eret
3:  ldr     x2, =0x44FF04                   /* HYP_INIT: MAIR as EL1's        */
    msr     mair_el2, x2
    ldr     x2, =0x80803520                 /* TCR: 4 GiB, 4K, WB, inner      */
    msr     tcr_el2, x2
    msr     ttbr0_el2, x1
    dsb     ish
    tlbi    alle2
    dsb     ish
    isb
    mrs     x2, sctlr_el2
    ldr     x3, =0x1005                     /* M, C, I                        */
    orr     x2, x2, x3
    msr     sctlr_el2, x2
    isb
    mov     x0, #0
    eret

/* ---- into the guest (x1 = its context) ------------------------------------- */
run_guest:
    mov     x0, x1
    add     x2, x0, #CTX_HX * 8
    stp     x18, x19, [x2, #0]
    stp     x20, x21, [x2, #16]
    stp     x22, x23, [x2, #32]
    stp     x24, x25, [x2, #48]
    stp     x26, x27, [x2, #64]
    stp     x28, x29, [x2, #80]
    str     x30, [x2, #96]
    mrs     x3, sp_el0
    mrs     x4, sp_el1
    stp     x3, x4, [x2, #104]
    mrs     x3, elr_el1
    mrs     x4, spsr_el1
    stp     x3, x4, [x2, #120]
    mrs     x3, elr_el2
    mrs     x4, spsr_el2
    stp     x3, x4, [x2, #136]
    mrs     x3, fpsr
    mrs     x4, fpcr
    stp     x3, x4, [x2, #152]
    add     x5, x0, #CTX_H_SYS * 8
    SAVE_SYS x5
    add     x5, x0, #CTX_HQ * 8
    SAVE_FP x5

    ldr     x2, [x0, #CTX_VTCR * 8]
    msr     vtcr_el2, x2
    ldr     x2, [x0, #CTX_VTTBR * 8]
    msr     vttbr_el2, x2
    ldr     x2, [x0, #CTX_VMPIDR * 8]
    msr     vmpidr_el2, x2
    ldr     x2, [x0, #CTX_CNTHCTL * 8]
    msr     cnthctl_el2, x2
    ldr     x2, [x0, #CTX_HCR * 8]
    msr     hcr_el2, x2

    add     x5, x0, #CTX_SYS * 8
    LOAD_SYS x5
    ldp     x3, x4, [x0, #CTX_SP_EL0 * 8]
    msr     sp_el0, x3
    msr     sp_el1, x4
    ldp     x3, x4, [x0, #CTX_ELR_EL1 * 8]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x3, x4, [x0, #CTX_FPSR * 8]
    msr     fpsr, x3
    msr     fpcr, x4
    add     x5, x0, #CTX_Q * 8
    LOAD_FP x5
    ldp     x3, x4, [x0, #CTX_PC * 8]
    msr     elr_el2, x3
    msr     spsr_el2, x4
    msr     tpidr_el2, x0
    isb
    ldp     x2, x3, [x0, #16]
    ldp     x4, x5, [x0, #32]
    ldp     x6, x7, [x0, #48]
    ldp     x8, x9, [x0, #64]
    ldp     x10, x11, [x0, #80]
    ldp     x12, x13, [x0, #96]
    ldp     x14, x15, [x0, #112]
    ldp     x16, x17, [x0, #128]
    ldp     x18, x19, [x0, #144]
    ldp     x20, x21, [x0, #160]
    ldp     x22, x23, [x0, #176]
    ldp     x24, x25, [x0, #192]
    ldp     x26, x27, [x0, #208]
    ldp     x28, x29, [x0, #224]
    ldr     x30, [x0, #240]
    ldp     x0, x1, [x0, #0]
    eret

/* ---- out of the guest (x0 = its context, x1 = kind; its x0 and x1 saved) ---- */
guest_exit:
    str     x1, [x0, #CTX_KIND * 8]
    stp     x2, x3, [x0, #16]
    stp     x4, x5, [x0, #32]
    stp     x6, x7, [x0, #48]
    stp     x8, x9, [x0, #64]
    stp     x10, x11, [x0, #80]
    stp     x12, x13, [x0, #96]
    stp     x14, x15, [x0, #112]
    stp     x16, x17, [x0, #128]
    stp     x18, x19, [x0, #144]
    stp     x20, x21, [x0, #160]
    stp     x22, x23, [x0, #176]
    stp     x24, x25, [x0, #192]
    stp     x26, x27, [x0, #208]
    stp     x28, x29, [x0, #224]
    str     x30, [x0, #240]
    mrs     x2, elr_el2
    mrs     x3, spsr_el2
    stp     x2, x3, [x0, #CTX_PC * 8]
    mrs     x2, sp_el0
    mrs     x3, sp_el1
    stp     x2, x3, [x0, #CTX_SP_EL0 * 8]
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x2, x3, [x0, #CTX_ELR_EL1 * 8]
    mrs     x2, fpsr
    mrs     x3, fpcr
    stp     x2, x3, [x0, #CTX_FPSR * 8]
    mrs     x2, esr_el2
    str     x2, [x0, #CTX_ESR * 8]
    mrs     x2, far_el2
    str     x2, [x0, #CTX_FAR * 8]
    mrs     x2, hpfar_el2
    str     x2, [x0, #CTX_HPFAR * 8]
    add     x5, x0, #CTX_SYS * 8
    SAVE_SYS x5
    add     x5, x0, #CTX_Q * 8
    SAVE_FP x5

    mov     x2, #(1 << 31)                  /* HCR: BoltOS's, nothing trapped */
    msr     hcr_el2, x2
    msr     vttbr_el2, xzr
    mov     x2, #3
    msr     cnthctl_el2, x2
    mrs     x2, mpidr_el1
    msr     vmpidr_el2, x2
    add     x5, x0, #CTX_H_SYS * 8
    LOAD_SYS x5
    add     x5, x0, #CTX_HQ * 8
    LOAD_FP x5
    add     x2, x0, #CTX_HX * 8
    ldp     x3, x4, [x2, #104]
    msr     sp_el0, x3
    msr     sp_el1, x4
    ldp     x3, x4, [x2, #120]
    msr     elr_el1, x3
    msr     spsr_el1, x4
    ldp     x3, x4, [x2, #136]
    msr     elr_el2, x3
    msr     spsr_el2, x4
    ldp     x3, x4, [x2, #152]
    msr     fpsr, x3
    msr     fpcr, x4
    ldp     x18, x19, [x2, #0]
    ldp     x20, x21, [x2, #16]
    ldp     x22, x23, [x2, #32]
    ldp     x24, x25, [x2, #48]
    ldp     x26, x27, [x2, #64]
    ldp     x28, x29, [x2, #80]
    ldr     x30, [x2, #96]
    msr     tpidr_el2, xzr
    ldr     x0, [x0, #CTX_KIND * 8]
    isb
    eret

/* ---- BoltOS's side: plain functions, so the compiler saves what hvc may clobber */
    .globl hyp_call
hyp_call:                       /* uint64_t hyp_call(uint64_t fn, uint64_t arg) */
    hvc     #0
    ret

    .section .bss
    .balign 8
    .globl hyp_fault
hyp_fault:
    .space  24
