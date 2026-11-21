/* ===========================================================================
 *  BoltOS Mini  -  mini/lx.c
 *
 *  A Linux program on a spare core (lx.h): its memory, loading it, running
 *  it at EL0, its traps, and the mailbox to the boot core.
 *
 *  Memory is both big RAM windows, handed out a 4 KiB page at a time from
 *  a bitmap: mmap, brk, the stack, the program's own files in memory, the
 *  framebuffer and the pack cache all come from there. The windows are
 *  identity-mapped like the rest of RAM, so an address the program sees is
 *  the address Mini sees; the program's core runs on a copy of the page
 *  tables in which only those windows are EL0's (cpu.c, mmu_user_table).
 *
 *  Loading is the kernel's half of exec: the program's PT_LOAD segments, the
 *  interpreter it names (musl's ld.so) if it is dynamic, and a stack with
 *  argv, envp and the auxiliary vector; then an eret to EL0 at the entry.
 *  An `svc #0` there comes back to Mini's vectors on this core (kind 4),
 *  and lx_trap hands it to lx_syscall. Anything else that traps -- a bad
 *  address, an undefined instruction, a fault in a syscall on the program's
 *  behalf -- ends the program with a report, and the core goes back to the
 *  pool. Exits come back the same way: a longjmp to where the job began.
 *
 *  The program's core never touches the card, the network or the kernel
 *  heap: it asks the boot core (lx_request), which answers from lx_poll in
 *  the main loop, as rt.c's programs do. What it prints goes through a ring.
 * ===========================================================================*/
#include <stdint.h>
#include <stdarg.h>
#include "mini.h"
#include "lxint.h"
#include "rt.h"
#include "pkg.h"
#include "fs.h"
#include "store.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

uint64_t mmu_user_table(const uint64_t *base, const uint64_t *size, int n);
void     mmu_use(uint64_t ttbr0);
int      lxfs_store_read(int pack, uint64_t off, void *buf, uint32_t len);   /* lx_fs.c, boot core */

#define OUTCAP  (64 * 1024)
#define CONCAP  (16 * 1024)
#define KARENA  (2u << 20)
#define LX_FAT_RESERVE (24ull << 20)

/* ---- the shared state ------------------------------------------------------------------- */
static struct {
    /* written by the boot core */
    volatile int      stop __attribute__((aligned(64)));
    volatile uint32_t out_tail;
    volatile uint32_t rq_served;
    /* written by the program's core */
    volatile uint32_t out_head __attribute__((aligned(64)));
    volatile int      state;
    volatile uint32_t rq_seq;
    lx_rq_t          *rq;
    int               exit_code, crashed;
    char              msg[200];
    /* set up before the start */
    int               cpu, active;
    lx_config_t       cfg;
    char              strs[4096];              /* argv, envp, paths: copies    */
    char             *out;                     /* [OUTCAP]                     */
    char              con[CONCAP];             /* the console's tail, boot core */
    int               con_n;
    char              line[512];               /* a line being put together     */
    int               line_n;
    uint64_t          t0, t1;
    rt_jmp_t          top;
    uint64_t          ttbr_user;
    int               cleaned;
} L;

static inline uint32_t acq(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void rel(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static void set_state(int s) { __atomic_store_n(&L.state, s, __ATOMIC_RELEASE); }
int lx_state(void) { return __atomic_load_n(&L.state, __ATOMIC_ACQUIRE); }
int lx_stopping(void) { return __atomic_load_n(&L.stop, __ATOMIC_ACQUIRE); }
int lx_exit_code(void) { return L.exit_code; }
int lx_crashed(void) { return L.crashed; }
int lx_cpu(void) { int s = lx_state(); return (s == LX_RUNNING || s == LX_STARTING) ? L.cpu : 0; }
uint64_t lx_elapsed_ms(void) { return L.t0 ? (L.t1 ? L.t1 : now_ms()) - L.t0 : 0; }

static int this_cpu(void) { uint64_t m; __asm__ volatile("mrs %0, mpidr_el1" : "=r"(m)); return (int)(m & 0xFF); }

/* ---- time on the program's core ----------------------------------------------------------- */
static uint64_t cntfrq(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 24000000; }
uint64_t lx_now_ns(void) {
    uint64_t c, f = cntfrq();
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(c));
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}
uint64_t lx_real_ns(void) {
    static uint64_t base_unix, base_ns;           /* the clock's second, pinned to a monotonic ns */
    uint64_t u = clock_unix(), n = lx_now_ns();
    if (u != base_unix) { base_unix = u; base_ns = n; }
    uint64_t sub = n - base_ns;
    if (sub > 999999999ull) sub = 999999999ull;
    return u * 1000000000ull + sub;
}
/* Waiting: WFE, which the counter's event stream (set up on this core in
 * job()) wakes every ~85 us, and the boot core's SEV sooner. */
void lx_wait_until(uint64_t ns) {
    while (lx_now_ns() < ns && !lx_stopping()) __asm__ volatile("wfe");
}

/* ---- output: a ring the boot core drains ---------------------------------------------------- */
void lx_out(const char *s, uint64_t n) {
    while (n) {
        uint32_t h = L.out_head, t = acq(&L.out_tail);
        uint32_t room = OUTCAP - 1 - (h - t);
        if (!room) { if (lx_stopping()) return; __asm__ volatile("wfe"); continue; }
        uint32_t k = n < room ? (uint32_t)n : room;
        for (uint32_t i = 0; i < k; i++) L.out[(h + i) % OUTCAP] = s[i];
        rel(&L.out_head, h + k);
        s += k; n -= k;
    }
}

void lx_log(const char *fmt, ...) {
    char b[400];
    int n = snprintf(b, sizeof b, "\x01");          /* marks Mini's own line ([lx] in the log) */
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    lx_out(b, (uint64_t)n);
}

/* ---- the mailbox ------------------------------------------------------------------------------- */
int lx_request(lx_rq_t *rq) {
    L.rq = rq;
    uint32_t seq = L.rq_seq + 1;
    rel(&L.rq_seq, seq);
    __asm__ volatile("dsb ish\nsev" ::: "memory");
    while (acq(&L.rq_served) != seq) __asm__ volatile("wfe");
    return rq->result;
}

static void serve(lx_rq_t *q) {
    q->result = 0;
    switch (q->kind) {
    case RQ_STORE:
        q->result = lxfs_store_read(q->pack, q->off, q->buf, q->len) ? -L_EIO : 0;
        break;
    case RQ_FSREAD: {
        char *b; uint32_t n;
        if (fs_read(q->path, &b, &n)) { q->result = -L_ENOENT; break; }
        uint32_t k = n < q->len ? n : q->len;
        memcpy(q->buf, b, k);
        kfree(b);
        q->result = (int)n;
        break;
    }
    case RQ_FSWRITE: {
        /* The FAT partition is Mini's too (settings, the next kernel): a
         * program's files never take the last LX_FAT_RESERVE of it. */
        uint64_t fr, tot;
        fs_ent_t old;
        uint64_t had = fs_stat(q->path, &old) ? 0 : old.size;
        if (!fs_space(&fr, &tot) && fr + had < (uint64_t)q->len + LX_FAT_RESERVE) {
            static uint64_t told;
            if (now_ms() - told > 10000) { told = now_ms(); kprintf("[lx] the card's FAT partition is nearly full: %s not saved\n", q->path); }
            q->result = -L_ENOSPC;
            break;
        }
        char dir[256];
        snprintf(dir, sizeof dir, "%s", q->path);
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) { *sl = 0; fs_mkdir(dir); }
        q->result = fs_write(q->path, q->buf, q->len) ? -L_EIO : 0;
        if (q->result) kprintf("[lx] can't write %s: %s\n", q->path, fs_why());
        break;
    }
    case RQ_MKDIR:  q->result = fs_mkdir(q->path) ? -L_EIO : 0; break;
    case RQ_REMOVE: q->result = fs_remove(q->path) ? -L_ENOENT : 0; break;
    case RQ_RENAME: {
        fs_ent_t st;
        if (!fs_stat(q->path2, &st)) fs_remove(q->path2);
        char dir[256];
        snprintf(dir, sizeof dir, "%s", q->path2);
        char *sl = strrchr(dir, '/');
        if (sl && sl != dir) { *sl = 0; fs_mkdir(dir); }
        q->result = fs_rename(q->path, q->path2) ? -L_EIO : 0;
        break;
    }
    case RQ_NET: lx_net_serve(q); break;
    default: q->result = -L_ENOSYS;
    }
}

