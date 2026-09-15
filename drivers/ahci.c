#include <stdint.h>
#include <stddef.h>
#include "ahci.h"
#include "hw.h"
#include "blk.h"
#include "dma.h"
#include "mmio.h"
#include "string.h"
#include "kprintf.h"
#include "timeout.h"

/* ===========================================================================
 *  AHCI 1.x host bus adapter (polled, DMA).
 *
 *  Each SATA port gets a 1 KiB command list (32 headers), a 256-byte received
 *  FIS area, and a command table. We run one command at a time out of slot 0
 *  and poll PxCI for completion -- simple and correct for BoltOS's synchronous
 *  block I/O. Reads/writes use READ/WRITE DMA EXT (48-bit LBA) through a 64 KiB
 *  bounce buffer described by a single PRDT entry.
 * ===========================================================================*/

/* Generic host control (ABAR offsets). */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_VS    0x10
#define GHC_AE    (1u << 31)   /* AHCI enable */
#define GHC_HR    (1u << 0)    /* HBA reset   */

/* Per-port register block: ABAR + 0x100 + port*0x80. */
#define PORT_BASE(p)  (0x100 + (p) * 0x80)
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0C
#define PxIS    0x10
#define PxIE    0x14
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28
#define PxSCTL  0x2C
#define PxSERR  0x30
#define PxSACT  0x34
#define PxCI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)

#define TFD_BSY  (1u << 7)
#define TFD_DRQ  (1u << 3)
#define TFD_ERR  (1u << 0)

#define SIG_SATA 0x00000101

#define ATA_IDENTIFY     0xEC
#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35
#define ATA_FLUSH_EX     0xEA
#define FIS_TYPE_H2D     0x27

#define BOUNCE_SECTORS   128          /* 64 KiB per command */

struct ahci_port {
    int           used;
    int           portno;
    volatile uint8_t *regs;           /* ABAR + PORT_BASE(portno) */
    struct dma_buf clb;               /* command list (1 KiB)     */
    struct dma_buf fb;                /* received FIS (256 B)      */
    struct dma_buf ctba;              /* command table            */
    struct dma_buf bounce;            /* 64 KiB data bounce        */
    uint64_t       sectors;
    char           model[48];
};

static volatile uint8_t *g_abar;
static struct ahci_port  g_ports[32];

static inline uint32_t hr(uint32_t off)             { return mmio_read32(g_abar, off); }
static inline void     hw_(uint32_t off, uint32_t v){ mmio_write32(g_abar, off, v); }
static inline uint32_t pr(struct ahci_port *p, uint32_t off)             { return mmio_read32(p->regs, off); }
static inline void     pw(struct ahci_port *p, uint32_t off, uint32_t v) { mmio_write32(p->regs, off, v); }

/* Waits, in microseconds. AHCI 1.3 gives the engine 500 ms to stop (10.1.2),
 * and a live link 10 ms to report PhyRdy -- so 100 ms for the link is already
 * ten times the spec. These used to be iteration counts in the millions, which
 * on a real HBA is tens of seconds each, paid once per port whether or not a
 * disk is attached. */
#define AHCI_STOP_US    500000ull
#define AHCI_LINK_US    100000ull
#define AHCI_IDLE_US   1000000ull
#define AHCI_CMD_US    5000000ull

static void port_stop(struct ahci_port *p) {
    uint32_t cmd = pr(p, PxCMD);
    cmd &= ~(CMD_ST | CMD_FRE);
    pw(p, PxCMD, cmd);
    wait_until(!(pr(p, PxCMD) & (CMD_CR | CMD_FR)), AHCI_STOP_US);
}
static void port_start(struct ahci_port *p) {
    /* This was `while (pr(p, PxCMD) & CMD_CR) { }` -- no bound at all. A port
     * whose command engine never goes idle hung the kernel here forever, with
     * the boot log stopped mid-line and nothing to say why. */
    if (!wait_until(!(pr(p, PxCMD) & CMD_CR), AHCI_STOP_US)) {
        kprintf("[ahci] port %d: command engine will not stop\n", p->portno);
        return;
    }
    uint32_t cmd = pr(p, PxCMD);
    cmd |= CMD_FRE;
    pw(p, PxCMD, cmd);
    cmd |= CMD_ST;
    pw(p, PxCMD, cmd);
}

