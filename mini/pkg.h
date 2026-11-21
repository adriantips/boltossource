/* ===========================================================================
 *  BoltOS Mini  -  mini/pkg.h
 *
 *  Packages: apps that aren't in the kernel. The Packages app lists what
 *  boltos.dev offers (https://boltos.dev/pkg/index.json), downloads one over
 *  the network, checks its SHA-256, keeps it on the SD card (store.c) and
 *  loads it -- then, and at every start after, its app joins the bar.
 *
 *  A package is a .bpk file (mini/tools/mkpkg.py makes one from an ELF):
 *  a header, the image (code and data, linked at 0), the places in it that
 *  hold an address of its own (moved by where it's loaded), and the places
 *  that hold a kernel function or variable, by name. The kernel fills those
 *  from its export table (exports.c), so a package calls gfx_fill or
 *  tls_connect_addr as the kernel's own apps do -- with the same headers.
 *  A name the kernel doesn't export stops the load, with a message.
 *
 *  The image's entry is pkg_main, which returns a pkg_info_t.
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include "ui.h"

#define PKG_ABI 1

typedef struct {
    uint32_t     abi;                 /* PKG_ABI, as the package was built        */
    const app_t *app;                 /* its app, for the bar and Home (or NULL)  */
    /* Every main-loop turn, whether its app is open or not: downloads, work
     * that finished on another core. 1 asks for a frame. May be NULL. */
    int  (*background)(void);
    /* Before it's removed: stop everything it started (other cores, downloads)
     * and free what it holds. May be NULL. */
    void (*unload)(void);
    /* Development builds: GET /app?<id>&<cmd> on the web page lands here, the
     * answer goes back as text. May be NULL. */
    int  (*dev)(const char *cmd, char *out, int cap);
} pkg_info_t;

typedef const pkg_info_t *(*pkg_entry_t)(void);

/* The file's header, at its start. */
#define BPK_MAGIC "BPK1"
typedef struct {
    char     magic[4];
    uint32_t head_size;               /* sizeof(bpk_head_t)                       */
    uint32_t abi;
    uint32_t flags;
    uint32_t image_size;              /* bytes of image in the file               */
    uint32_t mem_size;                /* image and its zeroed .bss                 */
    uint32_t entry;                   /* pkg_main, from the image's start         */
    uint32_t n_rel;                   /* u32 offsets: add the load address        */
    uint32_t n_imp;                   /* imports: u32 offset, u8 length, name     */
    uint32_t imp_bytes;
    char     id[32], name[48], version[16];
    uint8_t  pad[256 - 136];
} bpk_head_t;

/* ---- the kernel's side (mini/pkg.c) -------------------------------------- */
typedef struct {
    char     id[32], name[48], version[16];
    char     summary[160], about[600], needs[120];
    char     url[256];
    uint8_t  sha256[32];
    uint32_t size;
} pkg_offer_t;                        /* one of what boltos.dev offers           */

enum { PK_NONE, PK_FETCHING, PK_INSTALLING, PK_FAILED };
typedef struct {
    int      state;                   /* PK_*                                    */
    char     id[32];
    uint64_t got, total;
    char     msg[128];
} pkg_job_t;

void pkg_init(void);                  /* at boot: load what's installed (soon after) */
int  pkg_poll(void);                  /* every loop: jobs, packages' background work */
void pkg_refresh(void);               /* fetch the list from boltos.dev again    */
int  pkg_catalog(const pkg_offer_t **out);       /* what's offered; -1 still fetching, -2 failed */
const char *pkg_catalog_error(void);
int  pkg_install(const pkg_offer_t *o);          /* starts it; pkg_job() follows it */
const pkg_job_t *pkg_job(void);
int  pkg_remove(const char *id);
/* Installed and loaded: its app (NULL if it has none or isn't loaded), its
 * version. 0 if it isn't installed. */
int  pkg_installed(const char *id, const app_t **app, char *version, int vcap);
const char *pkg_load_error(const char *id);      /* why an installed one didn't load */
int  pkg_debug(char *out, int cap);
int  pkg_dev(const char *id, const char *cmd, char *out, int cap);

/* For dev endpoints: install a .bpk sent straight from the Mac. */
int  pkg_install_file(const uint8_t *file, uint32_t len, char *msg, int cap);

/* ---- the export table (mini/exports.c) ----------------------------------- */
typedef struct { const char *name; const void *addr; } kexport_t;
extern const kexport_t mini_exports[];
extern const int mini_nexports;

/* ---- the big memory (mini/pkg.c) ----------------------------------------- *
 *  The low and high windows of mini.h. A video takes the high one while it
 *  plays; a package asks with a release callback, which is called (on this
 *  core, between frames) when a video wants the window back.             */
void *bigmem_claim(int high, const char *who, void (*release)(void));
void  bigmem_free(int high);
const char *bigmem_owner(int high);              /* NULL when free           */
int   bigmem_take_for_video(void);               /* 0 once the high window is the video's */
void  bigmem_video_done(void);
int   bigmem_evict(int high);                       /* its owner lets go; 0 once free */
void  bigmem_keep(void);                            /* in a release callback: "no"    */