/* ---- the program's memory: pages of both windows ------------------------------------------------ */
#define NWIN 2
static const uint64_t win_base[NWIN] = { MINI_LOW_BASE, MINI_HIGH_BASE };
static const uint64_t win_size[NWIN] = { MINI_LOW_SIZE, MINI_HIGH_SIZE };
#define NPAGES ((MINI_LOW_SIZE + MINI_HIGH_SIZE) / 4096)
static uint64_t *bm;                          /* NPAGES bits, kmalloc'd: 1 = in use */
static uint64_t *bmk;                         /* and of those, Mini's own (lx_kpages) */
static uint64_t pages_used, hint;
static uint8_t *karena; static uint64_t karena_used;

static int64_t page_of(uint64_t a) {
    if (a >= win_base[0] && a < win_base[0] + win_size[0]) return (int64_t)((a - win_base[0]) >> 12);
    if (a >= win_base[1] && a < win_base[1] + win_size[1]) return (int64_t)((MINI_LOW_SIZE + a - win_base[1]) >> 12);
    return -1;
}
static uint64_t addr_of(uint64_t p) {
    uint64_t o = p << 12;
    return o < MINI_LOW_SIZE ? win_base[0] + o : win_base[1] + (o - MINI_LOW_SIZE);
}
static inline int bit(uint64_t p) { return (int)(bm[p >> 6] >> (p & 63) & 1); }
static void mark(uint64_t p, uint64_t n, int on) {
    for (uint64_t i = p; i < p + n; i++) {
        if (on) bm[i >> 6] |= 1ull << (i & 63); else bm[i >> 6] &= ~(1ull << (i & 63));
    }
    if (on) pages_used += n; else pages_used -= n;
}

/* k free pages in a row inside one window, from page `from` on. */
static int64_t find_run(uint64_t lo, uint64_t hi, uint64_t from, uint64_t k) {
    uint64_t run = 0, start = from;
    for (uint64_t p = from; p < hi; ) {
        if ((p & 63) == 0 && run == 0 && bm[p >> 6] == ~0ull) { p += 64; start = p; continue; }
        if (bit(p)) { run = 0; start = ++p; continue; }
        if (++run == k) return (int64_t)start;
        p++;
    }
    (void)lo;
    return -1;
}

void *lx_pages(uint64_t bytes) {
    uint64_t k = (bytes + 4095) >> 12;
    if (!k) k = 1;
    uint64_t n0 = MINI_LOW_SIZE >> 12, ranges[2][2] = { { 0, n0 }, { n0, NPAGES } };
    int64_t at = -1;
    /* From the last place a run was found, then from each window's start. */
    for (int w = 0; w < 2 && at < 0; w++)
        if (hint >= ranges[w][0] && hint < ranges[w][1]) at = find_run(ranges[w][0], ranges[w][1], hint, k);
    for (int w = 0; w < 2 && at < 0; w++) at = find_run(ranges[w][0], ranges[w][1], ranges[w][0], k);
    if (at < 0) return 0;
    mark((uint64_t)at, k, 1);
    hint = (uint64_t)at + k;
    void *p = (void *)(uintptr_t)addr_of((uint64_t)at);
    memset(p, 0, k << 12);
    return p;
}

