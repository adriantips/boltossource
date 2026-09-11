#include <stdint.h>
#include <stddef.h>
#include "vault.h"
#include "fs.h"
#include "users.h"
#include "crypto.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "notify.h"

/* ===========================================================================
 *  BoltVault. See include/vault.h for what a vault is, and for the honest
 *  account of what it protects.
 *
 *  The archive on disk:
 *
 *      "BOLTVLT1"      8
 *      nonce          12     fresh from the DRBG on every seal
 *      tag            16     AES-256-GCM over the plaintext, with the magic
 *                            as additional data
 *      plain_len       4     little-endian
 *      ciphertext      plain_len
 *
 *  and the plaintext inside it:
 *
 *      "BVA1"          4
 *      count           4
 *      count entries of:  u32 namelen, u32 datalen, u8 is_dir, u8 pad[3],
 *                         name[namelen], data[datalen]
 *
 *  Names are paths relative to the vault directory, so a vault keeps its
 *  subdirectories, and a directory is emitted before anything inside it. The
 *  inner magic is checked after opening: the GCM tag already proves the key
 *  was right, and this proves the archive is one of ours rather than a file
 *  that happened to be sitting at that path.
 * ===========================================================================*/

#define ARCH_MAGIC    "BOLTVLT1"
#define ARCH_HDR      (8 + 12 + 16 + 4)
#define PLAIN_MAGIC   "BVA1"
#define VAULT_REL_MAX 192                 /* longest path inside a vault */
#define VAULT_PATH_MAX (VAULT_REL_MAX + 80)

/* Only the current user's vault is ever opened, so one live entry and one live
 * key is all this holds. /etc/vault.conf carries a line per account. */
static struct {
    int      loaded;                      /* conf has been read for `owner`  */
    int      configured;                  /* that user has a vault           */
    char     owner[USER_NAME_MAX];
    uint8_t  salt[VAULT_SALT_LEN];
    uint32_t rounds;
    char     dir[64];                     /* absolute */
    int      unlocked;
    uint8_t  key[VAULT_KEY_LEN];
} V;

static char err[96] = "";
static void fail(const char *m) { strncpy(err, m, sizeof err - 1); err[sizeof err - 1] = 0; }
const char *vault_error(void) { return err; }

static void wipe(void *p, uint32_t n) { volatile uint8_t *q = p; while (n--) *q++ = 0; }

