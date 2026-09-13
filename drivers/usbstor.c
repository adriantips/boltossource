#include <stdint.h>
#include <stddef.h>
#include "usb.h"
#include "blk.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  USB mass storage: bulk-only transport (BOT) carrying SCSI.
 *
 *  Every USB stick, every external drive, and every phone that offers itself
 *  as a disk speaks this. The protocol is three transfers and no state:
 *
 *      out   31-byte Command Block Wrapper -- a tag, a length, a direction,
 *            and a SCSI command block stuffed in the back of it
 *      in/out the data, if the command has any
 *      in    13-byte Command Status Wrapper -- the same tag back, how much
 *            data did not move, and pass/fail
 *
 *  That is genuinely all of it, and it is the reason mass storage was the
 *  first USB class anybody implemented. The parts that are not obvious:
 *
 *  A device that does not like a command does not answer it -- it HALTS the
 *  pipe. Every transfer after that fails until the halt is cleared on both
 *  the controller and the device, and a driver that does not know this reads
 *  one bad sector and then reports the disk as gone. So a failed data stage
 *  clears the halt and still goes on to read the status, because the status is
 *  what says *why*.
 *
 *  And the tag matters. It is there so a status can be matched to the command
 *  it belongs to, and a device that answers with somebody else's tag has lost
 *  sync badly enough that the only honest response is a full reset.
 *
 *  Nothing here knows what a TRB is; see include/usb.h.
 * ===========================================================================*/

#define CBW_SIG   0x43425355u      /* "USBC" */
#define CSW_SIG   0x53425355u      /* "USBS" */
#define CBW_LEN   31
#define CSW_LEN   13

/* SCSI opcodes we issue. A deliberately short list: this is a block device, so
 * it needs to know how big it is and how to move sectors, and nothing else. */
#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A

/* One BOT command moves at most this much. The bounce buffer under the pipe is
 * 64 KiB, and a 17-bit transfer-length field caps a single descriptor just
 * under that; 64 sectors keeps a comfortable margin and is more than any
 * caller here asks for in one go. */
#define MAX_SECTORS   64

#define MAX_STOR      4

typedef struct {
    int        used;
    usb_bulk_t pipe;
    uint32_t   tag;
    uint64_t   blocks;          /* count, not the last LBA                 */
    uint32_t   block_size;
    char       model[48];
    char       name[8];
} stor_t;

static stor_t g_stor[MAX_STOR];
static int    g_count;

/* --- byte order ----------------------------------------------------------
 *  SCSI is big-endian and x86 is not. These four helpers are the whole of the
 *  conversion, spelled out rather than macro'd, because getting one of them
 *  backwards produces a disk whose size is a plausible-looking wrong number.
 * ------------------------------------------------------------------------*/
static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- one command ----------------------------------------------------------
 *  Returns 0 if the device reported the command passed, 1 if it reported a
 *  failure (the caller may want to ask why with REQUEST SENSE), and -1 if the
 *  transport itself came apart.
 * ------------------------------------------------------------------------*/
