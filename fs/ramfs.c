#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "kheap.h"
#include "string.h"
#include "pit.h"
#include "blk.h"
#include "boot.h"
#include "users.h"
#include "kprintf.h"
#include "notify.h"

static fs_node *root_;
static fs_node *cwd_;

/* persistence state (see bottom of file) */
static blkdev_t *g_disk;       /* backing disk, or 0 for RAM-only */
static uint64_t g_base;        /* first LBA of the BoltFS partition (0 = whole disk) */
static uint64_t g_cap;         /* usable sectors from g_base (partition length) */
static int      g_autosave;    /* flush on every mutation once mounted */
static void     fs_autosave(void);

/* --------------------------------------------------------------------------
 *  node allocation
 * --------------------------------------------------------------------------*/
static fs_node *node_new(const char *name, int is_dir, fs_node *parent) {
    fs_node *n = (fs_node *)kmalloc(sizeof(fs_node));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_NAME_MAX);
    n->is_dir = is_dir;
    n->parent = parent;
    n->mtime  = pit_ticks();
    /* A new node belongs to whoever made it, with the conventional default
     * mode. Nodes restored from disk have theirs overwritten by the loader. */
    n->mode = (uint16_t)(is_dir ? FS_MODE_DIR : FS_MODE_FILE);
    int uid = users_current_uid();
    int gid = users_current_gid();
    n->uid  = (uint16_t)(uid < 0 ? 0 : uid);
    n->gid  = (uint16_t)(gid < 0 ? 0 : gid);
    if (parent) {                               /* append to sibling list */
        if (!parent->child) parent->child = n;
        else {
            fs_node *c = parent->child;
            while (c->next) c = c->next;
            c->next = n;
        }
    }
    return n;
}

void fs_init(void) {
    /* Through node_new, so the root is built the same way as every other
     * directory: it used to be assembled by hand and came out with no mode,
     * owner or mtime at all, which fs_may() then had to read as a node from
     * an image predating permissions. */
    root_ = node_new("/", 1, 0);
    if (!root_) panic("fs_init: no memory for the filesystem root");
    cwd_ = root_;

    /* seed a small tree so the file commands have something to show */
    fs_create("/etc", 1);
    fs_create("/home", 1);
    fs_create("/tmp", 1);
    fs_node *f;
    f = fs_create("/etc/motd", 0);
    if (f) fs_write(f, "Welcome to BoltOS.\nType 'help' for commands.\n", 45);
    f = fs_create("/etc/version", 0);
    if (f) fs_write(f, "BoltOS 0.2 (64-bit long mode)\n", 30);
    f = fs_create("/home/readme.txt", 0);
    if (f) fs_write(f,
        "BoltOS in-RAM filesystem.\n"
        "Files live in the kernel heap and reset on reboot.\n"
        "Create one with:  write notes.txt hello world\n", 122);

    /* A JavaScript demo page: open it in the Browser (address: /home/jsdemo.html)
     * to see the BoltJS engine read and rewrite the DOM. */
    f = fs_create("/home/jsdemo.html", 0);
    if (f) {
        const char *js =
            "<html><head><title>JS Demo</title></head><body>"
            "<h1>BoltJS DOM Demo</h1>"
            "<p id=\"out\">placeholder text (should be replaced)</p>"
            "<p id=\"list\">loop result here</p>"
            "<div id=\"calc\">calc result here</div>"
            "<script>"
            "document.getElementById('out').innerHTML = 'This text was set by JavaScript!';"
            "var s=''; for(var i=1;i<=5;i++){ s += i*i + ' '; }"
            "document.getElementById('list').textContent = 'Squares 1..5: ' + s;"
            "function fact(n){ return n<=1 ? 1 : n*fact(n-1); }"
            "document.getElementById('calc').innerHTML = '7! = ' + fact(7) + ', and 10! = ' + fact(10);"
            "document.title = 'BoltJS ran on this page';"
            "document.write('<p>This paragraph was generated at runtime by document.write().</p>');"
            "console.log('script finished; fact(6)=' + fact(6));"
            "</script>"
            "</body></html>";
        fs_write(f, js, (uint32_t)strlen(js));
    }

    /* BoltJS async demo: Promises + JSON.parse + Promise.all + new Promise.
     * Run it from the shell with:  js /home/promise.js
     * (timers/fetch need the browser's pump; this file is pump-free on purpose
     * so it verifies the microtask queue deterministically.) */
    f = fs_create("/home/promise.js", 0);
    if (f) {
        const char *js =
            "console.log('sync-start');\n"
            "Promise.resolve(10).then(function(v){ console.log('then1='+v); return v+5; })"
            "                   .then(function(v){ console.log('then2='+v); });\n"
            "Promise.reject('boom').catch(function(e){ console.log('caught='+e); });\n"
            "var d=JSON.parse('{\"n\":42,\"s\":\"hi\",\"a\":[1,2,3],\"b\":true}');\n"
            "console.log('json='+d.n+','+d.s+','+d.a[1]+','+d.b);\n"
            "Promise.all([Promise.resolve(1),Promise.resolve(2),3])"
            "       .then(function(a){ console.log('all='+a[0]+a[1]+a[2]); });\n"
            "new Promise(function(res,rej){ res('made'); }).then(function(v){ console.log('ctor='+v); });\n"
            "console.log('sync-end');\n";
        fs_write(f, js, (uint32_t)strlen(js));
    }

    /* fetch() over the real network:  js /home/fetch.js  */
    f = fs_create("/home/fetch.js", 0);
    if (f) {
        const char *js =
            "console.log('fetch-start');\n"
            "fetch('http://example.com/')"
            "  .then(function(r){ console.log('status='+r.status+' ok='+r.ok); return r.text(); })"
            "  .then(function(t){ console.log('bodylen='+t.length); });\n"
            "console.log('fetch-issued');\n";
        fs_write(f, js, (uint32_t)strlen(js));
    }

    /* An example project, so that the Code app's tree and the `build` command
     * have something real on a machine nobody has written anything on yet. It
     * is deliberately the smallest shape that is still a project: a header, two
     * sources that need each other, and the build file naming them.
     *
     *     build /home/Documents/hello        from the shell
     *     Code -> hello -> main.c -> Run     from the desktop
     */
    fs_create("/home/Documents", 1);
    fs_create("/home/Documents/hello", 1);
    f = fs_create("/home/Documents/hello/greet.h", 0);
    if (f) {
        const char *s =
            "#ifndef GREET_H\n"
            "#define GREET_H\n"
            "int greet(string who);\n"
            "#endif\n";
        fs_write(f, s, (uint32_t)strlen(s));
    }
    f = fs_create("/home/Documents/hello/greet.c", 0);
    if (f) {
        const char *s =
            "#include \"greet.h\"\n"
            "\n"
            "int greet(string who) {\n"
            "    printf(\"Hello, %s!\\n\", who);\n"
            "    return 0;\n"
            "}\n";
        fs_write(f, s, (uint32_t)strlen(s));
    }
    f = fs_create("/home/Documents/hello/main.c", 0);
    if (f) {
        const char *s =
            "#include \"greet.h\"\n"
            "\n"
            "int main() {\n"
            "    greet(\"BoltOS\");\n"
            "    for (int i = 1; i <= 3; i++)\n"
            "        printf(\"  %d squared is %d\\n\", i, i * i);\n"
            "    return 0;\n"
            "}\n";
        fs_write(f, s, (uint32_t)strlen(s));
    }
    f = fs_create("/home/Documents/hello/build.txt", 0);
    if (f) {
        const char *s =
            "# What `build` and the Code app's Run button compile here.\n"
            "target hello\n"
            "  src main.c greet.c\n";
        fs_write(f, s, (uint32_t)strlen(s));
    }
}

