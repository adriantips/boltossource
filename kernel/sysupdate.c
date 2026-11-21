/* ===========================================================================
 *  BoltOS  -  kernel/sysupdate.c
 *
 *  New versions of the desktop and of BoltOS TV, from boltos.dev (see
 *  include/sysupdate.h for the files). It asks once the network is up and
 *  every six hours after, and then:
 *
 *    - a quick update, with automatic updates on, found in the first minutes
 *      after a start, installs and restarts into itself straight away;
 *    - anything else -- a big update, automatic updates off, or one found
 *      later -- waits for Update. A quick one still goes in at the next start.
 *
 *  Installing writes the new kernel.bin onto the disk this machine started
 *  from, beside the old one, and switches both ways of starting over:
 *
 *    1. KERNEL.NEW, written to the EFI system partition in one run of
 *       clusters, and read back against its SHA-256;
 *    2. the boot header at LBA 33, pointed at those sectors -- one sector
 *       write, and legacy BIOS starts the new kernel (stage2 reads sectors,
 *       not files, which is why the run has to be unbroken);
 *    3. kernel.bin's directory entry, pointed at the same clusters -- one
 *       more, and UEFI starts it too;
 *    4. KERNEL.NEW's entry removed and the old kernel's clusters freed.
 *
 *  A power cut leaves one kernel or the other at every step; an update that
 *  stopped between 2 and 4 is finished, or cleared away, by the next one. A
 *  disk with no EFI partition (QEMU's raw image) gets the new kernel in its
 *  free sectors after the old one, and only the header changes.
 *
 *  Which disk is "the one this machine started from" is worked out from
 *  what is on it: the boot header or the EFI partition's kernel.bin has
 *  this very kernel's length. A CD can't be written, so a machine started
 *  from one is told to use a USB stick instead.
 *
 *  The switch (Settings) and the version an update was last restarted for
 *  are kept in UPDATE.CFG on the same EFI partition. The second is what
 *  stops a release that doesn't start from being installed over and over.
 * ===========================================================================*/
#include <stdint.h>
#include "sysupdate.h"
#include "blk.h"
#include "boot.h"
#include "fat32.h"
#include "crypto.h"
#include "http.h"
#include "sched.h"
#include "io.h"
#include "timeout.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define EVERY_MS    (6ull * 3600 * 1000)
#define RETRY_MS    (10ull * 60 * 1000)
#define START_MS    (10ull * 60 * 1000)        /* "at the start": this soon after boot */
#define QUIET_MS    15000
#define MAX_KERNEL  (200u << 20)
#define CFG_PATH    "/UPDATE.CFG"
#define PREFS_PATH  "/PREFS.CFG"                /* the product's own settings: sysupd_prefs */

static const sysupd_host_t *H;
static sysupd_t U;
static struct {
    int      autoupd;
    char     tried[16];
    char     told[16];
    char     url[256];
    uint8_t  sha[32];
    uint64_t next, restart_at;
    int      startup, by_hand, loaded;
    volatile int want_check, want_install, want_save, want_load;
    volatile int thread;                        /* the update thread is running  */
    volatile int answered, installed;           /* for poll(), from the thread   */
    char     prefs[512];                        /* PREFS.CFG, as text            */
} S;

extern char __bss_start[];
#define KERNEL_LINK 0xFFFFFFFF80100000ull       /* linker.ld: where kernel.bin starts */

static uint64_t ms(void) { return now_us() / 1000; }

const sysupd_t *sysupd_state(void) { return &U; }
int sysupd_auto(void) { return S.autoupd; }

static void say(int state, const char *fmt, const char *arg) {
    snprintf(U.msg, sizeof U.msg, fmt, arg ? arg : "");
    U.state = state;
}

