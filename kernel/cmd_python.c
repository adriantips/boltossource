/* ===========================================================================
 *  BoltOS  -  kernel/cmd_python.c
 *  The `python` shell command: an interactive BoltPython REPL, a file runner,
 *  and `-c` inline execution. The REPL keeps the desktop alive while it blocks
 *  on the keyboard by pumping the compositor (see gui_pump()).
 * ===========================================================================*/
#include <stdint.h>
#include "commands.h"
#include "boltpy.h"
#include "kprintf.h"
#include "console.h"
#include "keyboard.h"
#include "string.h"
#include "kheap.h"
#include "fs.h"
#include "gui.h"

/* Line reader that keeps the GUI responsive while waiting for keys. */
static int py_readline(char *buf, int cap) {
    int len = 0;
    for (;;) {
        int ci = kbd_trygetc();
        if (ci < 0) { gui_pump(); __asm__ volatile("hlt"); continue; }
        char c = (char)ci;
        if (c == '\n') { kputc('\n'); buf[len] = 0; return len; }
        if (c == '\b') { if (len) { len--; kputc('\b'); } continue; }
        if ((unsigned char)c >= 32 && len < cap - 1) { buf[len++] = c; kputc(c); }
    }
}

/* Net bracket depth of a buffer, ignoring brackets inside strings/comments. */
static int scan_depth(const char *s) {
    int depth = 0; char q = 0; int comment = 0;
    for (; *s; s++) {
        char c = *s;
        if (comment) { if (c == '\n') comment = 0; continue; }
        if (q) {
            if (c == '\\' && s[1]) s++;
            else if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '#') { comment = 1; continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    return depth;
}

/* Does this single line open an indented block (trailing ':' outside strings)? */
static int opens_block(const char *s) {
    char q = 0; int last = 0;
    for (; *s; s++) {
        char c = *s;
        if (q) { if (c == '\\' && s[1]) s++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; last = c; continue; }
        if (c == '#') break;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') last = c;
    }
    return last == ':';
}
static int ends_backslash(const char *s) {
    int n = 0; while (s[n]) n++;
    return n > 0 && s[n - 1] == '\\';
}
static int is_blank(const char *s) {
    for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '\r') return 0;
    return 1;
}

/* ----------------------------- the REPL --------------------------------- */
static void repl(void) {
    bpy_vm *vm = bpy_new();
    if (!vm) { kprintf("python: out of memory\n"); return; }

    kprintf("%s\n", bpy_version());
    kprintf("Interactive shell. Type exit() or quit() to leave.\n");

    char line[256];
    char acc[4096];

    for (;;) {
        kprintf(">>> ");
        if (py_readline(line, sizeof(line)) < 0) break;

        if (strcmp(line, "exit()") == 0 || strcmp(line, "quit()") == 0 ||
            strcmp(line, "exit") == 0   || strcmp(line, "quit") == 0) break;
        if (is_blank(line)) continue;

        int block = opens_block(line);
        int depth = scan_depth(line);
        int cont  = ends_backslash(line);

        if (depth <= 0 && !block && !cont) { bpy_exec(vm, line, 1); continue; }

        /* gather a multi-line statement */
        int n = 0;
        for (int i = 0; line[i] && n < (int)sizeof(acc) - 2; i++) acc[n++] = line[i];
        acc[n++] = '\n'; acc[n] = 0;

        for (;;) {
            kprintf("... ");
            char c2[256];
            py_readline(c2, sizeof(c2));
            depth = scan_depth(acc);
            if (block) {
                if (depth <= 0 && is_blank(c2)) break;
            } else {
                if (is_blank(c2)) { /* allow blank to terminate a bracket block too */ }
            }
            for (int i = 0; c2[i] && n < (int)sizeof(acc) - 2; i++) acc[n++] = c2[i];
            acc[n++] = '\n'; acc[n] = 0;
            depth = scan_depth(acc); cont = ends_backslash(c2);
            if (!block && depth <= 0 && !cont) break;
        }
        bpy_exec(vm, acc, 1);
    }

    bpy_free(vm);
    kprintf("\n");
}

/* --------------------------- the command -------------------------------- */
int cmd_python(int argc, char **argv) {
    if (argc == 1) { repl(); return 0; }

    /* python -c CODE...  (shell splits on spaces, so re-join the tail) */
    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) { kprintf("python: -c needs code\n"); return 1; }
        char code[2048]; int n = 0;
        for (int i = 2; i < argc && n < (int)sizeof(code) - 2; i++) {
            if (i > 2) code[n++] = ' ';
            for (int j = 0; argv[i][j] && n < (int)sizeof(code) - 2; j++) code[n++] = argv[i][j];
        }
        code[n] = 0;
        return bpy_run(code);
    }

    /* python FILE  -- run a script from the ramfs */
    fs_node *f = fs_lookup(argv[1]);
    if (!f)         { kprintf("python: can't open file '%s'\n", argv[1]); return 1; }
    if (f->is_dir)  { kprintf("python: '%s' is a directory\n", argv[1]); return 1; }

    char *src = (char *)kmalloc(f->size + 1);
    if (!src) { kprintf("python: out of memory\n"); return 1; }
    for (uint32_t i = 0; i < f->size; i++) src[i] = (char)f->data[i];
    src[f->size] = 0;

    int rc = bpy_run(src);
    kfree(src);
    return rc;
}
