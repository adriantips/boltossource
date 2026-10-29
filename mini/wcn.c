/* ===========================================================================
 *  BoltOS Mini  -  mini/wcn.c
 *
 *  The Orange Pi Zero3's Wi-Fi and Bluetooth: a Unisoc UWE5622 ("Marlin3
 *  Lite") on SDIO (mini/sdio.c). The chip is a Cortex-M with a radio; it
 *  does nothing until the Pi loads its firmware, and then both Wi-Fi
 *  (wifi.c) and Bluetooth (bt.c) talk to that firmware in packets.
 *
 *  Its SDIO function 1 has two windows:
 *    0x0F  "direct": reads and writes land in the chip's memory, at the
 *          address in function 0's registers 0x15C-0x15F
 *    0x20  "packets": a stream of packets, each behind a 4-byte header
 *          that says its type (the channel) and length
 *
 *  Starting it, as Unisoc's Linux driver (uwe5622/unisocwcn) does:
 *    1. the card on the bus; function 1 on, 840-byte blocks
 *    2. its ID at 0x4083C208 (0x2355B00x for this chip)
 *    3. the firmware, written to 0x40500000
 *    4. its CPU out of reset (bit 0 of 0x40088288)
 *    5. a handshake in a struct at 0x405E73B0: it waits for our SDIO
 *       settings, then for Allwinner's "bind" check, then says it's ready.
 *
 *  The chip may sleep only when the host says it may; Mini never does, so
 *  it is always awake and nothing has to wake it first.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "sdio.h"
#include "wcn.h"
#include "fs.h"
#include "crypto.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define DT_PORT         0x0F
#define PK_PORT         0x20
#define FBR_SYSADDR0    0x15C
#define BLK             840

#define CHIPID_REG      0x4083C208u
#define CP_START_ADDR   0x40500000u
#define CP_RESET_REG    0x40088288u
#define SYNC_ADDR       0x405E73B0u
#define SYNC_CALI_WAITING       0xF0F0F0F1u
#define SYNC_CALI_WRITE_DONE    0xF0F0F0F2u
#define SYNC_VERIFY_WAITING     0xF0F0F0F6u
#define SYNC_VERIFY_WRITE_DONE  0xF0F0F0F7u
#define SYNC_ALL_FINISHED       0xF0F0F0FFu

extern const uint8_t _binary_wcnmodem_bin_start[], _binary_wcnmodem_bin_end[];
extern const uint8_t _binary_wifi_ini_bin_start[], _binary_wifi_ini_bin_end[];
extern const uint8_t _binary_bt_pskey_bin_start[], _binary_bt_pskey_bin_end[];
extern const uint8_t _binary_bt_rf_bin_start[], _binary_bt_rf_bin_end[];

struct sync_info {                       /* the chip's side of the handshake */
    uint32_t init_status;
    uint32_t mem_pd_bt_start, mem_pd_bt_end, mem_pd_wifi_start, mem_pd_wifi_end;
    uint32_t prj_type, tsx_dac_data, sdio_config;
    uint32_t dcache_status, dcache_start, dcache_end;
    uint32_t mem_pd_status, mem_ap_cmd, rsvd[3];
    uint32_t bind_verify[4];
};

#define MAXPAC          (2 * BLK)        /* the first read, and the most one packet may be */
#define TXCAP           (4 * BLK)
#define RXCAP           (128 * 1024)

static struct {
    int state;                           /* WCN_* */
    uint32_t chipid;
    char why[96];
    uint8_t *bounce;                     /* aligned, a block and a bit */
    uint32_t boot_ms;
    /* packets */
    uint8_t *tx, *rx;
    uint32_t dtbs;                       /* how much the next read takes */
    wcn_rx_fn rxfn[26];
    uint64_t next_poll;
    uint32_t tx_pkts, rx_pkts, rx_reads, irqs, tx_errs, rx_errs;
} W;

int wcn_state(void) { return W.state; }
const char *wcn_why(void) { return W.why; }
uint32_t wcn_chipid(void) { return W.chipid; }

