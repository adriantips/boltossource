/* ===========================================================================
 *  BoltOS  -  kernel/cmd_user.c
 *  The identity and permission commands: whoami, users/useradd/userdel/passwd,
 *  groups, sudo, chmod, chown, id.
 *
 *  These all sit on top of kernel/users.c (the account table) and the fs_may /
 *  fs_chmod / fs_chown checks in fs/ramfs.c, so the shell cannot reach anything
 *  the VFS would refuse -- there is one permission model, not one per caller.
 * ===========================================================================*/
#include <stdint.h>
#include "commands.h"
#include "users.h"
#include "fs.h"
#include "kprintf.h"
#include "string.h"
#include "pit.h"
#include "gui.h"
#include "framebuffer.h"

/* ------------------------------------------------------------- identity ----*/
int cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *u = users_current();
    if (!u[0]) { kprintf("(not logged in)\n"); return 0; }
    kprintf("%s%s%s\n", u,
            users_is_root()  ? "  [root]"  : "",
            (!users_is_root() && users_is_admin()) ? "  [admin]" : "");
    return 0;
}

int cmd_id(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *name = users_current();
    if (!name[0]) { kprintf("id: not logged in\n"); return 1; }
    kprintf("uid=%d(%s) gid=%d(%s) home=%s%s\n",
            users_current_uid(), name,
            users_current_gid(), users_current_gid() == GID_ADMIN ? "admin" : "users",
            users_home(), users_sudo_active() ? "  [elevated]" : "");
    return 0;
}

int cmd_groups(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("%s : %s\n", users_current()[0] ? users_current() : "(nobody)",
            users_is_admin() ? "admin" : "users");
    return 0;
}

/* ------------------------------------------------------------- accounts ----*/
static int require_admin(const char *who) {
    if (users_is_admin() || users_current_uid() < 0) return 1;
    kprintf("%s: permission denied (needs admin; try 'sudo %s ...')\n", who, who);
    return 0;
}

