#include <stdint.h>
#include "shell.h"
#include "commands.h"
#include "kprintf.h"
#include "console.h"
#include "keyboard.h"
#include "xhci.h"
#include "fs.h"
#include "pit.h"
#include "string.h"
#include "kheap.h"
#include <stdio.h>

#define LINESZ  256
#define MAXARG  24

/* ===========================================================================
 *  Shared helpers (declared in commands.h)
 * ===========================================================================*/
void sh_utoa(uint64_t v, char *out) {
    char tmp[24]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i--) out[j++] = tmp[i];
    out[j] = 0;
}

void sh_pad(const char *s, int width) {
    int n = 0;
    while (s[n]) { kputc(s[n]); n++; }
    for (; n < width; n++) kputc(' ');
}

void sh_human(uint64_t b, char *out) {
    if (b < 1024) { sh_utoa(b, out); strcat(out, "B"); return; }
    uint64_t div; const char *unit;
    if      (b < 1024ull * 1024)             { div = 1024ull;             unit = "K"; }
    else if (b < 1024ull * 1024 * 1024)      { div = 1024ull * 1024;      unit = "M"; }
    else                                     { div = 1024ull * 1024 * 1024;unit = "G"; }
    uint64_t whole = b / div;
    uint64_t tenth = ((b % div) * 10) / div;
    char w[24], t[4];
    sh_utoa(whole, w); sh_utoa(tenth, t);
    strcpy(out, w); strcat(out, "."); strcat(out, t); strcat(out, unit);
}

uint64_t sh_uptime_ms(void) {
    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    return pit_ticks() * 1000ull / hz;
}

int sh_readline(char *buf, int cap) {
    int len = 0;
    for (;;) {
        xhci_poll();                          /* pump USB HID keyboard, if any */
        int ci = kbd_trygetc();
        if (ci < 0) { __asm__ volatile("hlt"); continue; }
        char c = (char)ci;
        if (c == '\n') { kputc('\n'); buf[len] = 0; return len; }
        if (c == '\b') { if (len) { len--; kputc('\b'); } continue; }
        /* Printable only. The key ring also carries navigation codes and the
         * resolved global shortcuts (0xC0 and up), and inserting one of those
         * into a password prompt or a debugger command is never what was
         * meant -- pressing Alt+Tab at a prompt should do nothing, not type. */
        if ((unsigned char)c >= 32 && (unsigned char)c < 127 && len < cap - 1)
            { buf[len++] = c; kputc(c); }
    }
}

int sh_yesno(const char *question) {
    char b[8];
    kprintf("%s? [y/N] ", question);
    sh_readline(b, sizeof(b));
    return b[0] == 'y' || b[0] == 'Y';
}

/* ===========================================================================
 *  Core command handlers
 * ===========================================================================*/
int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) kputc(' '); kprintf("%s", argv[i]); }
    kputc('\n');
    return 0;
}
int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); return 0; }

int cmd_help(int argc, char **argv) {
    if (argc > 1) {
        return cmd_man(argc, argv);
    }
    const char *cat = 0;
    for (int i = 0; i < commands_count; i++) {
        if (!cat || strcmp(cat, commands[i].cat) != 0) {
            cat = commands[i].cat;
            kprintf("\n%s:\n", cat);
        }
        kprintf("  "); sh_pad(commands[i].name, 11); kprintf("%s\n", commands[i].help);
    }
    kprintf("\n%d commands total.  'help NAME' for one command.\n", commands_count);
    return 0;
}

/* ===========================================================================
 *  Command table (grouped by category for help output)
 * ===========================================================================*/
