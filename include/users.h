#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS multi-user accounts.
 *
 *  A /etc/passwd-style table persisted to the filesystem. Each record is
 *
 *      name:salt:hash:uid:gid:home
 *
 *  where salt and hash are hex. Passwords are stretched with PBKDF2-HMAC-SHA256
 *  (net/crypto.c) over a per-account random salt -- the old scheme was a single
 *  unsalted FNV-1a pass, which is a checksum, not a password hash: identical
 *  passwords collided visibly and the whole table fell to a wordlist in
 *  milliseconds. Stretching makes each guess cost real work, and the salt means
 *  the work cannot be shared between accounts.
 *
 *  One "current user" is the identity the shell and GUI act as after login.
 *  Membership of GID_ADMIN is what `sudo` checks.
 * ===========================================================================*/
#define USER_MAX      16
#define USER_NAME_MAX 24
#define USER_SALT_LEN 16
#define USER_HASH_LEN 32
#define USER_HOME_MAX 40

/* PBKDF2 iterations. High enough to cost meaningfully per guess, low enough
 * that a login on this kernel's integer-only SHA-256 stays instant. */
#define USER_KDF_ROUNDS 4096

#define GID_ADMIN 0        /* may sudo, may write outside its own home */
#define GID_USER  100

typedef struct {
    char     name[USER_NAME_MAX];
    uint8_t  salt[USER_SALT_LEN];
    uint8_t  hash[USER_HASH_LEN];
    int      uid;
    int      gid;
    char     home[USER_HOME_MAX];
} user_t;

void        users_init(void);                       /* load /etc/passwd or seed defaults */
int         users_auth(const char *name, const char *pw);  /* uid on success, -1 on fail */
int         users_add(const char *name, const char *pw, int uid);  /* 0 ok, -1 fail */
int         users_add_full(const char *name, const char *pw, int uid, int gid);
int         users_del(const char *name);            /* 0 ok; refuses the last admin */
int         users_set_password(const char *name, const char *pw);
int         users_count(void);
const user_t *users_get(int i);
const user_t *users_find(const char *name);
const user_t *users_find_uid(int uid);
int         users_next_uid(void);                   /* a free uid for a new account */

void        users_login(int uid);                   /* set the current identity */
void        users_logout(void);
const char *users_current(void);                    /* current username, "" if none */
int         users_current_uid(void);                /* uid, or -1 if logged out */
int         users_current_gid(void);
int         users_is_root(void);                    /* uid 0 */
int         users_is_admin(void);                   /* uid 0, or gid GID_ADMIN */
const char *users_home(void);                       /* current user's home, or "/" */

/* ---------------------------------------------------------------------------
 *  Privilege elevation. `sudo CMD` runs one command as root; the elevation is
 *  scoped to that command and dropped afterwards, so nothing stays elevated by
 *  accident. Authentication is required once per grace period.
 * ------------------------------------------------------------------------- */
int         users_sudo_begin(void);   /* 1 if elevated, 0 if refused */
void        users_sudo_end(void);
int         users_sudo_active(void);
void        users_sudo_authenticated(void);   /* start/refresh the grace period */
int         users_sudo_fresh(void);           /* still inside the grace period? */

/* ---------------------------------------------------------------------------
 *  Login throttling. A wrong password costs an increasing delay, so guessing at
 *  the sign-in screen or the lock screen is not free.
 * ------------------------------------------------------------------------- */
void        users_note_failure(void);
void        users_note_success(void);
int         users_fail_count(void);
uint64_t    users_lockout_ms(void);   /* how long the next attempt must wait */
