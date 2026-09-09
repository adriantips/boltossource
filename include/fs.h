#pragma once
#include <stdint.h>

/* Filesystem tree in the kernel heap, backed by the BoltFS persistence layer
 * below when eligible storage is attached. Otherwise changes remain in RAM. */

#define FS_NAME_MAX   32
#define FS_PATH_MAX   256
#define FS_TRASH_MAX  120
#define FS_MAX_DEPTH  32

/* Permission bits, POSIX layout so the numbers people already know work:
 *   0400 owner read   0200 owner write   0100 owner execute
 *   0040 group read   0020 group write   0010 group execute
 *   0004 other read   0002 other write   0001 other execute
 * Defaults are 0644 for files and 0755 for directories. */
#define FS_MODE_MASK   0777
#define FS_MODE_FILE   0644
#define FS_MODE_DIR    0755
#define FS_PERM_READ   4
#define FS_PERM_WRITE  2
#define FS_PERM_EXEC   1

typedef struct fs_node {
    char            name[FS_NAME_MAX];
    int             is_dir;
    struct fs_node *parent;
    struct fs_node *child;          /* first child  (directories) */
    struct fs_node *next;           /* next sibling */
    uint8_t        *data;           /* file contents (files) */
    uint32_t        size;
    uint32_t        cap;
    uint64_t        mtime;          /* pit ticks at last write */
    uint16_t        mode;           /* permission bits (see FS_MODE_*)        */
    uint16_t        uid;            /* owner                                   */
    uint16_t        gid;            /* owning group                            */
    char            tpath[FS_TRASH_MAX]; /* original abs path while trashed */
} fs_node;

/* ---- ownership and permissions ------------------------------------------ *
 *  Enforcement is centralised in fs_may(): the shell, the VFS and the GUI all
 *  ask the same question, so a path cannot be reached through one of them that
 *  another refuses. The current identity comes from users_current_uid(), which
 *  sudo temporarily reports as 0.
 * ------------------------------------------------------------------------- */
int      fs_may(fs_node *n, int want);          /* want = FS_PERM_* bits; 1 = allowed */
int      fs_may_path(const char *path, int want);
int      fs_may_create_in(fs_node *dir);        /* write permission on a directory   */
void     fs_chown_node(fs_node *n, int uid, int gid, int mode);
int      fs_chmod(const char *path, int mode);
int      fs_chown(const char *path, int uid, int gid);
void     fs_mode_string(fs_node *n, char *out);  /* "-rw-r--r--"; out >= 11 */

void     fs_init(void);
fs_node *fs_root(void);
fs_node *fs_cwd(void);
void     fs_set_cwd(fs_node *d);
int      fs_chdir(const char *path);

fs_node *fs_lookup(const char *path);                 /* existing node, or 0 */
fs_node *fs_child(fs_node *dir, const char *name);
fs_node *fs_create(const char *path, int is_dir);     /* create leaf; 0 on fail */
fs_node *fs_parent_of(const char *path, char *leaf_out, uint32_t leaf_cap);
int      fs_remove_node(fs_node *n);                  /* recursive free + unlink */
int      fs_remove(const char *path);
int      fs_write(fs_node *n, const void *data, uint32_t len);
int      fs_append(fs_node *n, const void *data, uint32_t len);
int      fs_move(fs_node *n, fs_node *newparent, const char *newname);
void     fs_abspath(fs_node *n, char *out, uint32_t cap);

uint64_t fs_total_bytes(void);
int      fs_count_nodes(void);

/* --------------------------------------------------------------------------
 *  Persistence: back the tree with a real disk (ATA / AHCI / NVMe).
 *  fs_persist_init selects existing BoltFS storage, loads its saved image,
 *  and enables autosave. Only a completely zero-filled scratch disk of at most
 *  64 MiB may be initialised automatically. Failed loads never trigger format;
 *  the disk is protected read-only and the seed tree remains live in RAM.
 *  Autosave commits each mutation. Without a data disk the FS stays RAM-only.
 *
 *  Commits are atomic: the partition holds two blob slots plus a mirrored
 *  superblock, and a commit writes the inactive slot, flushes the device cache,
 *  then swaps the superblock generation. A power cut therefore always leaves a
 *  mountable image -- either the previous one or the new one, never a torn mix.
 *  See the commentary at the top of fs/ramfs.c's persistence section.
 * --------------------------------------------------------------------------*/
void     fs_persist_init(void);              /* call once, after the disk probes */
int      fs_sync(void);                      /* commit the tree; 0 ok, -1 err  */
int      fs_shutdown(void);                  /* commit and mark the image clean */
int      fs_persist_active(void);            /* 1 if backed by a real disk */
const char *fs_persist_media(void);          /* "NVMe"/"SSD"/"HDD"/"" */
const char *fs_persist_model(void);          /* disk model, or "" */
int      fs_was_unclean(void);               /* 1 if this boot followed a crash */
uint64_t fs_generation(void);                /* live commit generation */
void     fs_persist_set_readonly(int ro);    /* recovery mode: never write */
void     fs_persist_detach(void);            /* stop persisting; tree stays in RAM */
int      fs_persist_readonly(void);

/* --------------------------------------------------------------------------
 *  fsck. Re-reads the on-disk image and checks every invariant the loader
 *  relies on: both superblocks, the blob checksum, and the record chain
 *  (parents precede children, sizes stay inside the blob, names terminate).
 *  Runs automatically at mount after an unclean shutdown; `fsck` in the shell
 *  runs it on demand, with -r to repair.
 *
 *  Returns 0 = clean, 1 = issues found (see the report), -1 = unreadable.
 *  `repair` re-commits the in-RAM tree, which restores both superblocks and
 *  clears the dirty flag.
 * --------------------------------------------------------------------------*/
#define FS_CHECK_MAX_ISSUES 8
#define FS_CHECK_ISSUE_LEN  72

typedef struct {
    int      sb_ok;          /* how many of the two superblocks validated  */
    int      checksum_ok;    /* the live blob matched its checksum         */
    int      structure_ok;   /* the record chain walked cleanly            */
    int      clean;          /* the image was marked cleanly shut down     */
    int      repaired;       /* a repair commit was written                */
    int      dirs, files;
    uint64_t bytes;
    uint64_t generation;
    uint64_t mounts;
    uint32_t slot;
    uint32_t node_count;
    uint64_t blob_bytes;
    int      nissue;         /* may exceed FS_CHECK_MAX_ISSUES             */
    char     issue[FS_CHECK_MAX_ISSUES][FS_CHECK_ISSUE_LEN];
} fs_check_report;

int      fs_check(int repair, fs_check_report *rep);

/* Write the live tree as a complete, clean image onto another disk/partition,
 * leaving this system's own mount alone. `base`/`cap` are the target partition
 * in sectors. Used by the installer to seed the machine being installed to. */
struct blkdev;
struct blkdev *fs_persist_disk(void);        /* actual attachment, including read-only */
int      fs_export_image(struct blkdev *dev, uint64_t base, uint64_t cap);