/* "1.10.2" against "1.9": < 0, 0, > 0. */
static int ver_cmp(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

/* ---- the description ---------------------------------------------------------------------- *
 *  It is release.sh's, flat, and Python writes anything past ASCII as \uXXXX: a
 *  key's string or number is all that's needed. Text comes out in the fonts'
 *  Latin-1, with the few punctuation marks cp1252 has. */
static int latin1(uint32_t u) {
    if (u < 0x100) return (int)u;
    switch (u) {
    case 0x2013: return 0x96; case 0x2014: return 0x97; case 0x2018: return 0x91; case 0x2019: return 0x92;
    case 0x201C: return 0x93; case 0x201D: return 0x94; case 0x2022: return 0x95; case 0x2026: return 0x85;
    case 0x2192: return '>';
    }
    return '?';
}

static int jfield(const char *j, const char *key, char *out, int cap) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j, pat);
    out[0] = 0;
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    int n = 0;
    if (*p != '"') {                                 /* a number                       */
        while ((*p >= '0' && *p <= '9') && n < cap - 1) out[n++] = *p++;
        out[n] = 0;
        return n;
    }
    for (p++; *p && *p != '"' && n < cap - 1; p++) {
        int c = (unsigned char)*p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = ' '; break;
            case 'u': {
                uint32_t u = 0;
                for (int k = 1; k <= 4 && p[k]; k++) {
                    char h = p[k];
                    u = u << 4 | (uint32_t)(h >= '0' && h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                p += 4;
                c = latin1(u);
                break;
            }
            default: c = (unsigned char)*p;
            }
        } else if (c >= 0xC0 && (p[1] & 0xC0) == 0x80) {   /* raw UTF-8, two bytes */
            c = latin1((uint32_t)(c & 0x1F) << 6 | (p[1] & 0x3F));
            p++;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static int unhex(const char *s, uint8_t *out, int n) {
    if ((int)strlen(s) != 2 * n) return -1;
    for (int i = 0; i < 2 * n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] << 4 | d : d);
    }
    return 0;
}

static int fetch(const char *url, const char *headers, char *buf, uint32_t cap, int *status) {
    if (H->fetch) return H->fetch(url, headers, buf, cap, status);
    return http_request("GET", url, headers, 0, 0, buf, cap, status, 0, 0);
}

/* ---- the boot disk ------------------------------------------------------------------------ */
typedef struct {
    blkdev_t *d;
    int       have_hdr;
    struct boot_header h;
    uint64_t  esp_base, esp_span;       /* 0 span: no EFI partition with kernel.bin */
} target_t;

static uint32_t running_len(void) { return (uint32_t)((uint64_t)(uintptr_t)__bss_start - KERNEL_LINK); }
static int is_us(uint32_t len) { uint32_t r = running_len(); return len <= r && len + 16 > r; }

static int read_header(blkdev_t *d, struct boot_header *h) {
    uint8_t sec[BLK_SECTOR];
    if (d->sectors <= BOOT_KERNEL_LBA || blk_read(d, BOOT_HDR_LBA, 1, sec) != 0) return 0;
    memcpy(h, sec, sizeof *h);
    return !memcmp(h->magic, BOOT_HDR_MAGIC, 8) && h->version >= 1 && h->version <= BOOT_HDR_VERSION &&
           h->kernel_sectors && h->kernel_lba + (uint64_t)h->kernel_sectors <= d->sectors;
}

/* The one FAT mount is borrowed; this puts back whatever had it. */
static struct { int had; blkdev_t *d; uint64_t base, span; } saved;
static void fat_borrow(void) { saved.had = fat32_where(&saved.d, &saved.base, &saved.span); }
static void fat_return(void) {
    fat32_flush();
    fat32_unmount();
    if (saved.had) fat32_mount_at(saved.d, saved.base, saved.span);
}

/* Mount d's EFI partition, if it has one with a kernel.bin: 1. */
static int mount_esp(blkdev_t *d, uint64_t *base, uint64_t *span) {
    blk_part p[4];
    if (blk_read_mbr(d, p) <= 0) return 0;
    for (int i = 0; i < 4; i++) {
        if (!p[i].present || (p[i].type != 0x0C && p[i].type != 0x0B && p[i].type != 0xEF && p[i].type != 0x06)) continue;
        if (fat32_mount_at(d, p[i].lba_start, p[i].sectors) != 0) continue;
        if (fat32_size("/kernel.bin") > 0) { *base = p[i].lba_start; *span = p[i].sectors; return 1; }
        fat32_unmount();
    }
    return 0;
}

