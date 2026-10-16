#ifndef THREADOS_RTC_H
#define THREADOS_RTC_H

#include <stdint.h>

/* Time read from the CMOS clock (RTC). The field layout must be
   identical to struct utime in user/usys.h - we pass it through a
   syscall. */
struct rtc_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
} __attribute__((packed));

void rtc_read(struct rtc_time *t);

#endif
