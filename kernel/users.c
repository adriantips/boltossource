/* ===========================================================================
 *  BoltOS  -  kernel/users.c
 *  Multi-user account table, persisted to /etc/passwd as
 *  "name:salt:hash:uid:gid:home" lines. The FS autosaves, so accounts survive
 *  reboots. See include/users.h for the model.
 * ===========================================================================*/
#include <stdint.h>
#include "users.h"
#include "fs.h"
#include "crypto.h"
#include "string.h"
#include "kprintf.h"
#include "pit.h"

static user_t users[USER_MAX];
static int    nuser;
static int    cur_uid = -1;
static int    cur_gid = GID_USER;
static char   cur_name[USER_NAME_MAX];
static char   cur_home[USER_HOME_MAX];

/* sudo state */
static int      sudo_depth;
static uint64_t sudo_auth_tick;
#define SUDO_GRACE_TICKS 300000ull        /* ~5 minutes at 1000 Hz */

/* login throttling */
static int      fail_count;
static uint64_t next_attempt_tick;

/* ---------------------------------------------------------------------------
 *  Password hashing: PBKDF2-HMAC-SHA256 over a per-account random salt.
 *
 *  The previous scheme was one unsalted FNV-1a pass. That is a checksum: two
 *  users with the same password got the same value on disk, and the whole table
 *  falls to a wordlist essentially instantly. Stretching makes each guess cost
 *  real work; the salt stops that work from being shared across accounts or
 *  precomputed.
 * ------------------------------------------------------------------------- */
static void pbkdf2(const char *pw, const uint8_t *salt, uint32_t rounds,
                   uint8_t out[USER_HASH_LEN]) {
    uint32_t pwlen = (uint32_t)strlen(pw);
    uint8_t block[USER_SALT_LEN + 4];
    memcpy(block, salt, USER_SALT_LEN);
    block[USER_SALT_LEN + 0] = 0; block[USER_SALT_LEN + 1] = 0;
    block[USER_SALT_LEN + 2] = 0; block[USER_SALT_LEN + 3] = 1;   /* INT(1) */

    uint8_t u[USER_HASH_LEN], acc[USER_HASH_LEN];
    hmac_sha256((const uint8_t *)pw, pwlen, block, sizeof block, u);
    memcpy(acc, u, USER_HASH_LEN);
    for (uint32_t i = 1; i < rounds; i++) {
        hmac_sha256((const uint8_t *)pw, pwlen, u, USER_HASH_LEN, u);
        for (int k = 0; k < USER_HASH_LEN; k++) acc[k] ^= u[k];
    }
    memcpy(out, acc, USER_HASH_LEN);
}

/* Constant-time compare, so a wrong password cannot be narrowed byte by byte
 * from how long the check took. */
