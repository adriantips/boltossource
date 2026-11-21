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
        /* The tree itself sits where apps' memory goes (mini.h): keep only a copy. */
        static char model_copy[64];
        if (model) { strncpy(model_copy, model, sizeof model_copy - 1); model = model_copy; }
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

/* ---- the temperature sensor ----------------------------------------------------------------------
 *  The H618's THS has four sensors: 0 the GPU, 1 the video engine, 2 the CPU, 3 the DRAM controller.
 *  Set up the way Linux's sun8i_thermal.c does it for the H616/H618: bus clock and reset, bit 16 of
 *  the system-control SRAM register cleared, each sensor's factory calibration from the efuses (the
 *  ambient temperature at the factory test and what each sensor read then), and a reading every
 *  0.25 s averaged over four samples. A reading r is 263655 - 81 r milli-degrees, plus the 8 degrees
 *  the vendor's code adds. Linux throttles the CPU from 60-70 C and calls 110 C critical.            */
#define H6_THS        0x05070400u
#define H6_SID_DATA   0x03006200u          /* the efuses as read: SID + 0x200 */
#define H6_SYSCON     0x03000000u

static int ths_raw_mc(uint32_t r) { return 263655 - (int)r * 81; }

static void ths_init(void) {
    w32(H6_CCU + 0x9FC, r32(H6_CCU + 0x9FC) | 1u | (1u << 16));       /* THS_BGR: gate on, out of reset */
    w32(H6_SYSCON, r32(H6_SYSCON) & ~(1u << 16));
    uint32_t a = r32(H6_SID_DATA + 0x14), b = r32(H6_SID_DATA + 0x18);
    uint16_t cal[4] = { (uint16_t)a, (uint16_t)(a >> 16), (uint16_t)b, (uint16_t)(b >> 16) };
    int calibrated = 0;
    if (cal[0]) {
        int ft = (cal[0] & 0xFFF) * 100;                                /* the factory's ambient, m°C */
        for (int i = 0; i < 4; i++) {
            int r = i == 3 ? (cal[1] >> 12) | ((cal[2] >> 12) << 4) | ((cal[3] >> 12) << 8) : cal[i + 1] & 0xFFF;
            int c = 0x800 - (ths_raw_mc((uint32_t)r) - ft) * 10 / 810;
            if (c & ~0xFFF) continue;                                   /* out of range: uncalibrated */
            uint32_t at = H6_THS + 0xA0 + (uint32_t)(i / 2) * 4, sh = (uint32_t)(i % 2) * 16;
            w32(at, (r32(at) & ~(0xFFFu << sh)) | ((uint32_t)c << sh));
            calibrated++;
        }
    }
    w32(H6_THS + 0x00, (48u - 1) | ((480u - 1) << 16));   /* acquire 2 us, a sample every 20 us */
    w32(H6_THS + 0x30, (1u << 2) | 1u);                    /* filter: the average of 4           */
    w32(H6_THS + 0x08, 365u << 12);                        /* a result every 0.25 s              */
    w32(H6_THS + 0x04, 0xFu);                              /* all four sensors on                */
    kprintf("[ths] temperature sensors on, %d of 4 calibrated (factory test at %d.%d C)\n", calibrated,
            (cal[0] & 0xFFF) / 10, (cal[0] & 0xFFF) % 10);
}

/* Milli-degrees C from one sensor, or -1000000 (no reading yet, or not an H618). */
int board_sensor_mc(int i) {
    if (g_plat != PLAT_H618 || i < 0 || i > 3) return -1000000;
    uint32_t r = r32(H6_THS + 0xC0 + (uint32_t)i * 4) & 0xFFF;
    if (!r) return -1000000;
    int t = ths_raw_mc(r) + 8000;
    return t < -40000 || t > 150000 ? -1000000 : t;
}

void board_init(void) {
    if (g_plat == PLAT_H618) gic_init(0x03021000u, 0x03022000u, 0);
    else                     gic_init(0x08000000u, 0x08010000u, gic_v3);
    timer_init();
    if (g_plat == PLAT_H618) ths_init();
}

void board_poll(void) {
    /* Fed while the main loop comes round at least every 30 s: a wait that
     * keeps the network turning but never ends still gets the Pi restarted. */
    extern volatile uint64_t g_loop_ms;
    if (g_plat == PLAT_H618 && now_ms() - g_loop_ms < 30000) h6_wdt_feed();
}

void board_led(int green, int on) {
    if (g_plat != PLAT_H618) return;
    h6_gpio_out(BANK_C, green ? 13 : 12, on);
}

/* PSCI CPU_ON (SMC64). 0 if the core is on its way. */
int board_cpu_on(int cpu, uint64_t entry, uint64_t ctx) {
    return (int)(int64_t)psci(0xC4000003u, (uint64_t)cpu, entry, ctx);
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

int board_temp_mc(void) { return board_sensor_mc(2); }      /* the CPU's sensor */