/* Build slot-0 command and poll PxCI. write=1 -> H2D write. Returns 0 / -1. */
static int port_exec(struct ahci_port *p, uint8_t ata_cmd, uint64_t lba,
                     uint32_t count, int write, uint64_t buf_phys, uint32_t bytes) {
    /* command header 0 */
    volatile uint32_t *hdr = (volatile uint32_t *)p->clb.virt;
    uint16_t cfl = 5;                                   /* H2D FIS = 5 dwords */
    /* bytes == 0 is a non-data command (FLUSH CACHE): no PRDT entry at all. */
    uint32_t prdtl = bytes ? (1u << 16) : 0;
    hdr[0] = cfl | (write ? (1u << 6) : 0) | prdtl;
    hdr[1] = 0;                                         /* PRDBC              */
    hdr[2] = (uint32_t)p->ctba.phys;
    hdr[3] = (uint32_t)(p->ctba.phys >> 32);

    /* command table: clear CFIS + one PRDT entry */
    uint8_t *ct = (uint8_t *)p->ctba.virt;
    memset(ct, 0, 0x80 + 16);

    uint8_t *fis = ct;
    fis[0] = FIS_TYPE_H2D;
    fis[1] = 0x80;                                      /* C=1 (command)      */
    fis[2] = ata_cmd;
    fis[3] = 0;                                         /* features           */
    fis[4] = (uint8_t)(lba);
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                                      /* device: LBA mode   */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[11] = 0;
    fis[12] = (uint8_t)(count);
    fis[13] = (uint8_t)(count >> 8);

    /* PRDT entry at offset 0x80 (skipped for non-data commands) */
    if (bytes) {
        volatile uint32_t *prdt = (volatile uint32_t *)(ct + 0x80);
        prdt[0] = (uint32_t)buf_phys;
        prdt[1] = (uint32_t)(buf_phys >> 32);
        prdt[2] = 0;
        prdt[3] = (bytes - 1) & 0x3FFFFF;               /* byte count - 1     */
    }

    /* wait for the port to be idle, then issue slot 0 */
    wait_until(!(pr(p, PxTFD) & (TFD_BSY | TFD_DRQ)), AHCI_IDLE_US);
    pw(p, PxIS, 0xFFFFFFFFu);
    pw(p, PxCI, 1);

    uint64_t deadline = now_us() + AHCI_CMD_US;
    for (;;) {
        int expired = now_us() >= deadline;             /* clock before registers */
        if (!(pr(p, PxCI) & 1)) {
            if (pr(p, PxTFD) & TFD_ERR) return -1;
            return 0;
        }
        if (pr(p, PxIS) & (1u << 30)) return -1;        /* TFES: task file error */
        if (expired) return -1;
    }
}

static int ahci_rw(struct ahci_port *p, uint64_t lba, uint32_t count, void *buf, int write) {
    uint8_t *u = buf;
    while (count) {
        uint32_t n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        uint32_t bytes = n * 512;
        if (write) memcpy(p->bounce.virt, u, bytes);
        if (port_exec(p, write ? ATA_WRITE_DMA_EX : ATA_READ_DMA_EX,
                      lba, n, write, p->bounce.phys, bytes) != 0) return -1;
        if (!write) memcpy(u, p->bounce.virt, bytes);
        lba += n; u += bytes; count -= n;
    }
    return 0;
}

static int ahci_blk_read(blkdev_t *b, uint64_t lba, uint32_t count, void *buf) {
    return ahci_rw((struct ahci_port *)b->drv, lba, count, buf, 0);
}
static int ahci_blk_write(blkdev_t *b, uint64_t lba, uint32_t count, const void *buf) {
    return ahci_rw((struct ahci_port *)b->drv, lba, count, (void *)(uintptr_t)buf, 1);
}
/* FLUSH CACHE EXT as a non-data command: the drive completes it only after its
 * write cache has reached media, which is the barrier the FS commit needs. */
static int ahci_blk_flush(blkdev_t *b) {
    return port_exec((struct ahci_port *)b->drv, ATA_FLUSH_EX, 0, 0, 0, 0, 0);
}

/* --- back from S3 ---------------------------------------------------------
 *  The command list, received-FIS area and command table are all still where
 *  they were -- S3 keeps the RAM -- and every port register that pointed at
 *  them is zero, along with the controller's AHCI-enable bit and the PCI
 *  command register's bus-master bit. So: reattach the device, re-enable AHCI,
 *  and hand each port its own addresses back. Nothing is reallocated, because
 *  nothing was lost.
 * ------------------------------------------------------------------------*/
static int ahci_blk_resume(blkdev_t *b) {
    struct ahci_port *p = (struct ahci_port *)b->drv;
    if (!p || !p->used) return -1;

    struct pci_dev pd;
    if (!pci_find_by_class(0x01, 0x06, &pd)) return -1;
    struct pci_bar bar;
    if (pci_bar(&pd, 5, &bar) != 0 || !bar.is_mmio) return -1;
    volatile void *abar = pci_map_bar(&bar);
    if (!abar) return -1;
    g_abar  = (volatile uint8_t *)abar;
    p->regs = g_abar + PORT_BASE(p->portno);
    pci_enable_bus_master(&pd);
    pci_write16(&pd, 0x04, pci_read16(&pd, 0x04) | (1u << 10));   /* INTx off */

    hw_(HBA_GHC, hr(HBA_GHC) | GHC_AE);

    /* The link has to have come back before the port is worth programming. A
     * disk that is still negotiating reads as "no device present", and
     * starting a port in that state gets you a command that never completes. */
    if (!wait_until((pr(p, PxSSTS) & 0x0F) == 3, AHCI_LINK_US)) return -1;

    port_stop(p);
    pw(p, PxCLB,  (uint32_t)p->clb.phys);
    pw(p, PxCLBU, (uint32_t)(p->clb.phys >> 32));
    pw(p, PxFB,   (uint32_t)p->fb.phys);
    pw(p, PxFBU,  (uint32_t)(p->fb.phys >> 32));
    pw(p, PxSERR, 0xFFFFFFFFu);      /* the sleep itself logs PHY errors */
    pw(p, PxIE,   0);
    port_start(p);

    /* One real command, because every register above can read back correct on
     * a port that will not actually move a sector. */
    return port_exec(p, ATA_IDENTIFY, 0, 0, 0, p->bounce.phys, 512);
}