static int fail(const char *why) {
    snprintf(W.why, sizeof W.why, "%s", why);
    kprintf("[wcn] %s\n", why);
    W.state = WCN_FAILED;
    return -1;
}

/* Waits that keep the links and the screen sharing turning. */
static void wait_ms(uint32_t ms) {
    uint64_t end = now_ms() + ms;
    while (now_ms() < end) { mini_pump(); delay_us(200); }
}

/* ---- the direct window ----------------------------------------------------------------- */
static int dt_set_addr(uint32_t a) {
    for (int i = 0; i < 4; i++)
        if (sdio_writeb(0, FBR_SYSADDR0 + (uint32_t)i, (uint8_t)(a >> (8 * i)))) return -1;
    return 0;
}

/* One address, then the bytes in block-sized pieces: each CMD53 starts at the
 * window again and the chip carries on from where the last one stopped. */
int wcn_dt_write(uint32_t addr, const void *buf, uint32_t len) {
    if (dt_set_addr(addr)) return -1;
    const uint8_t *p = buf;
    while (len) {
        uint32_t n = len >= BLK ? BLK : (len > 512 ? 512 : len);
        memcpy(W.bounce, p, n);
        if (sdio_rw(1, 1, DT_PORT, 1, W.bounce, (n + 3) & ~3u)) return -1;
        p += n; len -= n;
    }
    return 0;
}

int wcn_dt_read(uint32_t addr, void *buf, uint32_t len) {
    if (dt_set_addr(addr)) return -1;
    uint8_t *p = buf;
    while (len) {
        uint32_t n = len >= BLK ? BLK : (len > 512 ? 512 : len);
        if (sdio_rw(0, 1, DT_PORT, 1, W.bounce, (n + 3) & ~3u)) return -1;
        memcpy(p, W.bounce, n);
        p += n; len -= n;
    }
    return 0;
}

int wcn_reg_read(uint32_t addr, uint32_t *v) { return wcn_dt_read(addr, v, 4); }
int wcn_reg_write(uint32_t addr, uint32_t v) { return wcn_dt_write(addr, &v, 4); }

/* ---- Allwinner's bind check ----------------------------------------------------------------
 *  The firmware hands over 16 bytes and wants them back transformed, to
 *  know it's on a board it was sold for. The transform is in Unisoc's
 *  driver (wcn_bind_verify_calculate_verify_data). */
static void expand_seed(const uint8_t seed[4], uint8_t out[36]) {
    uint8_t h[32];
    sha256(seed, 4, h);
    for (int i = 0; i < 4; i++) { memcpy(&out[i * 9], &h[i * 8], 8); out[i * 9 + 8] = seed[i]; }
}

static void bind_verify(const uint8_t in[16], uint8_t out[16]) {
    uint8_t seed[4], buf[36], a, b, c;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        a = in[n++]; b = in[n++]; c = in[n++];
        seed[i] = (uint8_t)((a & b) ^ (a & c) ^ (b & c) ^ in[i + 12]);
    }
    expand_seed(seed, buf);
    n = 0;
    for (int i = 0; i < 12; i++) { a = buf[n++]; b = buf[n++]; c = buf[n++]; out[i] = (uint8_t)((a & b) ^ (a & c) ^ (b & c)); }
    n = 0;
    for (int i = 0; i < 4; i++) { a = out[n++]; b = out[n++]; c = out[n++]; seed[i] = (uint8_t)((a & b) ^ (~a & c)); }
    expand_seed(seed, buf);
    n = 0;
    for (int i = 0; i < 12; i++) { a = buf[n++]; b = buf[n++]; c = buf[n++]; out[i] = (uint8_t)((a & b) ^ (~a & c)); }
    n = 0;
    for (int i = 0; i < 4; i++) { a = out[n++]; b = out[n++]; c = out[n++]; out[i + 12] = (uint8_t)((a & b) ^ (a & c) ^ (b & c) ^ seed[i]); }
}

/* ---- the firmware ------------------------------------------------------------------------------
 *  build-mini.sh puts it in the kernel when it can fetch it. The kernels
 *  boltos.dev sends as updates are built without it, so a kernel that has
 *  it leaves a copy on the card at boot, and one that doesn't reads that. */
