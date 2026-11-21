/* ===========================================================================
 *  BoltOS Mini  -  mini/arch/start.S
 *
 *  The first instructions. The file starts with the 64-byte header of a
 *  Linux arm64 "Image", because that is the format both of our loaders
 *  understand: U-Boot's `booti` on the Orange Pi, and QEMU's -kernel. Both
 *  put the image 2 MiB into RAM (text_offset), leave the MMU and caches off,
 *  and pass the device tree's address in x0.
 *
 *  From there: drop from EL2 to EL1 if we were entered at EL2 (U-Boot runs
 *  there; QEMU without virtualisation starts at EL1), set the stack, clear
 *  .bss, turn the MMU on -- until it is on, every access is Device memory and
 *  an unaligned one faults -- install the vectors, and call kmain(dtb).
 * ===========================================================================*/

    .section .text.head, "ax"
    .globl _start
_start:
    b       real_start              /* code0                                  */
    .long   0                       /* code1                                  */
    .quad   0x200000                /* text_offset: RAM base + 2 MiB          */
    .quad   __image_size            /* image_size, .bss included              */
    .quad   0x2                     /* flags: LE, 4K pages, fixed placement   */
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"               /* magic                                  */
    .long   0

real_start:
    mov     x19, x0                 /* the device tree                        */

    adr     x20, in_el1
    b       to_el1
in_el1:
    mov     x2, #(3 << 20)          /* CPACR_EL1.FPEN: FP/SIMD at EL1         */
    msr     cpacr_el1, x2
    isb

    ldr     x2, =__stack_top
    mov     sp, x2

    ldr     x2, =__bss_start
    ldr     x3, =__bss_end
1:  cmp     x2, x3
    b.hs    2f
    stp     xzr, xzr, [x2], #16
    b       1b
2:
    bl      mmu_init
    ldr     x2, =hyp_present        /* we came from EL2: vm.c can run a guest */
    str     w21, [x2]

    ldr     x2, =vectors
    msr     vbar_el1, x2
    isb

    mov     x0, x19
    bl      kmain
3:  wfe
    b       3b

/* Into EL1 from wherever the loader left us, then on to x20. x21 says
 * whether there was an EL2 to leave: if so its vectors are the hypervisor's
 * (hyp.S), for vm.c to call on later. */
to_el1:
    mov     x21, #0
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    9f
    ldr     x2, =hyp_vectors
    msr     vbar_el2, x2
    msr     tpidr_el2, xzr
    mov     x21, #1

    /* EL2 -> EL1. EL1 is AArch64, owns its own timer, and nothing traps.     */
    mov     x2, #(1 << 31)          /* HCR_EL2.RW                             */
    msr     hcr_el2, x2
    mrs     x2, cnthctl_el2
    orr     x2, x2, #3              /* EL1PCEN | EL1PCTEN                     */
    msr     cnthctl_el2, x2
    msr     cntvoff_el2, xzr
    mrs     x2, midr_el1
    msr     vpidr_el2, x2
    mrs     x2, mpidr_el1
    msr     vmpidr_el2, x2
    mov     x2, #0x33ff             /* CPTR_EL2: no FP/SIMD traps             */
    msr     cptr_el2, x2
    msr     hstr_el2, xzr
    ldr     x2, =0x30d00800         /* SCTLR_EL1 RES1 bits, MMU off           */
    msr     sctlr_el1, x2
    mov     x2, #0x3c5              /* EL1h, DAIF masked                      */
    msr     spsr_el2, x2
    msr     elr_el2, x20
    eret
9:  br      x20


/* ---------------------------------------------------------------------------
 *  The other cores (cpu.c, cpu_start): PSCI's CPU_ON starts one here with
 *  its number in x0, the MMU off. The same way down to EL1, a stack of its
 *  own, the boot core's page tables and vectors, then secondary_main(cpu). */
    .globl secondary_start
secondary_start:
    mov     x19, x0
    adr     x20, 1f
    b       to_el1
1:  mov     x2, #(3 << 20)
    msr     cpacr_el1, x2
    isb
    ldr     x2, =secondary_stacks
    add     x2, x2, x19, lsl #18    /* 256 KiB each: the top of core n's is n * 256 KiB in */
    mov     sp, x2
    bl      mmu_enable
    ldr     x2, =vectors
    msr     vbar_el1, x2
    isb
    mov     x0, x19
    bl      secondary_main
2:  wfe
    b       2b