/* With the FAT mount borrowed; on success the target's ESP (if any) is mounted. */
static int find_target(target_t *t, char *why, int cap) {
    memset(t, 0, sizeof *t);
    target_t best = { 0 };
    int score = 0;
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (!d || d->sector_size != BLK_SECTOR) continue;
        target_t c = { 0 };
        c.d = d;
        c.have_hdr = read_header(d, &c.h);
        int esp = mount_esp(d, &c.esp_base, &c.esp_span);
        int64_t klen = esp ? fat32_size("/kernel.bin") : -1;
        if (esp) fat32_unmount();
        int s = 0;
        if (c.have_hdr && is_us(c.h.kernel_bytes)) s += 4;
        if (klen > 0 && is_us((uint32_t)klen)) s += 4;
        if (!s && (c.have_hdr || esp)) s = 1;               /* a BoltOS disk, an older kernel on it */
        if (s && d->is_boot) s += 2;
        if (s && d->write) s += 1;
        if (s > score) { score = s; best = c; }
    }
    if (!score) { snprintf(why, (size_t)cap, "No disk here has BoltOS on it to update"); return -1; }
    if (!best.d->write) {
        snprintf(why, (size_t)cap, "This copy started from a CD, which can't be changed. Updates need the USB stick image.");
        return -1;
    }
    *t = best;
    if (t->esp_span && fat32_mount_at(t->d, t->esp_base, t->esp_span) != 0) t->esp_span = 0;
    snprintf(U.disk, sizeof U.disk, "%s, %s", t->d->name, t->esp_span ? "EFI partition" : "raw sectors");
    return 0;
}

static uint32_t sum32(const uint8_t *k, uint32_t len) {
    uint32_t s = 0, i = 0;
    for (; i + 4 <= len; i += 4) s += (uint32_t)k[i] | (uint32_t)k[i + 1] << 8 | (uint32_t)k[i + 2] << 16 | (uint32_t)k[i + 3] << 24;
    uint8_t tail[4] = { 0 };
    for (uint32_t j = 0; i + j < len; j++) tail[j] = k[i + j];
    if (i < len) s += (uint32_t)tail[0] | (uint32_t)tail[1] << 8 | (uint32_t)tail[2] << 16 | (uint32_t)tail[3] << 24;
    return s;                                           /* the zero padding adds nothing */
}

static int write_header(blkdev_t *d, uint64_t lba, uint32_t len, uint32_t ksum) {
    uint8_t sec[BLK_SECTOR];
    memset(sec, 0, sizeof sec);
    struct boot_header *h = (struct boot_header *)sec;
    memcpy(h->magic, BOOT_HDR_MAGIC, 8);
    h->version = BOOT_HDR_VERSION;
    h->kernel_lba = (uint32_t)lba;
    h->kernel_sectors = (len + BLK_SECTOR - 1) / BLK_SECTOR;
    h->kernel_bytes = len;
    h->kernel_sum = ksum;
    uint32_t s = 0;
    for (unsigned i = 0; i < sizeof(struct boot_header) - sizeof(uint32_t); i++) s += sec[i];
    h->checksum = s;
    return blk_write(d, BOOT_HDR_LBA, 1, sec) != 0 || blk_flush(d) != 0 ? -1 : 0;
}

/* What is at lba, read back from the disk: 0 if its SHA-256 is `want`. */
static int verify(blkdev_t *d, uint64_t lba, uint32_t len, const uint8_t want[32]) {
    enum { CHUNK = 128 };                               /* sectors: 64 KiB          */
    uint8_t *b = kmalloc(CHUNK * BLK_SECTOR);
    if (!b) return -1;
    sha256_ctx c;
    sha256_init(&c);
    for (uint32_t done = 0; done < len; ) {
        uint32_t n = len - done > CHUNK * BLK_SECTOR ? CHUNK * BLK_SECTOR : len - done;
        if (blk_read(d, lba + done / BLK_SECTOR, (n + BLK_SECTOR - 1) / BLK_SECTOR, b) != 0) { kfree(b); return -1; }
        sha256_update(&c, b, n);
        done += n;
    }
    kfree(b);
    uint8_t got[32];
    sha256_final(&c, got);
    return memcmp(got, want, 32) ? -1 : 0;
}

