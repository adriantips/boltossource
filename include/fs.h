#pragma once
#include <stdint.h>

/* In-RAM filesystem: a tree of directories and byte-buffer files living in the
 * kernel heap. No backing store - state is lost on reboot (see `snapshot`). */

#define FS_NAME_MAX   32
#define FS_PATH_MAX   256
#define FS_TRASH_MAX  120
#define FS_MAX_DEPTH  32

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
    char            tpath[FS_TRASH_MAX]; /* original abs path while trashed */
} fs_node;

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
 *  Persistence: back the tree with a real ATA disk (SSD/HDD). fs_persist_init
 *  attaches the data disk, loads a saved image if one is present (else formats
 *  the current seeded tree onto it), and enables autosave so every mutation is
 *  flushed. Without a data disk the FS stays RAM-only and these are no-ops.
 * --------------------------------------------------------------------------*/
void     fs_persist_init(void);              /* call once, after ata_init() */
int      fs_sync(void);                      /* flush whole tree; 0 ok, -1 err */
int      fs_persist_active(void);            /* 1 if backed by a real disk */
const char *fs_persist_media(void);          /* "SSD"/"HDD"/"" */
const char *fs_persist_model(void);          /* disk model, or "" */
