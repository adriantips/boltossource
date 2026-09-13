#pragma once

/* ===========================================================================
 *  Power source.
 *
 *  What this can honestly say is narrower than anybody would like. A battery
 *  on a PC is an ACPI control-method device: you read its charge by *running*
 *  the _BST method in the firmware's AML bytecode, and BoltOS has no AML
 *  interpreter. There is no port to read and no register to poll; the older
 *  alternatives (APM, and SBS over SMBus) are gone from modern machines.
 *
 *  So this reports what it actually knows. The FADT's preferred power
 *  management profile says whether the firmware thinks this is a laptop, which
 *  is enough to tell "there is no battery here" from "there is a battery here
 *  and I cannot read it" -- and those are different sentences to put in front
 *  of somebody. When an AML interpreter exists, power_provide() is where a
 *  real reading arrives, and nothing above this line changes.
 * ========================================================================= */

typedef struct {
    int present;      /* a battery was actually reported                    */
    int percent;      /* 0..100, meaningless unless present                 */
    int charging;     /* 1 charging, 0 discharging                          */
} battery_t;

/* 1 if `b` was filled in, 0 if nothing can report a battery. */
int power_battery(battery_t *b);

/* A line to show a person: "On AC power", "Battery 62% charging", or
 * "Battery state unavailable" on a machine that has one we cannot read. */
const char *power_summary(void);

/* Whether the firmware describes this machine as portable (FADT preferred
 * power management profile 2 or 3: Mobile, or a Tablet/Appliance profile). */
int power_is_mobile(void);

/* Where a real battery driver arrives. NULL restores "nothing reports one". */
void power_provide(int (*read)(battery_t *b));
