/* ===========================================================================
 *  BoltOS  -  kernel/app_devmgr.c
 *  Device Manager: every piece of hardware the kernel found, grouped the way
 *  somebody looking for a missing driver would group it.
 *
 *  `lspci` already dumps the bus, and Sysinfo already summarises the machine.
 *  Neither answers the question you actually have at 2am, which is "is the
 *  network card there at all, and did anything claim it". So this is a tree:
 *  categories collapsed by default, a device under each, and a properties
 *  pane with the vendor/device pair, where it sits on the bus, its IRQ and
 *  its BARs -- the four things you need before you can even start guessing.
 *
 *  The tree is rebuilt from a real PCI rescan on open and on Refresh, not
 *  cached from boot: hot-plugging a USB stick should show up here without a
 *  reboot, and it does, because blk_count() grows.
 * ===========================================================================*/
#include <stdint.h>
#include <stdio.h>
#include "gui.h"
#include "hw.h"
#include "blk.h"
#include "smp.h"
#include "usb.h"
#include "keyboard.h"
#include "commands.h"   /* sh_human */
#include "string.h"

#define MAXDEV    160
#define MAXROW    (MAXDEV + 16)
#define ROW_H     20
#define HEAD_H    42

/* Categories, in the order Windows lists them -- which is roughly "the thing
 * you are looking for is near the top". */
enum {
    CAT_CPU = 0, CAT_DISK, CAT_STORAGE, CAT_DISPLAY, CAT_NET, CAT_AUDIO,
    CAT_USB, CAT_INPUT, CAT_BRIDGE, CAT_OTHER, NCAT
};

static const char *CATNAME[NCAT] = {
    "Processors",
    "Disk drives",
    "Storage controllers",
    "Display adapters",
    "Network adapters",
    "Sound, video and game controllers",
    "USB controllers",
    "Keyboards and pointing devices",
    "System devices",
    "Other devices",
};

/* Where a device came from decides which properties make sense to show. */
enum { SRC_PCI = 0, SRC_CPU, SRC_BLK, SRC_FIXED };

typedef struct {
    char    name[52];
    char    detail[64];       /* second line in the properties pane */
    uint8_t cat;
    uint8_t src;
    uint8_t ok;               /* 0 = the kernel has no driver bound  */
    struct pci_dev pci;       /* valid when src == SRC_PCI           */
    int     blk_index;        /* valid when src == SRC_BLK           */
} dev_t;

typedef struct { int cat; int dev; } row_t;   /* dev < 0 => the category header */

static struct {
    dev_t   dev[MAXDEV];
    int     ndev;
    int     open_cat[NCAT];
    row_t   row[MAXROW];
    int     nrow;
    int     sel;              /* index into dev[], or -1 */
    int     scroll;
    int     rows, cw, ch;
} D;

/* ---------------------------------------------------------------------------
 *  Vendor names. There is no pci.ids on this machine and there is not going to
 *  be one -- it is a megabyte of text to name twenty devices. These are the
 *  vendors that actually turn up in a PC or a QEMU guest; everything else
 *  prints as a hex id, which is what you would paste into a search engine
 *  anyway.
 * -------------------------------------------------------------------------*/
static const char *vendor_name(uint16_t v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD/ATI";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU";              /* the stdvga's made-up id */
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat";
    case 0x10EC: return "Realtek";
    case 0x14E4: return "Broadcom";
    case 0x1969: return "Qualcomm Atheros";
    case 0x168C: return "Atheros";
    case 0x15B3: return "Mellanox";
    case 0x1106: return "VIA";
    case 0x1039: return "SiS";
    case 0x1033: return "NEC";
    case 0x1912: return "Renesas";
    case 0x144D: return "Samsung";
    case 0x1E0F: return "KIOXIA";
    case 0x1C5C: return "SK hynix";
    case 0x1179: return "Toshiba";
    case 0x1B4B: return "Marvell";
    case 0x197B: return "JMicron";
    case 0x1D6A: return "Aquantia";
    case 0x5333: return "S3 Graphics";
    case 0x80EE: return "VirtualBox";
    case 0x15AD: return "VMware";
    default:     return 0;
    }
}

/* PCI class/subclass -> one of our categories. The subclass matters more than
 * the class here: 0x0C03 is USB and 0x0C05 is the SMBus, and putting them in
 * the same bucket would defeat the point of the bucket. */