/* An update that stopped part-way: finished if the header already moved
 * to KERNEL.NEW, cleared away otherwise. */
static void tidy(target_t *t) {
    uint32_t c, len; uint64_t lba;
    if (fat32_locate("/KERNEL.NEW", &c, &len, &lba) != 0) return;
    uint32_t kc;
    if (fat32_locate("/kernel.bin", &kc, 0, 0) == 0 && kc == c) { fat32_remove("/KERNEL.NEW", 0); return; }
    if (t->have_hdr && t->h.kernel_lba == lba && t->h.kernel_bytes == len) {
        kprintf("[update] finishing an update that stopped half-way\n");
        if (fat32_take("/kernel.bin", "/KERNEL.NEW") == 0) fat32_remove("/KERNEL.NEW", 0);
        return;
    }
    fat32_remove("/KERNEL.NEW", 1);
}

static int install_on(target_t *t, const uint8_t *k, uint32_t len, char *why, int cap) {
    uint32_t sectors = (len + BLK_SECTOR - 1) / BLK_SECTOR;
    if (t->esp_span) {
        tidy(t);
        if (t->have_hdr) read_header(t->d, &t->h);
        uint64_t lba;
        int r = fat32_write_contiguous("/KERNEL.NEW", k, len, &lba);
        if (r == -2) { snprintf(why, (size_t)cap, "There isn't room for it beside the old one on the EFI partition"); return -1; }
        if (r) { snprintf(why, (size_t)cap, "The EFI partition wouldn't take it"); return -1; }
        if (verify(t->d, lba, len, S.sha)) {
            fat32_remove("/KERNEL.NEW", 1);
            snprintf(why, (size_t)cap, "The disk didn't keep what was written to it"); return -1;
        }
        if (t->have_hdr && write_header(t->d, lba, len, sum32(k, len))) {
            fat32_remove("/KERNEL.NEW", 1);
            snprintf(why, (size_t)cap, "The boot header couldn't be written"); return -1;
        }
        if (fat32_take("/kernel.bin", "/KERNEL.NEW")) { snprintf(why, (size_t)cap, "kernel.bin couldn't be switched over"); return -1; }
        fat32_remove("/KERNEL.NEW", 0);
        return 0;
    }
    if (!t->have_hdr) { snprintf(why, (size_t)cap, "This disk has no boot header to point at a new kernel"); return -1; }
    /* Raw sectors: in front of the old kernel if it fits there, else after it. */
    uint64_t at = BOOT_KERNEL_LBA;
    if (at + sectors > t->h.kernel_lba) at = ((uint64_t)t->h.kernel_lba + t->h.kernel_sectors + 2047) / 2048 * 2048;
    if (at + sectors > t->d->sectors) { snprintf(why, (size_t)cap, "This disk has no room for a second kernel"); return -1; }
    uint8_t *tail = kmalloc(BLK_SECTOR);
    if (!tail) { snprintf(why, (size_t)cap, "Out of memory"); return -1; }
    int bad = len / BLK_SECTOR && blk_write(t->d, at, len / BLK_SECTOR, k) != 0;
    if (!bad && len % BLK_SECTOR) {
        memset(tail, 0, BLK_SECTOR);
        memcpy(tail, k + len / BLK_SECTOR * BLK_SECTOR, len % BLK_SECTOR);
        bad = blk_write(t->d, at + len / BLK_SECTOR, 1, tail) != 0;
    }
    kfree(tail);
    if (bad || blk_flush(t->d)) { snprintf(why, (size_t)cap, "The disk wouldn't take it"); return -1; }
    if (verify(t->d, at, len, S.sha)) { snprintf(why, (size_t)cap, "The disk didn't keep what was written to it"); return -1; }
    if (write_header(t->d, at, len, sum32(k, len))) { snprintf(why, (size_t)cap, "The boot header couldn't be written"); return -1; }
    return 0;
}

