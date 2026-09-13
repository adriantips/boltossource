/* Drive BoltMake on the host, over a filesystem small enough to lie to.
 *
 * The thing worth testing about a build tool is the decision, not the build:
 * whether it runs a recipe it should have run and skips one it should have
 * skipped. In QEMU that is slow, and the interesting case -- an mtime that
 * goes backwards across a reboot, which is why this make compares stamps
 * rather than ages -- cannot be staged at all without rebooting.
 *
 * So kernel/make.c ships as it is and everything under it is replaced: a
 * filesystem of at most 64 files in an array, a clock that is a counter I can
 * set, and a shell that understands two commands. A script drives it:
 *
 *   file NAME TEXT      put a file there, with a fresh mtime
 *   tick N              set the clock (to stage a reboot, wind it back)
 *   make [TARGET...]    run, and print every recipe line that executed
 *   cat NAME            print a file
 *
 * A recipe line can say `gen OUT FROM...` -- concatenate the inputs into OUT
 * -- or `fail`, which returns nonzero. That is enough to tell a build that
 * happened from one that did not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "fs.h"

size_t strlcpy(char *d, const char *s, size_t cap);
size_t strlcat(char *d, const char *s, size_t cap);

size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return n;
}
size_t strlcat(char *d, const char *s, size_t cap) {
    size_t dl = strnlen(d, cap);
    if (dl == cap) return cap + strlen(s);
    return dl + strlcpy(d + dl, s, cap - dl);
}

void *kmalloc(unsigned long long n) { return malloc(n ? (size_t)n : 1); }
void  kfree(void *p) { free(p); }

/* ---- a filesystem of 64 files ------------------------------------------- */

#define NFILE 64
static struct { char path[128]; fs_node node; int used; } files[NFILE];
static uint64_t clock_ticks = 100;

fs_node *fs_lookup(const char *path) {
    for (int i = 0; i < NFILE; i++)
        if (files[i].used && !strcmp(files[i].path, path)) return &files[i].node;
    return 0;
}

fs_node *fs_create(const char *path, int is_dir) {
    fs_node *e = fs_lookup(path);
    if (e) return e;
    for (int i = 0; i < NFILE; i++) {
        if (files[i].used) continue;
        files[i].used = 1;
        strlcpy(files[i].path, path, sizeof files[i].path);
        memset(&files[i].node, 0, sizeof files[i].node);
        files[i].node.is_dir = is_dir;
        files[i].node.mode = FS_MODE_FILE;
        return &files[i].node;
    }
    return 0;
}

int fs_write(fs_node *n, const void *data, uint32_t len) {
    free(n->data);
    n->data = malloc(len ? len : 1);
    if (!n->data) return -1;
    memcpy(n->data, data, len);
    n->size = len;
    n->mtime = ++clock_ticks;
    return 0;
}

int fs_may(fs_node *n, int want) { (void)n; (void)want; return 1; }

static const char *path_of(const fs_node *n) {
    for (int i = 0; i < NFILE; i++)
        if (files[i].used && &files[i].node == n) return files[i].path;
    return "?";
}

/* make.c prints through kprintf; here that is printf with the same format
 * letters, which is all of %s %d %u and a newline. */
void kprintf(const char *fmt, ...);
void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ---- a shell that knows two commands ------------------------------------ */

static int quiet_shell;

int sh_run_script(const char *text, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!quiet_shell) printf("RUN %s\n", text);

    char buf[1024];
    strlcpy(buf, text, sizeof buf);
    char *save = 0;
    char *tok = strtok_r(buf, " \t", &save);
    if (!tok) return 0;

    if (!strcmp(tok, "fail")) return 2;

    if (!strcmp(tok, "gen")) {
        char *out = strtok_r(0, " \t", &save);
        if (!out) return 1;
        char body[4096];
        body[0] = 0;
        for (char *in = strtok_r(0, " \t", &save); in; in = strtok_r(0, " \t", &save)) {
            fs_node *f = fs_lookup(in);
            if (!f) { printf("  (gen: %s missing)\n", in); return 1; }
            size_t have = strlen(body);
            size_t take = f->size;
            if (have + take + 1 > sizeof body) take = sizeof body - have - 1;
            memcpy(body + have, f->data, take);
            body[have + take] = 0;
        }
        fs_node *o = fs_lookup(out);
        if (!o) o = fs_create(out, 0);
        if (!o) return 1;
        return fs_write(o, body, (uint32_t)strlen(body));
    }
    return 0;
}

#include "../../kernel/make.c"

/* ---- the script --------------------------------------------------------- */

int main(void) {
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;

        char *save = 0;
        char *cmd = strtok_r(line, " ", &save);
        if (!cmd) continue;

        if (!strcmp(cmd, "file")) {
            char *name = strtok_r(0, " ", &save);
            const char *body = save ? save : "";
            fs_node *f = fs_lookup(name);
            if (!f) f = fs_create(name, 0);
            /* A makefile written from a script has \\t for its recipe indent,
             * because a literal tab does not survive being typed into one. */
            char real[4096];
            int o = 0;
            for (const char *p = body; *p && o < (int)sizeof real - 1; p++) {
                if (p[0] == '\\' && p[1] == 'n') { real[o++] = '\n'; p++; }
                else if (p[0] == '\\' && p[1] == 't') { real[o++] = '\t'; p++; }
                else real[o++] = *p;
            }
            real[o] = 0;
            fs_write(f, real, (uint32_t)o);
            printf("FILE %s %u\n", name, (unsigned)f->size);
            continue;
        }

        if (!strcmp(cmd, "tick")) {
            clock_ticks = strtoull(strtok_r(0, " ", &save), 0, 10);
            printf("TICK %llu\n", (unsigned long long)clock_ticks);
            continue;
        }

        if (!strcmp(cmd, "cat")) {
            char *name = strtok_r(0, " ", &save);
            fs_node *f = fs_lookup(name);
            if (!f) { printf("CAT %s <missing>\n", name); continue; }
            printf("CAT %s %.*s\n", name, (int)f->size, (const char *)f->data);
            continue;
        }

        if (!strcmp(cmd, "make")) {
            const char *goals[8];
            int ng = 0;
            for (char *g = strtok_r(0, " ", &save); g && ng < 8; g = strtok_r(0, " ", &save))
                goals[ng++] = g;
            char err[160] = "";
            int rc = make_run("Makefile", goals, ng, 0, 0, err, sizeof err);
            if (err[0]) printf("ERR %s\n", err);
            printf("DONE %d\n", rc);
            fflush(stdout);
            continue;
        }

        printf("?? %s\n", cmd);
    }
    (void)path_of;
    (void)quiet_shell;
    return 0;
}
