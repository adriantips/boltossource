/* ===========================================================================
 *  BoltOS  -  drivers/backlight.c
 *  Brightness, and the seam a real backlight driver would arrive through.
 *
 *  There is one provider today and it is not hardware: the compositor's blit
 *  scales every pixel by the level on its way to VRAM. It costs three table
 *  lookups a pixel and nothing at all at 100%, which is where it sits unless
 *  somebody asks for something else.
 * ========================================================================= */
#include "backlight.h"
#include "framebuffer.h"

static const char *prov_name = "software dimmer";
static int (*prov_set)(int pct);          /* 0 = the software dimmer */
static int level = 100;

void backlight_provide(const char *name, int (*set)(int pct)) {
    prov_set  = set;
    prov_name = name && name[0] ? name : (set ? "backlight" : "software dimmer");
    /* Whatever was dim is not dim any more: real hardware is about to own the
     * level, and leaving the software scale in would darken on top of it. */
    fb_set_dim(100);
    backlight_set(level);
}

const char *backlight_name(void) { return prov_name; }
int         backlight_get(void)  { return level; }

int backlight_set(int pct) {
    if (pct < 10)  pct = 10;
    if (pct > 100) pct = 100;
    if (prov_set) {
        if (prov_set(pct) != 0) return -1;
    } else {
        fb_set_dim(pct);
    }
    level = pct;
    return 0;
}