const command_t commands[] = {
    /* File & Directory */
    { "ls",       "File & Directory", "list directory contents",            cmd_ls },
    { "tree",     "File & Directory", "show directory tree",                cmd_tree },
    { "cd",       "File & Directory", "change directory",                   cmd_cd },
    { "pwd",      "File & Directory", "print working directory",            cmd_pwd },
    { "mkdir",    "File & Directory", "create directory",                   cmd_mkdir },
    { "rm",       "File & Directory", "remove file/dir ([-r])",             cmd_rm },
    { "cp",       "File & Directory", "copy file",                          cmd_cp },
    { "mv",       "File & Directory", "move or rename",                     cmd_mv },
    { "find",     "File & Directory", "search for files",                   cmd_find },
    { "touch",    "File & Directory", "create empty file / update mtime",   cmd_touch },
    { "write",    "File & Directory", "write text into a file",             cmd_write },
    { "edit",     "File & Directory", "full-screen text editor: edit FILE", cmd_edit },
    { "trash",    "File & Directory", "move file to trash",                 cmd_trash },
    { "recover",  "File & Directory", "restore trashed file",               cmd_recover },

    /* File Inspection */
    { "cat",      "File Inspection",  "print file contents",                cmd_cat },
    { "head",     "File Inspection",  "first lines of file ([-N])",         cmd_head },
    { "tail",     "File Inspection",  "last lines of file ([-N])",          cmd_tail },
    { "hex",      "File Inspection",  "hexadecimal file viewer",            cmd_hex },
    { "meta",     "File Inspection",  "show file metadata",                 cmd_meta },
    { "diff",     "File Inspection",  "compare two files",                  cmd_diff },
    { "grep",     "File Inspection",  "search text in file",                cmd_grep },
    { "checksum", "File Inspection",  "fnv-1a hash of file",                cmd_checksum },
    { "preview",  "File Inspection",  "quick file preview",                 cmd_preview },
    { "count",    "File Inspection",  "count lines/words/chars",            cmd_count },

    /* System Information */
    { "sysinfo",  "System",           "system overview",                    cmd_sysinfo },
    { "cpuinfo",  "System",           "CPU details (CPUID)",                cmd_cpuinfo },
    { "meminfo",  "System",           "memory usage",                       cmd_meminfo },
    { "mem",      "System",           "memory usage (alias)",               cmd_mem },
    { "diskinfo", "System",           "disk/storage status",                cmd_diskinfo },
    { "sync",     "System",           "commit the filesystem to disk",      cmd_sync },
    { "fsck",     "System",           "check the filesystem ([-r] repair)",  cmd_fsck },
    { "poweroff", "System",           "sync, then power off (ACPI S5)",     cmd_poweroff },
    { "reboot",   "System",           "sync, then restart",                 cmd_reboot },
    { "install",  "System",           "install BoltOS onto a disk",         cmd_install },
    { "dmesg",    "System",           "kernel message log ([-N], save)",    cmd_dmesg },
    { "crashlog", "System",           "persisted crash reports ([clear])",  cmd_crashlog },
    { "core",     "System",           "post-mortem: core list|show FILE",   cmd_core },
    { "uptime",   "System",           "time since boot",                    cmd_uptime },
    { "winrun",   "System",           "run a Windows .exe (PE32+ loader)",   cmd_winrun },
    { "compile",  "System",           "compile .c/.cpp/.cs/.py to a real .exe",cmd_compile },
    { "battery",  "System",           "battery status",                     cmd_battery },
    { "sensors",  "System",           "hardware sensors",                   cmd_sensors },
    { "devices",  "System",           "connected hardware (PCI)",           cmd_devices },
    { "version",  "System",           "OS version information",             cmd_version },
    { "health",   "System",           "system health report",              cmd_health },

    /* Process Management */
    { "ps",       "Process",          "list kernel tasks",                  cmd_ps },
    { "kill",     "Process",          "terminate a task",                   cmd_kill },
    { "top",      "Process",          "live task monitor",                  cmd_top },
    { "freeze",   "Process",          "suspend a task",                     cmd_freeze },
    { "resume",   "Process",          "resume a task",                      cmd_resume },
    { "services", "Process",          "list background services",           cmd_services },
    { "service",  "Process",          "manage a service",                   cmd_service },
    { "jobs",     "Process",          "show shell jobs",                    cmd_jobs },
    { "priority", "Process",          "change task priority",               cmd_priority },
    { "monitor",  "Process",          "track resource usage",               cmd_monitor },

    /* Networking */
    { "netinfo",  "Network",          "network configuration",              cmd_netinfo },
    { "ifconfig", "Network",          "show/set the interface, saved to /etc",cmd_ifconfig },
    { "hosts",    "Network",          "local name table: hosts add NAME IP", cmd_hosts },
    { "dns",      "Network",          "resolver cache: dns [NAME|flush]",    cmd_dns },
    { "ntp",      "Network",          "clock sync: ntp [sync|on|off|server]",cmd_ntp },
    { "serve",    "Network",          "HTTP file server: serve [PORT] [DIR]",cmd_serve },
    { "netget",   "Network",          "fetch a URL over a ring-3 socket",    cmd_netget },
    { "sshd",     "Network",          "SSH server: sshd [PORT] (sudo for 22)",cmd_sshd },
    { "sshkey",   "Network",          "show the SSH host key fingerprint",   cmd_sshkey },
    { "ping",     "Network",          "connectivity test",                  cmd_ping },
    { "trace",    "Network",          "route tracing",                      cmd_trace },
    { "ports",    "Network",          "open port viewer",                   cmd_ports },
    { "download", "Network",          "download a file over HTTP",          cmd_download },
    { "browse",   "Network",          "fetch + render a page as text",      cmd_browse },
    { "domq",     "Network",          "DOM query: domq URL|FILE SELECTOR",  cmd_domq },
    { "domlayout","Network",          "box-layout tree: domlayout URL|FILE",cmd_domlayout },
    { "upload",   "Network",          "upload a file",                      cmd_upload },
    { "wifi",     "Network",          "Wi-Fi management",                   cmd_wifi },
    { "firewall", "Network",          "firewall controls",                  cmd_firewall },
    { "share",    "Network",          "share files over network",           cmd_share },
    { "scan",     "Network",          "discover nearby devices",            cmd_scan },

    /* Bonus / Unique */
    { "focus",    "Bonus",            "block distractions",                 cmd_focus },
    { "snapshot", "Bonus",            "save/restore fs snapshot",           cmd_snapshot },
    { "timeline", "Bonus",            "show activity history",              cmd_timeline },
    { "vault",    "Bonus",            "encrypted file storage (toy)",       cmd_vault },
    { "doctor",   "Bonus",            "diagnose and fix issues",            cmd_doctor },
    { "assistant","Bonus",            "built-in terminal assistant",        cmd_assistant },
    { "sandbox",  "Bonus",            "run a command isolated",             cmd_sandbox },
    { "workspace","Bonus",            "save/restore sessions",              cmd_workspace },
    { "panic",    "Bonus",            "halt and secure the system",         cmd_panic },
    { "story",    "Bonus",            "narrative log of events",            cmd_story },

    /* Hardware / Platform */
    { "fat",      "Hardware",         "FAT32 volume: fat ls|cat|write|mkdir", cmd_fat },
    { "power",    "Hardware",         "power off / reboot (ACPI)",          cmd_power },
    { "cpus",     "Hardware",         "list CPUs / SMP status",             cmd_cpus },
    { "acpiinfo", "Hardware",         "ACPI tables + APIC/HPET",            cmd_acpiinfo },
    { "play",     "Hardware",         "play a tone: play [Hz] [ms]",        cmd_play },
    { "dhcp",     "Hardware",         "request a DHCP lease",               cmd_dhcp },
    { "clip",     "Hardware",         "clipboard: clip show|set|clear",     cmd_clip },
    { "crash",    "Hardware",         "trigger panic: crash null|div|ud",   cmd_crash },
    { "whoami",   "System",           "print the current user",             cmd_whoami },
    { "id",       "System",           "show uid, gid, home and elevation",  cmd_id },
    { "groups",   "System",           "show group membership",              cmd_groups },
    { "users",    "System",           "accounts: list|add|del|passwd|su",   cmd_users },
    { "useradd",  "System",           "create an account (admin)",          cmd_useradd },
    { "userdel",  "System",           "remove an account (admin)",          cmd_userdel },
    { "passwd",   "System",           "change a password",                  cmd_passwd },
    { "sudo",     "System",           "run one command as root",            cmd_sudo },
    { "chmod",    "File & Directory", "change permissions: chmod 644 FILE", cmd_chmod },
    { "chown",    "File & Directory", "change owner: chown USER FILE",      cmd_chown },
    { "logout",   "System",           "return to the sign-in screen",       cmd_logout },
    { "lock",     "System",           "lock the session (Ctrl+Alt+L)",      cmd_lock },
    { "pkg",      "System",           "packages: list|install|remove|run",  cmd_pkg },
    { "ext2",     "Hardware",         "read an ext2 volume: ext2 ls|cat",   cmd_ext2 },

    /* Core */
    { "run",      "Core",             "run an ELF64 program: run PATH [args]", cmd_run },
    { "notify",   "Core",             "post or read notifications",         cmd_notify },
    { "screenshot","Core",            "capture the screen to a PNG",        cmd_screenshot },
    { "desk",     "Core",             "virtual desktops: desk [N|move N]",  cmd_desk },
    { "keys",     "Core",             "list or change global shortcuts",    cmd_keys },
    { "logs",     "System",           "the kernel log kept across reboots",  cmd_logs },
    { "man",      "Core",             "manual: man COMMAND | -k WORD | -c", cmd_man },
    { "sh",       "Shell",            "run a shell script: sh FILE [args]", cmd_sh },
    { "source",   "Shell",            "run a script in the current session", cmd_sh },
    { ".",        "Shell",            "source FILE (alias)",                cmd_sh },
    { "test",     "Shell",            "test a file, string or integer",     cmd_test },
    { "[",        "Shell",            "test EXPR with a closing ]",         cmd_test },
    { "true",     "Shell",            "return success (0)",                 cmd_true },
    { "false",    "Shell",            "return failure (1)",                 cmd_false },
    { "set",      "Shell",            "list variables, or set NAME VALUE", cmd_set },
    { "unset",    "Shell",            "remove a variable: unset NAME",      cmd_unset },
    { "help",     "Core",             "show this list",                     cmd_help },
    { "echo",     "Core",             "print text back",                    cmd_echo },
    { "clear",    "Core",             "clear the screen",                   cmd_clear },
    { "python",   "Core",             "BoltPython REPL / run a .py file",    cmd_python },
    { "cc",       "Core",             "compile + run C/C++/C#: cc FILE...",  cmd_cc },
    { "build",    "Core",             "build a project: build [-t TGT] [DIR]", cmd_build },
    { "vcs",      "Core",             "version control: init|add|commit|log|diff", cmd_vcs },
    { "js",       "Core",             "run JavaScript in ring 3 (BoltJS)",  cmd_js },
    { "webx",     "Networking",       "ring-3 web browser (DOM+layout+JS)", cmd_webx },
    { "libctest", "Core",             "self-test the freestanding C library", cmd_libctest },
    { "nstest",   "Core",             "self-test the ported NetSurf libraries", cmd_nstest },
};
const int commands_count = (int)(sizeof(commands) / sizeof(commands[0]));