int cmd_users(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";

    if (strcmp(sub, "list") == 0) {
        kprintf("Accounts (%d):\n", users_count());
        sh_pad("  NAME", 18); sh_pad("UID", 7); sh_pad("GROUP", 8); kprintf("HOME\n");
        for (int i = 0; i < users_count(); i++) {
            const user_t *u = users_get(i);
            char num[12];
            kprintf("  "); sh_pad(u->name, 16);
            sh_utoa((uint64_t)u->uid, num); sh_pad(num, 7);
            sh_pad(u->gid == GID_ADMIN ? "admin" : "users", 8);
            kprintf("%s%s\n", u->home,
                    strcmp(u->name, users_current()) == 0 ? "  (current)" : "");
        }
        return 0;
    }
    if (strcmp(sub, "add") == 0 && argc > 3) {
        if (!require_admin("users")) return 1;
        int uid = users_next_uid();
        if (users_add(argv[2], argv[3], uid) != 0) { kprintf("users: add failed (exists?)\n"); return 1; }
        kprintf("added '%s' (uid %d, home %s)\n", argv[2], uid,
                users_find(argv[2]) ? users_find(argv[2])->home : "?");
        return 0;
    }
    if (strcmp(sub, "del") == 0 && argc > 2) {
        if (!require_admin("users")) return 1;
        int rc = users_del(argv[2]);
        if (rc == -1) { kprintf("users: no such user '%s'\n", argv[2]); return 1; }
        if (rc == -2) { kprintf("users: refusing to remove the last admin account\n"); return 1; }
        if (rc == -3) { kprintf("users: cannot remove the account you are signed in as\n"); return 1; }
        kprintf("removed '%s' (the home directory is left in place)\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "passwd") == 0 && argc > 3) {
        if (strcmp(argv[2], users_current()) != 0 && !require_admin("users")) return 1;
        if (users_set_password(argv[2], argv[3]) != 0) { kprintf("users: no such user\n"); return 1; }
        kprintf("password changed for '%s'\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "su") == 0 && argc > 3) {
        uint64_t wait = users_lockout_ms();
        if (wait) { kprintf("su: too many failures; wait %lus\n", (unsigned long)(wait / 1000 + 1)); return 1; }
        int uid = users_auth(argv[2], argv[3]);
        if (uid < 0) { users_note_failure(); kprintf("su: authentication failed\n"); return 1; }
        users_note_success();
        users_login(uid);
        kprintf("now acting as '%s'\n", argv[2]);
        return 0;
    }
    kprintf("usage: users list\n"
            "       users add NAME PASS       (admin)\n"
            "       users del NAME            (admin)\n"
            "       users passwd NAME PASS    (yourself, or admin)\n"
            "       users su NAME PASS\n");
    return 1;
}

int cmd_useradd(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: useradd NAME PASSWORD\n"); return 1; }
    char *a[4] = { (char *)"users", (char *)"add", argv[1], argv[2] };
    return cmd_users(4, a);
}

int cmd_userdel(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: userdel NAME\n"); return 1; }
    char *a[3] = { (char *)"users", (char *)"del", argv[1] };
    return cmd_users(3, a);
}

int cmd_passwd(int argc, char **argv) {
    /* passwd            -> change your own, prompting
     * passwd NAME       -> change NAME's, prompting (admin)
     * passwd NAME PASS  -> non-interactive (what scripts and tests use) */
    const char *who = (argc > 1) ? argv[1] : users_current();
    if (!who || !who[0]) { kprintf("passwd: not logged in\n"); return 1; }
    if (strcmp(who, users_current()) != 0 && !require_admin("passwd")) return 1;

    char pw[40];
    if (argc > 2) {
        strncpy(pw, argv[2], sizeof pw - 1); pw[sizeof pw - 1] = 0;
    } else {
        /* Changing your own password proves you know the current one first. */
        if (strcmp(who, users_current()) == 0 && !users_is_root()) {
            char old[40];
            kprintf("current password: ");
            sh_readline(old, sizeof old);
            if (users_auth(who, old) < 0) {
                users_note_failure();
                kprintf("passwd: authentication failed\n");
                return 1;
            }
        }
        kprintf("new password: ");
        sh_readline(pw, sizeof pw);
        char again[40];
        kprintf("retype new password: ");
        sh_readline(again, sizeof again);
        if (strcmp(pw, again) != 0) { kprintf("passwd: passwords do not match\n"); return 1; }
    }
    if (!pw[0]) { kprintf("passwd: refusing to set an empty password\n"); return 1; }
    if (users_set_password(who, pw) != 0) { kprintf("passwd: no such user '%s'\n", who); return 1; }
    kprintf("password updated for '%s'\n", who);
    return 0;
}

/* ----------------------------------------------------------------- sudo ----*/
/* Run one command as root. The elevation lasts exactly as long as that command
 * and is dropped afterwards, so nothing stays elevated by accident -- which is
 * the entire difference between this and the previous model, where every shell
 * simply acted as whoever happened to be logged in with no boundary at all.
 *
 * Authentication is cached for a grace period, the way sudo everywhere does:
 * long enough not to be infuriating, short enough that an unattended terminal
 * is not a standing root shell. */
int cmd_sudo(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: sudo COMMAND [ARGS...]\n");
        kprintf("       runs one command as root; you must be in the admin group\n");
        return 1;
    }
    if (users_current_uid() < 0) { kprintf("sudo: not logged in\n"); return 1; }
    if (users_is_root()) {                       /* already root: just run it */
        const command_t *c = cmd_lookup(argv[1]);
        if (!c) { kprintf("sudo: unknown command: %s\n", argv[1]); return 1; }
        return c->fn(argc - 1, argv + 1);
    }
    if (!users_is_admin()) {
        kprintf("sudo: %s is not in the admin group; this incident is recorded\n",
                users_current());
        return 1;
    }

    if (!users_sudo_fresh()) {
        uint64_t wait = users_lockout_ms();
        if (wait) { kprintf("sudo: too many failures; wait %lus\n", (unsigned long)(wait / 1000 + 1)); return 1; }
        char pw[40];
        kprintf("[sudo] password for %s: ", users_current());
        sh_readline(pw, sizeof pw);
        if (users_auth(users_current(), pw) < 0) {
            users_note_failure();
            kprintf("sudo: authentication failed\n");
            return 1;
        }
        users_note_success();
        users_sudo_authenticated();
    }

    const command_t *c = cmd_lookup(argv[1]);
    if (!c) { kprintf("sudo: unknown command: %s\n", argv[1]); return 1; }

    if (!users_sudo_begin()) { kprintf("sudo: refused\n"); return 1; }
    int rc = c->fn(argc - 1, argv + 1);
    users_sudo_end();                            /* always, even if it failed */
    return rc;
}

/* ------------------------------------------------------------- session -----*/
int cmd_lock(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!fb_present()) {
        kprintf("lock: the screen lock needs the desktop; there is nothing to\n"
                "      lock on a serial-only session.\n");
        return 1;
    }
    if (!users_current()[0]) { kprintf("lock: not signed in\n"); return 1; }
    kprintf("locking the session; your password unlocks it\n");
    gui_lock();
    return 0;
}

/* ---------------------------------------------------- chmod / chown --------*/
static int parse_octal(const char *s, int *out) {
    int v = 0, any = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '7') return -1;
        v = v * 8 + (*s - '0');
        any = 1;
    }
    if (!any || v > 07777) return -1;
    *out = v;
    return 0;
}

int cmd_chmod(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: chmod MODE FILE...      (MODE is octal, e.g. 644, 755)\n");
        return 1;
    }
    int mode;
    if (parse_octal(argv[1], &mode) != 0) { kprintf("chmod: bad mode '%s'\n", argv[1]); return 1; }
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        int rc = fs_chmod(argv[i], mode);
        if (rc == -1) { kprintf("chmod: %s: no such file\n", argv[i]); bad = 1; }
        else if (rc == -2) { kprintf("chmod: %s: not the owner\n", argv[i]); bad = 1; }
    }
    return bad;
}

int cmd_chown(int argc, char **argv) {
    if (argc < 3) {
        kprintf("usage: chown USER FILE...     (root only)\n");
        return 1;
    }
    const user_t *u = users_find(argv[1]);
    int uid, gid = -1;
    if (u) { uid = u->uid; gid = u->gid; }
    else {
        uid = atoi(argv[1]);
        if (uid == 0 && argv[1][0] != '0') { kprintf("chown: no such user '%s'\n", argv[1]); return 1; }
    }
    int bad = 0;
    for (int i = 2; i < argc; i++) {
        int rc = fs_chown(argv[i], uid, gid);
        if (rc == -1) { kprintf("chown: %s: no such file\n", argv[i]); bad = 1; }
        else if (rc == -2) { kprintf("chown: only root may change ownership\n"); return 1; }
    }
    return bad;
}