static const struct { const char *name; const uint8_t *start, *end; } FW[WCN_NFW] = {
    { "wcnmodem.bin", _binary_wcnmodem_bin_start, _binary_wcnmodem_bin_end },
    { "wifi_ini.bin", _binary_wifi_ini_bin_start, _binary_wifi_ini_bin_end },
    { "bt_pskey.bin", _binary_bt_pskey_bin_start, _binary_bt_pskey_bin_end },
    { "bt_rf.bin",    _binary_bt_rf_bin_start,    _binary_bt_rf_bin_end },
};
static struct { char *data; uint32_t len; int tried; } fw_card[WCN_NFW];

static void fw_path(int i, char *out, int cap) { snprintf(out, (size_t)cap, "/System/Firmware/%s", FW[i].name); }

const uint8_t *wcn_blob(int which, uint32_t *len) {
    *len = 0;
    if (which < 0 || which >= WCN_NFW) return 0;
    uint32_t n = (uint32_t)(FW[which].end - FW[which].start);
    if (n) { *len = n; return FW[which].start; }
    if (!fw_card[which].tried && fs_ready()) {
        char p[64];
        fw_path(which, p, sizeof p);
        fw_card[which].tried = 1;
        if (fs_read(p, &fw_card[which].data, &fw_card[which].len) == 0)
            kprintf("[wcn] %s from the card (%u bytes)\n", FW[which].name, (unsigned)fw_card[which].len);
    }
    *len = fw_card[which].len;
    return (const uint8_t *)fw_card[which].data;
}

static int fw_on_card(int i) {
    char p[64];
    fs_ent_t e;
    fw_path(i, p, sizeof p);
    return fs_stat(p, &e) == 0 && e.size == (uint32_t)(FW[i].end - FW[i].start);
}

void wcn_keep_firmware(void) {
    if (!fs_ready() || FW[WCN_FW_MODEM].end == FW[WCN_FW_MODEM].start) return;
    for (int i = 0; i < WCN_NFW; i++) {
        if (fw_on_card(i)) continue;
        char p[64];
        fw_path(i, p, sizeof p);
        fs_mkdir("/System/Firmware");
        if (fs_write(p, FW[i].start, (uint32_t)(FW[i].end - FW[i].start)) == 0) kprintf("[wcn] kept %s on the card\n", p);
        else kprintf("[wcn] couldn't keep %s on the card: %s\n", p, fs_why());
    }
}

int wcn_firmware_safe(void) {
    if (FW[WCN_FW_MODEM].end == FW[WCN_FW_MODEM].start) return 1;     /* nothing here to lose */
    for (int i = 0; i < WCN_NFW; i++) if (!fw_on_card(i)) return 0;
    return 1;
}

/* ---- starting it ---------------------------------------------------------------------------- */
static int load_firmware(void) {
    uint32_t size;
    const uint8_t *fw = wcn_blob(WCN_FW_MODEM, &size);
    if (size < 65536) return fail("This kernel has no Wi-Fi firmware, and the card has no copy of it");
    uint64_t t = now_ms();
    for (uint32_t off = 0; off < size; off += 32768) {
        uint32_t n = size - off < 32768 ? size - off : 32768;
        if (wcn_dt_write(CP_START_ADDR + off, fw + off, n)) return fail("Writing the firmware into the chip failed");
        if ((off & 0x3FFFF) == 0) mini_pump();
    }
    /* Read a little back: if that differs, nothing after it will work. */
    uint32_t chk[8];
    if (wcn_dt_read(CP_START_ADDR + 0x1000, chk, sizeof chk) || memcmp(chk, fw + 0x1000, sizeof chk))
        return fail("The firmware didn't read back as written");
    kprintf("[wcn] firmware in: %u KiB in %lu ms\n", (unsigned)(size / 1024), (unsigned long)(now_ms() - t));
    return 0;
}

