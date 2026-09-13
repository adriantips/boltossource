#pragma once

/* ===========================================================================
 *  Brightness.
 *
 *  The real thing is a panel backlight, and reaching one means a GPU's own
 *  registers -- Intel's BLC_PWM_CTL, the AMD and Nvidia equivalents -- or an
 *  ACPI method, which needs an AML interpreter this OS does not have. None of
 *  it exists on the machine this is developed against: QEMU's display has no
 *  backlight at all. Writing register programming that cannot once be executed
 *  is how you ship a driver that has never worked, which is the same argument
 *  that shaped the Wi-Fi radio seam in include/wifi.h.
 *
 *  So brightness is a seam with one implementation behind it today: a software
 *  dimmer that scales every pixel on its way to the panel. That is what every
 *  desktop OS falls back to for a monitor whose backlight it cannot reach, it
 *  is genuinely what you want at two in the morning, and a real driver
 *  registering itself later changes nothing above this line.
 * ========================================================================= */

/* A real backlight driver takes over by calling this. Passing a NULL setter
 * hands control back to the software dimmer. */
void backlight_provide(const char *name, int (*set)(int pct));

const char *backlight_name(void);   /* what is doing the dimming            */
int  backlight_get(void);           /* 10..100                              */

/* Clamped to 10..100. Nought is a screen nobody can get back from without
 * knowing the shortcut, which is a brick as far as the person holding it is
 * concerned. Returns 0, or -1 if the provider refused. */
int  backlight_set(int pct);