static int pci_category(const struct pci_dev *d) {
    switch (d->class) {
    case 0x01: return CAT_STORAGE;
    case 0x02: return CAT_NET;
    case 0x03: return CAT_DISPLAY;
    case 0x04: return CAT_AUDIO;
    case 0x06: return CAT_BRIDGE;
    case 0x09: return CAT_INPUT;
    case 0x0D: return CAT_NET;                       /* wireless */
    case 0x0C: return d->subclass == 0x03 ? CAT_USB : CAT_BRIDGE;
    case 0x00: case 0x05: case 0x08: return CAT_BRIDGE;
    default:   return CAT_OTHER;
    }
}

/* A friendlier name than "serial bus controller 0c:03". Subclasses BoltOS has
 * a driver for get named after the driver, because that is the information you
 * want: "xHCI" means the USB app will see your keyboard. */
static const char *pci_kind(const struct pci_dev *d) {
    if (d->class == 0x01) switch (d->subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA / AHCI controller";
        case 0x08: return "NVMe controller";
        default:   return "Mass storage controller";
    }
    if (d->class == 0x0C && d->subclass == 0x03) switch (d->prog_if) {
        case 0x00: return "UHCI host controller";
        case 0x10: return "OHCI host controller";
        case 0x20: return "EHCI host controller";
        case 0x30: return "xHCI host controller";
        default:   return "USB host controller";
    }
    if (d->class == 0x02) return "Ethernet controller";
    if (d->class == 0x0D) return "Wireless controller";
    if (d->class == 0x03) return "Graphics adapter";
    if (d->class == 0x04) return "Audio device";
    if (d->class == 0x06) return d->subclass == 0x00 ? "Host bridge"
                        : d->subclass == 0x01 ? "ISA bridge"
                        : d->subclass == 0x04 ? "PCI-to-PCI bridge" : "Bridge";
    if (d->class == 0x0C && d->subclass == 0x05) return "SMBus controller";
    return pci_class_name(d->class);
}

/* Does the kernel actually drive this, or is it just sitting on the bus? The
 * honest answer is a lookup table of what has a driver compiled in, and it
 * being a table is the point -- add a driver, add a line. */
static int pci_has_driver(const struct pci_dev *d) {
    if (d->class == 0x01 && (d->subclass == 0x01 || d->subclass == 0x06 || d->subclass == 0x08)) return 1;
    if (d->class == 0x02) return 1;
    if (d->class == 0x03) return 1;                              /* the firmware framebuffer */
    if (d->class == 0x04) return 1;
    if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30) return 1;
    if (d->class == 0x06) return 1;                              /* bridges need no driver */
    return 0;
}

static dev_t *push(int cat, int src) {
    if (D.ndev >= MAXDEV) return 0;
    dev_t *e = &D.dev[D.ndev++];
    memset(e, 0, sizeof *e);
    e->cat = (uint8_t)cat; e->src = (uint8_t)src; e->ok = 1; e->blk_index = -1;
    return e;
}

