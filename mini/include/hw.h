#pragma once
#include <stdint.h>

/* BoltOS Mini: the one piece of the desktop's hw.h the shared code uses,
 * the wall clock (net/x509.c checks certificate dates against it). It comes
 * from SNTP or the Mac (mini/inet.c, pi.py). */
struct rtc_time { uint16_t year; uint8_t mon, day, hour, min, sec; };
void rtc_now(struct rtc_time *t);
