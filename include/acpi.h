#pragma once
#include <stdint.h>

/* ===========================================================================
 *  ACPI (Advanced Configuration and Power Interface).
 *
 *  We locate the RSDP in the BIOS area, walk the RSDT/XSDT, and cache the
 *  tables that matter for a desktop OS: FADT (power management + reset), MADT
 *  (Local APIC / IO-APIC topology for SMP + interrupt routing) and HPET. AML is
 *  not interpreted in full; \_S5 is recovered with a targeted DSDT byte scan so
 *  a clean ACPI poweroff works under QEMU and on real firmware.
 * ===========================================================================*/

/* All System Description Tables start with this header. */
struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* The FADT's preferred power management profile (ACPI 6.x table 5-34): 1 is a
 * desktop, 2 mobile, 3 a workstation, 8 a tablet. 0 when there is no FADT or
 * the firmware did not say. It is the only thing about batteries that can be
 * read without running the firmware's AML -- see include/power.h. */
int acpi_pm_profile(void);

/* Generic Address Structure (used by FADT reset reg, HPET base, ...). */
struct acpi_gas {
    uint8_t  address_space;   /* 0 = system memory, 1 = system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* Parsed MADT topology, filled by acpi_init(). */
#define ACPI_MAX_CPUS    64
#define ACPI_MAX_IOAPIC  8
#define ACPI_MAX_ISO     32

struct acpi_ioapic {
    uint8_t  id;
    uint32_t address;          /* MMIO base */
    uint32_t gsi_base;         /* global system interrupt base */
};
struct acpi_iso {              /* interrupt source override */
    uint8_t  bus;
    uint8_t  source;           /* legacy IRQ */
    uint32_t gsi;              /* remapped global system interrupt */
    uint16_t flags;            /* polarity/trigger */
};

struct acpi_info {
    int      present;
    int      revision;
    uint64_t lapic_addr;       /* local APIC MMIO base */

    int      cpu_count;
    uint8_t  cpu_lapic_id[ACPI_MAX_CPUS];

    int      ioapic_count;
    struct acpi_ioapic ioapic[ACPI_MAX_IOAPIC];

    int      iso_count;
    struct acpi_iso iso[ACPI_MAX_ISO];

    uint64_t hpet_addr;        /* 0 if no HPET */
    uint8_t  hpet_seq;
};

void acpi_init(void);
const struct acpi_info *acpi_get(void);

/* Find a cached table by 4-char signature ("APIC","HPET","FACP"...). NULL if
 * absent. Returned pointer is in the direct map and valid for the kernel's life. */
const struct acpi_sdt_header *acpi_find(const char *sig);

/* Power: clean ACPI poweroff / reset. Both fall back to legacy paths (QEMU
 * debug-exit ports, 0xCF9, 8042) if ACPI is unavailable. Neither returns on
 * success. */
void acpi_poweroff(void);

/* --- sleep states ---------------------------------------------------------
 *  The SLP_TYP values the firmware's DSDT names for each \_Sn_ state, already
 *  shifted into position for the PM1 control register. 1 if this machine
 *  declares that state at all, which is the only way to know: there is no
 *  capability bit for S3 anywhere, only the presence of a \_S3_ package.
 * ------------------------------------------------------------------------*/
int  acpi_sleep_type(int n, uint16_t *slp_a, uint16_t *slp_b);
void acpi_pm1_ports(uint16_t *a, uint16_t *b);

/* Leave `phys` in the FACS for the firmware to jump to on wake. Returns -1 if
 * there is no FACS to leave it in, which means suspending would be a one-way
 * trip. Must be below 1 MiB: the CPU comes back in real mode. */
int  acpi_set_waking_vector(uint32_t phys);
void acpi_reboot(void);

/* Translate a legacy ISA IRQ to its Global System Interrupt via MADT overrides
 * (identity if none). Used by the IO-APIC wiring. */
uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t *flags_out);
