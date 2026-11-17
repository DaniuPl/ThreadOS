#ifndef THREADOS_ARM64_RTC_H
#define THREADOS_ARM64_RTC_H

#include <stdint.h>

/* The PL031 real-time clock (rtc.c) */
struct rtc_time {
    int year, month, day;       /* month 1-12, day 1-31 */
    int hour, min, sec;
    int wday;                   /* 0 Sunday ... 6 Saturday */
};

void rtc_init(uint64_t base);
int rtc_present(void);
uint32_t rtc_seconds(void);     /* since 1 January 1970, UTC */
void rtc_split(uint32_t t, struct rtc_time *out);

#endif
