#pragma once
#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 *  BoltOS Mini  -  mini/mini.h
 *
 *  BoltOS for small ARM boards: the Orange Pi Zero3 (Allwinner H618, four
 *  Cortex-A53s, 1 GiB) first, and QEMU's `virt` machine, which runs the very
 *  same kernel image on a Mac so the Pi's screen can be tried without the Pi.
 *
 *  The desktop and TV kernels are x86-64. This one is AArch64, so what it
 *  shares with them is the portable layer -- libc, the glass drawing code,
 *  the fonts, the heap -- and the machine underneath is its own: the boot
 *  path, the MMU, the GIC, the timer, and the drivers for what an H618 has.
 *
 *  There is one thread. kmain sets the machine up and then runs a loop that
 *  polls every device, lets the shell draw a frame when something changed,
 *  and sleeps until the next timer tick. Interrupts only count time. Nothing
 *  on a 1 GHz A53 needs more than that, and nothing can deadlock.
 * ===========================================================================*/

/* ---- the board ------------------------------------------------------------ */
enum { PLAT_UNKNOWN = 0, PLAT_QEMU, PLAT_H618 };
extern int g_plat;
extern const char *g_board_name;       /* "Orange Pi Zero3", "QEMU virt"      */
extern uint64_t g_ram_base, g_ram_size;