static void scan(void) {
    D.ndev = 0;
    D.sel  = -1;

    /* --- processors ---------------------------------------------------- */
    char brand[49];
    hw_cpu_brand(brand);
    if (!brand[0]) strlcpy(brand, "x86-64 processor", sizeof brand);
    int ncpu = smp_cpu_count();
    if (ncpu < 1) ncpu = 1;
    for (int i = 0; i < ncpu; i++) {
        dev_t *e = push(CAT_CPU, SRC_CPU);
        if (!e) break;
        strlcpy(e->name, brand, sizeof e->name);
        snprintf(e->detail, sizeof e->detail, "Logical core %d of %d", i, ncpu);
    }

    /* --- disks ---------------------------------------------------------- */
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *b = blk_get(i);
        if (!b) continue;
        dev_t *e = push(CAT_DISK, SRC_BLK);
        if (!e) break;
        e->blk_index = i;
        if (b->model[0]) strlcpy(e->name, b->model, sizeof e->name);
        else             strlcpy(e->name, b->name,  sizeof e->name);
        char sz[16];
        sh_human(b->sectors * (uint64_t)b->sector_size, sz);
        snprintf(e->detail, sizeof e->detail, "%s  %s  %s", b->name, blk_media(b), sz);
    }

    /* --- the PCI bus ----------------------------------------------------- */
    static struct pci_dev bus[128];        /* static: 128 * 12 bytes is a lot of stack */
    int n = pci_scan(bus, 128);
    for (int i = 0; i < n; i++) {
        dev_t *e = push(pci_category(&bus[i]), SRC_PCI);
        if (!e) break;
        e->pci = bus[i];
        e->ok  = (uint8_t)pci_has_driver(&bus[i]);
        const char *vn = vendor_name(bus[i].vendor);
        if (vn) snprintf(e->name, sizeof e->name, "%s %s", vn, pci_kind(&bus[i]));
        else    snprintf(e->name, sizeof e->name, "%s [%04x:%04x]", pci_kind(&bus[i]),
                         bus[i].vendor, bus[i].device);
        snprintf(e->detail, sizeof e->detail, "PCI bus %d, device %d, function %d",
                 bus[i].bus, bus[i].slot, bus[i].func);
    }

    /* --- the things that are not on a bus we can enumerate ---------------- */
    dev_t *e = push(CAT_INPUT, SRC_FIXED);
    if (e) { strlcpy(e->name, "Standard PS/2 keyboard", sizeof e->name);
             strlcpy(e->detail, "i8042 controller, port 0x60", sizeof e->detail); }
    e = push(CAT_INPUT, SRC_FIXED);
    if (e) { strlcpy(e->name, "PS/2 compatible mouse", sizeof e->name);
             strlcpy(e->detail, "i8042 auxiliary port", sizeof e->detail); }
    if (usbstor_count() > 0) {
        for (int i = 0; i < usbstor_count(); i++) {
            e = push(CAT_USB, SRC_FIXED);
            if (!e) break;
            snprintf(e->name, sizeof e->name, "USB mass storage device (%s)", usbstor_name(i));
            strlcpy(e->detail, "Bulk-only transport, SCSI command set", sizeof e->detail);
        }
    }
    e = push(CAT_BRIDGE, SRC_FIXED);
    if (e) { strlcpy(e->name, "Programmable interrupt timer", sizeof e->name);
             strlcpy(e->detail, "i8254 PIT, 1 kHz tick", sizeof e->detail); }
    e = push(CAT_BRIDGE, SRC_FIXED);
    if (e) { strlcpy(e->name, "System CMOS / real time clock", sizeof e->name);
             strlcpy(e->detail, "MC146818, ports 0x70/0x71", sizeof e->detail); }
}

/* Flatten the tree into rows once per draw, so hit-testing a click is the same
 * arithmetic as drawing and cannot drift from it. */
static void build_rows(void) {
    D.nrow = 0;
    for (int c = 0; c < NCAT; c++) {
        int count = 0;
        for (int i = 0; i < D.ndev; i++) if (D.dev[i].cat == c) count++;
        if (!count) continue;                       /* empty categories are noise */
        if (D.nrow < MAXROW) { D.row[D.nrow].cat = c; D.row[D.nrow].dev = -1; D.nrow++; }
        if (!D.open_cat[c]) continue;
        for (int i = 0; i < D.ndev && D.nrow < MAXROW; i++)
            if (D.dev[i].cat == c) { D.row[D.nrow].cat = c; D.row[D.nrow].dev = i; D.nrow++; }
    }
}

/* ------------------------------------------------------------- drawing ---- */

/* A triangle, pointing right when collapsed and down when open. */
static void chevron(int x, int y, int open, uint32_t col) {
    for (int i = 0; i < 5; i++)
        if (open) g_fill(x + i, y + 2 + i, 9 - 2 * i, 1, col);
        else      g_fill(x + 2 + i, y + i, 1, 9 - 2 * i, col);
}

/* A small box per category, tinted by what is in it. Cheaper than a real icon
 * set and it makes the tree scannable, which is all an icon is for here. */
static void cat_glyph(int x, int y, int cat) {
    static const uint32_t tint[NCAT] = {
        0xFF8A65, 0x9CCC65, 0x4DB6AC, 0xBA68C8,
        0x64B5F6, 0xFFD54F, 0x81C784, 0xE57373, 0x90A4AE, 0xA1887F,
    };
    uint32_t c = tint[cat];
    g_round(x, y, 12, 12, 3, c, 255);
    g_fill(x + 3, y + 3, 6, 6, 0x181820);
}

