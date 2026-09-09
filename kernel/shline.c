#include <stdint.h>
#include "shell.h"
#include "commands.h"
#include "fs.h"
#include "users.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include <stdio.h>

/* ===========================================================================
 *  BoltOS  -  kernel/shline.c
 *  Command history and completion.
 *
 *  Retyping a command you typed a minute ago, in full, with no mistakes, is
 *  the single most repeated piece of work on a machine with a shell -- and
 *  BoltOS made you do it every time. Nothing about the OS was missing; it just
 *  was not pleasant to use, which for something meant to be lived in every day
 *  is the same problem.
 *
 *  The two front-ends -- the serial/framebuffer shell and the GUI Terminal
 *  window -- share this one implementation, because a shell that completes a
 *  filename in one window and not the other is worse than one that never does.
 *
 *  History is kept in memory while the machine runs and written to
 *  ~/.bolt_history when the session ends, so the next boot starts where the
 *  last one left off. It is not written on every command: the filesystem
 *  commits on every mutation, and paying a disk commit for the privilege of
 *  remembering `ls` is not a trade worth making.
 * ===========================================================================*/

#define HIST_MAX  48
#define HIST_LINE 256
#define MATCH_MAX 96

static char hist[HIST_MAX][HIST_LINE];
static int  hist_n;                    /* how many are stored (<= HIST_MAX)   */
static int  hist_loaded;

void sh_history_reset(void) {
    memset(hist, 0, sizeof hist);
    hist_n = hist_loaded = 0;
}

static void hist_file(char *out, uint32_t cap) {
    const char *home = users_home();
    if (!home || !home[0] || (home[0] == '/' && !home[1])) home = "/tmp";
    snprintf(out, cap, "%s/.bolt_history", home);
}

void sh_history_load(void) {
    if (hist_loaded) return;
    hist_loaded = 1;
    char path[160];
    hist_file(path, sizeof path);
    fs_node *f = fs_lookup(path);
    if (!f || f->is_dir || !f->data || !fs_may(f, FS_PERM_READ)) return;
    uint32_t i = 0;
    while (i < f->size && hist_n < HIST_MAX) {
        uint32_t j = i, k = 0;
        while (j < f->size && f->data[j] != '\n') j++;
        while (i < j && k < HIST_LINE - 1) hist[hist_n][k++] = (char)f->data[i++];
        hist[hist_n][k] = 0;
        if (k) hist_n++;
        i = j + 1;
    }
}

void sh_history_save(void) {
    if (!hist_n) return;
    char path[160];
    hist_file(path, sizeof path);
    uint32_t cap = (uint32_t)hist_n * HIST_LINE + 1;
    char *b = (char *)kmalloc(cap);
    if (!b) return;
    uint32_t o = 0;
    for (int i = 0; i < hist_n && o < cap - 2; i++) {
        for (const char *s = hist[i]; *s && o < cap - 2; s++) b[o++] = *s;
        b[o++] = '\n';
    }
    fs_node *f = fs_lookup(path);
    if (!f) f = fs_create(path, 0);
    if (f && !f->is_dir && fs_may(f, FS_PERM_WRITE)) {
        fs_chown_node(f, -1, -1, 0600);
        fs_write(f, b, o);
    }
    kfree(b);
}

void sh_history_add(const char *line) {
    sh_history_load();
    if (!line || !line[0]) return;
    /* A leading space opts out. Known credential-taking commands are excluded
     * even when they occur inside a compound line. Interactive password entry
     * uses sh_readline and never reaches history. */
    if (line[0] == ' ' || line[0] == '\t' || strstr(line, "users ") ||
        strstr(line, "useradd ") || strstr(line, "passwd ") ||
        strstr(line, "install ") || strstr(line, "vault ")) return;
    /* The same command twice in a row is one entry: repeating a command is
     * normal and a history full of it is useless. */
    if (hist_n && !strcmp(hist[hist_n - 1], line)) return;
    if (hist_n == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) memcpy(hist[i - 1], hist[i], HIST_LINE);
        hist_n--;
    }
    uint32_t i = 0;
    for (; line[i] && i < HIST_LINE - 1; i++) hist[hist_n][i] = line[i];
    hist[hist_n][i] = 0;
    hist_n++;
}