int lx_pages_at(uint64_t addr, uint64_t bytes) {
    if (addr & 4095) return -1;
    uint64_t k = (bytes + 4095) >> 12;
    int64_t p = page_of(addr), q = page_of(addr + (k << 12) - 1);
    if (p < 0 || q < 0 || q - p + 1 != (int64_t)k) return -1;
    for (uint64_t i = 0; i < k; i++) if (bit((uint64_t)p + i)) return -1;
    mark((uint64_t)p, k, 1);
    memset((void *)(uintptr_t)addr, 0, k << 12);
    return 0;
}

int lx_pages_used(uint64_t addr, uint64_t bytes) {
    uint64_t k = (bytes + 4095) >> 12;
    int64_t p = page_of(addr), q = page_of(addr + (k << 12) - 1);
    if (p < 0 || q < 0 || q - p + 1 != (int64_t)k) return 0;
    for (uint64_t i = 0; i < k; i++) if (!bit((uint64_t)p + i)) return 0;
    return 1;
}

void lx_unpages(uint64_t addr, uint64_t bytes) {
    addr &= ~4095ull;
    uint64_t k = (bytes + 4095) >> 12;
    for (uint64_t i = 0; i < k; i++) {
        int64_t p = page_of(addr + (i << 12));
        if (p >= 0 && bit((uint64_t)p) && !(bmk[p >> 6] >> (p & 63) & 1)) mark((uint64_t)p, 1, 0);
    }
}

void *lx_kpages(uint64_t bytes) {
    void *v = lx_pages(bytes);
    if (!v) return 0;
    int64_t p = page_of((uint64_t)(uintptr_t)v);
    for (uint64_t i = 0; i < (bytes + 4095) >> 12; i++) bmk[(uint64_t)(p + (int64_t)i) >> 6] |= 1ull << ((uint64_t)(p + (int64_t)i) & 63);
    return v;
}

void lx_kunpages(uint64_t addr, uint64_t bytes) {
    uint64_t k = (bytes + 4095) >> 12;
    for (uint64_t i = 0; i < k; i++) {
        int64_t p = page_of((addr & ~4095ull) + (i << 12));
        if (p < 0) continue;
        bmk[p >> 6] &= ~(1ull << (p & 63));
        if (bit((uint64_t)p)) mark((uint64_t)p, 1, 0);
    }
}

int lx_pages_force(uint64_t addr, uint64_t bytes) {
    if (addr & 4095) return -1;
    uint64_t k = (bytes + 4095) >> 12;
    int64_t p = page_of(addr), q = page_of(addr + (k << 12) - 1);
    if (p < 0 || q < 0 || q - p + 1 != (int64_t)k) return -1;
    for (uint64_t i = 0; i < k; i++) if (bmk[(uint64_t)(p + (int64_t)i) >> 6] >> ((uint64_t)(p + (int64_t)i) & 63) & 1) return -1;
    for (uint64_t i = 0; i < k; i++) if (!bit((uint64_t)p + i)) mark((uint64_t)p + i, 1, 1);
    return 0;
}

uint64_t lx_mem_total(void) { return (uint64_t)NPAGES << 12; }
uint64_t lx_mem_free(void) { return ((uint64_t)NPAGES - pages_used) << 12; }

int lx_uok(uint64_t p, uint64_t len) {
    if (p + len < p) return 0;
    for (int w = 0; w < NWIN; w++)
        if (p >= win_base[w] && p + len <= win_base[w] + win_size[w]) return 1;
    return 0;
}

int lx_ustr(uint64_t p, char *out, int cap) {
    for (int i = 0; i < cap; i++) {
        if (!lx_uok(p + (uint64_t)i, 1)) return -L_EFAULT;
        out[i] = *(const char *)(uintptr_t)(p + (uint64_t)i);
        if (!out[i]) return 0;
    }
    out[cap - 1] = 0;
    return -L_ENAMETOOLONG;
}

void *lx_kalloc(uint64_t n) {
    n = (n + 15) & ~15ull;
    if (karena_used + n > KARENA) return 0;
    void *p = karena + karena_used;
    karena_used += n;
    memset(p, 0, n);
    return p;
}

void lx_sync_code(uint64_t addr, uint64_t len) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ull << ((ctr >> 16) & 15);
    for (uint64_t a = addr & ~(line - 1); a < addr + len; a += line) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\nic iallu\ndsb ish\nisb" ::: "memory");
}

