#ifndef THREADOS_HID_H
#define THREADOS_HID_H

#include <stdint.h>

/* HID report descriptors (kernel/hid.c): where in a report each input
   value lies and what it means, and which usages a report says are
   down. Enough for keys - keyboards, media keys - not for joysticks. */

#define HID_FIELDS 32
#define HID_USAGES 16

#define HID_PAGE_KEYBOARD 0x07
#define HID_PAGE_CONSUMER 0x0C

struct hid_field {
    uint8_t report_id;          /* 0 = the reports carry no id */
    uint8_t array;              /* each value names a usage (vs one per usage) */
    uint8_t size;               /* bits per value */
    uint8_t count;              /* how many values */
    uint16_t bit;               /* the first value's place, after the id byte */
    uint16_t page;
    uint16_t umin, umax;        /* the usages as a range (umax >= umin), */
    uint8_t nusages;            /* or as a list */
    uint16_t usages[HID_USAGES];
    uint16_t upage[HID_USAGES]; /* a usage's own page, when it gave one */
    int32_t lmin, lmax;
};

struct hid_map {
    int nfields;
    int ids;                    /* the reports start with an id byte */
    struct hid_field f[HID_FIELDS];
};

/* 0 = understood (possibly with no input fields at all), -1 = not a
   descriptor this parser can follow */
int hid_parse(const uint8_t *desc, int len, struct hid_map *m);

/* Does the map have any field with usages on this page? */
int hid_has_page(const struct hid_map *m, uint16_t page);

/* The usages a report says are down, as page << 16 | usage: buttons
   whose bit is set, and the usages an array names. Returns how many
   were written (at most max). */
int hid_pressed(const struct hid_map *m, const uint8_t *report, int len,
                uint32_t *out, int max);

#endif