static void extract_model(const uint16_t *id, char *out) {
    /* ATA IDENTIFY words 27..46: model, byte-swapped per word */
    for (int i = 0; i < 20; i++) {
        out[i * 2]     = (char)(id[27 + i] >> 8);
        out[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out[40] = 0;
    for (int i = 39; i >= 0 && (out[i] == ' ' || out[i] == 0); i--) out[i] = 0;
}

static int port_init(int portno) {
    struct ahci_port *p = &g_ports[portno];
    p->portno = portno;
    p->regs   = g_abar + PORT_BASE(portno);

    uint32_t ssts = pr(p, PxSSTS);
    if ((ssts & 0x0F) != 3) return -1;                  /* device present + PHY ready */
    if (pr(p, PxSIG) != SIG_SATA) return -1;            /* SATA disk only (not ATAPI) */

    port_stop(p);
    if (dma_alloc(1024, &p->clb)    != 0) return -1;
    if (dma_alloc(256,  &p->fb)     != 0) return -1;
    if (dma_alloc(4096, &p->ctba)   != 0) return -1;
    if (dma_alloc(BOUNCE_SECTORS * 512, &p->bounce) != 0) return -1;

    pw(p, PxCLB,  (uint32_t)p->clb.phys);
    pw(p, PxCLBU, (uint32_t)(p->clb.phys >> 32));
    pw(p, PxFB,   (uint32_t)p->fb.phys);
    pw(p, PxFBU,  (uint32_t)(p->fb.phys >> 32));
    pw(p, PxSERR, 0xFFFFFFFFu);
    pw(p, PxIE,   0);
    port_start(p);

    /* IDENTIFY into the bounce buffer */
    if (port_exec(p, ATA_IDENTIFY, 0, 0, 0, p->bounce.phys, 512) != 0) {
        kprintf("[ahci] port %d IDENTIFY failed\n", portno);
        return -1;
    }
    const uint16_t *id = (const uint16_t *)p->bounce.virt;
    uint64_t sec48 = *(const uint64_t *)(id + 100) & 0xFFFFFFFFFFFFull;
    uint32_t sec28 = *(const uint32_t *)(id + 60);
    p->sectors = sec48 ? sec48 : sec28;
    extract_model(id, p->model);
    p->used = 1;

    blkdev_t b; memset(&b, 0, sizeof b);
    b.sectors     = p->sectors;
    b.sector_size = 512;
    b.is_ssd      = 1;
    b.is_boot     = 0;
    b.drv         = p;
    b.read  = ahci_blk_read;
    b.write = ahci_blk_write;
    b.flush = ahci_blk_flush;
    b.resume = ahci_blk_resume;
    int idx = 0;
    for (int i = 0; i < portno; i++) if (g_ports[i].used) idx++;
    b.name[0] = 's'; b.name[1] = 'd'; b.name[2] = (char)('a' + idx); b.name[3] = 0;
    strncpy(b.model, p->model[0] ? p->model : "SATA disk", sizeof b.model - 1);
    blk_register(&b);

    uint64_t mb = (p->sectors * 512) / (1024 * 1024);
    kprintf("[ahci] port %d: %s, %lu MiB\n", portno, p->model, mb);
    return 0;
}

void ahci_init(void) {
    struct pci_dev pd;
    if (!pci_find_by_class(0x01, 0x06, &pd)) return;        /* mass storage / SATA */

    struct pci_bar bar;
    if (pci_bar(&pd, 5, &bar) != 0 || !bar.is_mmio || !pci_map_bar(&bar)) {
        kprintf("[ahci] ABAR (BAR5) map failed\n");
        return;
    }
    g_abar = (volatile uint8_t *)bar.virt;
    pci_enable_bus_master(&pd);
    pci_write16(&pd, 0x04, pci_read16(&pd, 0x04) | (1u << 10));  /* INTx disable */

    hw_(HBA_GHC, hr(HBA_GHC) | GHC_AE);                     /* AHCI enable */

    uint32_t pi = hr(HBA_PI);
    int found = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        if (port_init(i) == 0) found++;
    }
    if (!found) kprintf("[ahci] controller present, no SATA disks\n");
}