/* ---- ELF ----------------------------------------------------------------------------------------- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ehdr_t;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;
enum { PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6 };

typedef struct { uint64_t base, entry, phdr; int phnum; char interp[128]; } image_t;

static int load_elf(const char *path, image_t *im, char *why, int cap) {
    lxfd_t fd;
    memset(&fd, 0, sizeof fd);
    memset(im, 0, sizeof *im);
    int r = lxfs_open(path, 0, 0, &fd);
    if (r) { snprintf(why, (size_t)cap, "%s: %s", path, r == -L_ENOENT ? "no such file" : "can't open it"); return -1; }
    ehdr_t eh;
    phdr_t ph[24];
    if (lxfs_read(&fd, &eh, sizeof eh, 0) != (int64_t)sizeof eh || memcmp(eh.ident, "\x7f" "ELF", 4) ||
        eh.ident[4] != 2 || eh.machine != 183 || (eh.type != 2 && eh.type != 3) ||
        eh.phentsize != sizeof(phdr_t) || eh.phnum > 24) {
        snprintf(why, (size_t)cap, "%s isn't an arm64 Linux program", path);
        lxfs_close(&fd); return -1;
    }
    lxfs_read(&fd, ph, sizeof(phdr_t) * eh.phnum, eh.phoff);
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type == PT_INTERP) {
            uint64_t n = ph[i].filesz < sizeof im->interp - 1 ? ph[i].filesz : sizeof im->interp - 1;
            lxfs_read(&fd, im->interp, n, ph[i].offset);
            im->interp[n] = 0;
        }
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    lo &= ~4095ull; hi = (hi + 4095) & ~4095ull;
    if (hi <= lo) { snprintf(why, (size_t)cap, "%s has nothing to load", path); lxfs_close(&fd); return -1; }
    uint64_t at;
    if (eh.type == 3) {
        void *p = lx_pages(hi - lo);
        if (!p) { snprintf(why, (size_t)cap, "out of memory loading %s", path); lxfs_close(&fd); return -1; }
        at = (uint64_t)(uintptr_t)p;
        im->base = at - lo;
    } else {
        if (lx_pages_at(lo, hi - lo)) { snprintf(why, (size_t)cap, "%s wants addresses Mini can't give it (non-PIE?)", path); lxfs_close(&fd); return -1; }
        im->base = 0;
    }
    for (int i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD || !ph[i].filesz) continue;
        int64_t got = lxfs_read(&fd, (void *)(uintptr_t)(im->base + ph[i].vaddr), ph[i].filesz, ph[i].offset);
        if (got != (int64_t)ph[i].filesz) { snprintf(why, (size_t)cap, "%s: short read", path); lxfs_close(&fd); return -1; }
    }
    lx_sync_code(im->base + lo, hi - lo);
    im->entry = im->base + eh.entry;
    im->phnum = eh.phnum;
    for (int i = 0; i < eh.phnum; i++) if (ph[i].type == PT_PHDR) im->phdr = im->base + ph[i].vaddr;
    if (!im->phdr)
        for (int i = 0; i < eh.phnum; i++)
            if (ph[i].type == PT_LOAD && eh.phoff >= ph[i].offset && eh.phoff < ph[i].offset + ph[i].filesz)
                im->phdr = im->base + ph[i].vaddr + (eh.phoff - ph[i].offset);
    lxfs_note_map(path, im->base + lo, hi - lo);
    lxfs_close(&fd);
    return 0;
}

/* The hardware's features, as Linux tells a program in AT_HWCAP. */
static uint64_t hwcap(void) {
    uint64_t isar0, pfr0, cap = 1 | 2;                       /* FP, ASIMD */
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    uint64_t aes = isar0 >> 4 & 15, sha1 = isar0 >> 8 & 15, sha2 = isar0 >> 12 & 15, crc = isar0 >> 16 & 15,
             atom = isar0 >> 20 & 15;
    if (aes >= 1) cap |= 1 << 3;
    if (aes >= 2) cap |= 1 << 4;
    if (sha1) cap |= 1 << 5;
    if (sha2) cap |= 1 << 6;
    if (crc) cap |= 1 << 7;
    if (atom >= 2) cap |= 1 << 8;
    if ((pfr0 >> 16 & 15) == 1) cap |= 1 << 9;               /* FPHP */
    if ((pfr0 >> 20 & 15) == 1) cap |= 1 << 10;              /* ASIMDHP */
    return cap;
}