/* ---------------------------------------------------------------------------
 *  Exception vectors. The "current EL, SP_ELx" group is Mini's own; the
 *  "lower EL" group is a Linux program at EL0 on a spare core (lx.c): its
 *  syscalls and its faults, kind 4. Each entry saves everything -- the general registers and
 *  all 32 SIMD registers, because the compiler is free to use them anywhere,
 *  including in code an interrupt interrupts -- and calls trap(frame, kind).
 *
 *  Frame layout (bytes): x0..x30 at 0..240, elr 248, spsr 256, fpsr 264,
 *  fpcr 272, q0..q31 at 288..799.
 * ------------------------------------------------------------------------- */
#define FRAME 800

.macro ENTRY kind
    .balign 0x80
    sub     sp, sp, #FRAME
    stp     x0, x1, [sp, #0]
    mov     x0, #\kind
    b       trap_common
.endm

    .section .bss
    .balign 4
    .globl hyp_present
hyp_present:
    .space  4

    .section .text
    .balign 0x800
    .globl vectors
vectors:
    ENTRY 0     /* current EL, SP_EL0: sync  */
    ENTRY 1     /*                     irq   */
    ENTRY 2     /*                     fiq   */
    ENTRY 3     /*                     error */
    ENTRY 0     /* current EL, SP_ELx: sync  */
    ENTRY 1
    ENTRY 2
    ENTRY 3
    ENTRY 4     /* lower EL, AArch64         */
    ENTRY 4
    ENTRY 4
    ENTRY 4
    ENTRY 4     /* lower EL, AArch32         */
    ENTRY 4
    ENTRY 4
    ENTRY 4

trap_common:
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x2, elr_el1
    stp     x30, x2, [sp, #240]
    mrs     x2, spsr_el1
    mrs     x3, fpsr
    stp     x2, x3, [sp, #256]
    mrs     x2, fpcr
    str     x2, [sp, #272]
    add     x2, sp, #288
    stp     q0, q1, [x2, #0]
    stp     q2, q3, [x2, #32]
    stp     q4, q5, [x2, #64]
    stp     q6, q7, [x2, #96]
    stp     q8, q9, [x2, #128]
    stp     q10, q11, [x2, #160]
    stp     q12, q13, [x2, #192]
    stp     q14, q15, [x2, #224]
    stp     q16, q17, [x2, #256]
    stp     q18, q19, [x2, #288]
    stp     q20, q21, [x2, #320]
    stp     q22, q23, [x2, #352]
    stp     q24, q25, [x2, #384]
    stp     q26, q27, [x2, #416]
    stp     q28, q29, [x2, #448]
    stp     q30, q31, [x2, #480]

    mov     x1, x0                  /* kind                                   */
    mov     x0, sp                  /* frame                                  */
    bl      trap

    add     x2, sp, #288
    ldp     q0, q1, [x2, #0]
    ldp     q2, q3, [x2, #32]
    ldp     q4, q5, [x2, #64]
    ldp     q6, q7, [x2, #96]
    ldp     q8, q9, [x2, #128]
    ldp     q10, q11, [x2, #160]
    ldp     q12, q13, [x2, #192]
    ldp     q14, q15, [x2, #224]
    ldp     q16, q17, [x2, #256]
    ldp     q18, q19, [x2, #288]
    ldp     q20, q21, [x2, #320]
    ldp     q22, q23, [x2, #352]
    ldp     q24, q25, [x2, #384]
    ldp     q26, q27, [x2, #416]
    ldp     q28, q29, [x2, #448]
    ldp     q30, q31, [x2, #480]
    ldr     x2, [sp, #272]
    msr     fpcr, x2
    ldp     x2, x3, [sp, #256]
    msr     spsr_el1, x2
    msr     fpsr, x3
    ldp     x30, x2, [sp, #240]
    msr     elr_el1, x2
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8, x9, [sp, #64]
    ldp     x6, x7, [sp, #48]
    ldp     x4, x5, [sp, #32]
    ldp     x2, x3, [sp, #16]
    ldp     x0, x1, [sp, #0]
    add     sp, sp, #FRAME
    eret

/* PSCI calls: the conduit depends on who is underneath. TF-A on the Orange
 * Pi answers SMC; QEMU's built-in PSCI answers HVC. */
    .globl psci_smc
psci_smc:
    smc     #0
    ret
    .globl psci_hvc
psci_hvc:
    hvc     #0
    ret
