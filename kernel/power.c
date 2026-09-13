/* ===========================================================================
 *  BoltOS  -  kernel/power.c
 *  What the machine can say about where its power is coming from.
 *
 *  Very little, and deliberately so. See include/power.h: reading a battery on
 *  a PC means executing the firmware's AML, and there is no AML interpreter
 *  here. This is the seam plus the one fact that is readable without one.
 * ========================================================================= */
#include <stdio.h>
#include "power.h"
#include "acpi.h"

static int (*prov_read)(battery_t *b);

void power_provide(int (*read)(battery_t *b)) { prov_read = read; }

int power_battery(battery_t *b) {
    if (!prov_read || !b) return 0;
    return prov_read(b) == 0;
}

/* ACPI 6.x table 5-34: 0 unspecified, 1 desktop, 2 mobile, 3 workstation,
 * 4 enterprise server, ... 8 tablet. Only 2 and 8 mean "it has a battery" with
 * any confidence; a workstation profile is a machine that plainly does not. */
int power_is_mobile(void) {
    int p = acpi_pm_profile();
    return p == 2 || p == 8;
}

const char *power_summary(void) {
    static char line[48];
    battery_t b;
    if (power_battery(&b) && b.present) {
        snprintf(line, sizeof line, "Battery %d%% %s",
                 b.percent, b.charging ? "charging" : "remaining");
        return line;
    }
    if (power_is_mobile()) return "Battery state unavailable";
    return "On AC power";
}