/* The stack Linux gives a new program: argc, argv, envp, auxv, the strings. */
static uint64_t build_stack(uint64_t top, const image_t *app, const image_t *ld, uint64_t *sp_out) {
    uint64_t p = top;
    const char *const *argv = L.cfg.argv, *const *envp = L.cfg.envp;
    int argc = 0, envc = 0;
    while (argc < LX_MAXARG && argv[argc]) argc++;
    while (envc < LX_MAXARG && envp[envc]) envc++;
    uint64_t av[LX_MAXARG], ev[LX_MAXARG];
    for (int i = envc - 1; i >= 0; i--) { size_t n = strlen(envp[i]) + 1; p -= n; memcpy((void *)(uintptr_t)p, envp[i], n); ev[i] = p; }
    for (int i = argc - 1; i >= 0; i--) { size_t n = strlen(argv[i]) + 1; p -= n; memcpy((void *)(uintptr_t)p, argv[i], n); av[i] = p; }
    p -= 8; memcpy((void *)(uintptr_t)p, "aarch64", 8);
    uint64_t plat = p;
    p -= 16;
    uint64_t rnd = p;
    for (int i = 0; i < 16; i++) ((uint8_t *)(uintptr_t)rnd)[i] = (uint8_t)(mini_rand() ^ (lx_now_ns() >> (i & 7)));
    p &= ~15ull;
    uint64_t aux[][2] = {
        { 3, app->phdr }, { 4, sizeof(phdr_t) }, { 5, (uint64_t)app->phnum }, { 6, 4096 },
        { 7, ld ? ld->base : 0 }, { 8, 0 }, { 9, app->entry }, { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 },
        { 15, plat }, { 16, hwcap() }, { 17, 100 }, { 23, 0 }, { 25, rnd }, { 26, 0 }, { 31, av[0] },
        { 51, 4096 }, { 0, 0 },
    };
    int nwords = 1 + argc + 1 + envc + 1 + (int)(sizeof aux / 8);
    p -= (uint64_t)nwords * 8;
    p &= ~15ull;
    uint64_t *w = (uint64_t *)(uintptr_t)p;
    int k = 0;
    w[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[k++] = av[i];
    w[k++] = 0;
    for (int i = 0; i < envc; i++) w[k++] = ev[i];
    w[k++] = 0;
    for (unsigned i = 0; i < sizeof aux / 16; i++) { w[k++] = aux[i][0]; w[k++] = aux[i][1]; }
    *sp_out = p;
    return 0;
}

/* ---- running it ------------------------------------------------------------------------------------ */
__asm__(
    ".text\n.global lx_enter_el0\n.type lx_enter_el0,%function\nlx_enter_el0:\n"
    "  msr sp_el0, x1\n  msr elr_el1, x0\n  mov x2, #0x3c0\n  msr spsr_el1, x2\n"
    "  msr fpcr, xzr\n  msr fpsr, xzr\n  msr tpidr_el0, xzr\n"
    "  movi v0.2d, #0\n  movi v1.2d, #0\n  movi v2.2d, #0\n  movi v3.2d, #0\n  movi v4.2d, #0\n  movi v5.2d, #0\n"
    "  movi v6.2d, #0\n  movi v7.2d, #0\n  movi v8.2d, #0\n  movi v9.2d, #0\n  movi v10.2d, #0\n  movi v11.2d, #0\n"
    "  movi v12.2d, #0\n  movi v13.2d, #0\n  movi v14.2d, #0\n  movi v15.2d, #0\n  movi v16.2d, #0\n  movi v17.2d, #0\n"
    "  movi v18.2d, #0\n  movi v19.2d, #0\n  movi v20.2d, #0\n  movi v21.2d, #0\n  movi v22.2d, #0\n  movi v23.2d, #0\n"
    "  movi v24.2d, #0\n  movi v25.2d, #0\n  movi v26.2d, #0\n  movi v27.2d, #0\n  movi v28.2d, #0\n  movi v29.2d, #0\n"
    "  movi v30.2d, #0\n  movi v31.2d, #0\n"
    "  mov x0, #0\n  mov x1, #0\n  mov x2, #0\n  mov x3, #0\n  mov x4, #0\n  mov x5, #0\n  mov x6, #0\n  mov x7, #0\n"
    "  mov x8, #0\n  mov x9, #0\n  mov x10, #0\n  mov x11, #0\n  mov x12, #0\n  mov x13, #0\n  mov x14, #0\n"
    "  mov x15, #0\n  mov x16, #0\n  mov x17, #0\n  mov x18, #0\n  mov x19, #0\n  mov x20, #0\n  mov x21, #0\n"
    "  mov x22, #0\n  mov x23, #0\n  mov x24, #0\n  mov x25, #0\n  mov x26, #0\n  mov x27, #0\n  mov x28, #0\n"
    "  mov x29, #0\n  mov x30, #0\n  isb\n  eret\n");
void lx_enter_el0(uint64_t entry, uint64_t sp) __attribute__((noreturn));

#define STACK_SIZE (16u << 20)

static int start_program(uint64_t *entry, uint64_t *sp) {
    image_t app, ld;
    char why[160];
    const char *path = L.cfg.argv[0];
    if (load_elf(path, &app, why, sizeof why)) { snprintf(L.msg, sizeof L.msg, "%s", why); return -1; }
    snprintf(lx_exe, sizeof lx_exe, "%s", path);
    int dyn = app.interp[0] != 0;
    if (dyn && load_elf(app.interp, &ld, why, sizeof why)) { snprintf(L.msg, sizeof L.msg, "%s", why); return -1; }
    void *stk = lx_pages(STACK_SIZE);
    if (!stk) { snprintf(L.msg, sizeof L.msg, "out of memory for the stack"); return -1; }
    build_stack((uint64_t)(uintptr_t)stk + STACK_SIZE, &app, dyn ? &ld : 0, sp);
    *entry = dyn ? ld.entry : app.entry;
    lx_log("%s: %s at %lx%s%s, %lu MiB free", path, dyn ? "dynamic" : "static", app.base,
           dyn ? ", interpreter " : "", dyn ? app.interp : "", (unsigned long)(lx_mem_free() >> 20));
    return 0;
}

static void job(void *arg) {
    (void)arg;
    uint64_t ttbr_old, sctlr_old, cntk_old;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr_old));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr_old));
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntk_old));
    L.active = 1;
    mmu_use(L.ttbr_user);
    /* EL0 may read the cache type and the counter, zero with DC ZVA, clean
     * caches for code it writes, wait with WFE/WFI. SPAN: no PAN on entry. */
    uint64_t sctlr = sctlr_old | (1ull << 26) | (1ull << 23) | (1ull << 18) | (1ull << 16) | (1ull << 15) | (1ull << 14);
    __asm__ volatile("msr sctlr_el1, %0\nisb" :: "r"(sctlr) : "memory");
    uint64_t mmfr1; __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    if (mmfr1 >> 20 & 15) __asm__ volatile(".inst 0xd500409f" ::: "memory");      /* msr pan, #0 */
    /* EL0 reads both counters; the event stream every 2^10 ticks wakes WFE. */
    __asm__ volatile("msr cntkctl_el1, %0\nisb" :: "r"((uint64_t)(3 | 4 | (9 << 4))));
    set_state(LX_RUNNING);
    uint64_t entry = 0, sp = 0;
    int r = rt_setjmp(&L.top);
    if (r == 0) {
        if (start_program(&entry, &sp) == 0) lx_enter_el0(entry, sp);
        L.exit_code = 127;
        L.crashed = 1;
    }
    /* Here after exit, a crash, Stop or a failed start: what it wrote goes to the card. */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    int n = lxfs_flush_all();
    if (n) lx_log("saved %d file%s to the card", n, n == 1 ? "" : "s");
    __asm__ volatile("msr cntkctl_el1, %0\nmsr sctlr_el1, %1\nisb" :: "r"(cntk_old), "r"(sctlr_old) : "memory");
    mmu_use(ttbr_old);
    L.active = 0;
    L.t1 = now_ms();
    set_state(LX_DONE);
    __asm__ volatile("dsb ish\nsev" ::: "memory");
}

void lx_exit(int code) {
    L.exit_code = code;
    if (!L.msg[0]) snprintf(L.msg, sizeof L.msg, "Exited with %d", code);
    rt_longjmp(&L.top, 1);
}

/* ---- traps from the program's core ------------------------------------------------------------- */
static volatile int trace_left;                 /* /lx?trace=N: log the next N syscalls */
typedef struct { uint64_t x[31]; uint64_t elr, spsr, fpsr, fpcr, pad; } lx_frame_t;

static const char *ec_text(uint32_t ec) {
    switch (ec) {
    case 0x00: return "an undefined instruction";
    case 0x0E: return "an illegal execution state";
    case 0x18: return "a system register it may not use";
    case 0x20: case 0x21: return "a bad instruction address";
    case 0x22: return "a misaligned pc";
    case 0x24: case 0x25: return "a bad data address";
    case 0x26: return "a misaligned stack";
    case 0x2F: return "an SError";
    case 0x3C: return "a breakpoint (BRK: abort or a trap)";
    default:   return "an exception";
    }
}