const command_t *cmd_lookup(const char *name) {
    for (int i = 0; i < commands_count; i++)
        if (strcmp(commands[i].name, name) == 0) return &commands[i];
    return 0;
}

/* ===========================================================================
 *  Line editor / REPL
 * ===========================================================================*/
/* Expand into separate storage: expansion may be longer than its source, and
 * a loop must be able to evaluate the same source again on its next iteration.
 * Operator metadata keeps a quoted ">" or "|" an ordinary argument. */
int sh_words(const char *text, char *buf, int cap, char **argv, int maxarg,
             unsigned char *operators) {
    int ac = 0, o = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        if (ac == maxarg || o >= cap - 1) goto overflow;
        argv[ac] = buf + o;
        if (operators) operators[ac] = 0;
        if (operators && (*p == '>' || *p == '|')) {
            char c = *p++;
            operators[ac] = (unsigned char)c;
            buf[o++] = c;
            if (*p == c) {
                if (c == '|') { kprintf("sh: || is not supported\n"); return -1; }
                if (o >= cap - 1) goto overflow;
                buf[o++] = *p++;
            }
        } else {
            int quote = 0;
            while (*p) {
                char c = *p;
                if (!quote && (c == ' ' || c == '\t' ||
                    (operators && (c == '>' || c == '|')))) break;
                if (!quote && (c == '"' || c == '\'')) { quote = c; p++; continue; }
                if (quote && c == quote) { quote = 0; p++; continue; }
                if (c == '\\' && p[1] && quote != '\'' &&
                    (quote != '"' || p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                    p++; c = *p;
                } else if (c == '$' && quote != '\'' && p[1]) {
                    sh_expand_at(&p, buf, cap, &o);
                    if (o >= cap - 1) goto overflow;
                    continue;
                }
                if (o >= cap - 1) goto overflow;
                buf[o++] = c; p++;
            }
            if (quote) { kprintf("sh: unterminated quote\n"); return -1; }
        }
        if (o >= cap) goto overflow;
        buf[o++] = 0;
        ac++;
    }
    return ac;
overflow:
    kprintf("sh: too many arguments or expanded command too long\n");
    return -1;
}

