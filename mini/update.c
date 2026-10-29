/* ===========================================================================
 *  BoltOS Mini  -  mini/update.c
 *
 *  New versions of BoltOS Mini, from boltos.dev. release.sh publishes a
 *  kernel and a small description of it:
 *
 *      https://boltos.dev/update/mini.json
 *        { "version": "1.2.0", "kind": "quick", "size": ..., "sha256": "...",
 *          "url": "https://boltos.dev/update/mini-1.2.0.bin", "notes": "..." }
 *
 *  The Pi asks once it's online and its clock is set (HTTPS needs the date),
 *  and every six hours after. What happens then depends on two things:
 *
 *    - A quick update, with automatic updates on (Settings, Updates), found
 *      in the first minutes after a start: it downloads, goes onto the card
 *      and the Pi restarts into it -- before anyone has started anything.
 *    - Anything else -- a big update, automatic updates off, or one found
 *      later on -- waits in Settings, Updates for the Update button, with a
 *      note in the bar. A quick one still installs itself at the next start.
 *
 *  Installing is a download into RAM, a check of its length and SHA-256
 *  against the description's, and fs_install_kernel, which writes the new
 *  BOLTOS.BIN beside the old one, reads it back, and only then switches
 *  the directory entry over: a power cut leaves one kernel or the other.
 *
 *  An update that doesn't take -- the Pi comes back still on the old
 *  version -- isn't tried by itself again, so a bad release can't make the
 *  Pi restart over and over. The Update button still offers it.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "update.h"
#include "dl.h"
#include "web.h"
#include "fs.h"
#include "wcn.h"
#include "ui.h"
#include "crypto.h"
#include "buildid.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#if defined(UPDATE_CHANNEL)
#define CHANNEL UPDATE_CHANNEL                  /* release.sh --channel: a test one */
#elif defined(MINI_RELEASE)
#define CHANNEL "mini-release"
#else
#define CHANNEL "mini"
#endif
#define UPDATE_URL   "https://boltos.dev/update/" CHANNEL ".json"
#define MAX_KERNEL   (64u << 20)
#define EVERY_MS     (6ull * 3600 * 1000)       /* ask again after an answer    */
#define RETRY_MS     (10ull * 60 * 1000)        /* ...and after no answer       */
#define START_MS     (10ull * 60 * 1000)        /* "at the start": this soon after boot */
#define QUIET_MS     15000                      /* nobody typing, before a restart */

static update_t U;
static struct {
    int      autoupd;                           /* the setting                  */
    uint64_t next;                              /* when to ask boltos.dev       */
    dl_t    *d;                                 /* the description, or the kernel */
    char    *buf; uint32_t len, cap;
    char     url[256];
    uint8_t  sha[32];
    int      by_hand;                           /* Update was pressed            */
    uint64_t restart_at;
    char     tried[16];                         /* restarted into this, and didn't get it */
    char     told[16];                          /* the version the bar has toasted */
    int      startup;                           /* the first answer since boot not had yet */
} S;

const update_t *update_state(void) { return &U; }
const char *update_version(void) { return BOLT_VERSION; }
int update_auto(void) { return S.autoupd; }

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

static void say(int state, const char *fmt, const char *arg) {
    U.state = state;
    snprintf(U.msg, sizeof U.msg, fmt, arg ? arg : "");
    shell_request_frame();
}

static void drop(void) {
    if (S.d) { dl_free(S.d); S.d = 0; }
    if (S.buf) { kfree(S.buf); S.buf = 0; }
    S.len = S.cap = 0;
}

