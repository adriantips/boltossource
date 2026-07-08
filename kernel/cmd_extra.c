#include <stdint.h>
#include "commands.h"
#include "fs.h"
#include "sysreg.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"

static int contains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}
static uint64_t to_secs(uint64_t ticks) {
    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    return ticks / hz;
}

/* -------------------------------- focus ---------------------------------- */
static int focus_on;
int cmd_focus(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "off") == 0) focus_on = 0;
    else if (argc > 1 && strcmp(argv[1], "on") == 0) focus_on = 1;
    else focus_on = !focus_on;
    kprintf("focus mode %s\n", focus_on ? "ON - notifications muted" : "off");
    evlog(focus_on ? "focus mode on" : "focus mode off");
    return 0;
}

/* ------------------------------ timeline --------------------------------- */
int cmd_timeline(int argc, char **argv) {
    (void)argc; (void)argv;
    int n = evlog_count();
    if (!n) { kprintf("timeline: no events recorded\n"); return 0; }
    kprintf("system timeline (%d events):\n", n);
    for (int i = 0; i < n; i++) {
        uint64_t t; const char *m = evlog_get(i, &t);
        kprintf("  [t+%lus] %s\n", (unsigned long)to_secs(t), m);
    }
    return 0;
}

/* -------------------------------- story ---------------------------------- */
int cmd_story(int argc, char **argv) {
    (void)argc; (void)argv;
    int n = evlog_count();
    kprintf("=== The story of this BoltOS session ===\n");
    kprintf("In the beginning there was BIOS, and it loaded the boot chain.\n");
    for (int i = 0; i < n; i++) {
        uint64_t t; const char *m = evlog_get(i, &t);
        kprintf("Then, at %lu seconds, %s.\n", (unsigned long)to_secs(t), m);
    }
    kprintf("And so the kernel waits, ready for your next command.\n");
    return 0;
}

/* ------------------------------ snapshot --------------------------------- */
static void snap_copy(fs_node *srcdir, const char *dstdir) {
    for (fs_node *c = srcdir->child; c; c = c->next) {
        if (strcmp(c->name, ".snap") == 0) continue;      /* never recurse store */
        char p[FS_PATH_MAX];
        strncpy(p, dstdir, sizeof(p));
        uint32_t L = (uint32_t)strlen(p);
        if (L && p[L - 1] != '/' && L + 1 < sizeof(p)) { p[L++] = '/'; p[L] = 0; }
        kstrlcat(p, c->name, sizeof(p));
        if (c->is_dir) { fs_create(p, 1); snap_copy(c, p); }
        else {
            fs_node *f = fs_create(p, 0);
            if (!f) f = fs_lookup(p);
            if (f) fs_write(f, c->data, c->size);
        }
    }
}
int cmd_snapshot(int argc, char **argv) {
    const char *act = (argc > 1) ? argv[1] : "list";
    fs_node *store = fs_lookup("/.snap");
    if (!store) store = fs_create("/.snap", 1);

    if (strcmp(act, "list") == 0) {
        kprintf("snapshots:\n");
        if (store) for (fs_node *c = store->child; c; c = c->next) kprintf("  %s\n", c->name);
        if (!store || !store->child) kprintf("  (none)  - try: snapshot save NAME\n");
        return 0;
    }
    if (argc < 3) { kprintf("usage: snapshot save|restore NAME   |   snapshot list\n"); return 1; }

    char path[FS_PATH_MAX];
    strncpy(path, "/.snap/", sizeof(path));
    kstrlcat(path, argv[2], sizeof(path));

    if (strcmp(act, "save") == 0) {
        fs_remove(path);                          /* replace if exists */
        if (!fs_create(path, 1)) { kprintf("snapshot: cannot create %s\n", path); return 1; }
        snap_copy(fs_cwd(), path);
        kprintf("saved snapshot '%s' of current directory\n", argv[2]);
    } else if (strcmp(act, "restore") == 0) {
        fs_node *snap = fs_lookup(path);
        if (!snap) { kprintf("snapshot: '%s' not found\n", argv[2]); return 1; }
        char cwdp[FS_PATH_MAX]; fs_abspath(fs_cwd(), cwdp, sizeof(cwdp));
        snap_copy(snap, cwdp);                     /* merge back into cwd */
        kprintf("restored snapshot '%s' into current directory (merge)\n", argv[2]);
    } else {
        kprintf("usage: snapshot save|restore NAME   |   snapshot list\n");
    }
    return 0;
}

/* ------------------------------- vault ----------------------------------- */
/* Toy XOR cipher - reversible, NOT real cryptography. Transforms a file's
 * bytes in place using a repeating key derived from the password. */
