/* Sterownik zegara czasu rzeczywistego (RTC / CMOS)
 *
 * Uklad RTC udostepnia rejestry przez parę portow: do 0x70 piszemy
 * numer rejestru, z 0x71 czytamy/zapisujemy jego wartosc. Rejestry
 * 0-9 to czas (sekundy, minuty, godziny, dzien, miesiac, rok).
 *
 * Dwie pulapki, ktore tu obslugujemy:
 *  - aktualizacja w toku (UIP, bit 7 rejestru A) - czytanie w jej
 *    trakcie da niespojny czas; czekamy, az opadnie, i czytamy dwa
 *    razy az do zgodnosci,
 *  - format BCD vs binarny (rejestr B, bit 2) - QEMU domyslnie BCD.
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

    /* Czytaj ponownie, az dwa odczyty sie zgodza (czas mogl tyknac) */
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
    if (!(regb & 0x04)) {         /* tryb BCD -> przelicz na binarny */
        a.sec = bcd_to_bin(a.sec);
        a.min = bcd_to_bin(a.min);
        /* zachowaj bit PM (0x80) przy dekodowaniu godziny w 12h */
        a.hour = (bcd_to_bin(a.hour & 0x7F)) | (a.hour & 0x80);
        a.day = bcd_to_bin(a.day);
        a.month = bcd_to_bin(a.month);
        a.year = bcd_to_bin((uint8_t)a.year);
    }

    if (!(regb & 0x02) && (a.hour & 0x80))   /* tryb 12h, PM */
        a.hour = ((a.hour & 0x7F) + 12) % 24;

    t->sec = a.sec;
    t->min = a.min;
    t->hour = a.hour;
    t->day = a.day;
    t->month = a.month;
    t->year = (uint16_t)a.year + 2000;       /* rejestr trzyma 2 cyfry */
}
