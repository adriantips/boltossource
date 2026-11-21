#pragma once
#include <stdint.h>
#include "gfx.h"

/* ===========================================================================
 *  BoltOS Mini  -  mini/engine.h
 *
 *  The web as Firefox draws it: the client of the BoltOS Web Engine
 *  (webengine/boltweb.py), a real Firefox that runs on another machine and
 *  sends its page here as H.264, while clicks, keys and the wheel go back.
 *  The Browser (app_browser.c) shows a page with it whenever it's switched
 *  on; mini/engine.c has the protocol and the decoding.
 * ===========================================================================*/
enum { ENG_OFF = 0, ENG_CONNECTING, ENG_LIVE, ENG_ERROR = -1 };

/* The engine's address ("host" or "host:port") and key, and whether pages go
 * to it at all. Kept across restarts. */
int  engine_enabled(void);
void engine_configure(const char *addr, const char *key, int on);
void engine_get_config(char *addr, int acap, char *key, int kcap, int *on);

/* Connect (or stay connected) with a page of w x h pixels; a different size
 * reconnects, and the engine restarts Firefox at it. */
int  engine_start(int w, int h);
void engine_stop(void);                  /* hang up; the decoding core goes back */
int  engine_state(void);                 /* ENG_*                                  */
const char *engine_message(void);        /* what it's doing, or what went wrong    */

/* Each turn of the loop while a page is up. 0 nothing new; 1 the page's
 * address, title or state changed (draw everything); 2 only the picture
 * changed, inside *x,*y,*w,*h (page pixels). */
int  engine_tick(int *x, int *y, int *w, int *h);
const surface_t *engine_picture(void);   /* the page as it is on screen, or NULL   */

void engine_page(char *url, int ucap, char *title, int tcap, int *loading, int *back, int *fwd);
void engine_go(const char *url);
void engine_back(void);
void engine_forward(void);
void engine_reload(void);
void engine_halt(void);                  /* stop loading                           */

/* Input, in page pixels. buttons: 1 left, 2 right, 4 middle; wheel +1 up. */
void engine_pointer(int x, int y, int buttons, int wheel);
void engine_key(int key, int ch, int mods);
/* Firefox's pointer (a hand over links, an I-beam over text), as ARGB with
 * its hot spot; NULL until it sent one. gen changes when it does. */
const uint32_t *engine_cursor(int *w, int *h, int *hx, int *hy, uint32_t *gen);

/* /engine on the dev page: ?set=host:port&key=K, ?on, ?off, ?go=URL */
int  engine_dev(const char *query, char *out, int cap);