static int bot_command(stor_t *s, const uint8_t *cdb, uint8_t cdb_len,
                       int dir_in, void *data, uint32_t data_len) {
    uint8_t cbw[CBW_LEN];
    memset(cbw, 0, sizeof cbw);
    uint32_t tag = ++s->tag;
    put_le32(cbw + 0, CBW_SIG);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;                         /* LUN 0; multi-LUN media is rare and
                                          * we do not enumerate past the first */
    cbw[14] = cdb_len & 0x1F;
    memcpy(cbw + 15, cdb, cdb_len > 16 ? 16 : cdb_len);

    if (usb_bulk_xfer(&s->pipe, 0, cbw, CBW_LEN) != CBW_LEN) return -1;

    int moved = 0;
    if (data_len) {
        moved = usb_bulk_xfer(&s->pipe, dir_in, data, data_len);
        /* A failed data stage is not the end of the exchange. The device still
         * owes us a status, and the status is the only thing that says whether
         * this was a rejected command or a broken pipe. Clear the halt and go
         * and read it. */
        if (moved < 0) usb_bulk_clear_halt(&s->pipe, dir_in);
    }

    uint8_t csw[CSW_LEN];
    memset(csw, 0, sizeof csw);
    int got = usb_bulk_xfer(&s->pipe, 1, csw, CSW_LEN);
    if (got < 0) {
        /* The status endpoint itself halted: clear it and try once more. This
         * is the documented recovery and it does work; a device that fails it
         * twice is one we stop talking to. */
        usb_bulk_clear_halt(&s->pipe, 1);
        got = usb_bulk_xfer(&s->pipe, 1, csw, CSW_LEN);
    }
    if (got != CSW_LEN) return -1;
    if (rd_le32(csw + 0) != CSW_SIG) return -1;
    if (rd_le32(csw + 4) != tag) {
        /* Answering with the wrong tag means the device and we disagree about
         * which command is in flight. There is no recovering that by carrying
         * on; the transport has to be reset. */
        usb_control(&s->pipe, 0x21, 0xFF, 0, s->pipe.iface, NULL, 0);
        usb_bulk_clear_halt(&s->pipe, 1);
        usb_bulk_clear_halt(&s->pipe, 0);
        return -1;
    }
    if (csw[12] == 2) {                          /* phase error: full reset */
        usb_control(&s->pipe, 0x21, 0xFF, 0, s->pipe.iface, NULL, 0);
        usb_bulk_clear_halt(&s->pipe, 1);
        usb_bulk_clear_halt(&s->pipe, 0);
        return -1;
    }
    if (csw[12] != 0) return 1;                  /* command failed, transport fine */
    if (data_len && moved < 0) return 1;
    return 0;
}

/* Ask why the last command failed, and throw the answer away apart from the
 * sense key -- which is the one byte that distinguishes "no medium" from
 * "not ready yet" from "you asked for a sector that is not there". */
static int sense_key(stor_t *s) {
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    memset(sense, 0, sizeof sense);
    if (bot_command(s, cdb, 6, 1, sense, sizeof sense) != 0) return -1;
    return sense[2] & 0x0F;
}

/* A freshly attached device is often not ready for the first second or so --
 * spinning up, or simply deciding it exists. The spec's answer is to ask
 * repeatedly, and a driver that asks once reports half the drives in the world
 * as empty. */
static int wait_ready(stor_t *s) {
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 20; i++) {
        int rc = bot_command(s, cdb, 6, 0, NULL, 0);
        if (rc == 0) return 0;
        if (rc < 0) return -1;
        int k = sense_key(s);
        if (k == 2) continue;                    /* NOT READY: ask again      */
        if (k == 6) continue;                    /* UNIT ATTENTION: media just
                                                  * arrived; the next one passes */
        if (k == 3) continue;                    /* MEDIUM ERROR on the probe  */
        return -1;
    }
    return -1;
}

/* --- block layer ---------------------------------------------------------- */

static int rw(stor_t *s, uint64_t lba, uint32_t count, void *buf, int write) {
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > MAX_SECTORS ? MAX_SECTORS : count;
        uint8_t cdb[10];
        memset(cdb, 0, sizeof cdb);
        cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
        be32(cdb + 2, (uint32_t)lba);
        be16(cdb + 7, (uint16_t)n);
        if (bot_command(s, cdb, 10, write ? 0 : 1, p, n * s->block_size) != 0)
            return -1;
        lba += n; count -= n; p += (uint64_t)n * s->block_size;
    }
    return 0;
}

static int stor_read(blkdev_t *b, uint64_t lba, uint32_t count, void *buf) {
    return rw((stor_t *)b->drv, lba, count, buf, 0);
}
static int stor_write(blkdev_t *b, uint64_t lba, uint32_t count, const void *buf) {
    return rw((stor_t *)b->drv, lba, count, (void *)buf, 1);
}
/* No flush. SYNCHRONIZE CACHE (0x35) exists, but a large fraction of USB
 * bridges answer it with a stall, and a barrier that fails is worse than one
 * that is honestly absent -- blk_flush() treats a null hook as "this device
 * needs none". TODO: issue it, and treat a stall as success, once there is a
 * real stick here to find out which of those two cases is more common. */