/* ---------------------------------------------------------------------------
 *  Permissions. One check, used by the shell, the VFS and the GUI alike.
 *
 *  root (uid 0, which is also what sudo reports) bypasses the check, as it does
 *  everywhere. Otherwise owner bits apply to the owner, group bits to members of
 *  the owning group, and other bits to everyone else -- the standard rule,
 *  because a surprising permission model is a broken one.
 *
 *  Nodes loaded from an image written before permissions existed have mode 0.
 *  Those are treated as unowned and allowed, so upgrading an install does not
 *  lock the user out of their own files; the first chmod gives them real bits.
 * ------------------------------------------------------------------------- */
int fs_may(fs_node *n, int want) {
    if (!n) return 0;
    if (n->mode == 0) return 1;                  /* pre-permissions node */
    int uid = users_current_uid();
    if (uid <= 0) return 1;                      /* root, or nobody logged in yet */
    int bits;
    if      ((int)n->uid == uid)                 bits = (n->mode >> 6) & 7;
    else if ((int)n->gid == users_current_gid()) bits = (n->mode >> 3) & 7;
    else                                         bits =  n->mode       & 7;
    return (bits & want) == want;
}

int fs_may_path(const char *path, int want) {
    fs_node *n = fs_lookup(path);
    return n ? fs_may(n, want) : 0;
}

int fs_may_create_in(fs_node *dir) {
    return dir && dir->is_dir && fs_may(dir, FS_PERM_WRITE);
}

void fs_chown_node(fs_node *n, int uid, int gid, int mode) {
    if (!n) return;
    if (uid >= 0)  n->uid  = (uint16_t)uid;
    if (gid >= 0)  n->gid  = (uint16_t)gid;
    if (mode >= 0) n->mode = (uint16_t)(mode & FS_MODE_MASK);
    fs_autosave();
}

int fs_chmod(const char *path, int mode) {
    fs_node *n = fs_lookup(path);
    if (!n) return -1;
    int uid = users_current_uid();
    if (uid > 0 && (int)n->uid != uid) return -2;   /* only the owner, or root */
    n->mode = (uint16_t)(mode & FS_MODE_MASK);
    fs_autosave();
    return 0;
}

int fs_chown(const char *path, int uid, int gid) {
    fs_node *n = fs_lookup(path);
    if (!n) return -1;
    if (users_current_uid() > 0) return -2;         /* giving a file away needs root */
    if (uid >= 0) n->uid = (uint16_t)uid;
    if (gid >= 0) n->gid = (uint16_t)gid;
    fs_autosave();
    return 0;
}

void fs_mode_string(fs_node *n, char *out) {
    int m = n ? n->mode : 0;
    if (!m) m = (n && n->is_dir) ? FS_MODE_DIR : FS_MODE_FILE;
    out[0] = (n && n->is_dir) ? 'd' : '-';
    static const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    out[10] = 0;
}

fs_node *fs_root(void) { return root_; }
fs_node *fs_cwd(void)  { return cwd_; }
void     fs_set_cwd(fs_node *d) { if (d && d->is_dir) cwd_ = d; }

int fs_chdir(const char *path) {
    fs_node *n = fs_lookup(path && *path ? path : "/");
    if (!n || !n->is_dir) return -1;
    cwd_ = n;
    return 0;
}

/* --------------------------------------------------------------------------
 *  lookup / path resolution
 * --------------------------------------------------------------------------*/
