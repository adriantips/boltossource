/* ===========================================================================
 *  BoltOS Mini  -  mini/board.c
 *
 *  The two machines Mini runs on, and the few things about each that the
 *  rest of the kernel asks for: a UART, the LEDs, the watchdog, reboot.
 *
 *  Orange Pi Zero3 (Allwinner H618)          QEMU `virt`
 *    UART0   16550 @ 0x05000000                PL011 @ 0x09000000
 *    GIC-400 0x03021000 / 0x03022000           GICv2 0x08000000 / 0x08010000
 *    LEDs    PC12 red (power), PC13 green      --
 *    WDT     0x030090A0, left running by       --
 *            U-Boot with 16 s on the clock
 *    PSCI    TF-A, over SMC                    QEMU, over HVC
 *
 *  Which one we are on comes from the device tree's root compatible.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"

int g_plat;
const char *g_board_name = "unknown board";
uint64_t g_ram_base = 0x40000000ull, g_ram_size = 0x40000000ull;
static int psci_hvc_conduit;
static int gic_v3;                       /* QEMU under HVF; never the Pi        */

uint64_t psci_smc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);
uint64_t psci_hvc(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);
static uint64_t psci(uint64_t fn, uint64_t a, uint64_t b, uint64_t c) {
    return psci_hvc_conduit ? psci_hvc(fn, a, b, c) : psci_smc(fn, a, b, c);
}

/* ---- H618 ----------------------------------------------------------------- */
#define H6_UART0      0x05000000u
#define H6_PIO        0x0300B000u
#define H6_CCU        0x03001000u
#define H6_WDT        0x030090A0u
#define PIO_BANK(n)   (H6_PIO + (n) * 0x24u)
#define PIO_CFG(n, p) (PIO_BANK(n) + ((p) / 8) * 4)
#define PIO_DAT(n)    (PIO_BANK(n) + 0x10)
#define BANK_C        2

void h6_gpio_out(int bank, int pin, int val) {
    uintptr_t cfg = PIO_CFG(bank, pin);
    int sh = (pin % 8) * 4;
    w32(cfg, (r32(cfg) & ~(0xFu << sh)) | (1u << sh));      /* function 1: output */
    if (val) set32(PIO_DAT(bank), 1u << pin);
    else     clr32(PIO_DAT(bank), 1u << pin);
}

static void h6_wdt_feed(void) { w32(H6_WDT + 0x10, (0x0A57u << 1) | 1u); }

/* ---- QEMU ------------------------------------------------------------------ */
#define PL011         0x09000000u

/* ---- UART --------------------------------------------------------------------- */
void uart_putc(char c) {
    if (g_plat == PLAT_H618) {
        for (int i = 0; i < 100000 && !(r32(H6_UART0 + 0x14) & 0x20); i++) { }
        w32(H6_UART0, (uint8_t)c);
    } else if (g_plat == PLAT_QEMU) {
        for (int i = 0; i < 100000 && (r32(PL011 + 0x18) & 0x20); i++) { }
        w32(PL011, (uint8_t)c);
    }
}

int uart_getc(void) {
    if (g_plat == PLAT_H618) return (r32(H6_UART0 + 0x14) & 1) ? (int)(r32(H6_UART0) & 0xFF) : -1;
    if (g_plat == PLAT_QEMU) return (r32(PL011 + 0x18) & 0x10) ? -1 : (int)(r32(PL011) & 0xFF);
    return -1;
}

/* ---- bring-up ------------------------------------------------------------------ */
void board_early(uint64_t dtb) {
    const void *fdt = (const void *)(uintptr_t)dtb;
    if (fdt_valid(fdt)) {
        if (fdt_compat_has(fdt, "allwinner,sun50i-h618") ||
            fdt_compat_has(fdt, "allwinner,sun50i-h616")) g_plat = PLAT_H618;
        else if (fdt_compat_has(fdt, "linux,dummy-virt")) g_plat = PLAT_QEMU;
        const char *model = fdt_model(fdt);
        uint64_t b, s;
        if (fdt_memory(fdt, &b, &s) && b == 0x40000000ull && s >= 0x10000000ull) {
            g_ram_base = b;
            g_ram_size = s > 0x40000000ull ? 0x40000000ull : s;  /* the map covers 1 GiB */
        }
        const char *ic = fdt_prop_str(fdt, "/intc", "compatible");
        gic_v3 = g_plat == PLAT_QEMU && ic && strstr(ic, "gic-v3");
        const char *m = fdt_prop_str(fdt, "/psci", "method");
        psci_hvc_conduit = m && strcmp(m, "hvc") == 0;
        if (g_plat == PLAT_H618) g_board_name = model ? model : "Allwinner H618";
        else if (g_plat == PLAT_QEMU) g_board_name = "QEMU virt (on your Mac)";
    } else {
        /* No tree: QEMU without one is the only loader that does this. */
        g_plat = PLAT_QEMU;
        g_board_name = "QEMU virt";
        psci_hvc_conduit = 1;
    }
    if (g_plat == PLAT_H618) {
        h6_wdt_feed();
        h6_gpio_out(BANK_C, 12, 1);        /* red: power, on                   */
        h6_gpio_out(BANK_C, 13, 0);        /* green: ours, off until the shell */
    }
}

void board_init(void) {
    if (g_plat == PLAT_H618) gic_init(0x03021000u, 0x03022000u, 0);
    else                     gic_init(0x08000000u, 0x08010000u, gic_v3);
    timer_init();
}

void board_poll(void) {
    if (g_plat == PLAT_H618) h6_wdt_feed();
}

void board_led(int green, int on) {
    if (g_plat != PLAT_H618) return;
    h6_gpio_out(BANK_C, green ? 13 : 12, on);
}

void board_reboot(void) {
    kprintf("[mini] restarting\n");
    irq_off();
    psci(0x84000009u, 0, 0, 0);            /* SYSTEM_RESET                     */
    if (g_plat == PLAT_H618) {             /* TF-A said no: the watchdog, now   */
        w32(H6_WDT + 0x14, 1);             /* CFG: reset the system             */
        w32(H6_WDT + 0x18, 1);             /* MODE: 0.5 s, enabled              */
    }
    for (;;) wfi();
}

void board_poweroff(void) {
    kprintf("[mini] powering off\n");
    irq_off();
    board_led(1, 0);
    psci(0x84000008u, 0, 0, 0);            /* SYSTEM_OFF                       */
    /* An Orange Pi on a Mac's USB port has no way to cut its own power: stop
     * feeding the watchdog would restart it, so turn the watchdog off and
     * leave only the red LED on. */
    if (g_plat == PLAT_H618) w32(H6_WDT + 0x18, 0);
    for (;;) wfi();
}

uint32_t board_cpu_mhz(void) {
    if (g_plat != PLAT_H618) return 0;
    uint32_t v = r32(H6_CCU + 0x000);      /* PLL_CPUX_CTRL                    */
    uint32_t n = ((v >> 8) & 0xFF) + 1, p = (v >> 16) & 3;
    return (24 * n) >> p;
}

int board_temp_mc(void) { return -1000000; }