static void devmgr_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    D.cw = cw; D.ch = ch;
    build_rows();

    g_fill(cx, cy, cw, ch, COL_PANEL);

    /* toolbar */
    g_text(cx + 14, cy + 14, "Devices on this machine", COL_TEXT, 1);
    char cnt[40];
    snprintf(cnt, sizeof cnt, "%d device%s", D.ndev, D.ndev == 1 ? "" : "s");
    int bw = 78;
    g_round(cx + cw - bw - 12, cy + 8, bw, 24, 6, COL_PANEL_3, 255);
    g_text(cx + cw - bw - 12 + (bw - g_text_width("Refresh", 1)) / 2, cy + 16, "Refresh", COL_TEXT, 1);
    g_text(cx + cw - bw - 24 - g_text_width(cnt, 1), cy + 14, cnt, COL_TEXT_DIM, 1);
    g_hline(cx + 10, cy + HEAD_H - 6, cw - 20, COL_PANEL_3);

    /* The properties pane goes under the tree when the window is narrow --
     * side by side below ~560px leaves neither half readable. */
    int stacked = cw < 560;
    int tree_w  = stacked ? cw : (cw * 3) / 5;
    int top     = cy + HEAD_H;
    int tree_h  = stacked ? (ch - HEAD_H) / 2 : ch - HEAD_H - 8;

    D.rows = tree_h / ROW_H;
    if (D.rows < 1) D.rows = 1;
    if (D.scroll > D.nrow - D.rows) D.scroll = D.nrow - D.rows;
    if (D.scroll < 0) D.scroll = 0;

    g_set_clip(cx, top, tree_w, tree_h);
    for (int r = 0; r < D.rows; r++) {
        int i = D.scroll + r;
        if (i >= D.nrow) break;
        int y = top + r * ROW_H;
        row_t *rw = &D.row[i];

        if (rw->dev < 0) {
            int count = 0;
            for (int k = 0; k < D.ndev; k++) if (D.dev[k].cat == rw->cat) count++;
            chevron(cx + 12, y + 5, D.open_cat[rw->cat], COL_TEXT_DIM);
            cat_glyph(cx + 26, y + 4, rw->cat);
            g_text(cx + 44, y + 6, CATNAME[rw->cat], COL_TEXT, 1);
            char c2[8]; snprintf(c2, sizeof c2, "%d", count);
            g_text(cx + 50 + g_text_width(CATNAME[rw->cat], 1), y + 6, c2, COL_TEXT_DIM, 1);
        } else {
            dev_t *d = &D.dev[rw->dev];
            if (rw->dev == D.sel) g_fill(cx + 4, y, tree_w - 8, ROW_H, COL_ACCENT_DIM);
            /* A device with nothing bound to it gets the yellow bang, same as
             * Windows, for the same reason: it is the only row that matters. */
            g_round(cx + 46, y + 7, 6, 6, 3, d->ok ? COL_GOOD : COL_WARN, 255);
            char nm[64];
            int maxc = (tree_w - 68) / 8;
            if (maxc < 4) maxc = 4;
            int len = (int)strlen(d->name);
            if (len > maxc) { memcpy(nm, d->name, (uint32_t)maxc - 1); nm[maxc - 1] = 0;
                              kstrlcat(nm, "\x7F", sizeof nm); }
            else strlcpy(nm, d->name, sizeof nm);
            g_text(cx + 60, y + 6, nm, rw->dev == D.sel ? 0xFFFFFF : COL_TEXT, 1);
        }
    }
    g_clear_clip();

    /* scrollbar, only when there is something to scroll */
    if (D.nrow > D.rows) {
        int bx = cx + tree_w - 7, bh = tree_h * D.rows / D.nrow;
        if (bh < 18) bh = 18;
        int by = top + (tree_h - bh) * D.scroll / (D.nrow - D.rows);
        g_fill(bx, top, 4, tree_h, COL_PANEL_2);
        g_round(bx, by, 4, bh, 2, COL_TEXT_DIM, 255);
    }

    /* ---- properties pane ------------------------------------------------ */
    int px, py, pw, phh;
    if (stacked) { px = cx + 10; py = top + tree_h + 8; pw = cw - 20; phh = ch - HEAD_H - tree_h - 16; }
    else         { px = cx + tree_w + 4; py = top; pw = cw - tree_w - 14; phh = tree_h; }
    if (pw < 60 || phh < 40) return;

    g_round(px, py, pw, phh, 8, COL_PANEL_2, 255);
    if (D.sel < 0 || D.sel >= D.ndev) {
        g_text(px + 14, py + 16, "Select a device.", COL_TEXT_DIM, 1);
        return;
    }
    dev_t *d = &D.dev[D.sel];
    g_set_clip(px, py, pw, phh);

    int y = py + 14;
    /* the name can be long; wrap it across at most two lines */
    int cols = (pw - 28) / 8;
    if (cols < 4) cols = 4;
    const char *p = d->name;
    for (int line = 0; line < 2 && *p; line++) {
        char t[80];
        int k = 0;
        while (p[k] && k < cols && k < (int)sizeof(t) - 1) { t[k] = p[k]; k++; }
        /* break on the last space so a word does not split */
        if (p[k] && line == 0) { int b = k; while (b > 0 && t[b - 1] != ' ') b--; if (b > cols / 2) k = b; }
        t[k] = 0;
        g_text(px + 14, y, t, COL_TEXT, 1);
        y += 16;
        p += k;
        while (*p == ' ') p++;
    }
    y += 4;
    g_text(px + 14, y, d->detail, COL_TEXT_DIM, 1); y += 22;
    g_hline(px + 14, y - 6, pw - 28, COL_PANEL_3);

    char line[80];
    if (d->src == SRC_PCI) {
        const char *vn = vendor_name(d->pci.vendor);
        snprintf(line, sizeof line, "Vendor    %04x  %s", d->pci.vendor, vn ? vn : "");
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        snprintf(line, sizeof line, "Device    %04x  rev %02x", d->pci.device, d->pci.revision);
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        snprintf(line, sizeof line, "Class     %02x:%02x:%02x  %s",
                 d->pci.class, d->pci.subclass, d->pci.prog_if, pci_class_name(d->pci.class));
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        snprintf(line, sizeof line, "Location  %02x:%02x.%x",
                 d->pci.bus, d->pci.slot, d->pci.func);
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        uint8_t irq = pci_interrupt_line(&d->pci);
        if (irq && irq != 0xFF) snprintf(line, sizeof line, "IRQ       %d", irq);
        else                    strlcpy(line, "IRQ       none assigned", sizeof line);
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 22;

        for (int b = 0; b < 6 && y < py + phh - 34; b++) {
            struct pci_bar bar;
            if (pci_bar(&d->pci, b, &bar) != 0) continue;
            char sz[16]; sh_human(bar.size, sz);
            if (bar.is_mmio)
                snprintf(line, sizeof line, "BAR%d      mem %llx  %s%s", b,
                         (unsigned long long)bar.phys, sz, bar.is_64bit ? "  64-bit" : "");
            else
                snprintf(line, sizeof line, "BAR%d      i/o %04x  %s", b, bar.port, sz);
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 16;
        }
    } else if (d->src == SRC_BLK) {
        blkdev_t *b = blk_get(d->blk_index);
        if (b) {
            char sz[16]; sh_human(b->sectors * (uint64_t)b->sector_size, sz);
            snprintf(line, sizeof line, "Capacity  %s", sz);
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
            snprintf(line, sizeof line, "Blocks    %llu x %u bytes",
                     (unsigned long long)b->sectors, b->sector_size);
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
            snprintf(line, sizeof line, "Media     %s", blk_media(b));
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
            snprintf(line, sizeof line, "Role      %s%s", b->is_boot ? "boot medium" : "data",
                     blk_fs_disk() == b ? ", mounted" : "");
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
            snprintf(line, sizeof line, "Cache     %s", b->flush ? "flushable" : "write-through");
            g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        }
    } else if (d->src == SRC_CPU) {
        char vend[13]; hw_cpu_vendor(vend);
        snprintf(line, sizeof line, "Vendor    %s", vend);
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        snprintf(line, sizeof line, "Max leaf  %u", hw_cpu_max_leaf());
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        uint32_t a, b, c, dd;
        cpuidx(1, 0, &a, &b, &c, &dd);
        snprintf(line, sizeof line, "Family    %u model %u stepping %u",
                 ((a >> 8) & 0xF) + ((a >> 20) & 0xFF), ((a >> 4) & 0xF) | ((a >> 12) & 0xF0), a & 0xF);
        g_text(px + 14, y, line, COL_TEXT_DIM, 1); y += 18;
        line[0] = 0;
        if (dd & (1u << 25)) kstrlcat(line, "SSE ", sizeof line);
        if (dd & (1u << 26)) kstrlcat(line, "SSE2 ", sizeof line);
        if (c  & (1u << 0))  kstrlcat(line, "SSE3 ", sizeof line);
        if (c  & (1u << 19)) kstrlcat(line, "SSE4.1 ", sizeof line);
        if (c  & (1u << 28)) kstrlcat(line, "AVX ", sizeof line);
        if (c  & (1u << 31)) kstrlcat(line, "hypervisor", sizeof line);
        g_text(px + 14, y, "Features", COL_TEXT_DIM, 1); y += 18;
        g_text(px + 14, y, line[0] ? line : "-", COL_TEXT_DIM, 1); y += 18;
    }

    /* Windows puts this at the bottom of the properties tab and it is the only
     * line most people read, so it gets the same billing. */
    int sy = py + phh - 26;
    if (sy > y) {
        g_hline(px + 14, sy - 8, pw - 28, COL_PANEL_3);
        g_round(px + 14, sy + 2, 6, 6, 3, d->ok ? COL_GOOD : COL_WARN, 255);
        g_text(px + 28, sy, d->ok ? "This device is working properly."
                                  : "No driver is bound to this device.",
               d->ok ? COL_GOOD : COL_WARN, 1);
    }
    g_clear_clip();
}