/* EL0 reading an ID register (MRS) traps; Linux answers with the value. */
static int emulate_mrs(lx_frame_t *f) {
    if (!lx_uok(f->elr, 4)) return 0;
    uint32_t in = *(const uint32_t *)(uintptr_t)f->elr;
    if ((in & 0xFFF00000u) != 0xD5300000u) return 0;
    uint32_t o0 = in >> 19 & 1, op1 = in >> 16 & 7, crn = in >> 12 & 15, crm = in >> 8 & 15, op2 = in >> 5 & 7, rt = in & 31;
    if (!o0 || op1 || crn) return 0;
    uint64_t v = 0;
    switch (crm << 3 | op2) {
    case 0x00: __asm__ volatile("mrs %0, midr_el1" : "=r"(v)); break;
    case 0x05: v = 0x80000000ull; break;
    case 0x20: __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(v)); break;
    case 0x21: __asm__ volatile("mrs %0, id_aa64pfr1_el1" : "=r"(v)); break;
    case 0x30: __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(v)); break;
    case 0x31: __asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(v)); break;
    case 0x38: __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(v)); break;
    case 0x39: __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(v)); break;
    default: v = 0;
    }
    if (rt != 31) f->x[rt] = v;
    f->elr += 4;
    return 1;
}

static void crash(lx_frame_t *f, uint64_t kind, uint64_t esr, uint64_t far) {
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;
    uint64_t sp0, off = 0;
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp0));
    const char *where = lxfs_where(f->elr, &off);
    if (kind != 4) {
        snprintf(L.msg, sizeof L.msg, "Crashed in a syscall: %s at %lx (address %lx)", ec_text(ec), f->elr, far);
    } else if (where) {
        const char *b = strrchr(where, '/');
        snprintf(L.msg, sizeof L.msg, "Crashed: %s in %s+0x%lx (address %lx)", ec_text(ec), b ? b + 1 : where, off, far);
    } else {
        snprintf(L.msg, sizeof L.msg, "Crashed: %s at pc %lx (address %lx)", ec_text(ec), f->elr, far);
    }
    lx_log("%s", L.msg);
    lx_log("  esr %lx  pc %lx  lr %lx  sp %lx", esr, f->elr, f->x[30], kind == 4 ? sp0 : (uint64_t)(uintptr_t)(f + 1));
    uint64_t lroff = 0;
    const char *lrw = lxfs_where(f->x[30], &lroff);
    if (lrw) lx_log("  lr is %s+0x%lx", lrw, lroff);
    for (int i = 0; i < 30; i += 3)
        lx_log("  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx", i, f->x[i], i + 1, f->x[i + 1], i + 2, f->x[i + 2]);
    L.crashed = 1;
    L.exit_code = 139;
    rt_longjmp(&L.top, 2);
}

int lx_trap(void *frame, uint64_t kind) {
    if (!L.active || this_cpu() != L.cpu) return 0;
    lx_frame_t *f = frame;
    uint64_t esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;
    if (kind == 4 && ec == 0x15) {
        if (lx_stopping()) { snprintf(L.msg, sizeof L.msg, "Stopped"); lx_exit(130); }
        uint64_t a0 = f->x[0];
        f->x[0] = (uint64_t)lx_syscall(f->x[8], f->x[0], f->x[1], f->x[2], f->x[3], f->x[4], f->x[5]);
        if (trace_left && f->x[8] != 101 && f->x[8] != 115 && f->x[8] != 113) {     /* not the clock */
            trace_left--;
            char s[48] = "";
            if (lx_uok(f->x[1], 1) && (f->x[8] == 56 || f->x[8] == 79 || f->x[8] == 48 || f->x[8] == 78 || f->x[8] == 35 || f->x[8] == 34))
                lx_ustr(f->x[1], s, sizeof s);
            lx_log("sys %lu(%lx, %lx, %lx, %lx) = %ld %s", f->x[8], a0, f->x[1], f->x[2], f->x[3], (long)f->x[0], s);
        }
        return 1;
    }
    if (kind == 4 && ec == 0x00 && emulate_mrs(f)) return 1;
    crash(f, kind, esr, far);
    return 1;
}

/* ---- the boot core's side ------------------------------------------------------------------------ */
static void give_back(void) {
    lxfs_free();
    if (bigmem_owner(0) && !strcmp(bigmem_owner(0), "Linux program")) bigmem_free(0);
    if (bigmem_owner(1) && !strcmp(bigmem_owner(1), "Linux program")) bigmem_free(1);
    if (bm) { kfree(bm); bm = 0; }
    if (bmk) { kfree(bmk); bmk = 0; }
    if (karena) { kfree(karena); karena = 0; }
    if (L.out) { kfree(L.out); L.out = 0; }
    lx_net_reset();
}

static void drain(void) {
    if (!L.out) return;
    uint32_t h = acq(&L.out_head), t = L.out_tail;
    while (t != h) {
        char c = L.out[t++ % OUTCAP];
        if (c == '\r') continue;
        if (c != '\n' && L.line_n < (int)sizeof L.line - 1) { L.line[L.line_n++] = c; continue; }
        if (c != '\n') continue;
        L.line[L.line_n] = 0;
        /* ANSI colour codes out: the log and the app's console are plain text. */
        char clean[512]; int k = 0, mine = L.line[0] == 1;
        for (int i = mine; L.line[i] && k < (int)sizeof clean - 1; i++) {
            if (L.line[i] == 0x1b && L.line[i + 1] == '[') { i += 2; while (L.line[i] && !((L.line[i] >= 'A' && L.line[i] <= 'Z') || (L.line[i] >= 'a' && L.line[i] <= 'z'))) i++; continue; }
            if ((unsigned char)L.line[i] >= 32 || L.line[i] == '\t') clean[k++] = L.line[i];
        }
        clean[k] = 0;
        kprintf(mine ? "[lx] %s\n" : "[lx:out] %s\n", clean);
        if (L.con_n + k + 2 > CONCAP) { int cut = CONCAP / 2; memmove(L.con, L.con + cut, (size_t)(L.con_n - cut)); L.con_n -= cut; }
        memcpy(L.con + L.con_n, clean, (size_t)k); L.con_n += k; L.con[L.con_n++] = '\n';
        L.line_n = 0;
    }
    rel(&L.out_tail, t);
}