static int sink(void *ctx, uint64_t off, const uint8_t *p, uint32_t n) {
    (void)ctx;
    if (off != S.len || S.len + n + 1 > S.cap) return -1;
    memcpy(S.buf + S.len, p, n);
    S.len += n;
    U.got = S.len;
    return 0;
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

/* ---- the setting ---------------------------------------------------------------------------- */
void update_set_auto(int on) {
    S.autoupd = !!on;
    uint8_t v = (uint8_t)S.autoupd;
    keep_put("upd_auto", &v, 1);
    shell_request_frame();
}

/* ---- asking --------------------------------------------------------------------------------- */
void update_check(void) {
    if (S.d || U.state == UPD_DOWNLOADING || U.state == UPD_INSTALLING || U.state == UPD_RESTART) return;
    S.next = now_ms();
}

static int online(void) { return clock_known() && inet_dns_server(); }

static void ask(void) {
    drop();
    S.cap = 16384;
    S.buf = kmalloc(S.cap);
    if (!S.buf) { say(UPD_FAILED, "Out of memory", 0); S.next = now_ms() + RETRY_MS; return; }
    S.d = dl_start(UPDATE_URL, 0, sink, 0);
    if (!S.d) { drop(); say(UPD_FAILED, "boltos.dev can't be asked", 0); S.next = now_ms() + RETRY_MS; return; }
    if (!U.have) say(UPD_CHECKING, "Checking boltos.dev" TXT_ELLIPSIS, 0);
}

static void answer(void) {
    S.buf[S.len] = 0;
    jarena *ar = 0;
    jv *root = json_parse(S.buf, (int)S.len, &ar);
    const char *ver = json_s(json_get(root, "version")), *url = json_s(json_get(root, "url"));
    const char *sha = json_s(json_get(root, "sha256")), *notes = json_s(json_get(root, "notes"));
    long long size = json_i(json_get(root, "size"));
    uint8_t sum[32];
    int ok = root && ver[0] && strlen(ver) < sizeof U.version && !strncmp(url, "https://", 8) && strlen(url) < sizeof S.url &&
             unhex(sha, sum, 32) == 0 && size > 4096 && size <= MAX_KERNEL;
    int startup = S.startup;
    S.startup = 0;
    U.checked = now_ms();
    S.next = now_ms() + EVERY_MS;
    if (!ok) { json_free(ar); drop(); say(UPD_FAILED, "boltos.dev's answer didn't make sense", 0); return; }
    if (ver_cmp(ver, BOLT_VERSION) <= 0) {
        json_free(ar); drop();
        U.have = 0;
        say(UPD_CURRENT, "BoltOS Mini %s is the newest.", BOLT_VERSION);
        kprintf("[update] %s is the newest (boltos.dev has %s)\n", BOLT_VERSION, ver);
        return;
    }
    U.have = 1;
    snprintf(U.version, sizeof U.version, "%s", ver);
    snprintf(U.date, sizeof U.date, "%s", json_s(json_get(root, "date")));
    utf8_to_cp1252(notes, (int)strlen(notes), U.notes, sizeof U.notes);
    U.size = (uint32_t)size;
    U.big = !strcmp(json_s(json_get(root, "kind")), "big");
    snprintf(S.url, sizeof S.url, "%s", url);
    memcpy(S.sha, sum, 32);
    json_free(ar); drop();
    kprintf("[update] boltos.dev has %s (%s, %u KiB)\n", U.version, U.big ? "big" : "quick", (unsigned)(U.size >> 10));
    say(UPD_AVAILABLE, "BoltOS Mini %s is ready to install.", U.version);
    /* By itself: a quick one, at the start, unless it's the one that didn't take. */
    if (S.autoupd && !U.big && startup && strcmp(S.tried, U.version)) {
        kprintf("[update] installing %s by itself\n", U.version);
        update_install();
        S.by_hand = 0;
        return;
    }
    if (strcmp(S.told, U.version)) {
        snprintf(S.told, sizeof S.told, "%s", U.version);
        char t[48];
        snprintf(t, sizeof t, "BoltOS Mini %s", U.version);
        shell_toast(t, "is ready: Settings, Updates");
    }
}

/* ---- installing ------------------------------------------------------------------------------ */
int update_install(void) {
    if (!U.have || S.d || U.state == UPD_DOWNLOADING || U.state == UPD_INSTALLING || U.state == UPD_RESTART) return -1;
    if (!fs_ready()) { say(UPD_FAILED, "The SD card's files aren't there: %s", fs_why()); return -1; }
    /* The update has no Wi-Fi firmware of its own: it has to be on the card first. */
    if (!wcn_firmware_safe()) wcn_keep_firmware();
    if (!wcn_firmware_safe()) { say(UPD_FAILED, "The Wi-Fi firmware couldn't be kept on the card first", 0); return -1; }
    drop();
    S.cap = U.size + 1;
    S.buf = kmalloc(S.cap);
    if (!S.buf) { say(UPD_FAILED, "Not enough memory for the download", 0); return -1; }
    S.d = dl_start(S.url, 0, sink, 0);
    if (!S.d) { drop(); say(UPD_FAILED, "That address can't be downloaded", 0); return -1; }
    S.by_hand = 1;
    U.got = 0;
    say(UPD_DOWNLOADING, "Downloading BoltOS Mini %s" TXT_ELLIPSIS, U.version);
    kprintf("[update] downloading %s from %s\n", U.version, S.url);
    return 0;
}

static void downloaded(void) {
    dl_free(S.d); S.d = 0;
    uint8_t sum[32];
    if (S.len != U.size) { drop(); say(UPD_FAILED, "The download was the wrong size", 0); return; }
    sha256(S.buf, S.len, sum);
    if (memcmp(sum, S.sha, 32)) { drop(); say(UPD_FAILED, "The download didn't match its checksum", 0); return; }
    say(UPD_INSTALLING, "Putting it on the SD card" TXT_ELLIPSIS, 0);
}

static void install(void) {
    uint64_t t = now_ms();
    int r = fs_install_kernel(S.buf, S.len);
    drop();
    if (r) { say(UPD_FAILED, "It couldn't go on the card: %s", fs_why()); return; }
    kprintf("[update] %s is on the card (%lu ms); restarting into it\n", U.version, (unsigned long)(now_ms() - t));
    keep_put("upd_try", U.version, (int)strlen(U.version));
    say(UPD_RESTART, "Restarting into BoltOS Mini %s" TXT_ELLIPSIS, U.version);
    shell_toast("Updated", "restarting to finish");
    /* Pressed by hand: straight away. By itself: once nobody is typing. */
    S.restart_at = now_ms() + (S.by_hand ? 1500 : 4000);
}

/* ---- the main loop's part ------------------------------------------------------------------- */
void update_init(void) {
    uint8_t v;
    S.autoupd = keep_get("upd_auto", &v, 1) == 1 ? v : 1;     /* on, unless turned off */
    S.startup = 1;
    char tried[16] = "";
    int n = keep_get("upd_try", tried, sizeof tried - 1);
    if (n > 0) {
        tried[n] = 0;
        keep_put("upd_try", 0, -1);
        if (!strcmp(tried, BOLT_VERSION)) {
            kprintf("[update] now on %s\n", BOLT_VERSION);
            char t[48];
            snprintf(t, sizeof t, "BoltOS Mini %s", BOLT_VERSION);
            shell_toast(t, "is installed");
        } else {
            snprintf(S.tried, sizeof S.tried, "%s", tried);
            kprintf("[update] restarted for %s but this is %s: it isn't tried by itself again\n", tried, BOLT_VERSION);
        }
    }
    say(UPD_WAITING, "Waiting for the network" TXT_ELLIPSIS, 0);
}

int update_poll(void) {
    int frame = 0;
    if (U.state == UPD_RESTART && S.restart_at && now_ms() >= S.restart_at) {
        if (S.by_hand || input_idle_ms() >= QUIET_MS) board_reboot();
        S.restart_at = now_ms() + 1000;
    }
    if (U.state == UPD_INSTALLING && !S.d) { install(); return 1; }
    if (!S.d) {
        if (U.state == UPD_WAITING && online() && !S.next) S.next = now_ms() + 2000;   /* a moment to settle */
        if (S.next && now_ms() >= S.next && online()) ask();
        /* The first answer since boot only counts as "the start" while it is one. */
        if (S.startup && now_ms() > START_MS) S.startup = 0;
        return frame;
    }
    int downloading = U.state == UPD_DOWNLOADING;
    uint64_t was = U.got;
    int r = dl_poll(S.d, downloading ? 8 : 4);
    if (U.got != was) frame = 1;
    if (r == DL_RUNNING) return frame;
    if (r == DL_FAILED) {
        char why[128];
        snprintf(why, sizeof why, "%s", dl_error(S.d));
        drop();
        if (!downloading) S.next = now_ms() + RETRY_MS;
        say(downloading || !U.have ? UPD_FAILED : UPD_AVAILABLE, downloading ? "The download stopped: %s" : "%s", why);
        return 1;
    }
    if (downloading) downloaded(); else answer();
    return 1;
}

/* ---- for the bar, and the web page ---------------------------------------------------------- */
int update_bar(char *out, int cap) {
    switch (U.state) {
    case UPD_DOWNLOADING:
        return snprintf(out, (size_t)cap, "updating %u%%", U.size ? (unsigned)(U.got * 100 / U.size) : 0);
    case UPD_INSTALLING: case UPD_RESTART:
        return snprintf(out, (size_t)cap, "updating");
    case UPD_AVAILABLE: case UPD_FAILED:
        return U.have ? snprintf(out, (size_t)cap, "update %s", U.version) : 0;
    }
    return 0;
}

int update_dev(const char *cmd, char *out, int cap) {
    if (!strcmp(cmd, "check")) update_check();
    else if (!strcmp(cmd, "install")) update_install();
    else if (!strcmp(cmd, "auto=0") || !strcmp(cmd, "auto=1")) update_set_auto(cmd[5] == '1');
    static const char *const st[] = { "waiting", "checking", "current", "available", "downloading", "installing",
                                      "restart", "failed" };
    return snprintf(out, (size_t)cap,
                    "running %s (%s), channel %s\nstate %s: %s\nauto %d, startup %d, tried '%s'\n"
                    "offer %s%s %s, %u bytes, got %lu\nchecked %lu s ago, next in %ld s\n",
                    BOLT_VERSION, BOLT_BUILD_ID, CHANNEL, st[U.state], U.msg, S.autoupd, S.startup, S.tried,
                    U.have ? U.version : "-", U.have ? (U.big ? " big" : " quick") : "", U.date, (unsigned)U.size,
                    (unsigned long)U.got, U.checked ? (unsigned long)((now_ms() - U.checked) / 1000) : 0ul,
                    S.next ? (long)(((int64_t)S.next - (int64_t)now_ms()) / 1000) : -1l);
}
