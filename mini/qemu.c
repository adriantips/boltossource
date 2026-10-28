/* ===========================================================================
 *  BoltOS Mini  -  mini/qemu.c
 *
 *  The two QEMU devices that are not virtio: fw_cfg, through which ramfb is
 *  configured (a framebuffer in guest RAM that QEMU's window scans out), and
 *  the PCIe host bridge, which exists here only so the OHCI driver can be
 *  exercised in QEMU (-device pci-ohci) before it meets the H618's.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "kprintf.h"
#include "string.h"

#define FWCFG_BASE  0x09020000u
#define FWCFG_DATA  (FWCFG_BASE + 0x0)
#define FWCFG_SEL   (FWCFG_BASE + 0x8)
#define FWCFG_DMA   (FWCFG_BASE + 0x10)
#define FW_CFG_FILE_DIR 0x19

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static uint64_t be64(uint64_t v) { return __builtin_bswap64(v); }

struct fwcfg_dma { uint32_t control, length; uint64_t address; };

static int fwcfg_dma(uint16_t sel, uint32_t ctl, void *buf, uint32_t len) {
    struct fwcfg_dma *d = (struct fwcfg_dma *)dma_alloc(sizeof *d, 64);
    d->control = be32(((uint32_t)sel << 16) | ctl | 0x08);    /* select       */
    d->length  = be32(len);
    d->address = be64((uint64_t)(uintptr_t)buf);
    dsb();
    uint64_t a = (uint64_t)(uintptr_t)d;
    w32(FWCFG_DMA, be32((uint32_t)(a >> 32)));
    w32(FWCFG_DMA + 4, be32((uint32_t)a));
    for (int i = 0; i < 1000000; i++) {
        uint32_t c = be32(*(volatile uint32_t *)&d->control);
        if (!(c & ~1u)) return 0;
        if (c & 1u) return -1;                                  /* error        */
    }
    return -1;
}

static int fwcfg_find(const char *name, uint16_t *sel) {
    w16(FWCFG_SEL, be16(FW_CFG_FILE_DIR));
    uint8_t c[4];
    for (int i = 0; i < 4; i++) c[i] = r8(FWCFG_DATA);
    uint32_t n = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) | ((uint32_t)c[2] << 8) | c[3];
    for (uint32_t i = 0; i < n && i < 256; i++) {
        uint8_t e[64];
        for (int k = 0; k < 64; k++) e[k] = r8(FWCFG_DATA);
        if (strcmp((const char *)e + 8, name) == 0) {
            *sel = (uint16_t)((e[4] << 8) | e[5]);
            return 0;
        }
    }
    return -1;
}

int qemu_ramfb_init(uint32_t *fb, int w, int h) {
    uint16_t sel;
    if (fwcfg_find("etc/ramfb", &sel) != 0) return -1;
    struct { uint64_t addr; uint32_t fourcc, flags, width, height, stride; } __attribute__((packed)) *cfg;
    cfg = dma_alloc(sizeof *cfg, 64);
    cfg->addr   = be64((uint64_t)(uintptr_t)fb);
    cfg->fourcc = be32(0x34325258u);                   /* 'XR24': XRGB8888     */
    cfg->flags  = 0;
    cfg->width  = be32((uint32_t)w);
    cfg->height = be32((uint32_t)h);
    cfg->stride = be32((uint32_t)w * 4);
    return fwcfg_dma(sel, 0x10, cfg, sizeof *cfg);     /* write               */
}

/* ---- PCIe, for the OHCI test rig -----------------------------------------------
 *  virt,highmem=off puts ECAM at 0x3F000000 and the 32-bit MMIO window at
 *  0x10000000. One function is all that is ever looked for.                  */
#define ECAM      0x3F000000u
#define PCI_MMIO  0x10000000u

uintptr_t qemu_pci_find(uint8_t cls, uint8_t sub, uint8_t progif, int *irq) {
    for (int dev = 0; dev < 32; dev++) {
        uintptr_t cfg = ECAM + ((uintptr_t)dev << 15);
        uint32_t id = r32(cfg);
        if ((id & 0xFFFF) == 0xFFFF) continue;
        uint32_t cc = r32(cfg + 8);
        if ((cc >> 24) != cls || ((cc >> 16) & 0xFF) != sub || ((cc >> 8) & 0xFF) != progif) continue;
        uintptr_t bar = PCI_MMIO + (uintptr_t)dev * 0x10000;
        w32(cfg + 0x10, (uint32_t)bar);
        w16(cfg + 0x04, r16(cfg + 0x04) | 0x6);       /* memory space, bus master */
        /* INTx of slot `dev`, pin A, swizzled onto virt's four SPIs 3..6. */
        if (irq) *irq = 32 + 3 + (dev % 4);
        kprintf("[pci] %02x:%02x.0 %04x:%04x at %lx\n", 0, dev, id & 0xFFFF, id >> 16, (uint64_t)bar);
        return bar;
    }
    return 0;
}