/* ---- UPDATE.CFG ------------------------------------------------------------------------------- */
static void cfg_io(int save) {
    char why[128];
    target_t t;
    fat_borrow();
    if (find_target(&t, why, sizeof why) == 0 && t.esp_span) {
        char b[96];
        if (save) {
            int n = snprintf(b, sizeof b, "auto=%d\ntried=%s\n", S.autoupd, S.tried);
            fat32_write(CFG_PATH, b, (uint32_t)n);
            fat32_write(PREFS_PATH, S.prefs, (uint32_t)strlen(S.prefs));
        } else {
            int pn = fat32_read(PREFS_PATH, S.prefs, sizeof S.prefs - 1);
            S.prefs[pn > 0 ? pn : 0] = 0;
            int n = fat32_read(CFG_PATH, b, sizeof b - 1);
            if (n > 0) {
                b[n] = 0;
                const char *a = strstr(b, "auto="), *tr = strstr(b, "tried=");
                if (a) S.autoupd = a[5] == '1';
                if (tr) {
                    int k = 0;
                    for (tr += 6; *tr && *tr != '\n' && k < (int)sizeof S.tried - 1; tr++) S.tried[k++] = *tr;
                    S.tried[k] = 0;
                }
            }
        }
    }
    fat_return();
}

/* ---- the thread ------------------------------------------------------------------------------- */
static void do_check(void) {
    char *buf = kmalloc(16384);
    if (!buf) { say(SU_FAILED, "Out of memory", 0); return; }
    char url[128];
    snprintf(url, sizeof url, "https://boltos.dev/update/%s.json", H->channel);
    int status = 0;
    int n = fetch(url, 0, buf, 16384, &status);
    if (n <= 0 || status != 200) {
        kfree(buf);
        char m[64];
        if (status > 0) snprintf(m, sizeof m, "boltos.dev answered %d", status);
        else snprintf(m, sizeof m, "boltos.dev couldn't be reached");
        say(U.have ? SU_AVAILABLE : SU_FAILED, "%s", m);
        return;
    }
    char ver[16], u[256], sha[80], size[16], kind[8];
    jfield(buf, "version", ver, sizeof ver);
    jfield(buf, "url", u, sizeof u);
    jfield(buf, "sha256", sha, sizeof sha);
    jfield(buf, "size", size, sizeof size);
    jfield(buf, "kind", kind, sizeof kind);
    uint8_t sum[32];
    long sz = strtol(size, 0, 10);
    if (!ver[0] || strncmp(u, "https://", 8) || unhex(sha, sum, 32) || sz < 4096 || sz > (long)MAX_KERNEL) {
        kfree(buf);
        say(SU_FAILED, "boltos.dev's answer didn't make sense", 0);
        return;
    }
    U.checked = ms();
    if (ver_cmp(ver, H->version) <= 0) {
        kfree(buf);
        U.have = 0;
        char m[64];
        snprintf(m, sizeof m, "%s %s is the newest.", H->product, H->version);
        say(SU_CURRENT, "%s", m);
        kprintf("[update] %s is the newest (boltos.dev has %s)\n", H->version, ver);
        return;
    }
    jfield(buf, "date", U.date, sizeof U.date);
    jfield(buf, "notes", U.notes, sizeof U.notes);
    kfree(buf);
    snprintf(U.version, sizeof U.version, "%s", ver);
    snprintf(S.url, sizeof S.url, "%s", u);
    memcpy(S.sha, sum, 32);
    U.size = (uint32_t)sz;
    U.big = !strcmp(kind, "big");
    U.have = 1;
    char m[64];
    snprintf(m, sizeof m, "%s %s is ready to install.", H->product, U.version);
    say(SU_AVAILABLE, "%s", m);
    kprintf("[update] boltos.dev has %s (%s, %u KiB)\n", U.version, U.big ? "big" : "quick", (unsigned)(U.size >> 10));
}

/* The kernel, a few MiB at a time (Range requests, which worker.js answers
 * from R2): a connection that drops costs that piece, not the whole file,
 * and each piece is asked for again a few times before the download gives up. */
#define PIECE (4u << 20)
static volatile uint32_t piece_at;               /* where the piece being fetched starts */

