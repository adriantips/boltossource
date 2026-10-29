/* ===========================================================================
 *  BoltOS Mini  -  mini/sdcard.c
 *
 *  The microSD card, on the H618's first SD/MMC controller (MMC0): the card
 *  Mini boots from, read and written a 512-byte sector at a time. U-Boot
 *  has used the card before us and may have left it in any state, so it is
 *  set up from the start, the way the SD spec says: CMD0, CMD8, ACMD41 until
 *  it's ready, CMD2/CMD3 for its address, CMD9 for its size, CMD7, a 4-bit
 *  bus, then high speed (50 MHz) if it can, else 25.
 *
 *  The controller is the same kind as MMC1 (sdio.c), and like there every
 *  transfer goes through its FIFO by the CPU. Multi-sector transfers end
 *  with the controller's automatic CMD12.
 *
 *  Two things write the card: mini/fs.c, files and folders in the FAT
 *  partition (never BOLTOS.BIN), and mini/store.c, packages past the
 *  partitions. U-Boot, in front of them, is never touched.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define MMC0        0x04020000u
#define CCU         0x03001000u
#define PIO         0x0300B000u
#define SMHC0_CLK   (CCU + 0x830)
#define SMHC_BGR    (CCU + 0x84C)
#define PLL_PERI0   (CCU + 0x020)

#define GCTRL   0x00
#define CLKCR   0x04
#define TMOUT   0x08
#define CTYPE   0x0C
#define BLKSZ   0x10
#define BCNT    0x14
#define CMDR    0x18
#define CARG    0x1C
#define RESP0   0x20
#define IMASK   0x30
#define RINT    0x38
#define STAS    0x3C
#define FTRGL   0x40
#define FUNS    0x44
#define DBGC    0x50
#define NTSR    0x5C
#define SAMPDL  0x144
#define FIFO    0x200

#define G_RESET       0x7u
#define G_FIFO_RESET  (1u << 1)
#define G_AHB         (1u << 31)
#define C_ON          (1u << 16)
#define C_LOWPOWER    (1u << 17)

#define CMD_RESP      (1u << 6)
#define CMD_LONG      (1u << 7)
#define CMD_CRC       (1u << 8)
#define CMD_DATA      (1u << 9)
#define CMD_WRITE     (1u << 10)
#define CMD_AUTOSTOP  (1u << 12)
#define CMD_WAITPRE   (1u << 13)
#define CMD_INIT      (1u << 15)
#define CMD_UPCLK     (1u << 21)
#define CMD_START     (1u << 31)

#define I_RESP_ERR    (1u << 1)
#define I_CMD_DONE    (1u << 2)
#define I_DATA_OVER   (1u << 3)
#define I_RESP_CRC    (1u << 6)
#define I_DATA_CRC    (1u << 7)
#define I_RESP_TO     (1u << 8)
#define I_DATA_TO     (1u << 9)
#define I_FIFO_ERR    (1u << 11)
#define I_START_ERR   (1u << 13)
#define I_AUTO_DONE   (1u << 14)
#define I_END_ERR     (1u << 15)
#define I_ERRORS      (I_RESP_ERR | I_RESP_CRC | I_DATA_CRC | I_RESP_TO | I_DATA_TO | I_FIFO_ERR | I_START_ERR | I_END_ERR)

#define S_FIFO_EMPTY  (1u << 2)
#define S_FIFO_FULL   (1u << 3)
#define S_BUSY        (1u << 9)

#define R1   (CMD_RESP | CMD_CRC)
#define R2   (CMD_RESP | CMD_LONG | CMD_CRC)
#define R3   (CMD_RESP)

static inline uint32_t mr(uint32_t r) { return r32(MMC0 + r); }
static inline void mw(uint32_t r, uint32_t v) { w32(MMC0 + r, v); }

static struct {
    int up, hc;                          /* set up; SDHC/SDXC (sector addresses)  */
    uint16_t rca;
    uint32_t hz, cid[4], csd[4];
    uint64_t sectors;
    uint32_t errors, last_err, retries;
    int last_cmd;
    uint64_t rd_bytes, wr_bytes, rd_us, wr_us;
} C;

static int update_clock(void) {
    mw(CMDR, CMD_START | CMD_UPCLK | CMD_WAITPRE);
    for (int i = 0; i < 2000; i++) {
        if (!(mr(CMDR) & CMD_START)) { mw(RINT, mr(RINT)); return 0; }
        delay_us(100);
    }
    kprintf("[sd] the clock didn't change\n");
    return -1;
}

