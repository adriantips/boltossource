#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltVault -- an encrypted directory for the persisted filesystem.
 *
 *  A vault is one directory (by default ~/vault) that exists in exactly one of
 *  two states:
 *
 *    unlocked  the directory holds ordinary files, and the sealed archive does
 *              not exist
 *    locked    the directory is empty, and everything that was in it is in one
 *              AES-256-GCM sealed archive beside it (~/.vault.bv)
 *
 *  The key is PBKDF2-HMAC-SHA256 over the vault's own password and a per-vault
 *  salt kept in /etc/vault.conf. It lives in kernel memory only while the vault
 *  is unlocked, and is wiped on lock -- which is what lets the session lock,
 *  logout and shutdown seal the vault without asking for the password again.
 *
 *  What this protects and what it does not.
 *
 *  A locked vault is unreadable without the password: the archive is all that
 *  is on the disk, and the disk is all a stolen machine gives up. That is the
 *  case worth protecting, and the session lock, logout and clean shutdown all
 *  lock the vault, so a machine that is off or unattended is a machine whose
 *  vault is sealed.
 *
 *  An *unlocked* vault is plain files in the ordinary filesystem, and BoltFS
 *  commits those to the disk like any others. Pulling the power out of a
 *  machine whose vault is open leaves the plaintext on the platter. Sealing
 *  the contents on their way into the image instead -- so that plaintext never
 *  reaches the disk at all -- means a filter in the persistence layer, and
 *  that is the honest next step rather than something this quietly does.
 *
 *  And the vault is responsible for what is inside it, which is not the same
 *  as everything that has ever passed through the machine. A secret typed at
 *  a prompt is in that account's shell history; one printed by `cat` is in the
 *  kernel log, which a clean shutdown writes to /var/log. Both are ordinary
 *  files outside the vault, and locking the vault does not go looking for
 *  copies of its contents elsewhere -- nothing here can. Put the file in the
 *  vault; do not read it out at a terminal you would not leave on screen.
 * ===========================================================================*/

#define VAULT_DIR_DEFAULT  "vault"        /* relative to the owner's home */
#define VAULT_ARCHIVE      ".vault.bv"
#define VAULT_CONF         "/etc/vault.conf"
#define VAULT_SALT_LEN     16
#define VAULT_KEY_LEN      32
#define VAULT_KDF_ROUNDS   16384          /* a vault is unlocked far less often
                                           * than a login happens, so it can
                                           * afford four times the work */
#define VAULT_MAX_BYTES    (8u << 20)     /* plaintext the archive may hold */

typedef struct {
    int      exists;                      /* a vault is configured for this user */
    int      unlocked;
    int      files;                       /* entries inside, when unlocked      */
    uint64_t bytes;                       /* plaintext bytes, when unlocked     */
    uint64_t archive_bytes;               /* sealed size, when locked           */
    char     dir[64];                     /* absolute path of the directory     */
} vault_status;

/* All of these act for the current user. 0 on success. */
int  vault_create(const char *password);  /* make a vault, left unlocked  */
int  vault_unlock(const char *password);  /* open the archive into the directory */
int  vault_lock(void);                    /* seal it; needs the key from unlock  */
int  vault_destroy(const char *password);  /* unlock, then forget it is a vault  */
void vault_get_status(vault_status *out);
const char *vault_error(void);            /* why the last call failed */

/* Seal every unlocked vault because the session is going away: the screen
 * locked, the user logged out, or the machine is shutting down. Silent, and
 * safe to call when there is nothing to do. */
void vault_lock_all(void);