static int download(char *buf, char *why, int cap) {
    char *tmp = kmalloc(PIECE + 65536);
    if (!tmp) { snprintf(why, (size_t)cap, "Not enough memory for the download"); return -1; }
    for (uint32_t off = 0; off < U.size; ) {
        uint32_t n = U.size - off < PIECE ? U.size - off : PIECE;
        char range[64];
        snprintf(range, sizeof range, "Range: bytes=%u-%u\r\n", off, off + n - 1);
        int got = -1, status = 0;
        for (int tries = 0; tries < 6 && got != (int)n; tries++) {
            if (tries) { kprintf("[update] piece at %u came back short (%d, HTTP %d); again\n", off, got, status); delay_us(2000000ull * tries); }
            piece_at = off;
            http_rx_bytes = 0;
            got = fetch(S.url, range, tmp, PIECE + 65536, &status);
            if (got == (int)U.size && status == 200 && off == 0) { memcpy(buf, tmp, U.size); kfree(tmp); return 0; }
            if (status != 206 && status != 200 && status > 0) break;
        }
        if (got != (int)n) {
            kfree(tmp);
            snprintf(why, (size_t)cap, status > 0 && status != 206 ? "boltos.dev answered %d" : "The download kept stopping part-way", status);
            return -1;
        }
        memcpy(buf + off, tmp, n);
        off += n;
        U.got = off;
    }
    kfree(tmp);
    return 0;
}

static void do_install(void) {
    char *buf = kmalloc(U.size + 16);
    if (!buf) { say(SU_FAILED, "Not enough memory for the download", 0); return; }
    say(SU_DOWNLOADING, "Downloading" "\x85", 0);
    U.got = 0;
    kprintf("[update] downloading %s from %s\n", U.version, S.url);
    char err[128];
    uint8_t sum[32];
    if (download(buf, err, sizeof err)) {
        kfree(buf);
        say(SU_FAILED, "%s", err);
        kprintf("[update] downloading %s failed: %s\n", U.version, err);
        return;
    }
    int n = (int)U.size;
    sha256(buf, (uint32_t)n, sum);
    if (memcmp(sum, S.sha, 32)) { kfree(buf); say(SU_FAILED, "The download didn't match its checksum", 0); return; }
    say(SU_INSTALLING, "Putting it on the disk" "\x85", 0);
    char why[128];
    target_t t;
    uint64_t t0 = ms();
    fat_borrow();
    int r = find_target(&t, why, sizeof why);
    if (r == 0) r = install_on(&t, (const uint8_t *)buf, (uint32_t)n, why, sizeof why);
    fat_return();
    kfree(buf);
    if (r) { say(SU_FAILED, "%s", why); kprintf("[update] installing %s failed: %s\n", U.version, why); return; }
    kprintf("[update] %s is on %s (%lu ms)\n", U.version, U.disk, (unsigned long)(ms() - t0));
    snprintf(S.tried, sizeof S.tried, "%s", U.version);
    cfg_io(1);
    char m[64];
    snprintf(m, sizeof m, "Restarting into %s %s" "\x85", H->product, U.version);
    say(SU_RESTART, "%s", m);
    S.installed = 1;
}

static void worker(void) {
    for (;;) {
        if (S.want_load)    { S.want_load = 0; cfg_io(0); S.loaded = 1; continue; }
        if (S.want_save)    { S.want_save = 0; cfg_io(1); continue; }
        if (S.want_install) { S.want_install = 0; do_install(); continue; }
        if (S.want_check)   { S.want_check = 0; do_check(); S.answered = 1; continue; }
        uint64_t f = irq_save();
        int more = S.want_load || S.want_save || S.want_install || S.want_check;
        if (!more) S.thread = 0;
        irq_restore(f);
        if (!more) break;
    }
    sched_exit_current();
    for (;;) __asm__ volatile("hlt");
}

static void kick(void) {
    uint64_t f = irq_save();
    int start = !S.thread;
    if (start) S.thread = 1;
    irq_restore(f);
    if (start && sched_add(worker, "update") < 0) { S.thread = 0; kprintf("[update] no thread for updates\n"); }
}

/* ---- the shell's side ------------------------------------------------------------------------- */
void sysupd_init(const sysupd_host_t *host) {
    H = host;
    S.autoupd = 1;
    S.startup = 1;
    say(SU_WAITING, "Waiting for the network" "\x85", 0);
    S.want_load = 1;
    kick();
}

void sysupd_set_auto(int on) { S.autoupd = !!on; S.want_save = 1; kick(); }