static int handshake(void) {
    /* Our SDIO settings, when it asks: on; receive in "SDMA" mode (one read
     * of a buffer of packets, its length said at its end); 840-byte blocks;
     * interrupts in band on DAT1; 20 ms wake pulses. */
    uint32_t cfg = 1u | (1u << 4) | (0u << 5) | (1u << 11) | (2u << 18);
    uint32_t st = 0;
    for (int i = 0; i < 25; i++) {
        if (wcn_reg_read(SYNC_ADDR, &st)) return fail("The chip stopped answering after its start");
        if (st == SYNC_CALI_WAITING) {
            wcn_reg_write(SYNC_ADDR + 7 * 4, cfg);
            wcn_reg_write(SYNC_ADDR, SYNC_CALI_WRITE_DONE);
            break;
        }
        wait_ms(20);
    }
    struct sync_info s;
    for (int i = 0; i < 100; i++) {
        if (wcn_dt_read(SYNC_ADDR, &s, sizeof s)) return fail("The chip stopped answering during its start");
        if (s.init_status == SYNC_ALL_FINISHED) {
            kprintf("[wcn] the firmware is running (project %x)\n", (unsigned)s.prj_type);
            return 0;
        }
        if (s.init_status == SYNC_VERIFY_WAITING) {
            uint8_t out[16];
            bind_verify((const uint8_t *)s.bind_verify, out);
            wcn_dt_write(SYNC_ADDR + 16 * 4, out, 16);
            wcn_reg_write(SYNC_ADDR, SYNC_VERIFY_WRITE_DONE);
            kprintf("[wcn] answered the bind check\n");
        }
        wait_ms(20);
    }
    char w[96];
    snprintf(w, sizeof w, "The firmware never said it was ready (state %x)", (unsigned)s.init_status);
    return fail(w);
}

int wcn_start(void) {
    if (W.state == WCN_READY || W.state == WCN_STARTING) return 0;
    if (g_plat != PLAT_H618) { snprintf(W.why, sizeof W.why, "No Wi-Fi chip here"); W.state = WCN_FAILED; return -1; }
    W.state = WCN_STARTING;
    W.why[0] = 0;
    uint64_t t0 = now_ms();
    if (!W.bounce) W.bounce = kmalloc(1024);
    if (!W.tx) W.tx = kmalloc(TXCAP);
    if (!W.rx) W.rx = kmalloc(RXCAP + 64);
    if (!W.bounce || !W.tx || !W.rx) return fail("Out of memory");
    W.dtbs = MAXPAC;
    if (sdio_init()) return fail("The Wi-Fi chip didn't answer on its bus");
    if (sdio_enable_fn(1)) return fail("The Wi-Fi chip's function 1 didn't start");
    if (sdio_set_blksz(1, BLK)) return fail("The Wi-Fi chip refused its block size");
    if (wcn_reg_read(CHIPID_REG, &W.chipid)) return fail("Reading the chip's ID failed");
    kprintf("[wcn] chip %08x (%s)\n", (unsigned)W.chipid,
            (W.chipid & 0xFFFFF000u) == 0x2355B000u ? "UWE5622" : "not the expected UWE5622");
    if ((W.chipid & 0xFFFFF000u) != 0x2355B000u) return fail("The Wi-Fi chip isn't the one this knows");
    if (load_firmware()) return -1;
    uint32_t rst;
    if (wcn_reg_read(CP_RESET_REG, &rst) || wcn_reg_write(CP_RESET_REG, rst & ~1u))
        return fail("Starting the chip's processor failed");
    if (handshake()) return -1;
    sdio_enable_irq(1);                                     /* packets announce themselves on DAT1 */
    W.boot_ms = (uint32_t)(now_ms() - t0);
    W.state = WCN_READY;
    kprintf("[wcn] ready in %u ms\n", (unsigned)W.boot_ms);
    return 0;
}

/* ---- packets -------------------------------------------------------------------------------
 *  Each one behind a 4-byte header: 6 bits padding, a checksum flag, a 16-bit
 *  length, an end flag, then the channel (4 bits) and a type (4 bits, 0).
 *  What we send on channel n arrives on the chip's n; what it sends on its
 *  channel n reaches us as n + 12 (Wi-Fi's commands go out on 8 and come
 *  back on 22). A write is the packets, an end header, and padding to whole
 *  blocks. A read is as long as the last one said the next would need: its
 *  last 8 bytes are the length of what's valid in it, and that need.     */