static inline void     w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t r32(uintptr_t a)             { return *(volatile uint32_t *)a; }
static inline void     w16(uintptr_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
static inline uint16_t r16(uintptr_t a)             { return *(volatile uint16_t *)a; }
static inline void     w8(uintptr_t a, uint8_t v)   { *(volatile uint8_t *)a = v; }
static inline uint8_t  r8(uintptr_t a)              { return *(volatile uint8_t *)a; }
static inline void     set32(uintptr_t a, uint32_t m) { w32(a, r32(a) | m); }
static inline void     clr32(uintptr_t a, uint32_t m) { w32(a, r32(a) & ~m); }

#define barrier()   __asm__ volatile("" ::: "memory")
#define dsb()       __asm__ volatile("dsb sy" ::: "memory")
#define dmb()       __asm__ volatile("dmb sy" ::: "memory")
#define isb()       __asm__ volatile("isb" ::: "memory")
#define wfi()       __asm__ volatile("wfi" ::: "memory")

/* ---- memory --------------------------------------------------------------- *
 *  Physical == virtual: one identity map, 2 MiB blocks. Two blocks are mapped
 *  uncached -- the DMA pool, where USB controllers read their descriptors,
 *  and the log window, which must still say what happened after a watchdog
 *  reset has thrown the caches away.                                         */
#define MINI_HEAP_BASE   0x41000000ull     /* 96 MiB kernel heap               */
#define MINI_HEAP_SIZE   0x06000000ull
#define MINI_DMA_BASE    0x47000000ull     /* 2 MiB, uncached                  */
#define MINI_DMA_SIZE    0x00200000ull
#define MINI_LOG_BASE    0x5F000000ull     /* 1 MiB, uncached; U-Boot DFU "log" */
#define MINI_LOG_SIZE    0x00100000ull

void  mmu_init(void);
void *dma_alloc(uint32_t size, uint32_t align);   /* zeroed, uncached, never freed */
static inline uint32_t dma_addr(const void *p) { return (uint32_t)(uintptr_t)p; }

/* ---- time ----------------------------------------------------------------- */
void     timer_init(void);
uint64_t now_us(void);
uint64_t now_ms(void);
void     delay_us(uint64_t us);
void     delay_ms(uint64_t ms);
extern volatile uint64_t g_ticks;      /* 1 kHz                                */

/* Wall clock. Unknown until something tells us (pi.py, the web page, the
 * Settings app): an Orange Pi Zero3 has no battery-backed clock. */
int      clock_known(void);
uint64_t clock_unix(void);             /* seconds, UTC                         */
void     clock_set_unix(uint64_t secs);
int      clock_tz_minutes(void);       /* offset from UTC                      */
void     clock_set_tz_minutes(int m);
void     clock_local(int *y, int *mo, int *d, int *h, int *mi, int *s, int *wday);

/* ---- interrupts ----------------------------------------------------------- */
void gic_init(uintptr_t dist, uintptr_t cpu, int v3);
void gic_enable(int irq);
static inline void irq_on(void)  { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_off(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }

/* ---- the board's own pieces (mini/board.c) --------------------------------- */
void board_early(uint64_t dtb);        /* UART, LEDs, watchdog: before anything */
void board_init(void);                 /* GIC, timer, then the rest            */
void board_poll(void);                 /* once a loop: watchdog                */
void board_led(int green, int on);
void board_reboot(void) __attribute__((noreturn));
void board_poweroff(void) __attribute__((noreturn));
void uart_putc(char c);
int  uart_getc(void);                  /* -1 if nothing                        */
int  board_temp_mc(void);              /* CPU temperature in milli-degrees C, or -1000000 */
uint32_t board_cpu_mhz(void);

/* ---- log ------------------------------------------------------------------ */
void     klog_init(void);
uint32_t klog_snapshot(char *out, uint32_t cap);  /* the tail, NUL-terminated */
uint32_t klog_previous(char *out, uint32_t cap);  /* the boot before's tail      */
uint32_t klog_boot_count(void);

/* ---- the device tree U-Boot or QEMU handed over --------------------------- */
int         fdt_valid(const void *fdt);
const char *fdt_root_compat(const void *fdt);           /* first string       */
int         fdt_compat_has(const void *fdt, const char *s);
int         fdt_memory(const void *fdt, uint64_t *base, uint64_t *size);
const char *fdt_prop_str(const void *fdt, const char *path, const char *prop);
const char *fdt_model(const void *fdt);

/* ---- the screen ----------------------------------------------------------- *
 *  There is no panel yet: the screen is a buffer in RAM. Whoever looks at it
 *  -- a VNC viewer on the Mac, QEMU's window through ramfb, and later an LCD
 *  -- reads `front`. The shell draws into `back` and screen_present() copies
 *  what changed across, so nobody ever sees half a frame.                     */
#include "gfx.h"
extern surface_t g_back;               /* draw here                            */
extern uint32_t *g_front;              /* shown: same size, stride == width    */
extern int scr_w, scr_h;
extern volatile uint32_t g_front_gen;  /* bumped every present                 */
int  screen_init(int w, int h);
void screen_present(int x, int y, int w, int h);

/* ---- input ---------------------------------------------------------------- */
enum {
    KEY_NONE = 0,
    KEY_CHAR,                          /* a printable character, in .ch        */
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_DELETE, KEY_INSERT,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
    KEY_F10, KEY_F11, KEY_F12,
    KEY_SUPER,                         /* the Windows / Command key, alone       */
};
enum { MOD_SHIFT = 1, MOD_CTRL = 2, MOD_ALT = 4, MOD_SUPER = 8 };
enum { EV_KEY = 1, EV_POINTER };
typedef struct {
    uint8_t  type;
    uint8_t  key;                      /* KEY_*                                */
    uint8_t  mods;                     /* MOD_*                                */
    uint8_t  buttons;                  /* pointer: bit0 left, bit1 right, bit2 middle */
    int32_t  ch;                       /* KEY_CHAR: the character (Latin-1)    */
    int16_t  x, y;                     /* pointer, screen pixels               */
    int8_t   wheel;                    /* +1 up, -1 down                       */
    uint8_t  source;                   /* where it came from, for the Monitor  */
} input_ev_t;
enum { SRC_USB = 1, SRC_VNC, SRC_QEMU, SRC_SERIAL };

void input_key(int key, int ch, int mods, int source);
void input_pointer(int x, int y, int buttons, int wheel, int source);
int  input_pop(input_ev_t *ev);
uint32_t input_count(int source);      /* events seen from a source            */
/* HID boot-protocol usage -> KEY_* / character, US layout. */
int  input_from_hid(uint8_t usage, int shift, int *ch);
/* X11 keysym (what VNC speaks) -> KEY_* / character. */
int  input_from_keysym(uint32_t sym, int *ch);

/* ---- USB ------------------------------------------------------------------ */
int  musb_init(void);                  /* the USB-C port, as a device          */
void musb_poll(void);
int  musb_configured(void);            /* the Mac has set us up                */
int  musb_busy(void);                  /* frames still queued for the Mac      */
int  usbh_init(void);                  /* the USB-A port and the header ones   */
void usbh_poll(void);
int  usbh_device_list(char *out, int cap);       /* for `lsusb` and the Monitor  */
int  usbh_keyboards(void);
int  usbh_mice(void);

/* ---- Ethernet (mini/emac.c) ------------------------------------------------ */
int  eth_init(void);                   /* the RJ45 socket: EMAC0 + YT8531 PHY  */
void eth_poll(void);
int  eth_link(void);                   /* Mbit/s, 0 while the cable is out      */
struct mini_netif *eth_netif(void);
int  eth_debug(char *out, int cap);

/* ---- QEMU's devices -------------------------------------------------------- */
int  qemu_ramfb_init(uint32_t *fb, int w, int h);
int  virtio_init(void);
void virtio_poll(void);

/* ---- network (mini/net.c) -------------------------------------------------- */
#include "minet.h"

/* ---- services -------------------------------------------------------------- */
void netsvc_init(void);
void netsvc_poll(void);
void netsvc_jobs(void);                /* work that waits, run from the main loop */
int  vnc_clients(void);
int  vnc_init(uint16_t port);
void vnc_poll(void);
extern char g_vnc_password[9];

/* ---- the shell ------------------------------------------------------------- */
void shell_init(void);
int  shell_frame(void);                /* 1 if it drew something                */
void shell_toast(const char *title, const char *detail);
void shell_request_frame(void);

/* ---- odds and ends ---------------------------------------------------------- */
uint32_t mini_rand(void);
const char *mini_version(void);
int  mini_uptime_str(char *out, int cap);
