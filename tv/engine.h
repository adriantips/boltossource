#pragma once
#include <stdint.h>
#include "gfx.h"

/* ===========================================================================
 *  BoltOS TV  -  tv/engine.h
 *
 *  The streaming services, through a real Firefox on another computer (the
 *  BoltOS Web Engine, webengine/): tv/engine.c is the link, tv/stream.c the
 *  app around it.
 * ===========================================================================*/
enum { ENG_OFF = 0, ENG_CONNECTING, ENG_LIVE, ENG_ERROR = -1 };

/* Where the engine is: "host", "host:port" or "key@host:port". Kept in
 * PREFS.CFG on the stick. */
void engine_get_address(char *out, int cap);
void engine_set_address(const char *addr);
int  engine_configured(void);

/* Connect (on a thread of its own) with a page of w x h, and open url. */
int  engine_start(const char *url, int w, int h);
void engine_stop(void);
int  engine_state(void);
const char *engine_message(void);
/* Each frame while the app is up: decode what came. 1 when there's a new
 * picture or the page's state changed. */
int  engine_tick(void);
const surface_t *engine_picture(void);
void engine_page(char *url, int ucap, char *title, int tcap, int *loading, int *back, int *fwd);
void engine_go(const char *url);
void engine_back(void);
void engine_reload(void);
/* Input, in page pixels; buttons 1 left, 2 right, 4 middle; X keysyms. */
void engine_mouse(int x, int y, int buttons);
void engine_wheel(int dy);                     /* +1 up */
void engine_keysym(uint32_t sym, int mods);
const uint32_t *engine_cursor(int *w, int *h, int *hx, int *hy);
void engine_stats(char *out, int cap);
