#pragma once
#include <stdint.h>

/* Archives: a pile of object files in one file, which is all a static library
 * has ever been.
 *
 * The format is System V / GNU ar, the one every `ar` on every Unix writes, so
 * an archive built here can be unpacked anywhere and one built elsewhere links
 * here. It is eight bytes of magic, then per member a 60-byte header of
 * space-padded ASCII and a body padded to an even length. Two members have
 * reserved names: "/" is ar's own symbol index, and "//" is a table of names
 * too long for the 16-byte field, which the header then refers to by offset.
 *
 * BoltAR writes the "//" table when it needs it and does not write the "/"
 * index at all: the linker reads each member's own symbol table to decide
 * whether it wants it, so an index would be a second copy of something already
 * in the file and one more thing to keep true. `ar` on a host will happily add
 * one with `ranlib`, and the reader below skips it either way.
 */

#define AR_NAME_MAX 64
#define AR_MAX_ERR  128

enum { AR_FILE = 0, AR_INDEX = 1, AR_LONGNAMES = 2 };

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    char           name[AR_NAME_MAX];
    int            kind;
} ar_member_t;

int ar_is_archive(const uint8_t *d, uint32_t len);

/* Step to the member at *pos (start it at 8). Returns 1 having filled `m`,
 * 0 at the end, -1 if the archive is malformed. `longnames`/`longlen` hold the
 * "//" table across calls and are updated when one turns up -- start both at
 * zero and pass the same pair back each time. */
int ar_next(const uint8_t *ar, uint32_t arlen, uint32_t *pos,
            const char **longnames, uint32_t *longlen, ar_member_t *m);

typedef struct {
    const char    *name;
    const uint8_t *data;
    uint32_t       len;
} ar_input_t;

/* Build an archive holding `n` members. Returns a kmalloc'd image with *len
 * set, or 0 with a message in err. */
uint8_t *ar_create(const ar_input_t *in, int n, uint32_t *len,
                   char *err, uint32_t errcap);