/* Each invocation owns its capture buffer. `sh inner.sh > out` can execute
 * another redirection without overwriting the outer command's captured text. */
typedef struct { char buf[32768]; uint32_t len; int overflow; } capture_t;
static capture_t *capture;
static void redir_sink(char c) {
    if (!capture) return;
    if (capture->len < sizeof capture->buf) capture->buf[capture->len++] = c;
    else capture->overflow = 1;
}

static fs_node *output_file(const char *path) {
    fs_node *f = fs_lookup(path);
    if (!f) {
        char leaf[FS_NAME_MAX];
        fs_node *d = fs_parent_of(path, leaf, sizeof leaf);
        if (!fs_may_create_in(d)) return 0;
        f = fs_create(path, 0);
    }
    return f && !f->is_dir && fs_may(f, FS_PERM_WRITE) ? f : 0;
}

static int exec_argv(char **argv, unsigned char *ops, int argc,
                     const char *inject, const char *pipe_out) {
    char *av[MAXARG + 1]; int ac = 0, append = 0;
    const char *out = pipe_out;
    for (int i = 0; i < argc; i++) {
        if (ops[i] == '>') {
            if (out || i + 1 >= argc || ops[i + 1]) {
                kprintf("sh: invalid redirection\n"); return 2;
            }
            append = argv[i][1] == '>';
            out = argv[++i];
        } else av[ac++] = argv[i];
    }
    if (inject) av[ac++] = (char *)inject;
    if (!ac) { kprintf("sh: missing command\n"); return 2; }
    const command_t *c = cmd_lookup(av[0]);
    if (!c) { kprintf("unknown command: %s  (try 'help')\n", av[0]); return 127; }
    if (!out) return c->fn(ac, av);

    /* Refuse an unwritable destination before executing the command. */
    if (!output_file(out)) { kprintf("sh: cannot write %s\n", out); return 1; }
    capture_t *buf = (capture_t *)kmalloc(sizeof *buf);
    if (!buf) { kprintf("sh: out of memory\n"); return 1; }
    buf->len = 0; buf->overflow = 0;
    capture_t *saved = capture;
    void (*prev)(char) = console_get_sink();
    capture = buf; console_set_sink(redir_sink);
    int rc = c->fn(ac, av);
    console_set_sink(prev); capture = saved;
    /* The command may have removed its output file; never keep a borrowed
     * fs_node pointer across a callback. */
    fs_node *f = output_file(out);
    if (buf->overflow) { kprintf("sh: output exceeds 32768 bytes\n"); rc = 1; }
    else if (!f || (append ? fs_append(f, buf->buf, buf->len) :
                            fs_write(f, buf->buf, buf->len)) < 0) {
        kprintf("sh: cannot write %s\n", out); rc = 1;
    }
    kfree(buf);
    return rc;
}