static int ct_equal(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t diff = 0;
    for (int i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

/* Salt material. No hardware RNG here, so mix the timestamp counter (which the
 * stack-canary seed already relies on) with the tick count and a running
 * counter, then hash it -- enough that two accounts made in the same second do
 * not share a salt. */
static void make_salt(uint8_t out[USER_SALT_LEN]) {
    static uint64_t counter;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t seed[3] = { ((uint64_t)hi << 32) | lo, pit_ticks(), ++counter };
    uint8_t digest[32];
    sha256(seed, sizeof seed, digest);
    memcpy(out, digest, USER_SALT_LEN);
}

static user_t *find(const char *name) {
    for (int i = 0; i < nuser; i++)
        if (strcmp(users[i].name, name) == 0) return &users[i];
    return 0;
}

/* ---- hex helpers -------------------------------------------------------- */
static void to_hex(const uint8_t *b, int n, char *out) {
    const char *d = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = d[b[i] >> 4]; out[i * 2 + 1] = d[b[i] & 0xF]; }
    out[n * 2] = 0;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int from_hex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int a = hexval(s[i * 2]), b = hexval(s[i * 2 + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return 0;
}

static void int_str(int v, char *out) {
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[12]; int t = 0; int neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    while (u) { tmp[t++] = (char)('0' + u % 10); u /= 10; }
    int k = 0; if (neg) out[k++] = '-';
    while (t) out[k++] = tmp[--t];
    out[k] = 0;
}

/* ---- /etc/passwd serialisation ------------------------------------------ */
static void save(void) {
    static char buf[USER_MAX * 180];
    int n = 0;
    for (int i = 0; i < nuser; i++) {
        char salt[USER_SALT_LEN * 2 + 1], hash[USER_HASH_LEN * 2 + 1], num[12];
        to_hex(users[i].salt, USER_SALT_LEN, salt);
        to_hex(users[i].hash, USER_HASH_LEN, hash);

        const char *fields[6];
        char uidbuf[12], gidbuf[12];
        int_str(users[i].uid, uidbuf);
        int_str(users[i].gid, gidbuf);
        fields[0] = users[i].name; fields[1] = salt;   fields[2] = hash;
        fields[3] = uidbuf;        fields[4] = gidbuf; fields[5] = users[i].home;
        (void)num;

        for (int f = 0; f < 6; f++) {
            for (const char *p = fields[f]; *p && n < (int)sizeof(buf) - 2; p++) buf[n++] = *p;
            if (n < (int)sizeof(buf) - 2) buf[n++] = (f == 5) ? '\n' : ':';
        }
    }
    buf[n] = 0;
    if (!fs_lookup("/etc")) fs_create("/etc", 1);
    fs_node *f = fs_lookup("/etc/passwd");
    if (!f) f = fs_create("/etc/passwd", 0);
    if (f) fs_write(f, buf, (uint32_t)n);
}

/* Split one line on ':' into at most `max` NUL-terminated pieces in place. */
static int split(char *line, char **out, int max) {
    int n = 0;
    char *p = line;
    out[n++] = p;
    while (*p && n < max) {
        if (*p == ':') { *p++ = 0; out[n++] = p; continue; }
        p++;
    }
    return n;
}

static int load(void) {
    fs_node *f = fs_lookup("/etc/passwd");
    if (!f || !f->data || f->size == 0) return -1;
    nuser = 0;
    const char *s = (const char *)f->data;
    uint32_t i = 0;
    while (i < f->size && nuser < USER_MAX) {
        char line[200]; int ln = 0;
        while (i < f->size && s[i] != '\n' && ln < 199) line[ln++] = s[i++];
        if (i < f->size && s[i] == '\n') i++;
        line[ln] = 0;
        if (ln == 0) continue;

        char *fld[8];
        int nf = split(line, fld, 8);
        if (nf < 5) continue;                       /* not a record we understand */

        user_t *u = &users[nuser];
        memset(u, 0, sizeof *u);
        strncpy(u->name, fld[0], USER_NAME_MAX - 1);
        if (from_hex(fld[1], u->salt, USER_SALT_LEN) != 0) continue;
        if (from_hex(fld[2], u->hash, USER_HASH_LEN) != 0) continue;
        u->uid = atoi(fld[3]);
        u->gid = atoi(fld[4]);
        if (nf >= 6 && fld[5][0]) strncpy(u->home, fld[5], USER_HOME_MAX - 1);
        else { strcpy(u->home, "/home/"); strncat(u->home, u->name, USER_HOME_MAX - 8); }
        nuser++;
    }
    return nuser > 0 ? 0 : -1;
}

/* Every account owns a home directory; create it if the tree does not have one
 * (a fresh install, or an account restored from a table written elsewhere). */
static void ensure_home(const user_t *u) {
    if (!u->home[0]) return;
    if (fs_lookup(u->home)) return;
    if (!fs_lookup("/home")) fs_create("/home", 1);
    fs_node *d = fs_create(u->home, 1);
    if (d) fs_chown_node(d, u->uid, u->gid, 0700 | 040000);
}

void users_init(void) {
    if (load() == 0) {
        for (int i = 0; i < nuser; i++) ensure_home(&users[i]);
        return;                                     /* restored from disk */
    }
    nuser = 0;
    users_add_full("root", "root", 0,    GID_ADMIN);   /* superuser        */
    users_add_full("user", "bolt", 1000, GID_ADMIN);   /* default desktop  */
    save();
}

int users_next_uid(void) {
    int uid = 1000;
    for (int i = 0; i < nuser; i++) if (users[i].uid >= uid) uid = users[i].uid + 1;
    return uid;
}

int users_add_full(const char *name, const char *pw, int uid, int gid) {
    if (nuser >= USER_MAX || !name || !name[0]) return -1;
    if (find(name)) return -1;
    user_t *u = &users[nuser];
    memset(u, 0, sizeof *u);
    strncpy(u->name, name, USER_NAME_MAX - 1);
    make_salt(u->salt);
    pbkdf2(pw ? pw : "", u->salt, USER_KDF_ROUNDS, u->hash);
    u->uid = uid;
    u->gid = gid;
    if (uid == 0) strcpy(u->home, "/root");
    else { strcpy(u->home, "/home/"); strncat(u->home, name, USER_HOME_MAX - 8); }
    nuser++;
    ensure_home(u);
    save();
    return 0;
}

int users_add(const char *name, const char *pw, int uid) {
    return users_add_full(name, pw, uid, uid == 0 ? GID_ADMIN : GID_USER);
}

int users_del(const char *name) {
    user_t *u = find(name);
    if (!u) return -1;
    /* Never leave the machine with no way to administer it. */
    int admins = 0;
    for (int i = 0; i < nuser; i++)
        if (users[i].uid == 0 || users[i].gid == GID_ADMIN) admins++;
    if ((u->uid == 0 || u->gid == GID_ADMIN) && admins <= 1) return -2;
    if (u->uid == cur_uid) return -3;               /* not the logged-in user */

    int idx = (int)(u - users);
    for (int i = idx; i + 1 < nuser; i++) users[i] = users[i + 1];
    nuser--;
    save();
    return 0;
}

int users_set_password(const char *name, const char *pw) {
    user_t *u = find(name);
    if (!u) return -1;
    make_salt(u->salt);                             /* new salt with every change */
    pbkdf2(pw ? pw : "", u->salt, USER_KDF_ROUNDS, u->hash);
    save();
    return 0;
}

int users_auth(const char *name, const char *pw) {
    user_t *u = find(name);
    uint8_t got[USER_HASH_LEN];
    if (!u) {
        /* Hash anyway against a throwaway salt: a missing account must take as
         * long to reject as a wrong password, or the table can be enumerated
         * just by timing the response. */
        uint8_t dummy[USER_SALT_LEN] = {0};
        pbkdf2(pw ? pw : "", dummy, USER_KDF_ROUNDS, got);
        return -1;
    }
    pbkdf2(pw ? pw : "", u->salt, USER_KDF_ROUNDS, got);
    return ct_equal(got, u->hash, USER_HASH_LEN) ? u->uid : -1;
}

int           users_count(void) { return nuser; }
const user_t *users_get(int i)  { return (i >= 0 && i < nuser) ? &users[i] : 0; }
const user_t *users_find(const char *name) { return find(name); }
const user_t *users_find_uid(int uid) {
    for (int i = 0; i < nuser; i++) if (users[i].uid == uid) return &users[i];
    return 0;
}

void users_login(int uid) {
    for (int i = 0; i < nuser; i++) {
        if (users[i].uid != uid) continue;
        cur_uid = uid;
        cur_gid = users[i].gid;
        strncpy(cur_name, users[i].name, sizeof cur_name - 1);
        strncpy(cur_home, users[i].home, sizeof cur_home - 1);
        ensure_home(&users[i]);
        sudo_depth = 0; sudo_auth_tick = 0;
        return;
    }
}

void users_logout(void) {
    cur_uid = -1; cur_gid = GID_USER;
    cur_name[0] = 0; cur_home[0] = 0;
    sudo_depth = 0; sudo_auth_tick = 0;
}

const char *users_current(void)     { return cur_uid >= 0 ? cur_name : ""; }
int         users_current_uid(void) { return sudo_depth ? 0 : cur_uid; }
int         users_current_gid(void) { return sudo_depth ? GID_ADMIN : cur_gid; }
int         users_is_root(void)     { return users_current_uid() == 0; }
int         users_is_admin(void)    { return users_is_root() || users_current_gid() == GID_ADMIN; }
const char *users_home(void)        { return cur_home[0] ? cur_home : "/"; }

/* ---- sudo --------------------------------------------------------------- */
int  users_sudo_begin(void) {
    if (cur_uid < 0) return 0;
    if (!users_is_admin() && cur_uid != 0) return 0;
    sudo_depth++;
    return 1;
}
void users_sudo_end(void)        { if (sudo_depth) sudo_depth--; }
int  users_sudo_active(void)     { return sudo_depth > 0; }
void users_sudo_authenticated(void) { sudo_auth_tick = pit_ticks() ? pit_ticks() : 1; }
int  users_sudo_fresh(void) {
    if (!sudo_auth_tick) return 0;
    return (pit_ticks() - sudo_auth_tick) < SUDO_GRACE_TICKS;
}

/* ---- login throttling ---------------------------------------------------- */
/* Each consecutive failure doubles the wait, to a ceiling. Cheap, and it turns
 * an online guessing attack from thousands of tries a second into a handful a
 * minute without ever locking a legitimate user out permanently. */
void users_note_failure(void) {
    if (fail_count < 16) fail_count++;
    uint64_t delay = 250ull << (fail_count > 6 ? 6 : fail_count);   /* <= 16 s */
    next_attempt_tick = pit_ticks() + delay;
}
void users_note_success(void) { fail_count = 0; next_attempt_tick = 0; }
int  users_fail_count(void)   { return fail_count; }

uint64_t users_lockout_ms(void) {
    uint64_t now = pit_ticks();
    return now < next_attempt_tick ? (next_attempt_tick - now) : 0;
}
