#include <stdint.h>
#include "mouse.h"
#include "interrupts.h"
#include "io.h"
#include "timeout.h"

/* ---------------------------------------------------------------------------
 *  8042 PS/2 controller + mouse. The controller multiplexes the keyboard and
 *  the "auxiliary" (mouse) device over ports 0x60 (data) / 0x64 (cmd+status).
 *  Status bit0 = output buffer full (data to read), bit1 = input buffer full
 *  (don't write yet), bit5 = the byte came from the mouse, not the keyboard.
 * ------------------------------------------------------------------------- */
#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static volatile int     mx, my;          /* absolute cursor position */
static volatile int     sw, sh;          /* screen bounds for clamping */
static volatile uint8_t btns;            /* current button bitmask */
/* Buttons that have gone down since the last read. The compositor samples
 * the mouse once a frame, and a full repaint can take longer than a real
 * click lasts -- without a latch, a fast click (or the second half of a
 * double-click) simply never happened. */
static volatile uint8_t btn_latch;
static volatile int     evt;             /* set on every decoded packet */

static volatile uint8_t pkt[4];
static volatile uint8_t phase;           /* which byte of the packet is next */
static volatile uint8_t pktsz = 3;       /* 3 (plain) or 4 (IntelliMouse wheel) */
static volatile int     wheelacc;        /* accumulated wheel delta (+up / -down) */

/* Real deadlines, not iteration counts. The 8042 answers in microseconds when
 * it is there at all; 20 ms is the same bound drivers/keyboard.c uses, and the
 * point of it is the machine that has no controller behind these ports, where
 * every read costs about a microsecond and the old 100,000-iteration spin was
 * a tenth of a second per access, paid on every one of a couple of dozen. */
#define PS2_WAIT_US 20000ull

static int wait_write(void) { return wait_until(!(inb(PS2_STATUS) & 2), PS2_WAIT_US) ? 0 : -1; }
static int wait_read(void)  { return wait_until(inb(PS2_STATUS) & 1,   PS2_WAIT_US) ? 0 : -1; }

/* -1 when nothing came back, which is a different fact from any byte value and
 * used to be indistinguishable from 0xFF. See mouse_init(). */
static int  ctl_read(void)        { return wait_read() == 0 ? inb(PS2_DATA) : -1; }
static void ctl_cmd(uint8_t c)    { if (wait_write() == 0) outb(PS2_CMD, c); }
static void ctl_write(uint8_t v)  { if (wait_write() == 0) outb(PS2_DATA, v); }

/* Send a byte to the mouse (prefix 0xD4) and read its acknowledge. 0 when the
 * device answered 0xFA, -1 otherwise -- which is how "there is a mouse on this
 * port" gets asked at all.
 *
 * It reads up to three replies rather than one, because the byte in front of
 * the acknowledge is not always the acknowledge. A mouse that has just been
 * powered or re-enabled announces itself with 0xAA (self-test passed) and then
 * its device id, and both of those are sitting in the output buffer when the
 * first command goes out. Taking the first byte as the answer would report no
 * mouse on a port that has one, which is a worse failure than the one this
 * whole function exists to avoid. */
static int mouse_cmd(uint8_t v) {
    ctl_cmd(0xD4);
    ctl_write(v);
    for (int i = 0; i < 3; i++) {
        int r = ctl_read();
        if (r == 0xFA) return 0;               /* acknowledged                */
        if (r < 0 || r == 0xFE) return -1;     /* silence, or "resend"        */
    }
    return -1;
}

static int present;                      /* a mouse answered on the aux port */
int mouse_ps2_present(void) { return present; }

/* finished a full packet (3 or 4 bytes): apply movement, buttons and wheel */
static void decode_packet(void) {
    uint8_t f = pkt[0];
    if (f & 0xC0) return;                      /* X/Y overflow -> drop packet */
    int dx = (int)pkt[1] - (int)((f << 4) & 0x100);   /* sign-extend 9-bit */
    int dy = (int)pkt[2] - (int)((f << 3) & 0x100);
    mx += dx;
    my -= dy;                                  /* screen Y grows downward */
    if (mx < 0)      mx = 0;
    if (my < 0)      my = 0;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
    uint8_t nb = f & 0x07;
    btn_latch |= (uint8_t)(nb & ~btns);
    btns = nb;
    if (pktsz == 4) {                          /* low nibble of byte3 is signed Z */
        int z = pkt[3] & 0x0F;
        if (z & 0x08) z -= 16;
        wheelacc += z;
    }
    evt = 1;
}

/* Apply a movement/button update from an external transport (USB HID boot
 * mouse). Deltas are already sign-extended; USB Y grows downward, matching the
 * screen, so it is added directly (unlike the PS/2 path which inverts it). */
