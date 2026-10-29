/* BoltOS Mini - mini/fs.h: files and folders on the microSD card's FAT32
 * partition (mini/fs.c). Everything here runs on the boot core.
 *
 * Paths are absolute, '/'-separated and case-insensitive, as FAT's are:
 * "/Code/hello.py". Names keep the case they were given and may be long. */
#pragma once
#include <stdint.h>

#define FS_NAME 128
#define FS_PATH 256

typedef struct {
    char     name[FS_NAME];
    uint32_t size;
    uint8_t  dir;
    uint16_t date, time;                  /* FAT's: ((y-1980)<<9 | m<<5 | d), (h<<11 | m<<5 | s/2) */
} fs_ent_t;

int  fs_mount(void);                      /* 0 once the card's FAT32 partition is mounted     */
int  fs_ready(void);
const char *fs_why(void);                 /* what the last failure was                         */
/* A folder's contents, folders first, then by name; hidden ones and the
 * Mac's "._" files are left out. The count, or -1. */
int  fs_list(const char *dir, fs_ent_t *out, int max);
int  fs_stat(const char *path, fs_ent_t *out);                 /* 0 if it exists       */
/* The whole file, NUL-terminated, in a kmalloc'd buffer the caller frees. */
int  fs_read(const char *path, char **out, uint32_t *len);
int  fs_write(const char *path, const void *data, uint32_t len);  /* makes or replaces */
/* BOLTOS.BIN, which fs_write won't touch: for an update (update.c) only. */
int  fs_install_kernel(const void *data, uint32_t len);
int  fs_mkdir(const char *path);          /* and the folders above it                          */
int  fs_remove(const char *path);         /* a file, or a folder and everything in it          */
int  fs_rename(const char *from, const char *to);              /* also into another folder */
int  fs_space(uint64_t *free, uint64_t *total);
/* `rel` against the folder `cwd`, with "." and ".." worked out: into out. */
int  fs_join(const char *cwd, const char *rel, char *out, int cap);
const char *fs_basename(const char *path);
void fs_date_str(uint16_t date, uint16_t time, char *out, int cap);  /* "2026-10-29 14:05" */
int  fs_debug(char *out, int cap);

/* The keep records (klog.c) outlive a power cut by being copied to the card. */
void fs_keep_restore(void);               /* once mounted: the card's copy, if RAM lost it    */
void fs_keep_changed(void);               /* keep_put calls it                                 */
void fs_poll(void);                       /* main loop: writes the copy a moment after a change */