/* ---- little-endian scalars ---------------------------------------------- */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- hex, for the salt in the config file ------------------------------- */
static void to_hex(const uint8_t *in, int n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[i * 2] = d[in[i] >> 4]; out[i * 2 + 1] = d[in[i] & 15]; }
    out[n * 2] = 0;
}
static int from_hex(const char *in, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = in[i * 2], b = in[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0'; else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        if (b >= '0' && b <= '9') lo = b - '0'; else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
static void u32_str(uint32_t v, char *out) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* ---- paths --------------------------------------------------------------- */
static void join(char *out, uint32_t cap, const char *a, const char *b) {
    uint32_t n = 0;
    while (a[n] && n < cap - 2) { out[n] = a[n]; n++; }
    if (n && out[n - 1] != '/') out[n++] = '/';
    uint32_t k = 0;
    while (b[k] && n < cap - 1) out[n++] = b[k++];
    out[n] = 0;
}

/* A relative path that will not fit is refused rather than truncated: two
 * truncated paths can collide, and a vault that quietly loses a file when it
 * is sealed would be worse than one that refuses to seal. */
static int relpath(char *out, uint32_t cap, const char *prefix, const char *name) {
    uint32_t n = 0;
    if (prefix[0]) {
        while (prefix[n]) { if (n + 2 >= cap) return -1; out[n] = prefix[n]; n++; }
        out[n++] = '/';
    }
    for (uint32_t k = 0; name[k]; k++) { if (n + 1 >= cap) return -1; out[n++] = name[k]; }
    out[n] = 0;
    return 0;
}

static void archive_path(char *out, uint32_t cap) {
    /* Beside the directory, not inside it: the archive is what the directory
     * turns into, and packing it into itself would be a fine joke. */
    char parent[64];
    strncpy(parent, V.dir, sizeof parent - 1); parent[sizeof parent - 1] = 0;
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) *slash = 0; else strcpy(parent, "/");
    join(out, cap, parent, VAULT_ARCHIVE);
}

/* ---- configuration ------------------------------------------------------- *
 *  "owner:saltHex:rounds:dir", one line per account. A salt is not a secret --
 *  it exists so the work of guessing one password cannot be reused against
 *  another -- so the file only needs to survive, not to hide. Losing it means
 *  no vault on this machine can ever be opened again.
 * ------------------------------------------------------------------------- */

/* Copy the config out, dropping any line for `skip`. Returns bytes written. */
static uint32_t conf_without(const char *skip, char *out, uint32_t cap) {
    fs_node *f = fs_lookup(VAULT_CONF);
    uint32_t n = 0;
    if (!f || !f->data) return 0;
    uint32_t i = 0;
    while (i < f->size) {
        uint32_t e = i;
        while (e < f->size && f->data[e] != '\n') e++;
        uint32_t len = e - i;
        int drop = 0;
        if (skip) {
            uint32_t k = 0;
            while (k < len && f->data[i + k] != ':') k++;
            drop = (k == strlen(skip) && memcmp(f->data + i, skip, k) == 0);
        }
        if (!drop && len && n + len + 1 < cap) {
            memcpy(out + n, f->data + i, len);
            n += len;
            out[n++] = '\n';
        }
        i = e + 1;
    }
    return n;
}

static void conf_write(const char *body, uint32_t len) {
    fs_node *f = fs_lookup(VAULT_CONF);
    if (!f) f = fs_create(VAULT_CONF, 0);
    if (!f) return;
    fs_write(f, body, len);
    fs_chown_node(f, 0, 0, 0600);         /* who has a vault is root's business */
}

static void conf_save(void) {
    char buf[192 * USER_MAX];
    uint32_t n = conf_without(V.owner, buf, sizeof buf);
    char salt[VAULT_SALT_LEN * 2 + 1], rounds[12];
    to_hex(V.salt, VAULT_SALT_LEN, salt);
    u32_str(V.rounds, rounds);
    const char *parts[4] = { V.owner, salt, rounds, V.dir };
    for (int i = 0; i < 4; i++) {
        for (const char *p = parts[i]; *p && n < sizeof buf - 2; p++) buf[n++] = *p;
        if (n < sizeof buf - 1) buf[n++] = (i == 3) ? '\n' : ':';
    }
    conf_write(buf, n);
}

static void conf_forget(const char *who) {
    char buf[192 * USER_MAX];
    uint32_t n = conf_without(who, buf, sizeof buf);
    if (n) conf_write(buf, n);
    else    fs_remove(VAULT_CONF);
}

/* Load the line belonging to `who`. */
static void conf_load(const char *who) {
    V.loaded = 1;
    V.configured = 0;
    fs_node *f = fs_lookup(VAULT_CONF);
    if (!f || !f->data || !f->size || !who || !who[0]) return;

    uint32_t i = 0;
    while (i < f->size) {
        uint32_t e = i;
        while (e < f->size && f->data[e] != '\n') e++;
        char line[192];
        uint32_t len = e - i;
        if (len && len < sizeof line) {
            memcpy(line, f->data + i, len);
            line[len] = 0;
            char *fld[4]; int nf = 0;
            fld[nf++] = line;
            for (char *p = line; *p && nf < 4; p++)
                if (*p == ':') { *p = 0; fld[nf++] = p + 1; }
            if (nf == 4 && strcmp(fld[0], who) == 0 &&
                from_hex(fld[1], V.salt, VAULT_SALT_LEN) == 0) {
                for (char *p = fld[3]; *p; p++) if (*p == '\r') { *p = 0; break; }
                strncpy(V.owner, fld[0], sizeof V.owner - 1); V.owner[sizeof V.owner - 1] = 0;
                V.rounds = 0;
                for (const char *p = fld[2]; *p >= '0' && *p <= '9'; p++)
                    V.rounds = V.rounds * 10 + (uint32_t)(*p - '0');
                if (V.rounds == 0) V.rounds = VAULT_KDF_ROUNDS;
                strncpy(V.dir, fld[3], sizeof V.dir - 1); V.dir[sizeof V.dir - 1] = 0;
                V.configured = V.owner[0] && V.dir[0];
                return;
            }
        }
        i = e + 1;
    }
}

/* The loaded entry has to belong to whoever is asking. A vault is one
 * account's; another account gets told there is none rather than being invited
 * to guess at its password. Switching user reloads. */
static int mine(void) {
    const char *me = users_current();
    if (!me || !me[0]) return 0;
    if (!V.loaded || strcmp(me, V.owner) != 0) {
        if (V.unlocked) return 0;         /* never drop a live key on a switch */
        conf_load(me);
    }
    return V.configured && strcmp(me, V.owner) == 0;
}

/* ---- packing ------------------------------------------------------------- */
/* Both passes walk the tree the same way, so measure and emit cannot disagree
 * about what is in it: one adds up, the other writes. */
static int measure(fs_node *dir, const char *prefix, uint32_t *count, uint64_t *bytes) {
    for (fs_node *c = dir->child; c; c = c->next) {
        char rel[VAULT_REL_MAX];
        if (relpath(rel, sizeof rel, prefix, c->name) != 0) return -1;
        (*count)++;
        *bytes += 12 + strlen(rel) + (c->is_dir ? 0 : c->size);
        if (*bytes > VAULT_MAX_BYTES) return -1;
        if (c->is_dir && measure(c, rel, count, bytes) != 0) return -1;
    }
    return 0;
}

static void emit(fs_node *dir, const char *prefix, uint8_t **pp) {
    for (fs_node *c = dir->child; c; c = c->next) {
        char rel[VAULT_REL_MAX];
        if (relpath(rel, sizeof rel, prefix, c->name) != 0) continue;  /* measure
                                                                        * already
                                                                        * refused */
        uint32_t nl = (uint32_t)strlen(rel);
        uint32_t dl = c->is_dir ? 0 : c->size;
        uint8_t *p = *pp;
        put32(p, nl); put32(p + 4, dl);
        p[8] = (uint8_t)(c->is_dir ? 1 : 0); p[9] = p[10] = p[11] = 0;
        memcpy(p + 12, rel, nl);
        if (dl && c->data) memcpy(p + 12 + nl, c->data, dl);
        *pp = p + 12 + nl + dl;
        if (c->is_dir) emit(c, rel, pp);
    }
}

/* ---- sealing ------------------------------------------------------------- */
static int seal_to_disk(void) {
    fs_node *dir = fs_lookup(V.dir);
    if (!dir || !dir->is_dir) { fail("the vault directory is missing"); return -1; }

    uint32_t count = 0;
    uint64_t body = 0;
    if (measure(dir, "", &count, &body) != 0) {
        fail("the vault holds more than it can seal, or a path is too long");
        return -1;
    }
    uint32_t plain_len = 8 + (uint32_t)body;
    uint8_t *plain = (uint8_t *)kmalloc(plain_len);
    if (!plain) { fail("out of memory sealing the vault"); return -1; }
    memcpy(plain, PLAIN_MAGIC, 4);
    put32(plain + 4, count);
    uint8_t *p = plain + 8;
    emit(dir, "", &p);

    uint8_t *out = (uint8_t *)kmalloc(ARCH_HDR + plain_len);
    if (!out) {
        wipe(plain, plain_len); kfree(plain);
        fail("out of memory sealing the vault");
        return -1;
    }
    memcpy(out, ARCH_MAGIC, 8);
    crypto_rand_bytes(out + 8, 12);                    /* a fresh nonce, always */
    put32(out + 36, plain_len);
    /* The magic is passed as additional data, so an archive cannot be passed
     * off as anything else without the tag noticing. */
    aes256_gcm_seal(V.key, out + 8, out, 8, plain, plain_len, out + ARCH_HDR, out + 20);
    wipe(plain, plain_len);
    kfree(plain);

    char apath[VAULT_PATH_MAX];
    archive_path(apath, sizeof apath);
    fs_node *a = fs_lookup(apath);
    if (!a) a = fs_create(apath, 0);
    if (!a || fs_write(a, out, ARCH_HDR + plain_len) != 0) {
        kfree(out);
        fail("could not write the sealed archive");
        return -1;
    }
    fs_chown_node(a, users_current_uid(), users_current_gid(), 0600);
    kfree(out);

    /* Only now that the archive exists is the plaintext removed. The other
     * order turns any failure here into a vault that is simply gone.
     *
     * TODO: this is where the plaintext stops existing, which is later than it
     * should be -- BoltFS has been committing these files all along. Sealing
     * them on the way into the image needs a filter in the persistence layer,
     * and that is a change to the one file in this tree I least want to be
     * clever in. */
    fs_node *c = dir->child;
    while (c) { fs_node *next = c->next; fs_remove_node(c); c = next; }
    return 0;
}

static int open_from_disk(void) {
    char apath[VAULT_PATH_MAX];
    archive_path(apath, sizeof apath);
    fs_node *a = fs_lookup(apath);
    if (!a || !a->data || a->size < ARCH_HDR) { fail("no sealed archive to open"); return -1; }
    if (memcmp(a->data, ARCH_MAGIC, 8) != 0) { fail("that is not a vault archive"); return -1; }
    uint32_t plain_len = get32(a->data + 36);
    if (plain_len > VAULT_MAX_BYTES || a->size != ARCH_HDR + plain_len) {
        fail("the sealed archive is damaged");
        return -1;
    }

    uint8_t *plain = (uint8_t *)kmalloc(plain_len ? plain_len : 1);
    if (!plain) { fail("out of memory opening the vault"); return -1; }
    int ok = aes256_gcm_open(V.key, a->data + 8, a->data, 8,
                             a->data + ARCH_HDR, plain_len, a->data + 20, plain);
    if (ok != 0 || plain_len < 8 || memcmp(plain, PLAIN_MAGIC, 4) != 0) {
        wipe(plain, plain_len ? plain_len : 1);
        kfree(plain);
        /* A failed tag is a wrong password far more often than a damaged file,
         * and either way nothing usable was decrypted: GCM authenticates before
         * anything here looks at the bytes. */
        fail("wrong password, or the archive has been tampered with");
        return -1;
    }

    fs_node *dir = fs_lookup(V.dir);
    if (!dir) dir = fs_create(V.dir, 1);
    if (!dir || !dir->is_dir) {
        wipe(plain, plain_len); kfree(plain);
        fail("the vault directory is missing");
        return -1;
    }
    fs_chown_node(dir, users_current_uid(), users_current_gid(), 0700);

    uint32_t count = get32(plain + 4), restored = 0;
    const uint8_t *p = plain + 8, *end = plain + plain_len;
    for (uint32_t i = 0; i < count; i++) {
        if ((uint64_t)(end - p) < 12) break;
        uint32_t nl = get32(p), dl = get32(p + 4);
        int isdir = p[8] != 0;
        /* The lengths come out of a file, so they are compared against what is
         * left rather than added to a pointer that could then wrap past it. */
        if (nl == 0 || nl >= VAULT_REL_MAX) break;
        if (12ull + nl + dl > (uint64_t)(end - p)) break;

        char rel[VAULT_REL_MAX], path[VAULT_PATH_MAX];
        memcpy(rel, p + 12, nl); rel[nl] = 0;
        join(path, sizeof path, V.dir, rel);
        fs_node *n = fs_create(path, isdir);
        if (n && !isdir && dl) fs_write(n, p + 12 + nl, dl);
        if (n) fs_chown_node(n, users_current_uid(), users_current_gid(),
                             isdir ? 0700 : 0600);
        restored++;
        p += 12 + nl + dl;
    }
    wipe(plain, plain_len);
    kfree(plain);
    if (restored != count) { fail("the sealed archive is damaged"); return -1; }

    fs_remove(apath);                    /* the directory is the vault again */
    return 0;
}

/* Sealing the files is not the same as the disk no longer holding them.
 * BoltFS commits alternate between two slots, so the commit that publishes the
 * sealed archive leaves the previous image -- the one with the plaintext in it
 * -- sitting in the other slot, complete and readable by anyone who takes the
 * disk out. Committing twice more writes both slots with the sealed image.
 *
 * What this does not do is scrub the partition. If the sealed image is shorter
 * than the one it replaced, the bytes past its end are still whatever was
 * there, and a vault that shrank can leave a tail behind. Zeroing the inactive
 * slot outright would also throw away the copy the loader falls back to when
 * the newest image will not check out, which is not a trade to make quietly.
 */
static void overwrite_previous_image(void) {
    if (!fs_persist_active() || fs_persist_readonly()) return;
    fs_sync();
    fs_sync();
}

/* ---- the public operations ---------------------------------------------- */
int vault_create(const char *password) {
    err[0] = 0;
    const char *me = users_current();
    if (!me || !me[0]) { fail("not logged in"); return -1; }
    if (mine()) { fail("this account already has a vault"); return -1; }
    if (!password || !password[0]) { fail("refusing to make a vault with no password"); return -1; }

    strncpy(V.owner, me, sizeof V.owner - 1); V.owner[sizeof V.owner - 1] = 0;
    crypto_rand_bytes(V.salt, VAULT_SALT_LEN);
    V.rounds = VAULT_KDF_ROUNDS;
    join(V.dir, sizeof V.dir, users_home(), VAULT_DIR_DEFAULT);

    fs_node *dir = fs_lookup(V.dir);
    if (!dir) dir = fs_create(V.dir, 1);
    if (!dir || !dir->is_dir) { fail("could not create the vault directory"); return -1; }
    fs_chown_node(dir, users_current_uid(), users_current_gid(), 0700);

    pbkdf2_sha256(password, (uint32_t)strlen(password), V.salt, VAULT_SALT_LEN,
                  V.rounds, V.key, VAULT_KEY_LEN);
    V.loaded = 1;
    V.configured = 1;
    V.unlocked = 1;                       /* a new vault is open, and empty */
    conf_save();
    return 0;
}

int vault_unlock(const char *password) {
    err[0] = 0;
    if (!mine()) { fail("no vault on this account"); return -1; }
    if (V.unlocked) { fail("the vault is already unlocked"); return -1; }
    if (!password) password = "";

    pbkdf2_sha256(password, (uint32_t)strlen(password), V.salt, VAULT_SALT_LEN,
                  V.rounds, V.key, VAULT_KEY_LEN);
    if (open_from_disk() != 0) {
        wipe(V.key, VAULT_KEY_LEN);       /* a wrong guess leaves nothing behind */
        users_note_failure();             /* and costs what a wrong login costs */
        return -1;
    }
    users_note_success();
    V.unlocked = 1;
    return 0;
}

int vault_lock(void) {
    err[0] = 0;
    if (!mine()) { fail("no vault on this account"); return -1; }
    if (!V.unlocked) { fail("the vault is already locked"); return -1; }
    if (seal_to_disk() != 0) return -1;
    wipe(V.key, VAULT_KEY_LEN);
    V.unlocked = 0;
    overwrite_previous_image();
    return 0;
}

int vault_destroy(const char *password) {
    err[0] = 0;
    if (!mine()) { fail("no vault on this account"); return -1; }
    /* Everything comes back out as ordinary files first. Removing a vault must
     * never be a way to lose what was in it. */
    if (!V.unlocked && vault_unlock(password) != 0) return -1;
    wipe(V.key, VAULT_KEY_LEN);
    V.unlocked = 0;
    V.configured = 0;
    conf_forget(V.owner);
    return 0;
}

void vault_get_status(vault_status *out) {
    memset(out, 0, sizeof *out);
    if (!mine()) return;
    out->exists = 1;
    out->unlocked = V.unlocked;
    strncpy(out->dir, V.dir, sizeof out->dir - 1);
    if (V.unlocked) {
        fs_node *dir = fs_lookup(V.dir);
        if (dir) {
            uint32_t count = 0; uint64_t bytes = 0;
            measure(dir, "", &count, &bytes);
            out->files = (int)count;
            out->bytes = bytes;
        }
    } else {
        char apath[VAULT_PATH_MAX];
        archive_path(apath, sizeof apath);
        fs_node *a = fs_lookup(apath);
        out->archive_bytes = a ? a->size : 0;
    }
}

void vault_lock_all(void) {
    if (!V.configured || !V.unlocked) return;
    /* Called from the lock screen, from logout and from shutdown, where there
     * is nobody to ask and nothing useful to do about a failure except say so
     * somewhere it will be found afterwards. */
    if (seal_to_disk() != 0) {
        kprintf("[vault] could not seal %s: %s\n", V.dir, err);
        notify_error("Vault", "the vault could not be sealed");
        return;
    }
    wipe(V.key, VAULT_KEY_LEN);
    V.unlocked = 0;
    overwrite_previous_image();
    kprintf("[vault] sealed %s\n", V.dir);
}
