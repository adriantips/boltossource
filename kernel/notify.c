/* ===========================================================================
 *  BoltOS  -  kernel/notify.c
 *  The notification store behind the desktop's toasts and notification centre.
 *
 *  A ring of NOTE_MAX entries in .bss. No allocation, so a notification cannot
 *  fail to be posted for want of memory -- which matters, because the things
 *  most worth telling somebody about (a failed commit, a disk gone read-only,
 *  the heap at 90%) are exactly the moments when allocation is a bad bet.
 *
 *  Everything is mirrored into the kernel log. A notification that only exists
 *  on a screen nobody is looking at is no use to a headless run, a serial
 *  console or `dmesg` after the fact.
 * ===========================================================================*/
#include "notify.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "hw.h"

static notification_t ring[NOTE_MAX];
static int      head;                  /* next slot to write                  */
static int      count;                 /* live entries, <= NOTE_MAX           */
static uint32_t next_id = 1;
static void   (*hook)(const notification_t *);

static const char *LEVEL_NAME[4] = { "info", "success", "warn", "error" };

const char *notify_level_name(int level) {
    return (level >= 0 && level < 4) ? LEVEL_NAME[level] : "info";
}

int notify_level_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < 4; i++) if (strcmp(name, LEVEL_NAME[i]) == 0) return i;
    /* the two spellings people actually type */
    if (strcmp(name, "err") == 0)     return NOTE_ERROR;
    if (strcmp(name, "warning") == 0) return NOTE_WARN;
    return -1;
}

/* Interrupts are masked around ring updates: notify_post() is reachable from a
 * timer tick (the watchdog) and from ordinary task context, and a torn head or
 * count would outlive the moment. */
static uint64_t lock(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static void unlock(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

/* i = 0 is the newest. The ring is written forward, so the newest lives at
 * head-1 and walking back from there gives newest-first. */
static notification_t *slot(int i) {
    if (i < 0 || i >= count) return 0;
    int at = head - 1 - i;
    while (at < 0) at += NOTE_MAX;
    return &ring[at];
}

uint32_t notify_post(int level, const char *source, const char *text) {
    if (!text || !text[0]) return 0;
    if (level < 0 || level > NOTE_ERROR) level = NOTE_INFO;
    if (!source || !source[0]) source = "System";

    struct rtc_time t; rtc_now(&t);

    uint64_t f = lock();
    notification_t *n = &ring[head];
    memset(n, 0, sizeof *n);
    n->id     = next_id++;
    if (next_id == 0) next_id = 1;              /* never hand out id 0 */
    n->level  = (uint8_t)level;
    n->hour   = t.hour;
    n->min    = t.min;
    n->posted = pit_ticks();
    strlcpy(n->source, source, sizeof n->source);
    strlcpy(n->text,   text,   sizeof n->text);
    head = (head + 1) % NOTE_MAX;
    if (count < NOTE_MAX) count++;
    uint32_t id = n->id;
    notification_t copy = *n;
    unlock(f);

    kprintf("[notify:%s] %s: %s\n", LEVEL_NAME[level], copy.source, copy.text);
    if (hook) hook(&copy);
    return id;
}

uint32_t notify_info   (const char *s, const char *t) { return notify_post(NOTE_INFO,    s, t); }
uint32_t notify_warn   (const char *s, const char *t) { return notify_post(NOTE_WARN,    s, t); }
uint32_t notify_error  (const char *s, const char *t) { return notify_post(NOTE_ERROR,   s, t); }
uint32_t notify_success(const char *s, const char *t) { return notify_post(NOTE_SUCCESS, s, t); }

int notify_count(void) { return count; }

const notification_t *notify_at(int i) { return slot(i); }

const notification_t *notify_by_id(uint32_t id) {
    if (!id) return 0;
    for (int i = 0; i < count; i++) { notification_t *n = slot(i); if (n->id == id) return n; }
    return 0;
}

int notify_unread(void) {
    int n = 0;
    for (int i = 0; i < count; i++) if (!slot(i)->read) n++;
    return n;
}

void notify_mark_read(uint32_t id) {
    uint64_t f = lock();
    for (int i = 0; i < count; i++) { notification_t *n = slot(i); if (n->id == id) { n->read = 1; break; } }
    unlock(f);
}

void notify_mark_all_read(void) {
    uint64_t f = lock();
    for (int i = 0; i < count; i++) slot(i)->read = 1;
    unlock(f);
}

/* Removing from the middle of a ring means compacting it. The alternative is a
 * tombstone flag every reader has to remember to skip, and readers that forget
 * are a bug you find later.
 *
 * The scratch copy is static rather than automatic: the ring is ~10 KB and a
 * kernel stack is 16, and this is called from a click handler that is already
 * some way down one. It is only touched with interrupts off, so the single
 * shared buffer cannot be entered twice. */
static notification_t compact_scratch[NOTE_MAX];

int notify_dismiss(uint32_t id) {
    uint64_t f = lock();
    int found = -1;
    for (int i = 0; i < count; i++) if (slot(i)->id == id) { found = i; break; }
    if (found < 0) { unlock(f); return 0; }

    notification_t *keep = compact_scratch;
    int k = 0;
    for (int i = count - 1; i >= 0; i--)            /* oldest -> newest */
        if (i != found) keep[k++] = *slot(i);
    memset(ring, 0, sizeof ring);
    for (int i = 0; i < k; i++) ring[i] = keep[i];
    head  = k % NOTE_MAX;
    count = k;
    unlock(f);
    return 1;
}

void notify_clear(void) {
    uint64_t f = lock();
    memset(ring, 0, sizeof ring);
    head = 0; count = 0;
    unlock(f);
}

void notify_set_hook(void (*fn)(const notification_t *)) { hook = fn; }
