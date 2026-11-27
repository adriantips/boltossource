/* ===========================================================================
 *  BoltBoot  -  mini/boot/boot.h
 *
 *  BoltOS Mini's own firmware for the Orange Pi Zero3 (Allwinner H618): what
 *  the boot ROM runs off the card, in place of U-Boot and TF-A.
 *
 *    boot0   in SRAM. The boot ROM loads it 8 KiB into the card (or the Mac
 *            hands it over USB, in the ROM's FEL mode), runs it in AArch32;
 *            it switches the core to AArch64 at EL3, sets up the clocks, the
 *            power chip and the DRAM, and reads boot1 off the card.
 *    boot1   in DRAM (later steps): the EL3 monitor that stays, and the boot
 *            manager that loads BOLTOS.BIN.
 *
 *  SRAM, as the boot ROM leaves it (0x20000..0x53BFF, 207 KiB):
 *
 *    0x20000  the eGON header, the AArch32 stubs, the FEL stash   (4 KiB)
 *    0x21000  left alone: the ROM's FEL mode keeps its stacks here (4 KiB)
 *    0x22000  boot0's AArch64 code and data
 *    0x50000  boot0's report: a log the Mac reads back over FEL   (4 KiB)
 *    0x52A00  top of boot0's stack
 *    0x53A00  a scratch spot for the Mac's little FEL helpers     (512 B)
 *
 *  Shared by the AArch32 stub (start32.S), the AArch64 code and, by its
 *  numbers, mini/tools/mkboot.py and mini/tools/pi.py.
 * ===========================================================================*/
#pragma once

#define SRAM_BASE          0x00020000
#define FEL_HOLE           0x00021000      /* ..0x21FFF: never written by boot0 */
#define BOOT0_ENTRY64      0x00022000
#define BOOT0_MAX          0x0000C000      /* what the ROM loads: 48 KiB at most */
#define BOOT0_REPORT       0x00050000
#define BOOT0_REPORT_SIZE  0x00001000
#define BOOT0_STACK_TOP    0x00052A00

/* In the 32-bit blob at 0x20000 (start32.S lays these out with .org). */
#define HDR_FLAGS          0x018           /* set by pi.py fel: 1 = return to FEL */
#define HDR_BOOT_MEDIA     0x028           /* written by the ROM on a card boot   */
#define OFF_START32        0x060
#define OFF_BACK_IN_32     0x400
#define OFF_FEL_STASH      0x800           /* sp, lr, cpsr, sctlr, vbar, sp_irq   */

#define BOOT_FLAG_FEL      1u

/* The H616/H618's writable copy of RVBAR, at one of two places depending on
 * the die (SRAM_VER_REG[7:0]), and the "CPU hotplug" mailbox the ROM checks
 * after a warm reset: the magic, then where to jump in AArch32. */
#define SRAM_VER_REG       0x03000024
#define RVBAR_A            0x09010040
#define RVBAR_B            0x08100040
#define R_CPUCFG           0x07000400
#define HOTPLUG_FLAG       (R_CPUCFG + 0x1C0)
#define HOTPLUG_ENTRY      (R_CPUCFG + 0x1C4)
#define HOTPLUG_MAGIC      0xFA50392F
