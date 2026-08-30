/* Real-time clock (RTC / CMOS) driver
 *
 * The RTC chip exposes its registers through a pair of ports: we write
 * a register number to 0x70 and read/write its value at 0x71.
 * Registers 0-9 are the time (seconds, minutes, hours, day, month,
 * year).
 *
 * Two traps that we handle here:
 *  - an update in progress (UIP, bit 7 of register A) - reading during
 *    one gives an inconsistent time; we wait for it to fall and read
 *    twice until the two agree,
 *  - BCD versus binary format (register B, bit 2) - QEMU uses BCD by
 *    default.
 */

#include "include/rtc.h"
#include "include/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SEC   0x00
#define RTC_MIN   0x02
#define RTC_HOUR  0x04
#define RTC_DAY   0x07
#define RTC_MONTH 0x08
#define RTC_YEAR  0x09
#define RTC_STA   0x0A
#define RTC_STB   0x0B

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    return cmos_read(RTC_STA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

void rtc_read(struct rtc_time *t)
{
    struct rtc_time a, b;

    while (update_in_progress())
        ;
    a.sec = cmos_read(RTC_SEC);   a.min = cmos_read(RTC_MIN);
    a.hour = cmos_read(RTC_HOUR); a.day = cmos_read(RTC_DAY);
    a.month = cmos_read(RTC_MONTH);
    a.year = cmos_read(RTC_YEAR);

    /* Read again until two reads agree (the clock may have ticked) */
    do {
        b = a;
        while (update_in_progress())
            ;
        a.sec = cmos_read(RTC_SEC);   a.min = cmos_read(RTC_MIN);
        a.hour = cmos_read(RTC_HOUR); a.day = cmos_read(RTC_DAY);
        a.month = cmos_read(RTC_MONTH);
        a.year = cmos_read(RTC_YEAR);
    } while (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
             a.day != b.day || a.month != b.month || a.year != b.year);

    uint8_t regb = cmos_read(RTC_STB);
    if (!(regb & 0x04)) {         /* BCD mode -> convert to binary */
        a.sec = bcd_to_bin(a.sec);
        a.min = bcd_to_bin(a.min);
        /* keep the PM bit (0x80) while decoding a 12h hour */
        a.hour = (bcd_to_bin(a.hour & 0x7F)) | (a.hour & 0x80);
        a.day = bcd_to_bin(a.day);
        a.month = bcd_to_bin(a.month);
        a.year = bcd_to_bin((uint8_t)a.year);
    }

    if (!(regb & 0x02) && (a.hour & 0x80))   /* 12h mode, PM */
        a.hour = ((a.hour & 0x7F) + 12) % 24;

    t->sec = a.sec;
    t->min = a.min;
    t->hour = a.hour;
    t->day = a.day;
    t->month = a.month;
    t->year = (uint16_t)a.year + 2000;       /* the register holds 2 digits */
}