#define RX_BASE 12

static inline uint32_t puh(int chn, uint32_t len, int eof) {
    return (len << 7) | ((uint32_t)eof << 23) | ((uint32_t)(chn & 15) << 24);
}

void wcn_set_rx(int chn, wcn_rx_fn fn) { if (chn >= 0 && chn < 26) W.rxfn[chn] = fn; }

int wcn_send(int chn, const void *data, int len) {
    if (W.state != WCN_READY || len <= 0) return -1;
    uint32_t body = ((uint32_t)len + 3) & ~3u, n = 4 + body + 4;
    uint32_t total = (n + BLK - 1) / BLK * BLK;
    if (total > TXCAP) return -1;
    uint32_t h = puh(chn, (uint32_t)len, 0);
    memcpy(W.tx, &h, 4);
    memcpy(W.tx + 4, data, (size_t)len);
    memset(W.tx + 4 + len, 0, total - 4 - (uint32_t)len);
    h = puh(0, 0, 1);
    memcpy(W.tx + 4 + body, &h, 4);
    if (sdio_rw(1, 1, PK_PORT, 0, W.tx, total)) { W.tx_errs++; return -1; }
    W.tx_pkts++;
    return 0;
}

static void dispatch(uint8_t *buf, uint32_t valid, uint32_t cap) {
    uint8_t *p = buf, *end = buf + cap;
    for (uint32_t parsed = 0; parsed < valid && p + 4 <= end; ) {
        uint32_t h;
        memcpy(&h, p, 4);
        if ((h >> 23) & 1) break;
        uint32_t len = (h >> 7) & 0xFFFF, type = h >> 28;
        int chn = (int)((h >> 24) & 15) + RX_BASE;
        if (type != 0xF) {
            parsed += len;
            if ((h >> 6) & 1) len += 2;
            if (len == 0 || len > MAXPAC - 4 || p + 4 + len > end) break;
            W.rx_pkts++;
            if (W.rxfn[chn]) W.rxfn[chn](chn, p + 4, (int)len);
        }
        p += 4 + ((len + 3) & ~3u);
    }
}

/* Read until the chip says there's no more. */
static void read_all(void) {
    sdio_readb(0, 0x05, 0);                                 /* CCCR INTx, as Linux reads it */
    for (int guard = 0; guard < 64; guard++) {
        uint32_t len = W.dtbs;
        if (sdio_rw(0, 1, PK_PORT, 0, W.rx, len)) { W.rx_errs++; W.dtbs = MAXPAC; return; }
        W.rx_reads++;
        uint32_t valid, next;
        memcpy(&valid, W.rx + len - 8, 4);
        memcpy(&next, W.rx + len - 4, 4);
        if (valid > len) valid = 0;
        dispatch(W.rx, valid, len - 8);
        if (!next) { W.dtbs = MAXPAC; return; }
        uint32_t off = (next >> 10) + 1, want = next + 8 * off + 64;
        want = (want + BLK - 1) / BLK * BLK;
        W.dtbs = want > RXCAP ? RXCAP / BLK * BLK : want;
    }
}

/* The chip pulled DAT1 low: it has something for us. Not re-entered: a receive
 * handler may send, and a send may wait with the machine turning. */
void wcn_poll(void) {
    static int busy;
    if (W.state != WCN_READY || busy) return;
    if (!sdio_irq_pending()) return;
    busy = 1;
    W.irqs++;
    read_all();
    busy = 0;
}

int wcn_debug(char *out, int cap) {
    int n = sdio_debug(out, cap);
    n += snprintf(out + n, (size_t)(cap - n), "wcn: state %d, chip %08x, %s; tx %u (%u errors), rx %u in %u reads, %u irqs (%u errors)\n",
                  W.state, (unsigned)W.chipid, W.why, (unsigned)W.tx_pkts, (unsigned)W.tx_errs,
                  (unsigned)W.rx_pkts, (unsigned)W.rx_reads, (unsigned)W.irqs, (unsigned)W.rx_errs);
    return n;
}
