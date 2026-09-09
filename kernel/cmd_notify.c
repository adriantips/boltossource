#include <stdint.h>
#include <stdio.h>
#include "commands.h"
#include "notify.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  BoltOS  -  kernel/cmd_notify.c
 *  `notify` -- read and post notifications from the shell.
 *
 *  The notification centre is a GUI surface, and everything a GUI surface can
 *  show should be reachable from a terminal too: a machine administered over
 *  SSH has no taskbar, and a script that finishes a long build wants to say so
 *  without knowing whether anyone is looking at a desktop.
 * ===========================================================================*/

static void usage(void) {
    kprintf("usage: notify [-l LEVEL] [-s SOURCE] TEXT   post a notification\n"
            "       notify list [-a]                     list unread (-a: all)\n"
            "       notify read [ID]                     mark one, or all, read\n"
            "       notify dismiss ID                    remove one\n"
            "       notify clear                         remove all\n"
            "  LEVEL is info (default), success, warn or error.\n");
}

static void print_one(const notification_t *e) {
    kprintf("  %c %3u  %02u:%02u  %-8s %-12s %s\n",
            e->read ? ' ' : '*', e->id, e->hour, e->min,
            notify_level_name(e->level), e->source, e->text);
}

static int list(int all) {
    int n = notify_count(), shown = 0;
    for (int i = n - 1; i >= 0; i--) {              /* oldest first, like a log */
        const notification_t *e = notify_at(i);
        if (!e || (!all && e->read)) continue;
        if (!shown++) kprintf("    ID   time   level    source       text\n");
        print_one(e);
    }
    if (!shown) kprintf("notify: %s\n", all ? "nothing stored" : "nothing unread");
    else kprintf("%d shown, %d unread of %d stored\n", shown, notify_unread(), n);
    return 0;
}

/* An id typed at the prompt. Returns 0 (never a live id) if it is not a number. */
static uint32_t parse_id(const char *s) {
    uint32_t v = 0;
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    return v;
}

int cmd_notify(int argc, char **argv) {
    if (argc < 2) return list(0);

    if (strcmp(argv[1], "list") == 0)
        return list(argc > 2 && strcmp(argv[2], "-a") == 0);

    if (strcmp(argv[1], "clear") == 0) {
        notify_clear(); kprintf("notify: cleared\n"); return 0;
    }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) { notify_mark_all_read(); kprintf("notify: all marked read\n"); return 0; }
        uint32_t id = parse_id(argv[2]);
        if (!notify_by_id(id)) { kprintf("notify: no notification %s\n", argv[2]); return 1; }
        notify_mark_read(id);
        return 0;
    }

    if (strcmp(argv[1], "dismiss") == 0) {
        if (argc < 3) { kprintf("notify: dismiss needs an ID (see 'notify list -a')\n"); return 1; }
        uint32_t id = parse_id(argv[2]);
        if (!notify_dismiss(id)) { kprintf("notify: no notification %s\n", argv[2]); return 1; }
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(); return 0; }

    /* everything else posts */
    int level = NOTE_INFO;
    const char *source = "Shell";
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            level = notify_level_from_name(argv[++i]);
            if (level < 0) { kprintf("notify: unknown level '%s'\n", argv[i]); return 1; }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            source = argv[++i];
        } else break;
    }
    if (i >= argc) { usage(); return 1; }

    /* remaining words join with single spaces, so an unquoted message works */
    char text[NOTE_TEXT_MAX];
    int n = 0;
    for (; i < argc && n < (int)sizeof(text) - 1; i++) {
        if (n) text[n++] = ' ';
        for (const char *p = argv[i]; *p && n < (int)sizeof(text) - 1; p++) text[n++] = *p;
    }
    text[n] = 0;

    uint32_t id = notify_post(level, source, text);
    if (!id) { kprintf("notify: nothing to say\n"); return 1; }
    return 0;
}