/* --------------------------------------------------------------- input ---- */
static void devmgr_scroll(window_t *w, int delta) {
    (void)w;
    D.scroll -= delta * 3;
    int last = D.nrow - D.rows;
    if (last < 0) last = 0;
    if (D.scroll > last) D.scroll = last;
    if (D.scroll < 0) D.scroll = 0;
    gui_request_redraw();
}

static void devmgr_click(window_t *w, int lx, int ly) {
    (void)w;
    if (!D.cw) return;

    int bw = 78;
    if (lx >= D.cw - bw - 12 && lx < D.cw - 12 && ly >= 8 && ly < 32) {
        int keep = D.scroll;
        scan();
        D.scroll = keep;
        gui_request_redraw();
        return;
    }

    int stacked = D.cw < 560;
    int tree_w  = stacked ? D.cw : (D.cw * 3) / 5;
    if (lx >= tree_w) return;
    if (ly < HEAD_H) return;

    int r = (ly - HEAD_H) / ROW_H + D.scroll;
    if (r < 0 || r >= D.nrow) return;
    if (D.row[r].dev < 0) D.open_cat[D.row[r].cat] = !D.open_cat[D.row[r].cat];
    else                  D.sel = D.row[r].dev;
    gui_request_redraw();
}

/* Arrows walk the flattened rows, which means they walk into and out of open
 * categories for free. Left/right fold the category the cursor is in. */
