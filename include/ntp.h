#pragma once
#include <stdint.h>

/* ===========================================================================
 *  SNTP client (RFC 4330). See net/ntp.c.
 *
 *  The RTC is the only time source this machine has, and on a VM it is often
 *  wrong from the moment it boots. That quietly breaks the things that matter
 *  most -- TLS certificate validity windows, file timestamps, and any reasoning
 *  about when something happened in the logs. One UDP round trip fixes it.
 *
 *  The RTC hardware is never written. Writing it means CMOS bank juggling that
 *  differs between chipsets, and a correction applied on read is both simpler
 *  and reversible, so the sync is an offset every clock read goes through.
 * ===========================================================================*/

struct rtc_time;

/* pool.ntp.org resolves to a rotating set; 216.239.35.0 (time.google.com) is a
 * stable anycast address that answers everywhere, which is what a default with
 * no DNS available yet needs to be. Host byte order. */
#define NTP_DEFAULT_SERVER 0xD8EF2300u   /* 216.239.35.0 */

/* Ask `server_ip` (0 = the default above) for the time and adopt the answer.
 * 0 = synced, -1 = no interface or the send failed, -2 = timed out,
 * -3 = the server sent a timestamp that predates the NTP epoch. */
int      ntp_sync(uint32_t server_ip, uint32_t timeout_ms);

int      ntp_synced(void);        /* 1 once a sync has succeeded this boot   */
int64_t  ntp_offset(void);        /* seconds added to every RTC read         */
uint32_t ntp_last_sync(void);     /* Unix time of the last successful sync   */

/* The corrected clock: Unix seconds, and the same broken down. Both fall back
 * to the raw RTC before the first sync, so callers never need to check. */
uint32_t ntp_now_unix(void);
void     ntp_now(struct rtc_time *out);

/* Calendar conversion, exposed because the shell and the log formatter want it
 * too. Proleptic Gregorian, UTC, no leap seconds. */
uint32_t ntp_unix_from_rtc(const struct rtc_time *t);
void     ntp_rtc_from_unix(uint32_t unix_secs, struct rtc_time *out);
