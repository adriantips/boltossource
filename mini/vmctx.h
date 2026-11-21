/* ===========================================================================
 *  BoltOS Mini  -  mini/vmctx.h
 *
 *  One virtual CPU's registers, as the EL2 stub (arch/hyp.S) and the VM
 *  (vm.c) both see them: an array of 64-bit words, these the indices. The
 *  guest's half is everything EL1 and EL0 can see; the host's half is what
 *  BoltOS needs back when the guest stops: the registers a C function
 *  keeps, its stack, its MMU and vectors, and the FP/SIMD file.
 * ===========================================================================*/
#pragma once

#define CTX_X          0            /* x0..x30                                   */
#define CTX_PC         31           /* ELR_EL2: where the guest goes on          */
#define CTX_PSTATE     32           /* SPSR_EL2                                  */
#define CTX_SP_EL0     33
#define CTX_SP_EL1     34
#define CTX_ELR_EL1    35
#define CTX_SPSR_EL1   36
#define CTX_SYS        37           /* NSYS EL1 system registers, hyp.S's order  */
#define NSYS           20
#define CTX_FPSR       57
#define CTX_FPCR       58
#define CTX_ESR        59           /* why it stopped: ESR_EL2, FAR_EL2, HPFAR_EL2 */
#define CTX_FAR        60
#define CTX_HPFAR      61
#define CTX_KIND       62           /* EXIT_*                                    */
#define CTX_HCR        63           /* the machine it runs on                    */
#define CTX_VTTBR      64
#define CTX_VTCR       65
#define CTX_VMPIDR     66
#define CTX_CNTHCTL    67
#define CTX_HX         68           /* host x18..x30: 13 words                   */
#define CTX_H_SP_EL0   81
#define CTX_H_SP_EL1   82
#define CTX_H_ELR_EL1  83
#define CTX_H_SPSR_EL1 84
#define CTX_H_ELR2     85
#define CTX_H_SPSR2    86
#define CTX_H_FPSR     87
#define CTX_H_FPCR     88
#define CTX_H_SYS      89           /* NSYS                                      */
#define CTX_Q          110          /* guest q0..q31: 64 words, 16-byte aligned  */
#define CTX_HQ         174          /* host q0..q31                              */
#define CTX_WORDS      238

/* hvc #0 from BoltOS, function in x0 */
#define HYP_PING       0x4B560000   /* answers 0x4B56                            */
#define HYP_RUN        0x4B560001   /* x1 = context: run the guest until it stops */
#define HYP_TLBI       0x4B560002   /* x1 = VTTBR: forget that VM's translations */
#define HYP_INIT       0x4B560003   /* x1 = EL2's page table: MMU and caches on  */

#define EXIT_SYNC      0            /* a trap: HVC, SMC, an MMIO access, ...     */
#define EXIT_IRQ       1            /* a physical interrupt                      */
#define EXIT_FIQ       2
#define EXIT_SERROR    3
