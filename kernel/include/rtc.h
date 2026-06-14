#ifndef PROTONOS_RTC_H
#define PROTONOS_RTC_H

#include <stdint.h>

/* Czas odczytany z zegara CMOS (RTC). Uklad pol musi byc identyczny
   ze struct utime w user/usys.h - przekazujemy go przez syscall. */
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