int sh_exec_simple(char *seg) {
    char *buf = (char *)kmalloc(4096);
    if (!buf) { kprintf("sh: out of memory\n"); return 1; }
    char *av[MAXARG]; unsigned char ops[MAXARG];
    int ac = sh_words(seg, buf, 4096, av, MAXARG, ops), rc = 0;
    if (ac < 0) { kfree(buf); return 2; }
    /* Pipes retain BoltShell's file-operand convention. A private temporary
     * for each invocation also makes nested scripts safe to redirect. */
    static unsigned pipe_id;
    char path[64]; path[0] = 0;
    const char *input = 0;
    for (int i = 0; i < ac;) {
        int end = i;
        while (end < ac && ops[end] != '|') end++;
        if (end == i || (end < ac && end + 1 == ac)) {
            kprintf("sh: missing pipeline command\n"); rc = 2; break;
        }
        if (end < ac && !path[0]) {
            /* Never reuse an existing file belonging to an earlier run. */
            do { snprintf(path, sizeof path, "/tmp/.pipe-%u", ++pipe_id); }
            while (fs_lookup(path));
        }
        rc = exec_argv(av + i, ops + i, end - i, input, end < ac ? path : 0);
        if (end < ac && rc) break;
        if (end < ac) input = path;
        i = end + 1;
    }
    if (path[0]) fs_remove(path);
    kfree(buf);
    return rc;
}

