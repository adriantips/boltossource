/* BoltOS Mini - mini/dl.h: one big file over HTTPS, a piece at a time
 * (mini/dl.c). Runs on the boot core, from the main loop. */
#pragma once
#include <stdint.h>

typedef struct dl dl_t;
/* Called with each piece in order: `off` is where it goes in the file.
 * Return < 0 to stop the download. */
typedef int (*dl_sink)(void *ctx, uint64_t off, const uint8_t *p, uint32_t n);
enum { DL_RUNNING = 0, DL_DONE = 1, DL_FAILED = -1 };

dl_t       *dl_start(const char *url, uint64_t from, dl_sink sink, void *ctx);
int         dl_poll(dl_t *d, int ms);       /* spend up to ms on it: DL_*         */
uint64_t    dl_total(const dl_t *d);        /* the whole file's length, 0 unknown */
uint64_t    dl_got(const dl_t *d);          /* bytes of it so far, from 0         */
uint32_t    dl_rate(const dl_t *d);         /* bytes a second, lately             */
const char *dl_error(const dl_t *d);        /* why it failed, or the last trouble */
void        dl_free(dl_t *d);
