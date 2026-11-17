/* The time of day: the PL031 real-time clock
 *
 * The virt machine's clock (arm,pl031 in the device tree) is one
 * register that counts seconds since 1970, set by QEMU from the host or
 * from -rtc base=. The calendar is worked out here: days since 1970 to
 * a year, a month and a day by Howard Hinnant's civil-from-days, which
 * is exact for the proleptic Gregorian calendar - no table of months.
 */

#include <stdint.h>
#include "rtc.h"

#define RTCDR 0x00                      /* the seconds */

static volatile uint32_t *rtc;

void rtc_init(uint64_t base)
{
    rtc = (volatile uint32_t *)(uintptr_t)base;
}

int rtc_present(void)
{
    return rtc != 0;
}

uint32_t rtc_seconds(void)
{
    return rtc ? rtc[RTCDR / 4] : 0;
}

void rtc_split(uint32_t t, struct rtc_time *out)
{
    uint32_t days = t / 86400, s = t % 86400;
    out->hour = (int)(s / 3600);
    out->min = (int)(s / 60 % 60);
    out->sec = (int)(s % 60);
    out->wday = (int)((days + 4) % 7);  /* 1 January 1970 was a Thursday */

    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    out->year = (int)(y + (out->month <= 2));
}