int lx_poll(void) {
    int st = lx_state();
    if (st == LX_IDLE) return 0;
    int news = 0;
    uint32_t seq = acq(&L.rq_seq);
    if (seq != L.rq_served && (st == LX_RUNNING || st == LX_STARTING)) {
        serve(L.rq);
        rel(&L.rq_served, seq);
        __asm__ volatile("dsb ish\nsev" ::: "memory");
    }
    lx_net_poll();
    uint32_t h0 = L.out ? acq(&L.out_head) : 0;
    if (L.out && h0 != L.out_tail) { drain(); news = 1; __asm__ volatile("sev"); }
    if (st == LX_DONE && !L.cleaned) {
        drain();
        L.cleaned = 1;
        give_back();
        kprintf("[lx] %s (%lu ms)\n", lx_message(), (unsigned long)lx_elapsed_ms());
        news = 1;
    }
    return news;
}

void lx_stop(void) {
    __atomic_store_n(&L.stop, 1, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish\nsev" ::: "memory");
}

/* Something else needs the windows (a video): the program ends first. */
static void release_cb(void) {
    snprintf(L.msg, sizeof L.msg, "Stopped: something else needed its memory");
    lx_stop();
    uint64_t end = now_ms() + 3000;
    while (lx_state() != LX_DONE && now_ms() < end) { lx_poll(); delay_us(200); }
    lx_poll();
}

const char *lx_message(void) {
    int st = lx_state();
    if (st == LX_STARTING) return "Starting";
    if (st == LX_RUNNING) return "Running";
    return L.msg[0] ? L.msg : "";
}

int lx_console(char *out, int cap) {
    int n = L.con_n < cap - 1 ? L.con_n : cap - 1;
    memcpy(out, L.con + (L.con_n - n), (size_t)n);
    out[n] = 0;
    return n;
}

static const char *keep(const char *s, int *at) {
    if (!s) return 0;
    int n = (int)strlen(s) + 1;
    if (*at + n > (int)sizeof L.strs) return 0;
    char *d = L.strs + *at;
    memcpy(d, s, (size_t)n);
    *at += n;
    return d;
}

int lx_start(const lx_config_t *c) {
    int st = lx_state();
    if (st == LX_STARTING || st == LX_RUNNING) { snprintf(L.msg, sizeof L.msg, "A Linux program is already running"); return -1; }
    if (st == LX_DONE && !L.cleaned) lx_poll();
    memset(&L, 0, sizeof L);
    /* Copies of everything it was given. */
    int at = 0;
    for (int i = 0; i < LX_MAXARG - 1 && c->argv[i]; i++) L.cfg.argv[i] = keep(c->argv[i], &at);
    for (int i = 0; i < LX_MAXARG - 1 && c->envp[i]; i++) L.cfg.envp[i] = keep(c->envp[i], &at);
    for (int i = 0; i < LX_MAXPACK && c->packs[i]; i++) L.cfg.packs[i] = keep(c->packs[i], &at);
    L.cfg.rw = keep(c->rw ? c->rw : "/Linux/rw", &at);
    L.cfg.cwd = keep(c->cwd ? c->cwd : "/", &at);
    L.cfg.fb_w = c->fb_w; L.cfg.fb_h = c->fb_h;
    L.cfg.cache_mb = c->cache_mb ? c->cache_mb : 64;
    if (!L.cfg.argv[0]) { snprintf(L.msg, sizeof L.msg, "No program given"); L.cleaned = 1; return -1; }
    /* Both windows of RAM: whoever has them is asked to let go first -- the
     * high one first, as Local AI gives both back through it. Whoever says
     * no (a running machine) keeps them, and the program doesn't start. */
    for (int w = 1; w >= 0; w--) {
        if (bigmem_owner(w) && bigmem_evict(w)) {
            snprintf(L.msg, sizeof L.msg, "%s is using the memory it needs: stop it first", bigmem_owner(w));
            L.cleaned = 1; return -1;
        }
    }
    for (int w = 0; w < 2; w++)
        if (!bigmem_claim(w, "Linux program", release_cb)) {
            snprintf(L.msg, sizeof L.msg, "%s is using the memory it needs", bigmem_owner(w) ? bigmem_owner(w) : "Something");
            if (w) bigmem_free(0);
            L.cleaned = 1; return -1;
        }
    bm = kmalloc((NPAGES + 63) / 64 * 8);
    bmk = kmalloc((NPAGES + 63) / 64 * 8);
    karena = kmalloc(KARENA);
    L.out = kmalloc(OUTCAP);
    if (!bm || !bmk || !karena || !L.out) { snprintf(L.msg, sizeof L.msg, "Out of memory"); give_back(); L.cleaned = 1; return -1; }
    memset(bm, 0, (NPAGES + 63) / 64 * 8);
    memset(bmk, 0, (NPAGES + 63) / 64 * 8);
    pages_used = 0; hint = 0; karena_used = 0;
    char why[160];
    if (lxfs_init(&L.cfg, why, sizeof why)) { snprintf(L.msg, sizeof L.msg, "%s", why); give_back(); L.cleaned = 1; return -1; }
    if (L.cfg.fb_w && lxdev_init(L.cfg.fb_w, L.cfg.fb_h)) { snprintf(L.msg, sizeof L.msg, "Out of memory for the framebuffer"); give_back(); L.cleaned = 1; return -1; }
    lxsys_reset();
    lx_net_reset();
    L.ttbr_user = mmu_user_table(win_base, win_size, NWIN);
    L.t0 = now_ms();
    set_state(LX_STARTING);
    __asm__ volatile("dsb ish" ::: "memory");
    static const int order[] = { 3, 2, 1 };
    for (int i = 0; i < 3; i++) {
        int cc = order[i];
        if (!core_pooled(cc) || core_busy(cc) || core_wanted(cc)) continue;
        L.cpu = cc;
        __asm__ volatile("dsb ish" ::: "memory");
        if (core_run(cc, job, 0) == 0) {
            kprintf("[lx] %s on core %d (%lu MiB of memory)\n", L.cfg.argv[0], cc, (unsigned long)(lx_mem_total() >> 20));
            return 0;
        }
    }
    set_state(LX_IDLE);
    give_back();
    L.cleaned = 1;
    snprintf(L.msg, sizeof L.msg, "Every spare core is busy");
    return -1;
}

/* ---- for testing from the Mac --------------------------------------------------------------------- */
int lx_dev(const char *q, char *out, int cap) {
    if (*q == '?') q++;
    if (!strncmp(q, "run=", 4)) {
        static char cmd[512];
        static char packs[256];
        static lx_config_t c;
        memset(&c, 0, sizeof c);
        snprintf(cmd, sizeof cmd, "%s", q + 4);
        static char cwdq[128], envq[512];
        char *amp = strchr(cmd, '&');
        packs[0] = cwdq[0] = envq[0] = 0;
        long fbw = 640, fbh = 480;
        /* &packs=a.pak,b.pak  &cwd=/dir  &env=K=V,K=V  &fb=WxH */
        while (amp) {
            *amp = 0;
            char *kv = amp + 1;
            amp = strchr(kv, '&');
            if (amp) *amp = 0;
            if (!strncmp(kv, "packs=", 6)) snprintf(packs, sizeof packs, "%s", kv + 6);
            else if (!strncmp(kv, "cwd=", 4)) snprintf(cwdq, sizeof cwdq, "%s", kv + 4);
            else if (!strncmp(kv, "env=", 4)) snprintf(envq, sizeof envq, "%s", kv + 4);
            else if (!strncmp(kv, "fb=", 3)) { fbw = strtol(kv + 3, 0, 10); const char *x = strchr(kv, 'x'); if (x) fbh = strtol(x + 1, 0, 10); }
            if (amp) *amp = '&';
        }
        /* argv: "+"-separated, %2F for '/' */
        int argc = 0;
        for (char *p = cmd; *p && argc < LX_MAXARG - 1; ) {
            c.argv[argc++] = p;
            char *e = strchr(p, '+');
            if (!e) break;
            *e = 0; p = e + 1;
        }
        for (int i = 0; i < argc; i++) {
            char *s = (char *)c.argv[i], *d = s;
            for (; *s; s++) {
                if (*s == '%' && s[1] && s[2]) { char hx[3] = { s[1], s[2], 0 }; *d++ = (char)strtol(hx, 0, 16); s += 2; }
                else *d++ = *s;
            }
            *d = 0;
        }
        int ne = 0;
        c.envp[ne++] = "HOME=/tmp"; c.envp[ne++] = "PATH=/usr/bin:/bin"; c.envp[ne++] = "TERM=dumb";
        for (char *p = envq; *p && ne < LX_MAXARG - 1; ) {
            c.envp[ne++] = p;
            char *e = strchr(p, ',');
            if (!e) break;
            *e = 0; p = e + 1;
        }
        c.envp[ne] = 0;
        if (cwdq[0]) c.cwd = cwdq;
        int np = 0;
        if (packs[0]) {
            for (char *p = packs; *p && np < LX_MAXPACK; ) {
                c.packs[np++] = p;
                char *e = strchr(p, ',');
                if (!e) break;
                *e = 0; p = e + 1;
            }
        } else {
            /* every store file ending in .pak */
            store_open(0);
            for (int i = 0; i < STORE_MAX && np < LX_MAXPACK; i++) {
                const store_entry_t *e = store_at(i);
                if (!e || !(e->flags & STORE_DONE)) continue;
                size_t n = strlen(e->name);
                if (n > 4 && !strcmp(e->name + n - 4, ".pak")) c.packs[np++] = e->name;
            }
        }
        c.rw = "/Linux/rw";
        c.fb_w = (int)fbw; c.fb_h = (int)fbh;
        if (lx_start(&c)) return snprintf(out, (size_t)cap, "not started: %s\n", L.msg);
        return snprintf(out, (size_t)cap, "started on core %d\n", L.cpu);
    }
    if (!strcmp(q, "stop")) { lx_stop(); return snprintf(out, (size_t)cap, "stopping\n"); }
    if (!strncmp(q, "trace=", 6)) { trace_left = (int)strtol(q + 6, 0, 10); return snprintf(out, (size_t)cap, "tracing %d syscalls\n", trace_left); }
    if (!strcmp(q, "con")) return lx_console(out, cap);
    int n = snprintf(out, (size_t)cap, "state %d: %s, %lu ms, exit %d%s, core %d\n", lx_state(), lx_message(),
                     (unsigned long)lx_elapsed_ms(), L.exit_code, L.crashed ? " (crashed)" : "", L.cpu);
    if (bm) n += snprintf(out + n, (size_t)(cap - n), "memory: %lu of %lu MiB in use\n",
                          (unsigned long)(pages_used >> 8), (unsigned long)(lx_mem_total() >> 20));
    n += lxfs_debug(out + n, cap - n);
    if (!strcmp(q, "status")) {
        int k = L.con_n > 3000 ? 3000 : L.con_n;
        n += snprintf(out + n, (size_t)(cap - n), "--- console ---\n%.*s", k, L.con + L.con_n - k);
    }
    return n;
}