static void prompt(void) {
    char path[FS_PATH_MAX];
    fs_abspath(fs_cwd(), path, sizeof(path));
    kprintf("bolt:%s> ", path);
}

void shell_exec_line(char *line) { sh_run_script(line, 0, 0); }

static void redraw(const char *line, int len, int pos, int oldlen, int oldpos) {
    if (oldpos) kprintf("\033[%dD", oldpos);
    kprintf("%s", line);
    for (int i = len; i < oldlen; i++) kputc(' ');
    int back = (oldlen > len ? oldlen : len) - pos;
    if (back) kprintf("\033[%dD", back);
}

void shell_run(void) {
    char line[LINESZ] = {0}, saved[LINESZ] = {0};
    int len = 0, pos = 0, hpos = 0;
    kprintf("\nBoltShell ready. %d commands. Type 'help'.\n", commands_count);
    prompt();
    for (;;) {
        xhci_poll();
        int ci = kbd_trygetc();
        if (ci < 0) { console_cursor_tick(); __asm__ volatile("hlt"); continue; }
        unsigned char c = (unsigned char)ci;
        int oldlen = len, oldpos = pos;
        if (c == '\n') {
            kputc('\n');
            sh_history_add(line);
            shell_exec_line(line);
            len = pos = hpos = 0; line[0] = 0;
            prompt();
            continue;
        } else if (c == KEY_UP || c == KEY_DOWN) {
            int want = hpos + (c == KEY_UP ? 1 : -1);
            if (want < 0 || want > sh_history_count()) continue;
            if (!hpos && want) strcpy(saved, line);
            hpos = want;
            strcpy(line, want ? sh_history_get(want) : saved);
            pos = len = (int)strlen(line);
        } else if (c == '\t') {
            if (pos != len) continue;
            int printed = 0;
            len = sh_complete(line, sizeof line, len, &printed); pos = len;
            if (printed) { prompt(); oldlen = oldpos = 0; }
        } else if (c == KEY_LEFT) { if (pos) pos--; }
        else if (c == KEY_RIGHT) { if (pos < len) pos++; }
        else if (c == KEY_HOME) pos = 0;
        else if (c == KEY_END) pos = len;
        else if (c == KEY_COPY) { kprintf("^C\n"); len = pos = hpos = 0; line[0] = 0; prompt(); continue; }
        else if (c == '\b' && pos) {
            memmove(line + pos - 1, line + pos, (uint32_t)(len - pos + 1)); len--; pos--;
        } else if (c == KEY_DEL && pos < len) {
            memmove(line + pos, line + pos + 1, (uint32_t)(len - pos)); len--;
        } else if (c >= 32 && c < 127 && len < LINESZ - 1) {
            memmove(line + pos + 1, line + pos, (uint32_t)(len - pos + 1));
            line[pos++] = (char)c; len++;
            if (oldpos == oldlen) { kputc((char)c); continue; }
        } else continue;
        redraw(line, len, pos, oldlen, oldpos);
    }
}