fs_node *fs_child(fs_node *dir, const char *name) {
    if (!dir || !dir->is_dir) return 0;
    for (fs_node *c = dir->child; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return 0;
}

fs_node *fs_lookup(const char *path) {
    if (!path || !*path) return cwd_;
    char buf[FS_PATH_MAX];
    strncpy(buf, path, sizeof(buf));

    fs_node *cur = (buf[0] == '/') ? root_ : cwd_;
    char *p = buf;
    while (*p == '/') p++;

    while (*p) {
        char *seg = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        while (*p == '/') p++;

        if (seg[0] == 0 || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        if (!cur->is_dir) return 0;
        cur = fs_child(cur, seg);
        if (!cur) return 0;
    }
    return cur;
}

/* Resolve everything up to the last component; return the parent dir and copy
 * the final name into leaf_out. */
fs_node *fs_parent_of(const char *path, char *leaf_out, uint32_t leaf_cap) {
    char buf[FS_PATH_MAX];
    if (!path || !leaf_cap || strlen(path) >= sizeof buf) return 0;
    strncpy(buf, path, sizeof(buf));
    /* strip a single trailing slash */
    uint32_t L = (uint32_t)strlen(buf);
    if (L > 1 && buf[L - 1] == '/') buf[L - 1] = 0;

    char *slash = strrchr(buf, '/');
    if (strlen(slash ? slash + 1 : buf) >= leaf_cap) return 0;
    if (!slash) {                               /* bare name -> cwd */
        strncpy(leaf_out, buf, leaf_cap);
        return cwd_;
    }
    strncpy(leaf_out, slash + 1, leaf_cap);
    if (slash == buf) return root_;             /* "/leaf" */
    *slash = 0;
    return fs_lookup(buf);
}

static int subtree_fits(fs_node *n, unsigned depth, unsigned pathlen) {
    if (depth > FS_MAX_DEPTH || pathlen >= FS_PATH_MAX) return 0;
    if (n)
        for (fs_node *c = n->child; c; c = c->next)
            if (!subtree_fits(c, depth + 1, pathlen + 1 + (unsigned)strlen(c->name))) return 0;
    return 1;
}

/* Mutations must respect the limits the disk parser enforces. Otherwise a
 * perfectly successful mkdir/move could create an image we cannot load back. */
static int location_fits(fs_node *tree, fs_node *parent, const char *name) {
    if (!name || !name[0] || strlen(name) >= FS_NAME_MAX || strchr(name, '/') ||
        !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    unsigned depth = 1, pathlen = 1 + (unsigned)strlen(name);
    for (fs_node *p = parent; p && p->parent; p = p->parent) {
        depth++;
        pathlen += 1 + (unsigned)strlen(p->name);
    }
    return subtree_fits(tree, depth, pathlen);
}

fs_node *fs_create(const char *path, int is_dir) {
    char leaf[FS_NAME_MAX];
    fs_node *parent = fs_parent_of(path, leaf, sizeof(leaf));
    if (!parent || !parent->is_dir) return 0;
    if (!location_fits(0, parent, leaf)) return 0;
    if (fs_child(parent, leaf)) return 0;       /* already exists */
    fs_node *n = node_new(leaf, is_dir, parent);
    if (n) fs_autosave();
    return n;
}

/* --------------------------------------------------------------------------
 *  remove
 * --------------------------------------------------------------------------*/
static void node_free_tree(fs_node *n) {
    fs_node *c = n->child;
    while (c) { fs_node *nx = c->next; node_free_tree(c); c = nx; }
    if (n->data) kfree(n->data);
    kfree(n);
}

int fs_remove_node(fs_node *n) {
    if (!n || n == root_) return -1;
    fs_node *parent = n->parent;
    if (parent) {                               /* unlink from sibling list */
        if (parent->child == n) parent->child = n->next;
        else {
            fs_node *c = parent->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    if (cwd_ == n) cwd_ = parent ? parent : root_;
    node_free_tree(n);
    fs_autosave();
    return 0;
}

int fs_remove(const char *path) {
    return fs_remove_node(fs_lookup(path));
}

/* --------------------------------------------------------------------------
 *  read/write
 * --------------------------------------------------------------------------*/
int fs_write(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    if (n->cap < len || !n->data) {
        uint8_t *nd = (uint8_t *)kmalloc(len ? len : 1);
        if (!nd) return -1;
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = len ? len : 1;
    }
    if (len) memcpy(n->data, data, len);
    n->size  = len;
    n->mtime = pit_ticks();
    fs_autosave();
    return 0;
}

int fs_append(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    uint32_t need = n->size + len;
    if (n->cap < need) {
        uint8_t *nd = (uint8_t *)kmalloc(need ? need : 1);
        if (!nd) return -1;
        if (n->size && n->data) memcpy(nd, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = need;
    }
    if (len) memcpy(n->data + n->size, data, len);
    n->size  = need;
    n->mtime = pit_ticks();
    fs_autosave();
    return 0;
}

/* --------------------------------------------------------------------------
 *  move / rename
 * --------------------------------------------------------------------------*/
static int is_ancestor(fs_node *anc, fs_node *n) {
    for (fs_node *p = n; p; p = p->parent) if (p == anc) return 1;
    return 0;
}

int fs_move(fs_node *n, fs_node *newparent, const char *newname) {
    if (!n || n == root_ || !newparent || !newparent->is_dir) return -1;
    if (is_ancestor(n, newparent)) return -1;   /* no moving into own subtree */
    if (!location_fits(n, newparent, newname)) return -1;
    if (fs_child(newparent, newname)) return -1;

    fs_node *op = n->parent;                     /* unlink from old parent */
    if (op) {
        if (op->child == n) op->child = n->next;
        else {
            fs_node *c = op->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    n->next = 0;
    n->parent = newparent;
    strncpy(n->name, newname, FS_NAME_MAX);
    if (!newparent->child) newparent->child = n;
    else {
        fs_node *c = newparent->child;
        while (c->next) c = c->next;
        c->next = n;
    }
    fs_autosave();
    return 0;
}

/* --------------------------------------------------------------------------
 *  helpers
 * --------------------------------------------------------------------------*/
void fs_abspath(fs_node *n, char *out, uint32_t cap) {
    if (!n) { strncpy(out, "?", cap); return; }
    if (n == root_) { strncpy(out, "/", cap); return; }

    fs_node *stack[FS_MAX_DEPTH];
    int depth = 0;
    for (fs_node *p = n; p && p != root_ && depth < FS_MAX_DEPTH; p = p->parent)
        stack[depth++] = p;

    uint32_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 < cap) out[pos++] = '/';
        const char *s = stack[i]->name;
        while (*s && pos + 1 < cap) out[pos++] = *s++;
    }
    out[pos < cap ? pos : cap - 1] = 0;
}

static uint64_t bytes_rec(fs_node *n) {
    uint64_t t = n->is_dir ? 0 : n->size;
    for (fs_node *c = n->child; c; c = c->next) t += bytes_rec(c);
    return t;
}
uint64_t fs_total_bytes(void) { return root_ ? bytes_rec(root_) : 0; }

static int count_rec(fs_node *n) {
    int t = 1;
    for (fs_node *c = n->child; c; c = c->next) t += count_rec(c);
    return t;
}
int fs_count_nodes(void) { return root_ ? count_rec(root_) - 1 : 0; }

/* ==========================================================================
 *  Persistence  -  BoltFS on-disk image (serialised tree on a real disk).
 *
 *  The tree is small enough to serialise whole, so BoltFS commits by rewriting
 *  the image rather than by patching it. What it does NOT do any more is
 *  rewrite it in place: that made every single mutation a window in which a
 *  power cut left a half-written image and destroyed the entire filesystem.
 *
 *  Instead the partition holds TWO blob slots and a mirrored superblock, and a
 *  commit is a shadow-page swap:
 *
 *      base+0   superblock A          (primary)
 *      base+1   superblock B          (mirror)
 *      base+2   slot 0                (half the remaining sectors)
 *      base+2+H slot 1                (the other half)
 *
 *    1. serialise the tree into the slot that is NOT live
 *    2. write it, then FLUSH the device cache
 *    3. write superblock A with generation+1 naming the new slot, FLUSH
 *    4. write superblock B identically, FLUSH
 *
 *  Whatever instant the power fails, a mount finds a consistent tree: before
 *  step 3 the live superblock still names the old, untouched slot; after it,
 *  the new slot is complete because step 2's flush already returned. A torn
 *  superblock A fails its checksum and the mount falls back to B. This gives
 *  the atomicity a write-ahead log would, without a log -- the whole-image
 *  rewrite already IS the redo record.
 *
 *  The superblock also carries a `clean` flag, cleared on mount and set only by
 *  fs_shutdown(). Finding it clear at mount means the last run ended badly, and
 *  the mount runs a consistency check before trusting the image.
 * ==========================================================================*/
#define BFS_MAGIC1  "BOLTFS1"          /* v1: single in-place image (legacy) */
#define BFS_MAGIC2  "BOLTFS2"          /* v2: A/B slots + mirrored superblock */
#define BFS_NOPAR   0xFFFFFFFFu
#define BFS_SB_A    0u                 /* sector offsets within the partition */
#define BFS_SB_B    1u
#define BFS_SLOT0   2u

/* v1 superblock, still parsed so an image written by an older build is
 * migrated rather than reformatted out from under the user. */
struct bfs_super1 {
    char     magic[8];
    uint32_t version;
    uint32_t node_count;
    uint64_t blob_bytes;
    uint32_t checksum;                 /* sum32 over the blob */
    uint32_t reserved;
};

struct bfs_super {
    char     magic[8];                 /* BFS_MAGIC2                          */
    uint32_t version;                  /* 2                                   */
    uint32_t slot;                     /* which blob slot this generation uses */
    uint64_t generation;               /* newest valid superblock wins         */
    uint32_t node_count;
    uint32_t clean;                    /* 1 = fs_shutdown ran; 0 = still live  */
    uint64_t blob_bytes;
    uint32_t blob_sum;                 /* FNV-1a over the blob                 */
    uint32_t slot_sectors;             /* sectors per slot at write time       */
    uint64_t mounts;                   /* lifetime mount count (diagnostics)   */
    uint32_t hdr_sum;                  /* FNV-1a over the bytes before it      */
    uint32_t reserved;
};

/* `version` doubles as the record layout: 2 = bfs_rec2 (no ownership),
 * 3 = bfs_rec (with mode/uid/gid). Commits always write 3. */

/* v2 record, still read so an image written before ownership existed keeps its
 * files. 56 bytes; `pad` was always zero. */
struct bfs_rec2 {
    uint32_t parent;
    uint32_t is_dir;
    uint32_t size;
    uint32_t pad;
    uint64_t mtime;
    char     name[FS_NAME_MAX];
};

/* v3 record: the same, plus the owner and mode. 64 bytes. */
struct bfs_rec {
    uint32_t parent;                   /* index of parent record, BFS_NOPAR for root */
    uint32_t is_dir;
    uint32_t size;                     /* file data length (0 for dirs) */
    uint16_t mode;                     /* permission bits                    */
    uint16_t uid;
    uint16_t gid;
    uint16_t reserved;
    uint64_t mtime;
    char     name[FS_NAME_MAX];
};

#define BFS_VERSION 3

static uint64_t g_slot_sectors;        /* usable sectors in each blob slot */
static uint32_t g_slot;                /* live slot (0/1) */
static uint64_t g_gen;                 /* live generation */
static uint64_t g_mounts;
static int      g_readonly;            /* recovery mode: never write */
static int      g_unclean;             /* mounted after an unclean shutdown */
static const char bfs_oom[] = "out of memory while loading filesystem";

/* FNV-1a/32. Stronger than the old byte sum, which could not tell a torn write
 * (a run of stale sectors mixed into fresh ones) from the real thing. */
static uint32_t fnv32(const uint8_t *p, uint64_t n) {
    uint32_t h = 2166136261u;
    for (uint64_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint64_t slot_lba(uint32_t slot) {
    return g_base + BFS_SLOT0 + (uint64_t)slot * g_slot_sectors;
}

static void measure(fs_node *n, uint32_t *cnt, uint64_t *bytes) {
    (*cnt)++;
    *bytes += sizeof(struct bfs_rec) + (n->is_dir ? 0 : n->size);
    for (fs_node *c = n->child; c; c = c->next) measure(c, cnt, bytes);
}

static uint32_t g_emit_idx;
static void emit(fs_node *n, uint32_t parent_idx, uint8_t **pp) {
    uint32_t myidx = g_emit_idx++;
    struct bfs_rec r;
    memset(&r, 0, sizeof r);
    r.parent = parent_idx;
    r.is_dir = (uint32_t)n->is_dir;
    r.size   = n->is_dir ? 0 : n->size;
    r.mode   = n->mode;
    r.uid    = n->uid;
    r.gid    = n->gid;
    r.mtime  = n->mtime;
    strncpy(r.name, n->name, FS_NAME_MAX);
    memcpy(*pp, &r, sizeof r); *pp += sizeof r;
    if (!n->is_dir && n->size) { memcpy(*pp, n->data, n->size); *pp += n->size; }
    for (fs_node *c = n->child; c; c = c->next) emit(c, myidx, pp);
}

/* Serialise the whole tree into a freshly allocated, sector-padded buffer.
 * *sectors gets the sector count, *blob the exact byte length. NULL on error. */
static uint8_t *serialise(uint32_t *cnt_out, uint64_t *blob_out, uint64_t *sectors_out) {
    uint32_t cnt = 0; uint64_t blob = 0;
    measure(root_, &cnt, &blob);

    uint64_t sectors = (blob + BLK_SECTOR - 1) / BLK_SECTOR;
    if (sectors > g_slot_sectors) {
        kprintf("fs: image (%lu sectors) exceeds slot size (%lu)\n",
                (unsigned long)sectors, (unsigned long)g_slot_sectors);
        return 0;
    }
    uint8_t *buf = (uint8_t *)kmalloc(sectors ? sectors * BLK_SECTOR : BLK_SECTOR);
    if (!buf) { kprintf("[--] fs: out of memory; commit not written\n"); return 0; }
    memset(buf, 0, (sectors ? sectors : 1) * BLK_SECTOR);

    uint8_t *p = buf;
    g_emit_idx = 0;
    emit(root_, BFS_NOPAR, &p);

    *cnt_out = cnt; *blob_out = blob; *sectors_out = sectors;
    return buf;
}

static void sb_fill(struct bfs_super *sb, uint32_t slot, uint64_t gen,
                    uint32_t cnt, uint64_t blob, uint32_t blob_sum, int clean) {
    memset(sb, 0, sizeof *sb);
    memcpy(sb->magic, BFS_MAGIC2, 8);
    sb->version      = BFS_VERSION;
    sb->slot         = slot;
    sb->generation   = gen;
    sb->node_count   = cnt;
    sb->clean        = clean ? 1u : 0u;
    sb->blob_bytes   = blob;
    sb->blob_sum     = blob_sum;
    sb->slot_sectors = (uint32_t)g_slot_sectors;
    sb->mounts       = g_mounts;
    sb->hdr_sum      = fnv32((const uint8_t *)sb,
                             sizeof *sb - 2 * sizeof(uint32_t));
}

static int sb_write(uint64_t lba, const struct bfs_super *sb) {
    uint8_t sec[BLK_SECTOR];
    memset(sec, 0, sizeof sec);
    memcpy(sec, sb, sizeof *sb);
    return blk_write(g_disk, lba, 1, sec);
}

/* Read and validate one superblock. 0 on success. */
static int sb_read(uint64_t lba, struct bfs_super *out) {
    uint8_t sec[BLK_SECTOR];
    if (blk_read(g_disk, lba, 1, sec) != 0) return -1;
    struct bfs_super *sb = (struct bfs_super *)sec;
    if (memcmp(sb->magic, BFS_MAGIC2, 8) != 0) return -1;
    if (sb->version != 2 && sb->version != BFS_VERSION) return -1;
    if (fnv32(sec, sizeof *sb - 2 * sizeof(uint32_t)) != sb->hdr_sum) return -1;
    uint32_t recsz = sb->version == 2 ? sizeof(struct bfs_rec2) : sizeof(struct bfs_rec);
    if (sb->slot > 1 || sb->clean > 1 || sb->slot_sectors != g_slot_sectors ||
        sb->blob_bytes < recsz || sb->blob_bytes > g_slot_sectors * BLK_SECTOR ||
        !sb->node_count || sb->node_count > sb->blob_bytes / recsz) return -1;
    *out = *sb;
    return 0;
}

/* The commit. `clean` marks the image as cleanly shut down; every ordinary
 * autosave passes 0 so a crash is always detectable afterwards. */
static int fs_commit(int clean) {
    if (!g_disk) return -1;
    if (g_readonly) return -1;

    uint32_t cnt; uint64_t blob, sectors;
    uint8_t *buf = serialise(&cnt, &blob, &sectors);
    if (!buf) return -1;
    uint32_t sum = fnv32(buf, blob);

    uint32_t next = g_slot ^ 1u;
    int rc = blk_write(g_disk, slot_lba(next), (uint32_t)(sectors ? sectors : 1), buf);
    kfree(buf);
    if (rc != 0) goto io_fail;

    /* Barrier. Without it a caching disk may still be holding the blob when the
     * superblock naming it lands, and a power cut in between would leave a
     * superblock pointing at garbage -- the exact failure this design exists to
     * prevent. */
    if (blk_flush(g_disk) != 0) goto io_fail;

    struct bfs_super sb;
    sb_fill(&sb, next, g_gen + 1, cnt, blob, sum, clean);
    if (sb_write(g_base + BFS_SB_A, &sb) != 0) goto io_fail;
    if (blk_flush(g_disk) != 0) goto io_fail;
    /* B is written only after A is durable, so the two can never be torn in a
     * way that loses both. */
    if (sb_write(g_base + BFS_SB_B, &sb) != 0) goto io_fail;
    if (blk_flush(g_disk) != 0) goto io_fail;

    g_slot = next;
    g_gen++;
    return 0;

io_fail:
    /* The primary might now name the new slot even though its flush failed.
     * Retrying with stale g_slot would overwrite that live image. Fence all
     * later commits until a reboot resolves which generation is durable. */
    g_readonly = 1;
    g_autosave = 0;
    kprintf("[--] fs: write/flush failed; disk protected READ-ONLY until reboot.\n"
            "     Recent changes are only in RAM; save a copy before rebooting.\n");
    /* The worst thing this could do is happen quietly. Someone who is told
     * copies their work off; someone who is not keeps working and loses the
     * day at the next reboot. */
    notify_error("Storage", "Disk write failed. Changes are RAM-only -- copy "
                            "your work elsewhere before rebooting.");
    return -1;
}

int fs_sync(void)     { return fs_commit(0); }
int fs_shutdown(void) { return fs_commit(1); }

static void fs_autosave(void) { if (g_autosave) fs_commit(0); }

/* Build a detached tree. The mount and fsck share this parser, so fsck cannot
 * approve an image the loader would reject. No live nodes change on corruption
 * or allocation failure. fsck skips file payload allocations (copy_data = 0). */
static fs_node *decode_tree(const uint8_t *buf, uint64_t blob, uint32_t cnt,
                           uint32_t version, int copy_data, const char **error) {
    uint32_t recsz = version >= 3 ? sizeof(struct bfs_rec) : sizeof(struct bfs_rec2);
    *error = "invalid record count";
    if (!cnt || cnt > blob / recsz) return 0;
    fs_node **map = (fs_node **)kmalloc(cnt * sizeof(fs_node *));
    *error = bfs_oom;
    if (!map) return 0;
    fs_node *tree = 0;
    uint64_t off = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        *error = "record list runs past the end of the blob";
        if (recsz > blob - off) goto fail;
        struct bfs_rec r;
        memset(&r, 0, sizeof r);
        if (version >= 3) {
            memcpy(&r, buf + off, sizeof r);
        } else {
            /* v2: no ownership on disk. mode 0 means "unowned", which fs_may
             * treats as allowed, so an upgraded install keeps working until the
             * first chmod gives its files real bits. */
            struct bfs_rec2 o;
            memcpy(&o, buf + off, sizeof o);
            r.parent = o.parent; r.is_dir = o.is_dir; r.size = o.size;
            r.mtime = o.mtime;
            memcpy(r.name, o.name, FS_NAME_MAX);
        }
        off += recsz;
        *error = "invalid node type or directory size";
        if (r.is_dir > 1 || (r.is_dir && r.size)) goto fail;
        *error = "file data runs past the end of the blob";
        if (r.size > blob - off) goto fail;
        *error = "invalid or unterminated node name";
        if (!memchr(r.name, 0, FS_NAME_MAX) || !r.name[0]) goto fail;

        fs_node *parent = 0;
        if (!i) {
            *error = "invalid root record";
            if (r.parent != BFS_NOPAR || !r.is_dir || strcmp(r.name, "/")) goto fail;
        } else {
            *error = "parent is missing, not a directory, or follows its child";
            if (r.parent >= i || !map[r.parent]->is_dir) goto fail;
            parent = map[r.parent];
            *error = "invalid or duplicate child name";
            if (strchr(r.name, '/') || !strcmp(r.name, ".") || !strcmp(r.name, "..") ||
                fs_child(parent, r.name)) goto fail;
            *error = "tree depth or path length exceeds filesystem limits";
            if (!location_fits(0, parent, r.name)) goto fail;
        }
        *error = bfs_oom;
        fs_node *n = node_new(r.name, (int)r.is_dir, parent);
        if (!n) goto fail;
        if (!i) tree = n;
        n->mtime = r.mtime;
        n->mode = !i && !r.mode ? FS_MODE_DIR : r.mode;
        n->uid = r.uid; n->gid = r.gid;
        n->size = r.size;
        if (copy_data && r.size) {
            n->data = (uint8_t *)kmalloc(r.size);
            if (!n->data) goto fail;
            memcpy(n->data, buf + off, r.size);
            n->cap = r.size;
        }
        map[i] = n;
        off += r.size;
    }
    *error = "trailing bytes after the last record";
    if (off != blob) goto fail;
    kfree(map);
    *error = 0;
    return tree;
fail:
    if (tree) node_free_tree(tree);
    kfree(map);
    return 0;
}

static int rebuild(const uint8_t *buf, uint64_t blob, uint32_t cnt, uint32_t version) {
    const char *error;
    fs_node *tree = decode_tree(buf, blob, cnt, version, 1, &error);
    if (!tree) { kprintf("[--] fs: %s\n", error); return error == bfs_oom ? -2 : -1; }
    fs_node *old = root_;
    root_ = cwd_ = tree;
    node_free_tree(old);
    return 0;
}

/* Load the blob a validated superblock points at, checksum it, rebuild. */
static int load_slot(const struct bfs_super *sb) {
    uint64_t sectors = (sb->blob_bytes + BLK_SECTOR - 1) / BLK_SECTOR;
    if (sb->slot > 1 || sectors > g_slot_sectors) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(sectors * BLK_SECTOR);
    if (!buf) { kprintf("[--] fs: out of memory reading filesystem image\n"); return -2; }
    if (blk_read(g_disk, slot_lba(sb->slot), (uint32_t)sectors, buf) != 0) {
        kfree(buf); return -1;
    }
    if (fnv32(buf, sb->blob_bytes) != sb->blob_sum) { kfree(buf); return -1; }

    int rc = rebuild(buf, sb->blob_bytes, sb->node_count, sb->version);
    kfree(buf);
    return rc;
}

/* Legacy v1 image: superblock at base+0, blob immediately after it. Loaded once
 * so an existing install keeps its files; the next commit writes v2. */
static int load_v1(void) {
    uint8_t sec0[BLK_SECTOR];
    if (blk_read(g_disk, g_base, 1, sec0) != 0) return -1;
    struct bfs_super1 *sb = (struct bfs_super1 *)sec0;
    if (memcmp(sb->magic, BFS_MAGIC1, 8) != 0) return -1;
    if (sb->version != 1 || sb->node_count == 0 ||
        sb->blob_bytes < sizeof(struct bfs_rec2) ||
        sb->blob_bytes > (g_cap - 1) * BLK_SECTOR ||
        sb->node_count > sb->blob_bytes / sizeof(struct bfs_rec2)) return -1;

    uint64_t sectors = (sb->blob_bytes + BLK_SECTOR - 1) / BLK_SECTOR;
    if (1 + sectors > g_cap) return -1;
    uint8_t *buf = (uint8_t *)kmalloc(sectors * BLK_SECTOR);
    if (!buf) return -2;
    if (blk_read(g_disk, g_base + 1, (uint32_t)sectors, buf) != 0) { kfree(buf); return -1; }

    uint32_t sum = 0;                                /* v1 used a plain byte sum */
    for (uint64_t i = 0; i < sb->blob_bytes; i++) sum += buf[i];
    if (sum != sb->checksum) { kfree(buf); return -1; }

    /* A v1 image predates ownership entirely, so it uses the v2 record layout. */
    int rc = rebuild(buf, sb->blob_bytes, sb->node_count, 2);
    kfree(buf);
    /* Migration writes slot 1 first, away from the old v1 blob. If that blob
     * extends into slot 1 there is no safe in-place migration. */
    if (!rc && 1 + sectors > BFS_SLOT0 + g_slot_sectors) {
        g_readonly = 1;
        kprintf("[--] fs: v1 image too large for safe migration; keeping disk READ-ONLY\n");
    }
    return rc;
}

/* ==========================================================================
 *  fsck  -  validate the on-disk image without trusting any of it.
 *
 *  Runs automatically at mount after an unclean shutdown, and on demand from
 *  the `fsck` shell command. It re-reads both superblocks and walks the live
 *  blob record by record, checking exactly the invariants rebuild() relies on,
 *  so anything it passes is safe to mount.
 * ==========================================================================*/
static void chk_note(fs_check_report *r, const char *msg) {
    if (r->nissue < FS_CHECK_MAX_ISSUES) {
        strncpy(r->issue[r->nissue], msg, FS_CHECK_ISSUE_LEN - 1);
        r->issue[r->nissue][FS_CHECK_ISSUE_LEN - 1] = 0;
    }
    r->nissue++;
}

int fs_check(int repair, fs_check_report *rep) {
    fs_check_report local;
    if (!rep) rep = &local;
    memset(rep, 0, sizeof *rep);

    if (!g_disk) { chk_note(rep, "no persistent disk attached"); return -1; }

    struct bfs_super a, b;
    int oka = sb_read(g_base + BFS_SB_A, &a) == 0;
    int okb = sb_read(g_base + BFS_SB_B, &b) == 0;
    rep->sb_ok = (oka ? 1 : 0) + (okb ? 1 : 0);
    if (!oka) chk_note(rep, "primary superblock unreadable or corrupt");
    if (!okb) chk_note(rep, "mirror superblock unreadable or corrupt");
    if (!oka && !okb) { chk_note(rep, "no valid superblock: image unreadable"); return -1; }

    /* Prefer the newest valid generation, exactly as the mount path does. */
    const struct bfs_super *sb = oka ? &a : &b;
    if (oka && okb && b.generation > a.generation) sb = &b;
    if (oka && okb && a.generation != b.generation)
        chk_note(rep, "superblock copies disagree (interrupted commit)");

    rep->generation = sb->generation;
    rep->slot       = sb->slot;
    rep->clean      = (int)sb->clean;
    rep->node_count = sb->node_count;
    rep->blob_bytes = sb->blob_bytes;
    rep->mounts     = sb->mounts;

    uint64_t sectors = (sb->blob_bytes + BLK_SECTOR - 1) / BLK_SECTOR;
    if (sb->slot > 1 || sectors > g_slot_sectors) {
        chk_note(rep, "superblock names a slot outside the partition");
        return -1;
    }
    uint8_t *buf = (uint8_t *)kmalloc(sectors * BLK_SECTOR);
    if (!buf) { chk_note(rep, "out of memory"); return -1; }
    if (blk_read(g_disk, slot_lba(sb->slot), (uint32_t)sectors, buf) != 0) {
        chk_note(rep, "read error on the live slot");
        kfree(buf); return -1;
    }
    if (fnv32(buf, sb->blob_bytes) != sb->blob_sum) {
        chk_note(rep, "blob checksum mismatch (torn or damaged image)");
        kfree(buf); return -1;
    }
    rep->checksum_ok = 1;

    const char *error;
    fs_node *tree = decode_tree(buf, sb->blob_bytes, sb->node_count, sb->version, 0, &error);
    kfree(buf);
    if (!tree) { chk_note(rep, error); return -1; }
    /* Walk without recursion beyond the parser's enforced FS_MAX_DEPTH. */
    fs_node *n = tree;
    while (n) {
        if (n->is_dir) rep->dirs++;
        else { rep->files++; rep->bytes += n->size; }
        if (n->child) { n = n->child; continue; }
        while (n && !n->next) n = n->parent;
        if (n) n = n->next;
    }
    node_free_tree(tree);
    rep->structure_ok = 1;

    /* Repair here means: re-establish the invariants a healthy image has --
     * both superblocks valid, agreeing, and marked clean. The in-RAM tree is
     * authoritative at this point, so a fresh commit is the repair. */
    if (repair && !g_readonly && (rep->nissue > 0 || !sb->clean)) {
        if (fs_commit(1) == 0) rep->repaired = 1;
        else chk_note(rep, "repair commit failed");
    }
    return rep->nissue ? 1 : 0;
}

int fs_was_unclean(void) { return g_unclean; }
void fs_persist_set_readonly(int ro) { g_readonly = ro ? 1 : 0; }
int  fs_persist_readonly(void) { return g_readonly; }
uint64_t fs_generation(void) { return g_gen; }

/* A selected disk is either an existing BoltFS image or a verified-empty
 * scratch disk. `fresh` is explicit: a failed load must NEVER imply format. */
static int fs_attach_partition(int *fresh) {
    blk_part part;
    *fresh = 0;
    if (blk_boltfs_partition(g_disk, &part)) {
        g_base = part.lba_start;
        g_cap = part.sectors;
    } else if (blk_is_blank_scratch(g_disk)) {
        if (g_readonly) return -1;
        g_base = 2048;
        g_cap = g_disk->sectors - g_base;
        if (blk_write_mbr_single(g_disk, BLK_PART_BOLTFS, g_base) != 0 ||
            blk_flush(g_disk) != 0) return -1;
        *fresh = 1;
        kprintf("[ok] fs: wrote MBR, BoltFS partition @ LBA %lu\n", (unsigned long)g_base);
    } else {
        /* Legacy raw BoltFS images have no partition table. Their headers are
         * validated below; nothing may be written unless a complete tree loads. */
        g_base = 0;
        g_cap = g_disk->sectors;
    }
    /* Two superblock sectors, then the partition split evenly into two slots. */
    g_slot_sectors = g_cap > BFS_SLOT0 ? (g_cap - BFS_SLOT0) / 2 : 0;
    return 0;
}

void fs_persist_init(void) {
    g_disk = blk_fs_disk();
    if (!g_disk) {
        kprintf("[--] fs: no eligible data disk found; RAM-only (volatile). Use `install` for a new disk.\n");
        return;
    }
    if (boot_is_recovery()) {
        g_readonly = 1;
        kprintf("[--] fs: recovery mode; the image will not be written\n");
    }

    int fresh;
    if (fs_attach_partition(&fresh) != 0 || g_slot_sectors == 0) {
        kprintf("[--] fs: cannot attach writable storage; RAM-only\n");
        g_disk = 0;
        return;
    }

    const char *media = blk_media(g_disk);
    const char *model = g_disk->model[0] ? g_disk->model : "disk";

    /* Newest valid superblock first, then its mirror, then a legacy v1 image. */
    struct bfs_super a, b;
    int oka = sb_read(g_base + BFS_SB_A, &a) == 0;
    int okb = sb_read(g_base + BFS_SB_B, &b) == 0;
    const struct bfs_super *cand[2] = { 0, 0 };
    if (oka && okb) {
        cand[0] = (a.generation >= b.generation) ? &a : &b;
        cand[1] = (cand[0] == &a) ? &b : &a;
    } else if (oka) cand[0] = &a;
    else if (okb)   cand[0] = &b;

    int loaded = 0, oom = 0;
    for (int i = 0; i < 2 && !loaded; i++) {
        if (!cand[i]) continue;
        int rc = load_slot(cand[i]);
        if (rc == -2) { oom = 1; break; }
        if (rc != 0) {
            if (i == 0 && cand[1])
                kprintf("[--] fs: newest image (gen %lu) is damaged; trying the mirror\n",
                        (unsigned long)cand[i]->generation);
            continue;
        }
        g_slot    = cand[i]->slot;
        g_gen     = cand[i]->generation;
        g_mounts  = cand[i]->mounts;
        g_unclean = !cand[i]->clean;
        loaded    = 1;
        kprintf("[ok] fs: loaded image from %s '%s' part@LBA%lu (gen %lu, slot %u, %d nodes)\n",
                media, model, (unsigned long)g_base,
                (unsigned long)g_gen, cand[i]->slot, fs_count_nodes());
    }

    int migrated = 0;
    if (!loaded && !oom && !fresh) {
        int rc = load_v1();
        if (rc == -2) oom = 1;
        if (!rc) {
            loaded = migrated = 1;
            g_slot = 0; g_gen = 0; g_unclean = 0;   /* migrate into slot 1 */
            kprintf("[ok] fs: loaded a v1 image from %s '%s' (%d nodes); migration pending\n",
                    media, model, fs_count_nodes());
        }
    }
    if (!loaded && !fresh) {
        g_autosave = 0;
        g_readonly = 1;
        kprintf("[--] fs: cannot load saved filesystem%s; disk protected READ-ONLY.\n"
                "     Seed files are RAM-only; original disk contents left untouched.\n",
                oom ? " (out of memory)" : " (invalid or unreadable image)");
        notify_error("Storage", "Saved filesystem could not be loaded. The disk "
                                "is protected; these files are RAM-only.");
        return;
    }
    if (fresh)
        kprintf("[ok] fs: formatting %s '%s' part@LBA%lu with the seed tree\n",
                media, model, (unsigned long)g_base);

    g_mounts++;

    /* An unclean shutdown, or an explicit boot-menu request, means the image is
     * checked before anything is allowed to depend on it. A just-migrated v1
     * image is skipped: there is no v2 superblock on the disk to check until the
     * commit below writes one, and checking anyway would report a scary (and
     * meaningless) failure on a perfectly good upgrade. */
    if (loaded && !migrated && (g_unclean || (boot_flags() & BOOT_FSCK))) {
        fs_check_report rep;
        kprintf("[--] fs: %s; checking the filesystem\n",
                g_unclean ? "last shutdown was unclean" : "fsck requested at boot");
        int rc = fs_check(0, &rep);
        kprintf("     fsck: %d dirs, %d files, %lu bytes, %d issue%s\n",
                rep.dirs, rep.files, (unsigned long)rep.bytes, rep.nissue,
                rep.nissue == 1 ? "" : "s");
        for (int i = 0; i < rep.nissue && i < FS_CHECK_MAX_ISSUES; i++)
            kprintf("     fsck: %s\n", rep.issue[i]);
        if (rc < 0)
            kprintf("[--] fs: the image did not pass; the loaded tree is the best copy\n");
    }

    if (g_readonly) {
        g_autosave = 0;
        kprintf("[ok] fs: mounted READ-ONLY (recovery); `sync` is disabled\n");
        return;
    }

    g_autosave = 1;
    fs_commit(0);               /* persist the tree (loaded or freshly seeded) */
}

/* ---------------------------------------------------------------------------
 *  Export the live tree as a complete, clean v2 image onto another disk or
 *  partition, without disturbing this system's own mount. The installer uses it
 *  to seed the target: the machine you are installing FROM writes its current
 *  filesystem to the machine you are installing TO.
 *
 *  The result is generation 1, slot 0, marked clean, with both superblocks
 *  written -- exactly what a first boot on the target expects to find.
 * ------------------------------------------------------------------------- */
int fs_export_image(blkdev_t *dev, uint64_t base, uint64_t cap) {
    if (!dev || base > dev->sectors || cap > dev->sectors - base || cap <= BFS_SLOT0) return -1;
    uint64_t slot_sectors = (cap - BFS_SLOT0) / 2;
    if (slot_sectors == 0) return -1;

    /* serialise() sizes against the live mount's slots, so measure here. */
    uint32_t cnt = 0; uint64_t blob = 0;
    measure(root_, &cnt, &blob);
    uint64_t sectors = (blob + BLK_SECTOR - 1) / BLK_SECTOR;
    if (sectors > slot_sectors) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(sectors ? sectors * BLK_SECTOR : BLK_SECTOR);
    if (!buf) return -1;
    memset(buf, 0, (sectors ? sectors : 1) * BLK_SECTOR);
    uint8_t *p = buf;
    g_emit_idx = 0;
    emit(root_, BFS_NOPAR, &p);
    uint32_t sum = fnv32(buf, blob);

    int rc = blk_write(dev, base + BFS_SLOT0, (uint32_t)(sectors ? sectors : 1), buf);
    kfree(buf);
    if (rc != 0) return -1;
    if (blk_flush(dev) != 0) return -1;

    /* sb_fill reads g_slot_sectors/g_mounts for the live mount, so build the
     * target's superblock by hand rather than borrowing that state. */
    struct bfs_super sb;
    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, BFS_MAGIC2, 8);
    sb.version      = BFS_VERSION;
    sb.slot         = 0;
    sb.generation   = 1;
    sb.node_count   = cnt;
    sb.clean        = 1;
    sb.blob_bytes   = blob;
    sb.blob_sum     = sum;
    sb.slot_sectors = (uint32_t)slot_sectors;
    sb.mounts       = 0;
    sb.hdr_sum      = fnv32((const uint8_t *)&sb, sizeof sb - 2 * sizeof(uint32_t));

    uint8_t sec[BLK_SECTOR];
    memset(sec, 0, sizeof sec);
    memcpy(sec, &sb, sizeof sb);
    if (blk_write(dev, base + BFS_SB_A, 1, sec) != 0) return -1;
    if (blk_flush(dev) != 0) return -1;
    if (blk_write(dev, base + BFS_SB_B, 1, sec) != 0) return -1;
    return blk_flush(dev);
}

/* Give up the backing disk: stop autosaving and forget it, leaving the tree
 * live in RAM. The installer calls this when the disk it is about to erase is
 * the one this system is persisting to -- the usual situation when installing
 * from live media with a single disk attached. Continuing to autosave into a
 * disk being repartitioned underneath us would corrupt both. */
void fs_persist_detach(void) {
    g_autosave = 0;
    g_disk     = 0;
    g_base = g_cap = g_slot_sectors = 0;
}

int fs_persist_active(void) { return g_disk != 0; }
blkdev_t *fs_persist_disk(void) { return g_disk; }
const char *fs_persist_media(void) { return g_disk ? blk_media(g_disk) : ""; }
const char *fs_persist_model(void) { return g_disk ? g_disk->model : ""; }
