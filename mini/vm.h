#pragma once
#include <stdint.h>
#include "gfx.h"

/* ===========================================================================
 *  BoltOS Mini  -  mini/vm.h
 *
 *  A second machine inside the Pi, for Firefox: BoltOS as a hypervisor
 *  (mini/vm.c, mini/arch/hyp.S) running a Linux guest on cores 1-3 with
 *  the two big windows of RAM, and the guest's devices -- a disk, a screen,
 *  a keyboard and a tablet, a network card -- made by BoltOS
 *  (mini/vdev.c). The Browser shows its screen.
 * ===========================================================================*/
enum { VM_OFF = 0, VM_LOADING, VM_RUNNING, VM_FAILED = -1 };

typedef struct {
    const char *kernel;                 /* store names                          */
    const char *root;                   /* the root filesystem (squashfs)       */
    const char *home;                   /* the writable disk: Firefox's profile */
    uint64_t    home_size;
    int         vcpus;                  /* 1..3                                 */
    int         screen_w, screen_h;
    const char *cmdline;                /* added to the kernel's own            */
} vm_config;

int  vm_supported(void);                /* the CPU came up at EL2: there's room for a guest */
int  vm_start(const vm_config *c);      /* 0: on its way (VM_LOADING)           */
void vm_stop(void);
int  vm_state(void);
const char *vm_message(void);           /* what it's doing, or what went wrong  */
void vm_poll(void);                     /* every turn of the main loop          */
int  vm_dev(const char *query, char *out, int cap);   /* /vm on the dev page   */

/* ---- getting Firefox onto the card (fxget.c) ------------------------------------ */
enum { FX_NONE, FX_LIST, FX_DOWNLOADING, FX_CHECKING, FX_READY, FX_FAILED };
int  fxget_state(uint64_t *got, uint64_t *total, uint32_t *rate);
void fxget_start(void);
void fxget_cancel(void);
const char *fxget_error(void);
int  fxget_poll(void);                      /* vm_poll calls it                  */

/* ---- the guest's screen and input (vdev.c), for the Browser ------------------- */
int  vgpu_size(int *w, int *h);              /* 1 once it shows something: the picture's size */
int  vgpu_damage(int *x, int *y, int *w, int *h);   /* 1: this changed since the last call */
/* The picture's rectangle into dst (stride in pixels), only what differs;
 * 1 with the rectangle narrowed to what did, 0 if nothing did. */
int  vgpu_copy(uint32_t *dst, int stride, int *x, int *y, int *w, int *h);
const uint32_t *vgpu_cursor(int *w, int *h, int *hx, int *hy, uint32_t *gen);
void vinput_key(int key, int ch, int mods);
void vinput_pointer(int x, int y, int buttons, int wheel);   /* screen pixels; wheel +1 up */
int  vdev_debug(char *out, int cap);

/* ---- for the devices (vdev.c) ------------------------------------------------ */
void *vm_ptr(uint64_t ipa, uint64_t len);   /* guest memory, or NULL outside it    */
void  vm_irq(int spi);                      /* raise an SPI (edge)                 */
#define VM_SPI_UART    1
#define VM_SPI_RTC     2
#define VM_SPI_VIRTIO  16                   /* + device index                      */
#define VM_VIRTIO_BASE 0x0A000000u
#define VM_VIRTIO_SIZE 0x200u
#define VM_VIRTIO_N    8
