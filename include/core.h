#pragma once
#include <stdint.h>
#include "interrupts.h"

/* ===========================================================================
 *  Core dumps.
 *
 *  A ring-3 crash already prints its registers and gets a line in the crash
 *  log, which tells you *that* a program died. It does not tell you what it was
 *  doing: the arguments, the locals and the call chain all live on the stack,
 *  and the stack goes away with the address space a moment later.
 *
 *  So the fault path saves it first. A core file is the trap frame plus as much
 *  of the user stack as is actually mapped, and the path of the executable --
 *  which is enough for `core show` to walk the frame chain afterwards and name
 *  every frame out of the program's own symbol table. The binary is on disk
 *  already; copying its text into the dump would only make the file bigger.
 *
 *  Dumps land in /var/crash and the oldest are pruned, so a program that
 *  crashes in a loop cannot fill the disk.
 * ===========================================================================*/

#define CORE_DIR        "/var/crash"
#define CORE_MAGIC      "BOLTCORE"
#define CORE_VERSION    1
#define CORE_STACK_MAX  (32 * 1024)     /* bytes of stack captured, at most  */
#define CORE_KEEP       8               /* dumps kept in /var/crash          */

typedef struct {
    char     magic[8];              /* "BOLTCORE", no NUL                    */
    uint32_t version;
    uint32_t pid;
    char     exe[64];               /* the program's path, for its symbols   */
    char     when[24];              /* "YYYY-MM-DD HH:MM:SS"                 */
    uint64_t cr2;                   /* faulting address (page faults)        */
    uint64_t stack_base;            /* lowest captured stack address         */
    uint32_t stack_len;             /* bytes of stack that follow the header */
    uint32_t flags;
    struct registers regs;
} core_hdr_t;

/* Every field above is naturally aligned, so the on-disk form is exactly the
 * in-memory one without packing it -- which matters because a packed struct
 * would make `&hdr->regs` an unaligned pointer for the reader. The assertion
 * is what keeps that true if a field is ever added. */
_Static_assert(sizeof(core_hdr_t) == 128 + sizeof(struct registers),
               "core_hdr_t has gained padding: the dump format would change");

/* Write a core file for the ring-3 process that just faulted. Called from the
 * fault path with the process's address space still loaded, which is the only
 * moment its stack can be read. Best effort: a crash report that fails to save
 * must never stop the kill from happening. */
void core_write(int pid, const char *exe, struct registers *r);