int sysupd_prefs_loaded(void) { return S.loaded >= 1; }
void sysupd_prefs_get(char *out, int cap) {
    uint64_t f = irq_save();
    snprintf(out, (size_t)cap, "%s", S.prefs);
    irq_restore(f);
}
void sysupd_prefs_set(const char *text) {
    uint64_t f = irq_save();
    snprintf(S.prefs, sizeof S.prefs, "%s", text ? text : "");
    irq_restore(f);
    S.want_save = 1;
    kick();
}

void sysupd_check(void) {
    if (U.state == SU_CHECKING || U.state == SU_DOWNLOADING || U.state == SU_INSTALLING || U.state == SU_RESTART) return;
    S.next = ms();
}

int sysupd_install(void) {
    if (!U.have || U.state == SU_CHECKING || U.state == SU_DOWNLOADING || U.state == SU_INSTALLING || U.state == SU_RESTART) return -1;
    S.by_hand = 1;
    U.state = SU_DOWNLOADING;
    S.want_install = 1;
    kick();
    return 0;
}

void sysupd_restart(void) { if (U.state == SU_RESTART && H->reboot) H->reboot(); }

int sysupd_poll(void) {
    static int last_state = -1;
    static uint32_t last_got;
    if (!H) return 0;
    uint64_t now = ms();
    if (S.loaded == 1) {                              /* what UPDATE.CFG said, once */
        S.loaded = 2;
        if (S.tried[0] && !strcmp(S.tried, H->version)) {
            kprintf("[update] now on %s\n", H->version);
            if (H->toast) { char t[48]; snprintf(t, sizeof t, "%s %s", H->product, H->version); H->toast(t, "is installed"); }
            S.tried[0] = 0;
            S.want_save = 1; kick();
        } else if (S.tried[0] && ver_cmp(S.tried, H->version) > 0)
            kprintf("[update] restarted for %s but this is %s: it isn't tried by itself again\n", S.tried, H->version);
    }
    if (S.loaded == 2 && !S.thread && U.state != SU_DOWNLOADING && U.state != SU_INSTALLING && U.state != SU_RESTART) {
        if (!S.next && H->online && H->online()) S.next = now + 2000;
        if (S.next && now >= S.next && H->online && H->online()) {
            S.next = 0;
            if (!U.have) say(SU_CHECKING, "Checking boltos.dev" "\x85", 0);
            S.want_check = 1;
            kick();
        }
    }
    if (S.startup && now > START_MS && !S.want_check && U.state != SU_CHECKING) S.startup = 0;
    if (S.answered) {
        S.answered = 0;
        int startup = S.startup;
        S.startup = 0;
        S.next = now + (U.state == SU_FAILED ? RETRY_MS : EVERY_MS);
        if (U.have && U.state == SU_AVAILABLE) {
            if (S.autoupd && !U.big && startup && strcmp(S.tried, U.version)) {
                kprintf("[update] installing %s by itself\n", U.version);
                sysupd_install();
                S.by_hand = 0;
            } else if (strcmp(S.told, U.version)) {
                snprintf(S.told, sizeof S.told, "%s", U.version);
                if (H->toast) { char t[48]; snprintf(t, sizeof t, "%s %s", H->product, U.version); H->toast(t, "is ready to install: Settings"); }
            }
        }
    }
    if (U.state == SU_DOWNLOADING) {
        uint32_t g = piece_at + (http_rx_bytes > PIECE ? PIECE : http_rx_bytes);
        U.got = g > U.size ? U.size : g;
    }
    if (S.installed) {
        S.installed = 0;
        S.restart_at = now + (S.by_hand ? 1500 : 4000);
        if (H->toast) H->toast("Updated", "restarting to finish");
    }
    if (U.state == SU_RESTART && S.restart_at && now >= S.restart_at) {
        if (S.by_hand || !H->idle_ms || H->idle_ms() >= QUIET_MS) { if (H->reboot) H->reboot(); }
        S.restart_at = now + 1000;
    }
    int changed = U.state != last_state || (U.state == SU_DOWNLOADING && U.got / 65536 != last_got / 65536);
    last_state = U.state; last_got = U.got;
    return changed;
}
