#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS notifications
 *
 *  gui_toast() showed one message, bottom-right, for two and a half seconds,
 *  and then it was gone. Anything that happened while you were looking at
 *  another part of the screen -- a download finishing, a commit failing, the
 *  battery getting low -- happened silently as far as you were concerned.
 *  A notification that cannot be read late is not a notification, it is an
 *  animation.
 *
 *  So notifications are kept. This is the store: a fixed ring in .bss, no
 *  allocation and no failure path, posted to from anywhere in the kernel
 *  (including before the GUI exists) and read back by the notification centre,
 *  the `notify` command, and whatever comes next.
 * ===========================================================================*/

enum { NOTE_INFO = 0, NOTE_SUCCESS, NOTE_WARN, NOTE_ERROR };

#define NOTE_MAX       64      /* ring capacity; oldest is dropped when full */
#define NOTE_SRC_MAX   20
#define NOTE_TEXT_MAX  120

typedef struct {
    uint32_t id;                    /* monotonic; never 0 for a live entry    */
    uint8_t  level;                 /* NOTE_*                                  */
    uint8_t  read;                  /* seen in the notification centre         */
    uint8_t  toasted;               /* its toast has been shown and expired    */
    uint8_t  hour, min;             /* wall clock when posted                  */
    uint64_t posted;                /* pit tick, for toast expiry              */
    char     source[NOTE_SRC_MAX];  /* the app or subsystem that posted it     */
    char     text[NOTE_TEXT_MAX];
} notification_t;

/* Post one. Returns its id (0 only if text is empty). `source` may be NULL,
 * which is recorded as "System". Safe to call from any context that can
 * kprintf -- notifications are mirrored into the kernel log so a headless run
 * and `dmesg` see them too. */
uint32_t notify_post(int level, const char *source, const char *text);

/* Convenience wrappers, so a call site reads as what it is. */
uint32_t notify_info(const char *source, const char *text);
uint32_t notify_warn(const char *source, const char *text);
uint32_t notify_error(const char *source, const char *text);
uint32_t notify_success(const char *source, const char *text);

int  notify_count(void);                    /* how many are stored (<= NOTE_MAX) */
const notification_t *notify_at(int i);     /* i = 0 is the newest; NULL past end */
const notification_t *notify_by_id(uint32_t id);
int  notify_unread(void);
void notify_mark_read(uint32_t id);
void notify_mark_all_read(void);
int  notify_dismiss(uint32_t id);           /* remove one; 1 if it was there      */
void notify_clear(void);                    /* remove all                          */

/* The compositor registers here so a notification posted from a driver or the
 * network stack still lights up the desktop. NULL unregisters. */
void notify_set_hook(void (*fn)(const notification_t *));

/* Level name ("info", "success", "warn", "error") and the reverse. */
const char *notify_level_name(int level);
int         notify_level_from_name(const char *name);   /* -1 if unknown */