void usbstor_attach(const usb_bulk_t *p) {
    if (!p || g_count >= MAX_STOR) return;
    stor_t *s = &g_stor[g_count];
    memset(s, 0, sizeof *s);
    s->pipe = *p;
    s->block_size = BLK_SECTOR;

    /* How many logical units. We only ever use the first, but asking is what
     * tells a device that does not support the request to stall EP0 -- which
     * has to be cleared before anything else is tried. */
    uint8_t maxlun = 0;
    if (usb_control(&s->pipe, 0xA1, 0xFE, 0, s->pipe.iface, &maxlun, 1) != 0)
        maxlun = 0;

    uint8_t inq[36];
    memset(inq, 0, sizeof inq);
    uint8_t cdb_inq[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    if (bot_command(s, cdb_inq, 6, 1, inq, sizeof inq) != 0) {
        kprintf("[usbstor] %x:%x did not answer INQUIRY\n", p->vendor, p->product);
        return;
    }
    /* Vendor (8) + product (16), space padded. Joined with one space and with
     * the padding trimmed, because "SanDisk         Cruzer          " on a
     * disk listing is not a model name, it is a column alignment accident. */
    {
        int n = 0;
        for (int i = 8; i < 32 && n < (int)sizeof s->model - 2; i++) {
            char c = (char)inq[i];
            if (c < 32 || c > 126) c = ' ';
            if (c == ' ' && (n == 0 || s->model[n - 1] == ' ')) continue;
            s->model[n++] = c;
        }
        while (n > 0 && s->model[n - 1] == ' ') n--;
        s->model[n] = 0;
        if (!n) strncpy(s->model, "USB disk", sizeof s->model - 1);
    }

    if (wait_ready(s) != 0) {
        kprintf("[usbstor] %s never became ready (no medium?)\n", s->model);
        return;
    }

    uint8_t cap[8];
    memset(cap, 0, sizeof cap);
    uint8_t cdb_cap[10];
    memset(cdb_cap, 0, sizeof cdb_cap);
    cdb_cap[0] = SCSI_READ_CAPACITY10;
    if (bot_command(s, cdb_cap, 10, 1, cap, sizeof cap) != 0) {
        kprintf("[usbstor] %s did not answer READ CAPACITY\n", s->model);
        return;
    }
    /* READ CAPACITY(10) reports the LAST addressable block, not the count. Off
     * by one here and the last sector of every USB disk is unreachable, which
     * shows up as a filesystem that works until it is nearly full. */
    uint32_t last = rd_be32(cap + 0);
    uint32_t bsz  = rd_be32(cap + 4);
    if (bsz == 0) bsz = BLK_SECTOR;
    if (bsz != BLK_SECTOR) {
        /* 4K-native media exists. The block layer here is 512-only, and
         * pretending otherwise would corrupt it quietly rather than loudly. */
        kprintf("[usbstor] %s has %u-byte blocks; only %u is supported\n",
                s->model, bsz, BLK_SECTOR);
        return;
    }
    s->block_size = bsz;
    s->blocks = (uint64_t)last + 1;

    s->name[0] = 'u'; s->name[1] = 's'; s->name[2] = 'b';
    s->name[3] = (char)('0' + g_count); s->name[4] = 0;

    blkdev_t b;
    memset(&b, 0, sizeof b);
    b.sectors     = s->blocks;
    b.sector_size = s->block_size;
    b.is_ssd      = 1;                 /* flash, near enough, and never a platter */
    b.is_boot     = 0;
    b.drv         = s;
    b.read        = stor_read;
    b.write       = stor_write;
    b.flush       = 0;
    strncpy(b.name, s->name, sizeof b.name - 1);
    strncpy(b.model, s->model, sizeof b.model - 1);
    if (blk_register(&b) < 0) {
        kprintf("[usbstor] no room in the block table for %s\n", s->model);
        return;
    }

    g_count++;
    uint64_t mb = (s->blocks * s->block_size) / (1024 * 1024);
    kprintf("[ok] USB disk %s: %s, %lu MiB (%lu blocks%s)\n",
            s->name, s->model, (unsigned long)mb, (unsigned long)s->blocks,
            maxlun ? ", LUN 0 of several" : "");
}

int usbstor_count(void) { return g_count; }
const char *usbstor_name(int i) {
    return (i >= 0 && i < g_count) ? g_stor[i].name : "";
}
