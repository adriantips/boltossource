#pragma once

/* Bring up the first xHCI USB 3.x host controller (PCI class 0Ch sub 03h
 * prog-if 30h), enumerate attached devices, and report them. Polled, no
 * interrupts. Safe no-op if no controller is present. */
void xhci_init(void);
void xhci_poll(void);   /* poll USB HID keyboards; call from idle loops */
/* Armed USB boot keyboards / mice, for the boot log's input summary. */
void xhci_hid_counts(int *keyboards, int *mice);
int  xhci_irq_driven(void);   /* 1 = HID on interrupts, 0 = polled */
/* A few lines saying what the controller is and what is attached to it, meant
 * to be printed at the very end of a boot where the log above has scrolled. */
void xhci_report(void);

/* One armed HID endpoint. `reports` is how many have actually arrived, which
 * is the only way to tell a keyboard that was never found from one that is
 * sitting there armed and silent. */
struct xhci_hid {
    uint8_t  slot, dci, proto;    /* proto: 1 = keyboard, 2 = mouse */
    uint16_t rptlen;
    uint32_t reports;
};
int xhci_hid_get(int i, struct xhci_hid *out);