void mouse_inject(int dx, int dy, uint8_t buttons, int wheel) {
    mx += dx; my += dy;
    if (mx < 0)      mx = 0;
    if (my < 0)      my = 0;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
    uint8_t nb = buttons & 0x07;
    btn_latch |= (uint8_t)(nb & ~btns);
    btns = nb;
    wheelacc += wheel;
    evt = 1;
}

static void on_irq(struct registers *r) {
    (void)r;
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 1) || !(st & 0x20)) return;     /* nothing, or it's keyboard data */
    uint8_t data = inb(PS2_DATA);

    switch (phase) {
    case 0:
        /* hack: bit3 of a real byte0 is always 1, and it's not just a resync
         * for lost sync mid-stream -- the controller likes to hand back one
         * stray byte right after 0xF4 enables streaming (and sometimes after
         * the knock sequence below), which would otherwise get read as
         * byte0 of the first packet and misalign every packet after it.
         * Same check catches both cases for free; no separate post-init flush. */
        if (!(data & 0x08)) return;
        pkt[0] = data; phase = 1; break;
    case 1:
        pkt[1] = data; phase = 2; break;
    case 2:
        pkt[2] = data;
        if (pktsz == 4) { phase = 3; break; }
        phase = 0; decode_packet(); break;
    case 3:
        pkt[3] = data; phase = 0; decode_packet(); break;
    }
}

/* Re-clamp the cursor to a new panel size after a resolution change. Keeps the
 * pointer on screen instead of stranded off the new bottom-right edge. */
void mouse_set_bounds(int screen_w, int screen_h) {
    sw = screen_w; sh = screen_h;
    if (mx > sw - 1) mx = sw - 1;
    if (my > sh - 1) my = sh - 1;
}

int mouse_init(int screen_w, int screen_h) {
    sw = screen_w; sh = screen_h;
    mx = screen_w / 2; my = screen_h / 2;
    btns = 0; phase = 0; evt = 0; present = 0;

    ctl_cmd(0xA8);                             /* enable auxiliary device */

    /* Anything the firmware or the aux device left in the output buffer is not
     * a reply to a command this driver has sent, and the probe below reads
     * replies. Bounded, for the same reason keyboard.c's drain is. */
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & 1); i++) inb(PS2_DATA);

    /* Read-modify-write the controller config byte -- and only if the read
     * actually produced one.
     *
     * ctl_read() used to hand back whatever inb() gave after a bounded spin,
     * with no way to say "nothing came back". On a machine whose 8042 does not
     * answer -- and on a board built in the last decade there may be nothing
     * at all behind these ports -- that is 0xFF. Bit 4 of the config byte is
     * *disable the first port's clock*, so this function's parting act was to
     * switch off the keyboard, on exactly the machines where it had no mouse
     * to show for it. keyboard_init() has checked its own read since the day
     * it was written; this one never did, and it runs second. */
    ctl_cmd(0x20);
    int cfg = ctl_read();
    if (cfg < 0) return 0;                     /* no controller: leave it alone */
    cfg |=  0x02;                              /* enable IRQ12 (mouse interrupt) */
    cfg &= ~0x20;                              /* enable the mouse clock */
    ctl_cmd(0x60);                             /* write controller config byte */
    ctl_write((uint8_t)cfg);

    /* Is there a device on the aux port at all? Every PS/2 mouse acknowledges
     * 0xF6 and an empty port answers nothing. Asking costs one 20 ms deadline
     * and saves eight of them in the knock below, on every machine whose mouse
     * is USB -- which is most of them now. */
    if (mouse_cmd(0xF6) != 0) return 0;        /* set defaults */

    /* IntelliMouse "knock": rates 200/100/80 then read device id; id 3 means a
     * scroll wheel is present and the mouse will stream 4-byte packets. */
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    if (mouse_cmd(0xF2) == 0 && ctl_read() == 0x03) pktsz = 4;   /* get device id */
    mouse_cmd(0xF3); mouse_cmd(100);           /* restore a sane sample rate */

    mouse_cmd(0xF4);                           /* enable packet streaming */

    irq_install(12, on_irq);
    present = 1;
    return 1;
}

int     mouse_x(void)        { return mx; }
int     mouse_y(void)        { return my; }
uint8_t mouse_buttons(void)  { return btns; }
uint8_t mouse_take_pressed(void) { uint8_t l = btn_latch; btn_latch = 0; return l; }
int     mouse_poll_event(void) { int e = evt; evt = 0; return e; }
int     mouse_wheel(void)      { int w = wheelacc; wheelacc = 0; return w; }
// hold on guys