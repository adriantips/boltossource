/* Minimal SNTP client (RFC 4330) -- enough to set the clock correctly, no
 * more. The RTC is the only time source this machine has, and on a VM it's
 * often wrong from the moment it boots, which quietly breaks the things that
 * matter most: TLS certificate validity windows, file timestamps, any
 * reasoning about "when did this happen" in the logs. One UDP round trip
 * fixes all of it.
 *
 * SNTP rather than full NTP on purpose. No discipline loop, no drift
 * estimation, no peer selection -- the machine asks a server what time it is
 * and believes the answer. That's the right trade when the only goal is
 * getting the year right. */
#include <stdint.h>
#include "ntp.h"
#include "net.h"
#include "netif.h"
#include "netcfg.h"
#include "hw.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

#define NTP_PORT   123
#define NTP_PKTLEN 48

/* Seconds between 1900-01-01 (the NTP epoch) and 1970-01-01 (the Unix one). */
#define NTP_UNIX_DELTA 2208988800u

static volatile int      got_reply;
static volatile uint32_t reply_secs;      /* transmit timestamp, NTP seconds */

/* Offset applied to every clock read, in seconds. The RTC hardware is left
 * alone -- writing it needs CMOS bank juggling that differs between chipsets,
 * and an offset is both simpler and reversible. */
static int64_t  clock_offset;
static int      synced;
static uint32_t last_sync_unix;

static void ntp_reply(uint32_t src_ip, uint16_t sport, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)sport;
    if (len < NTP_PKTLEN || got_reply) return;
    /* Bytes 40..43 are the transmit timestamp's integer seconds, big-endian. */
    reply_secs = ((uint32_t)data[40] << 24) | ((uint32_t)data[41] << 16) |
                 ((uint32_t)data[42] << 8)  |  (uint32_t)data[43];
    got_reply = 1;
}

/* --------------------------------------------------------- calendar math ---*/
static int is_leap(uint32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const uint8_t mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };

uint32_t ntp_unix_from_rtc(const struct rtc_time *t) {
    /* Days since 1970-01-01. */
    uint32_t days = 0;
    for (uint32_t y = 1970; y < t->year; y++) days += is_leap(y) ? 366 : 365;
    for (int m = 0; m < t->mon - 1 && m < 12; m++)
        days += mdays[m] + ((m == 1 && is_leap(t->year)) ? 1 : 0);
    days += (uint32_t)t->day - 1;
    return days * 86400u + (uint32_t)t->hour * 3600u +
           (uint32_t)t->min * 60u + t->sec;
}

void ntp_rtc_from_unix(uint32_t unix_secs, struct rtc_time *out) {
    uint32_t days = unix_secs / 86400u, rem = unix_secs % 86400u;
    out->hour = (uint8_t)(rem / 3600);
    out->min  = (uint8_t)((rem % 3600) / 60);
    out->sec  = (uint8_t)(rem % 60);

    uint32_t y = 1970;
    for (;;) {
        uint32_t dy = is_leap(y) ? 366 : 365;
        if (days < dy) break;
        days -= dy; y++;
    }
    out->year = (uint16_t)y;

    int m = 0;
    for (; m < 12; m++) {
        uint32_t dm = mdays[m] + ((m == 1 && is_leap(y)) ? 1 : 0);
        if (days < dm) break;
        days -= dm;
    }
    out->mon = (uint8_t)(m + 1);
    out->day = (uint8_t)(days + 1);
}

/* ---------------------------------------------------------------- client ---*/
int ntp_sync(uint32_t server_ip, uint32_t timeout_ms) {
    struct netif *nif = netif_default();
    if (!nif) return -1;
    if (!server_ip) server_ip = NTP_DEFAULT_SERVER;

    uint8_t pkt[NTP_PKTLEN];
    memset(pkt, 0, sizeof pkt);
    /* LI = 0 (no warning), VN = 4, Mode = 3 (client). */
    pkt[0] = (0 << 6) | (4 << 3) | 3;

    got_reply = 0;
    udp_listen(NTP_PORT, ntp_reply);

    /* Retry rather than fire once. The first send right after DHCP usually has
     * to resolve the gateway's MAC first, and a datagram dropped while ARP is
     * still in flight would otherwise make the whole sync fail -- which is
     * exactly the boot where the clock is most likely to be wrong. */
    int sent = 0;
    for (int attempt = 0; attempt < 3 && !got_reply; attempt++) {
        if (udp_send(nif, server_ip, NTP_PORT, NTP_PORT, pkt, sizeof pkt) == 0) sent = 1;
        uint64_t deadline = pit_ticks() + timeout_ms / 3 + 1;
        while (!got_reply && pit_ticks() < deadline) {
            netif_poll_all();
            __asm__ volatile("pause");
        }
    }
    udp_unlisten(NTP_PORT);
    if (!sent)      return -1;
    if (!got_reply) return -2;

    if (reply_secs < NTP_UNIX_DELTA) return -3;       /* nonsense timestamp */
    uint32_t now_unix = reply_secs - NTP_UNIX_DELTA;

    struct rtc_time rt;
    rtc_now(&rt);
    uint32_t rtc_unix = ntp_unix_from_rtc(&rt);
    clock_offset   = (int64_t)now_unix - (int64_t)rtc_unix;
    synced         = 1;
    last_sync_unix = now_unix;
    return 0;
}

int      ntp_synced(void)      { return synced; }
int64_t  ntp_offset(void)      { return clock_offset; }
uint32_t ntp_last_sync(void)   { return last_sync_unix; }

uint32_t ntp_now_unix(void) {
    struct rtc_time rt;
    rtc_now(&rt);
    int64_t v = (int64_t)ntp_unix_from_rtc(&rt) + clock_offset;
    return v > 0 ? (uint32_t)v : 0;
}

void ntp_now(struct rtc_time *out) {
    if (!synced) { rtc_now(out); return; }
    ntp_rtc_from_unix(ntp_now_unix(), out);
}
