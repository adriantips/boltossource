#pragma once
#include <stdint.h>
#include "gfx.h"

/* ===========================================================================
 *  BoltOS TV  -  tv/yt.h
 *
 *  YouTube, played by the TV itself: tv/ytplay.c finds and streams a video
 *  (picture through BoltOS Mini's H.264 decoder, sound through tv/aac.c),
 *  tv/youtube.c is the app around it -- search, a grid, the player screen.
 * ===========================================================================*/

/* ---- search (tv/ytplay.c) ------------------------------------------------ */
typedef struct {
    char id[12];
    char title[160];
    char channel[80];
    char meta[80];                 /* "1.2M views · 3 days ago"                */
    char badge[16];                /* "12:34", "LIVE"                          */
} yt_item;

#define YT_MAX_RESULTS 40
/* Ask YouTube (on the network thread); poll until it isn't 0. */
int  yt_search(const char *query);
int  yt_search_poll(void);                    /* 0 running, 1 done, -1 failed  */
const yt_item *yt_results(int *n);
const char *yt_search_error(void);

/* ---- playing ------------------------------------------------------------- */
enum { YT_IDLE = 0, YT_LOADING, YT_PLAYING, YT_PAUSED, YT_ERROR = -1 };

int  yt_open(const char *id);                 /* starts loading; 0 or -1        */
void yt_close(void);
/* Each turn of the shell's loop while the player is up: fetch, decode, keep
 * the sound fed. 1 when the picture or what the screen says changed. */
int  yt_tick(uint64_t now);
int  yt_state(void);
int  yt_buffering(void);
const char *yt_message(void);                 /* "Buffering…", why it failed    */
const surface_t *yt_picture(void);            /* the frame due now, or NULL     */
void yt_toggle(void);                         /* play / pause                    */
void yt_seek(int secs);                       /* relative                        */
int  yt_pos_ms(void);
int  yt_len_ms(void);
int  yt_loaded_ms(void);                      /* how far the download has got    */
const char *yt_title(void);
const char *yt_author(void);
int  yt_has_sound(void);