static void devmgr_key(window_t *w, char c) {
    (void)w;
    unsigned char u = (unsigned char)c;
    if (u == KEY_PGUP) { devmgr_scroll(0, +5); return; }
    if (u == KEY_PGDN) { devmgr_scroll(0, -5); return; }

    int cur = -1;
    for (int i = 0; i < D.nrow; i++) if (D.row[i].dev == D.sel) { cur = i; break; }

    if (u == KEY_UP || u == KEY_DOWN) {
        int step = (u == KEY_DOWN) ? 1 : -1;
        int i = (cur < 0) ? (step > 0 ? -1 : D.nrow) : cur;
        for (i += step; i >= 0 && i < D.nrow; i += step)
            if (D.row[i].dev >= 0) { D.sel = D.row[i].dev; break; }
        /* keep the selection on screen */
        for (int k = 0; k < D.nrow; k++)
            if (D.row[k].dev == D.sel) {
                if (k < D.scroll) D.scroll = k;
                if (k >= D.scroll + D.rows) D.scroll = k - D.rows + 1;
                break;
            }
        gui_request_redraw();
    } else if (u == KEY_LEFT || u == KEY_RIGHT) {
        int cat = (D.sel >= 0 && D.sel < D.ndev) ? D.dev[D.sel].cat : 0;
        D.open_cat[cat] = (u == KEY_RIGHT);
        gui_request_redraw();
    }
}

void devmgr_app_init(void) {
    memset(&D, 0, sizeof D);
    D.sel = -1;
    /* The two people open this for are the two that are usually wrong. */
    D.open_cat[CAT_DISPLAY] = 1;
    D.open_cat[CAT_NET]     = 1;
    scan();

    window_t *win = gui_add_window("Device Manager", 700, 460, 0x80CBC4, ICON_DEVICE);
    if (!win) return;
    win->draw   = devmgr_draw;
    win->click  = devmgr_click;
    win->scroll = devmgr_scroll;
    win->key    = devmgr_key;
    win->min_w  = 420; win->min_h = 300;
    win->x = 150; win->y = 90;
}
