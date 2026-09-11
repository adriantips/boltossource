#include <stdint.h>
#include "commands.h"
#include "vault.h"
#include "users.h"
#include "kprintf.h"
#include "string.h"

/* ---------------------------------------------------------------------------
 *  `vault` -- the encrypted directory, from the prompt.
 *
 *  Every subcommand that needs a password takes it either as an argument or by
 *  prompting, the same way `passwd` does: the argument form is what scripts and
 *  the regression tests use, and the prompt is what a person uses.
 * ------------------------------------------------------------------------- */

static int ask(const char *prompt, char *out, uint32_t cap) {
    kprintf("%s", prompt);
    sh_readline(out, cap);
    return out[0] ? 0 : -1;
}

static void human(uint64_t b, char *out) { sh_human(b, out); }

static int status(void) {
    vault_status s;
    vault_get_status(&s);
    if (!s.exists) {
        kprintf("vault: no vault on this account ('vault create' makes one)\n");
        return 1;
    }
    char sz[12];
    if (s.unlocked) {
        human(s.bytes, sz);
        kprintf("vault: unlocked at %s -- %d entr%s, %s\n",
                s.dir, s.files, s.files == 1 ? "y" : "ies", sz);
        kprintf("       the contents are ordinary files until it is locked\n");
    } else {
        human(s.archive_bytes, sz);
        kprintf("vault: locked -- %s sealed, would open at %s\n", sz, s.dir);
    }
    return 0;
}

int cmd_vault(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";
    char pw[64];

    if (!strcmp(sub, "status")) return status();

    if (!strcmp(sub, "create")) {
        if (argc > 2) { strncpy(pw, argv[2], sizeof pw - 1); pw[sizeof pw - 1] = 0; }
        else {
            if (ask("vault password: ", pw, sizeof pw) != 0) {
                kprintf("vault: refusing to make a vault with no password\n");
                return 1;
            }
            char again[64];
            ask("retype vault password: ", again, sizeof again);
            if (strcmp(pw, again) != 0) { kprintf("vault: passwords do not match\n"); return 1; }
        }
        if (vault_create(pw) != 0) { kprintf("vault: %s\n", vault_error()); return 1; }
        vault_status s; vault_get_status(&s);
        kprintf("vault created at %s and left unlocked\n", s.dir);
        kprintf("put files there, then 'vault lock' to seal them\n");
        return 0;
    }

    if (!strcmp(sub, "unlock")) {
        if (argc > 2) { strncpy(pw, argv[2], sizeof pw - 1); pw[sizeof pw - 1] = 0; }
        else ask("vault password: ", pw, sizeof pw);
        if (vault_unlock(pw) != 0) { kprintf("vault: %s\n", vault_error()); return 1; }
        vault_status s; vault_get_status(&s);
        kprintf("vault unlocked: %d entr%s at %s\n",
                s.files, s.files == 1 ? "y" : "ies", s.dir);
        return 0;
    }

    if (!strcmp(sub, "lock")) {
        if (vault_lock() != 0) { kprintf("vault: %s\n", vault_error()); return 1; }
        vault_status s; vault_get_status(&s);
        char sz[12]; human(s.archive_bytes, sz);
        kprintf("vault locked: %s sealed\n", sz);
        return 0;
    }

    if (!strcmp(sub, "destroy")) {
        if (argc > 2) { strncpy(pw, argv[2], sizeof pw - 1); pw[sizeof pw - 1] = 0; }
        else ask("vault password: ", pw, sizeof pw);
        if (vault_destroy(pw) != 0) { kprintf("vault: %s\n", vault_error()); return 1; }
        kprintf("vault removed; its contents are now ordinary files\n");
        return 0;
    }

    kprintf("usage: vault [status] | create [PASS] | unlock [PASS] | lock | destroy [PASS]\n");
    return 1;
}