/* The card's clock: the module clock / M / 2^N, then / 2 in the controller
 * (new timing mode), as sdio.c does it for MMC1. */
static int set_clock(uint32_t hz) {
    mw(CLKCR, mr(CLKCR) & ~C_ON);
    if (update_clock()) return -1;
    uint32_t src, sel;
    if (hz <= 400000) { src = 24000000; sel = 0; }
    else {
        uint32_t v = r32(PLL_PERI0);
        uint32_t n = ((v >> 8) & 0xFF) + 1, m = ((v >> 1) & 1) + 1, p0 = ((v >> 16) & 7) + 1;
        src = 24000000u / m * n / p0;
        sel = 1;
    }
    uint32_t div = (src / 2 + hz - 1) / hz, n = 0;
    while (div > 16) { n++; div = (div + 1) / 2; }
    if (n > 3) return -1;
    w32(SMHC0_CLK, (1u << 31) | (sel << 24) | (n << 8) | (div - 1));
    C.hz = src / 2 / div >> n;
    set32(MMC0 + NTSR, 1u << 31);
    mw(SAMPDL, 1u << 7);
    mw(CLKCR, (mr(CLKCR) & ~0xFFu & ~C_LOWPOWER) | C_ON);
    return update_clock();
}

static void host_reset(void) {
    mw(GCTRL, G_RESET);
    for (int i = 0; i < 1000 && (mr(GCTRL) & G_RESET); i++) delay_us(10);
    mw(FTRGL, 0x20070008u);
    mw(TMOUT, 0xFFFFFFFFu);
    mw(IMASK, 0);
    mw(RINT, 0xFFFFFFFFu);
    mw(DBGC, 0xDEB);
    mw(FUNS, 0xCEAAu << 16);
    mw(GCTRL, mr(GCTRL) | G_AHB);
}

static int wait_rint(uint32_t bits, uint32_t ms) {
    uint64_t end = now_ms() + ms;
    for (;;) {
        uint32_t r = mr(RINT);
        if (r & I_ERRORS) return -(int)(r & I_ERRORS);
        if ((r & bits) == bits) return 0;
        if (now_ms() > end) return -0x10000;
    }
}

/* The card holds DAT0 low while it programs what was written. */
static int wait_not_busy(uint32_t ms) {
    uint64_t end = now_ms() + ms;
    while (mr(STAS) & S_BUSY) if (now_ms() > end) return -0x80000;
    return 0;
}

static int command(int idx, uint32_t arg, uint32_t flags, uint32_t *resp,
                   void *data, uint32_t len, int write) {
    if (wait_not_busy(1000)) { C.errors++; C.last_cmd = idx; C.last_err = 0x80000; return -0x80000; }
    mw(RINT, 0xFFFFFFFFu);
    uint32_t c = CMD_START | flags | (uint32_t)idx;
    if (idx == 0) c |= CMD_INIT;
    uint32_t blocks = len / 512;
    if (data) {
        c |= CMD_DATA | CMD_WAITPRE | (write ? CMD_WRITE : 0);
        if (blocks > 1) c |= CMD_AUTOSTOP;
        mw(BLKSZ, len < 512 ? len : 512);
        mw(BCNT, len);
    }
    mw(CARG, arg);
    mw(CMDR, c);
    int err = 0;
    if (data) {
        uint32_t *w = data, words = len / 4;
        uint64_t end = now_ms() + 1000 + len / 2000;      /* a second, and 2 MB/s at the slowest */
        for (uint32_t i = 0; i < words && !err; ) {
            uint32_t r = mr(RINT);
            if (r & I_ERRORS) { err = -(int)(r & I_ERRORS); break; }
            uint32_t s = mr(STAS);
            if (write) {
                if (s & S_FIFO_FULL) { if (now_ms() > end) err = -0x20000; continue; }
                uint32_t room = 64 - ((s >> 17) & 0x3FFF);  /* the FIFO is 64 words deep */
                while (room-- && i < words) mw(FIFO, w[i++]);
            } else {
                uint32_t lvl = (s >> 17) & 0x3FFF;
                if (!lvl) { if (now_ms() > end) err = -0x20000; continue; }
                while (lvl-- && i < words) w[i++] = mr(FIFO);
            }
        }
    }
    if (!err) err = wait_rint(I_CMD_DONE, 200);
    if (!err && data) err = wait_rint(I_DATA_OVER, 1000 + len / 2000);
    if (!err && data && blocks > 1) err = wait_rint(I_AUTO_DONE, 500);
    if (!err && data && write) err = wait_not_busy(2000);
    if (!err && resp) {
        resp[0] = mr(RESP0);
        if (flags & CMD_LONG) { resp[1] = mr(RESP0 + 4); resp[2] = mr(RESP0 + 8); resp[3] = mr(RESP0 + 12); }
    }
    if (err) {
        C.errors++; C.last_err = (uint32_t)-err; C.last_cmd = idx;
        mw(GCTRL, mr(GCTRL) | G_RESET);
        for (int i = 0; i < 1000 && (mr(GCTRL) & G_RESET); i++) delay_us(10);
        mw(GCTRL, mr(GCTRL) | G_AHB);
        update_clock();
    }
    mw(RINT, 0xFFFFFFFFu);
    mw(GCTRL, mr(GCTRL) | G_FIFO_RESET);
    return err;
}

