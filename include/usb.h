#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/usb.h
 *  The seam between a USB host controller and a class driver.
 *
 *  drivers/xhci.c knows about rings, doorbells, slots and transfer descriptors.
 *  drivers/usbstor.c knows about command blocks, SCSI and logical blocks.
 *  Neither has any business knowing the other, and this is the whole of what
 *  passes between them: a pair of bulk endpoints that have already been
 *  configured, a way to push bytes through either of them, and a control pipe
 *  for the two class requests mass storage needs.
 *
 *  The seam is shaped this way for the same reason include/wifi.h's radio is:
 *  one day somebody writes an EHCI driver for a machine too old to have xHCI,
 *  and when they do, nothing above this line should have to change. It is also
 *  the reason usbstor.c can be read on its own -- bulk-only transport is
 *  fiddly enough without a TRB in the middle of it.
 *
 *  No interrupts, no queueing, no overlap: one transfer at a time, and
 *  usb_bulk_xfer() returns when it is done or has given up. A disk read is
 *  already a blocking operation everywhere above the block layer.
 * ========================================================================= */

typedef struct usb_bulk {
    void    *hc;               /* host-controller private handle            */
    uint16_t vendor, product;
    uint8_t  iface;            /* bInterfaceNumber, for the class requests  */
    char     port[8];          /* "usb1" -- where it is plugged in          */
} usb_bulk_t;

/* Move up to `len` bytes through the pipe. dir_in = 1 for device->host.
 * Returns the number of bytes actually transferred (which may be short, and a
 * short read is not an error in this protocol), or -1 if the transfer failed
 * or the endpoint halted. A halted endpoint is cleared before returning, so
 * the next call starts from a working pipe. */
int usb_bulk_xfer(usb_bulk_t *p, int dir_in, void *buf, uint32_t len);

/* A control transfer on endpoint 0. Mass storage needs exactly two:
 * GET_MAX_LUN and the Bulk-Only Mass Storage Reset. Returns 0 on success. */
int usb_control(usb_bulk_t *p, uint8_t bmReqType, uint8_t bReq,
                uint16_t wValue, uint16_t wIndex, void *buf, uint16_t len);

/* Clear a halt on one of the pipe's endpoints (dir_in picks which). This is
 * the recovery half of bulk-only transport and is not optional: a device that
 * rejects a command halts the pipe, and every transfer after that fails until
 * somebody clears it. */
int usb_bulk_clear_halt(usb_bulk_t *p, int dir_in);

/* The host controller calls this once it has configured a bulk-only
 * mass-storage interface. `p` is copied; the caller may reuse its struct. */
void usbstor_attach(const usb_bulk_t *p);

int         usbstor_count(void);          /* how many USB disks are attached */
const char *usbstor_name(int i);          /* "usb0", for the shell           */
