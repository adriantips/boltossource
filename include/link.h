#pragma once
#include <stdint.h>

/* BoltLD -- a static linker for the objects BoltAS writes.
 *
 * `as -c` stopped short of a program on purpose: it leaves holes where names
 * go and a list of which name goes in which hole. This is the half that reads
 * that list. It concatenates the .text of every input, concatenates the .data,
 * decides where the two land, works out what every name is worth, and fills the
 * holes in. What comes out is an ordinary ET_EXEC with the same two segments
 * `as` produces directly, so kernel/elf.c cannot tell the two routes apart.
 *
 * Static only. There is nothing dynamic on this machine to link against, so
 * R_X86_64_PLT32 -- which every assembler emits for a call to a global -- is
 * resolved exactly like PC32, which is what a real static link does with it.
 *
 * Archives are pulled from the way ar-fed linkers have always pulled from them:
 * a member joins the link only if it defines a name that is still missing, and
 * the scan repeats until a pass adds nothing. Order on the command line still
 * matters less here than it does with ld(1), because the loop runs to a fixed
 * point rather than once left to right -- a deliberate difference, and the one
 * that stops `ld a.o -lfoo -lbar` from failing when bar needs foo.
 */

#define LD_MAX_ERR    192
#define LD_MAX_INPUT  32

typedef struct {
    const char    *name;      /* for diagnostics: the path it was read from */
    const uint8_t *data;
    uint32_t       len;
} ld_input_t;

/* Link `n` inputs -- ET_REL objects, `!<arch>` archives, or a mix -- into a
 * freshly allocated executable image. `entry` names the entry point ("_start"
 * when null). Returns the image (caller kfree()s it) with *len set, or 0 with
 * a message in err.
 *
 * `stats` is optional; when given it is filled with what the link did, which
 * `ld -v` prints and the tests check.
 */
typedef struct {
    uint32_t objects;         /* units that actually made it into the link  */
    uint32_t from_archives;   /* how many of those were pulled from an .a   */
    uint32_t relocs;          /* holes filled                               */
    uint32_t text, data;      /* merged section sizes                       */
    uint64_t entry;
} ld_stats_t;

uint8_t *ld_link(const ld_input_t *in, int n, const char *entry,
                 uint32_t *len, ld_stats_t *stats, char *err, uint32_t errcap);