static int acmd(int idx, uint32_t arg, uint32_t flags, uint32_t *resp) {
    uint32_t r;
    if (command(55, (uint32_t)C.rca << 16, R1, &r, 0, 0, 0)) return -1;
    return command(idx, arg, flags, resp, 0, 0, 0);
}

/* CSD bits [hi:lo], the way the spec numbers them. This controller hands
 * over all 128 bits as they came, RESP0 the lowest word (U-Boot reads them
 * the same way). */
static uint32_t csd_bits(int hi, int lo) {
    uint32_t v = 0;
    for (int b = hi; b >= lo; b--) v = v << 1 | ((C.csd[b / 32] >> (b % 32)) & 1);
    return v;
}

/* CMD6: switch to high speed. 1 if the card says it did. */
static int high_speed(void) {
    uint32_t st[16];
    if (command(6, 0x00FFFFF1u, R1, 0, st, 64, 0)) return 0;
    const uint8_t *b = (const uint8_t *)st;
    return (b[16] & 0x0F) == 1;                           /* group 1's function: high speed */
}

uint64_t vblk_sectors(void);
int vblk_rw(int write, uint64_t lba, void *buf, uint32_t n);

int sd_init(void) {
    if (C.up) return 0;                                   /* fs.c and store.c both ask */
    if (g_plat == PLAT_QEMU) {                            /* the stand-in: virtio.c's disk */
        if (!vblk_sectors()) return -1;
        C.up = 1; C.hc = 1; C.sectors = vblk_sectors();
        kprintf("[sd] QEMU's disk: %lu MiB\n", (unsigned long)(C.sectors >> 11));
        return 0;
    }
    if (g_plat != PLAT_H618) return -1;
    memset(&C, 0, sizeof C);
    /* MMC0's pins are PF0-PF5, function 2; U-Boot set them, and set them again here. */
    for (int p = 0; p <= 5; p++) {
        uintptr_t r = PIO + 5 * 0x24u + (uintptr_t)(p / 8) * 4u;
        int sh = (p % 8) * 4;
        w32(r, (r32(r) & ~(0xFu << sh)) | (2u << sh));
    }
    clr32(SMHC_BGR, 1u << 16);
    clr32(SMHC_BGR, 1u << 0);
    w32(SMHC0_CLK, (1u << 31) | (1u << 8) | 14u);
    set32(SMHC_BGR, 1u << 0);
    set32(SMHC_BGR, 1u << 16);
    delay_us(100);
    host_reset();
    mw(CTYPE, 0);
    if (set_clock(400000)) return -1;

    uint32_t r[4] = { 0 };
    command(0, 0, 0, 0, 0, 0, 0);
    delay_ms(2);
    int v2 = command(8, 0x1AA, R1, r, 0, 0, 0) == 0 && (r[0] & 0xFFF) == 0x1AA;
    uint64_t end = now_ms() + 1500;
    int ready = 0;
    while (now_ms() < end) {
        if (acmd(41, (v2 ? 1u << 30 : 0) | 0x00FF8000u, R3, r) == 0 && (r[0] >> 31)) { ready = 1; break; }
        delay_ms(10);
    }
    if (!ready) { kprintf("[sd] the card didn't become ready (ocr %x)\n", (unsigned)r[0]); return -1; }
    C.hc = (r[0] >> 30) & 1;
    if (command(2, 0, R2, C.cid, 0, 0, 0)) { kprintf("[sd] CMD2 failed\n"); return -1; }
    if (command(3, 0, R1, r, 0, 0, 0)) { kprintf("[sd] CMD3 failed\n"); return -1; }
    C.rca = (uint16_t)(r[0] >> 16);
    if (command(9, (uint32_t)C.rca << 16, R2, C.csd, 0, 0, 0)) { kprintf("[sd] CMD9 failed\n"); return -1; }
    if (csd_bits(127, 126) == 1) C.sectors = ((uint64_t)csd_bits(69, 48) + 1) * 1024;          /* CSD 2.0 */
    else {
        uint64_t cs = csd_bits(73, 62), mult = csd_bits(49, 47), bl = csd_bits(83, 80);
        C.sectors = (cs + 1) << (mult + 2) << bl >> 9;
    }
    if (command(7, (uint32_t)C.rca << 16, R1, r, 0, 0, 0)) { kprintf("[sd] CMD7 failed\n"); return -1; }
    if (!C.hc) command(16, 512, R1, r, 0, 0, 0);
    if (acmd(6, 2, R1, r) == 0) mw(CTYPE, 1);             /* 4-bit */
    set_clock(25000000);
    int hs = high_speed();
    if (hs) set_clock(50000000);
    /* Read the first sector back to prove the clock: if high speed garbles it, fall back. */
    static uint8_t probe[512] __attribute__((aligned(4)));
    C.up = 1;
    extern int sd_read(uint64_t lba, void *buf, uint32_t n);
    if (sd_read(0, probe, 1) != 0 && hs) { set_clock(25000000); hs = 0; }
    if (sd_read(0, probe, 1) != 0) { C.up = 0; kprintf("[sd] the card won't read\n"); return -1; }
    kprintf("[sd] card: %lu MiB, %s, %u kHz, 4-bit%s\n", (unsigned long)(C.sectors >> 11),
            C.hc ? "SDHC/SDXC" : "SDSC", (unsigned)(C.hz / 1000), hs ? ", high speed" : "");
    return 0;
}