int cmd_vault(int argc, char **argv) {
    if (argc < 4) {
        kprintf("usage: vault lock|unlock FILE PASSWORD\n");
        kprintf("note : toy XOR cipher for demo - not secure\n");
        return (argc < 2) ? 0 : 1;
    }
    int lock = strcmp(argv[1], "lock") == 0;
    if (!lock && strcmp(argv[1], "unlock") != 0) { kprintf("vault: use 'lock' or 'unlock'\n"); return 1; }
    fs_node *n = fs_lookup(argv[2]);
    if (!n || n->is_dir) { kprintf("vault: %s: not a file\n", argv[2]); return 1; }
    const char *pw = argv[3];
    uint32_t pl = (uint32_t)strlen(pw);
    if (!pl) { kprintf("vault: empty password\n"); return 1; }
    for (uint32_t i = 0; i < n->size; i++) n->data[i] ^= (uint8_t)pw[i % pl];
    kprintf("%s '%s' with toy XOR cipher (%lu bytes)\n",
            lock ? "locked" : "unlocked", argv[2], (unsigned long)n->size);
    return 0;
}

/* ------------------------------ assistant -------------------------------- */
int cmd_assistant(int argc, char **argv) {
    if (argc < 2) {
        kprintf("BoltAssistant (offline keyword bot, no LLM).\n");
        kprintf("ask me what you want to do, e.g.:  assistant show memory\n");
        return 0;
    }
    kprintf("you asked about:");
    for (int i = 1; i < argc; i++) kprintf(" %s", argv[i]);
    kprintf("\nrelevant commands:\n");
    int hits = 0;
    for (int ci = 0; ci < commands_count; ci++) {
        int match = 0;
        for (int i = 1; i < argc && !match; i++)
            if (contains(commands[ci].name, argv[i]) || contains(commands[ci].help, argv[i])) match = 1;
        if (match) { kprintf("  "); sh_pad(commands[ci].name, 10); kprintf(" %s\n", commands[ci].help); hits++; }
    }
    if (!hits) kprintf("  (nothing matched - try 'help' for the full list)\n");
    return 0;
}

/* ------------------------------ sandbox ---------------------------------- */
/* Runs a command with the working directory confined to /tmp/.sandbox. This
 * isolates cwd-relative file effects; it is not a security boundary (single
 * address space, shared heap). */
int cmd_sandbox(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: sandbox COMMAND [args...]\n"); return 1; }
    if (!fs_lookup("/tmp")) fs_create("/tmp", 1);
    fs_node *box = fs_lookup("/tmp/.sandbox");
    if (!box) box = fs_create("/tmp/.sandbox", 1);
    if (!box) { kprintf("sandbox: cannot create sandbox dir\n"); return 1; }

    const command_t *c = cmd_lookup(argv[1]);
    if (!c) { kprintf("sandbox: unknown command: %s\n", argv[1]); return 1; }

    fs_node *saved = fs_cwd();
    fs_set_cwd(box);
    kprintf("[sandbox /tmp/.sandbox] running '%s'\n", argv[1]);
    int rc = c->fn(argc - 1, argv + 1);
    fs_set_cwd(saved);
    return rc;
}

/* ----------------------------- workspace --------------------------------- */
#define WS_MAX 8
static char ws_name[WS_MAX][16];
static char ws_path[WS_MAX][FS_PATH_MAX];
static int  ws_n;

int cmd_workspace(int argc, char **argv) {
    const char *act = (argc > 1) ? argv[1] : "list";
    if (strcmp(act, "list") == 0) {
        kprintf("workspaces:\n");
        for (int i = 0; i < ws_n; i++) { kprintf("  "); sh_pad(ws_name[i], 12); kprintf(" %s\n", ws_path[i]); }
        if (!ws_n) kprintf("  (none)  - try: workspace save NAME\n");
        return 0;
    }
    if (argc < 3) { kprintf("usage: workspace save|restore NAME | workspace list\n"); return 1; }

    if (strcmp(act, "save") == 0) {
        int slot = -1;
        for (int i = 0; i < ws_n; i++) if (strcmp(ws_name[i], argv[2]) == 0) slot = i;
        if (slot < 0 && ws_n < WS_MAX) slot = ws_n++;
        if (slot < 0) { kprintf("workspace: table full\n"); return 1; }
        strncpy(ws_name[slot], argv[2], sizeof(ws_name[slot]));
        fs_abspath(fs_cwd(), ws_path[slot], sizeof(ws_path[slot]));
        kprintf("saved workspace '%s' -> %s\n", argv[2], ws_path[slot]);
    } else if (strcmp(act, "restore") == 0) {
        for (int i = 0; i < ws_n; i++)
            if (strcmp(ws_name[i], argv[2]) == 0) {
                if (fs_chdir(ws_path[i]) == 0) kprintf("restored workspace '%s' -> %s\n", argv[2], ws_path[i]);
                else kprintf("workspace: path %s no longer exists\n", ws_path[i]);
                return 0;
            }
        kprintf("workspace: '%s' not found\n", argv[2]);
    } else {
        kprintf("usage: workspace save|restore NAME | workspace list\n");
    }
    return 0;
}

/* ------------------------------- panic ----------------------------------- */
int cmd_panic(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("WARNING: this halts the kernel. You will need to reboot the VM.\n");
    if (!sh_yesno("really trigger a kernel panic")) { kprintf("aborted\n"); return 0; }
    panic("user requested panic via shell");
    return 0;   /* unreachable */
}
