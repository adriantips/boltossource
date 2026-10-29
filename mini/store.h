/* BoltOS Mini - mini/store.h: files on the microSD card, past its
 * partitions (mini/store.c). Everything here runs on the boot core. */
#pragma once
#include <stdint.h>

#define STORE_MAX  64
enum { STORE_USED = 1, STORE_DONE = 2 };

typedef struct {
    char     name[48];                    /* "pkg/localai.bpk", "ai/qwen3-0.6b-q4_0.gguf" */
    uint64_t lba, sectors;                /* where it is on the card, how much it has   */
    uint64_t size;                        /* bytes: expected, then actual once done     */
    uint64_t written;                     /* bytes written from the start so far        */
    uint32_t flags;
    uint8_t  sha256[32];                  /* once done                                  */
    char     meta[140];                   /* whatever its owner wants to remember       */
} store_entry_t;                          /* 256 bytes                                  */

int  store_open(int create);              /* 0: open; else store_why() says why          */
int  store_is_open(void);
const char *store_why(void);
int  store_count(void);
const store_entry_t *store_at(int i);     /* 0..STORE_MAX-1, NULL for an empty slot      */
const store_entry_t *store_find(const char *name);
const store_entry_t *store_create(const char *name, uint64_t size);
int  store_write(const store_entry_t *e, uint64_t off, const void *buf, uint32_t len);
int  store_read(const store_entry_t *e, uint64_t off, void *buf, uint32_t len);
int  store_progress(const char *name, uint64_t written, int done, const uint8_t *sha256);
int  store_set_meta(const char *name, const char *meta);
int  store_remove(const char *name);
uint64_t store_free(void);
uint64_t store_total(void);
int  store_debug(char *out, int cap);