uint64_t sd_sectors(void) { return C.up ? C.sectors : 0; }

#define MAX_RUN 256u                                      /* sectors per command: 128 KiB */

static int xfer(int write, uint64_t lba, void *buf, uint32_t n) {
    if (!C.up) return -1;
    if (g_plat == PLAT_QEMU) {
        for (uint32_t k; n; n -= k, lba += k, buf = (uint8_t *)buf + k * 512) {
            k = n > MAX_RUN ? MAX_RUN : n;
            if (vblk_rw(write, lba, buf, k)) return -1;
        }
        return 0;
    }
    if (lba + n > C.sectors || ((uintptr_t)buf & 3)) return -1;
    uint8_t *p = buf;
    uint64_t t0 = now_us();
    while (n) {
        uint32_t k = n > MAX_RUN ? MAX_RUN : n;
        uint32_t arg = C.hc ? (uint32_t)lba : (uint32_t)(lba * 512);
        int idx = write ? (k > 1 ? 25 : 24) : (k > 1 ? 18 : 17);
        uint32_t r;
        int err = -1;
        for (int tries = 0; tries < 3 && err; tries++) {
            if (tries) { C.retries++; delay_ms(5); }
            err = command(idx, arg, R1, &r, p, k * 512, write);
        }
        if (err) {
            kprintf("[sd] %s of %u sectors at %lu failed (%x)\n", write ? "write" : "read", (unsigned)k,
                    (unsigned long)lba, (unsigned)-err);
            return -1;
        }
        p += k * 512; lba += k; n -= k;
    }
    uint64_t dt = now_us() - t0;
    if (write) { C.wr_bytes += (uint64_t)(p - (uint8_t *)buf); C.wr_us += dt; }
    else       { C.rd_bytes += (uint64_t)(p - (uint8_t *)buf); C.rd_us += dt; }
    return 0;
}

int sd_read(uint64_t lba, void *buf, uint32_t n)        { return xfer(0, lba, buf, n); }
int sd_write(uint64_t lba, const void *buf, uint32_t n) { return xfer(1, lba, (void *)buf, n); }

int sd_debug(char *out, int cap) {
    uint64_t rk = C.rd_us ? C.rd_bytes * 1000000 / C.rd_us >> 10 : 0, wk = C.wr_us ? C.wr_bytes * 1000000 / C.wr_us >> 10 : 0;
    return snprintf(out, (size_t)cap, "sd: %s, %lu MiB, %u kHz, read %lu MiB (%lu KiB/s), wrote %lu MiB (%lu KiB/s), "
                    "errors %u (last cmd %d: %x), retries %u\n",
                    C.up ? "up" : "down", (unsigned long)(C.sectors >> 11), (unsigned)(C.hz / 1000),
                    (unsigned long)(C.rd_bytes >> 20), (unsigned long)rk, (unsigned long)(C.wr_bytes >> 20),
                    (unsigned long)wk, (unsigned)C.errors, C.last_cmd, (unsigned)C.last_err, (unsigned)C.retries);
}