int sh_history_count(void) { sh_history_load(); return hist_n; }

const char *sh_history_get(int back) {
    sh_history_load();
    if (back < 1 || back > hist_n) return 0;
    return hist[hist_n - back];
}

/* ------------------------------------------------------------ completion --- */
/* The token the caret sits in. Quoting is not honoured here: a completion that
 * gets it wrong costs a keystroke, and the alternative is a second, subtly
 * different tokenizer living next to the real one. */
static int tok_start(const char *line, int len) {
    int i = len;
    while (i > 0 && line[i - 1] != ' ' && line[i - 1] != '\t') i--;
    return i;
}
static int is_first_token(const char *line, int start) {
    for (int i = 0; i < start; i++)
        if (line[i] != ' ' && line[i] != '\t') return 0;
    return 1;
}

/* How many leading characters `a` and `b` share. */
static int common(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return i;
}

int sh_complete(char *line, int cap, int len, int *printed) {
    if (printed) *printed = 0;
    if (len >= cap - 1) return len;

    int start = tok_start(line, len);
    char tok[192];
    int tlen = len - start;
    if (tlen > (int)sizeof(tok) - 1) return len;
    memcpy(tok, line + start, (uint32_t)tlen);
    tok[tlen] = 0;

    const char *match[MATCH_MAX];
    int n = 0;
    int isdir[MATCH_MAX];
    int cmds = is_first_token(line, start) && !strchr(tok, '/');

    /* What is being completed: a command name, or a path. */
    char dirpath[192];
    const char *prefix = tok;
    if (cmds) {
        for (int i = 0; i < commands_count && n < MATCH_MAX; i++)
            if (!strncmp(commands[i].name, tok, (uint32_t)tlen)) {
                isdir[n] = 0;
                match[n++] = commands[i].name;
            }
    } else {
        const char *slash = 0;
        for (const char *p = tok; *p; p++) if (*p == '/') slash = p;
        fs_node *d;
        if (slash) {
            int dl = (int)(slash - tok);
            if (dl == 0) { dirpath[0] = '/'; dirpath[1] = 0; }
            else { memcpy(dirpath, tok, (uint32_t)dl); dirpath[dl] = 0; }
            prefix = slash + 1;
            d = fs_lookup(dirpath);
        } else {
            d = fs_cwd();
        }
        if (!d || !d->is_dir || !fs_may(d, FS_PERM_READ | FS_PERM_EXEC)) return len;
        uint32_t pl = (uint32_t)strlen(prefix);
        for (fs_node *c = d->child; c && n < MATCH_MAX; c = c->next)
            if (!strncmp(c->name, prefix, pl)) {
                isdir[n] = c->is_dir;
                match[n++] = c->name;
            }
    }
    if (!n) return len;

    /* Extend by as much as every candidate agrees on. */
    int share = (int)strlen(match[0]);
    for (int i = 1; i < n; i++) {
        int c = common(match[0], match[i]);
        if (c < share) share = c;
    }
    int plen = (int)strlen(prefix);
    int added = 0;
    for (int i = plen; i < share && len < cap - 2; i++) {
        line[len++] = match[0][i];
        added++;
    }
    if (n == 1 && len < cap - 2) {
        /* One answer: finish it. A directory gets a slash so the next Tab
         * descends into it; anything else gets a space. */
        line[len++] = isdir[0] ? '/' : ' ';
        added++;
    }
    line[len] = 0;

    /* Nothing to add and several ways to go: show them, which is the only
     * useful thing left to do. */
    if (!added && n > 1) {
        kputc('\n');
        for (int i = 0; i < n; i++) {
            kprintf("%s%s  ", match[i], isdir[i] ? "/" : "");
            if ((i % 6) == 5) kputc('\n');
        }
        if ((n % 6) != 0) kputc('\n');
        if (printed) *printed = 1;
    }
    return len;
}
