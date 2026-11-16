/* HID report descriptors
 *
 * A boot keyboard sends the same eight bytes as every other boot
 * keyboard, and the BIOS's protocol is all the keys need. Everything
 * else a HID device says - the loudness keys on a keyboard's second
 * interface, a volume knob, the extra keys of a keyboard that reports
 * more than six at once - comes in a report whose layout the device
 * describes itself, in a small language of items (Device Class
 * Definition for HID 1.11, section 6.2.2):
 *
 *   global items set the state: the usage page, the logical range, how
 *   many bits a value has and how many values follow, the report id
 *   local items name the usages of the next main item, one by one or
 *   as a range, and are forgotten after it
 *   main items lay out the report: an Input of Count values of Size
 *   bits, constant (padding) or data, each value its own usage
 *   (variable) or each naming one (array)
 *
 * The parse turns that into fields with a report id and a bit offset;
 * a report is then read field by field. Output and feature reports are
 * passed over: nothing here writes to a device.
 */

#include <stdint.h>
#include "include/hid.h"
#include "include/string.h"

struct globals {
    uint16_t page;
    int32_t lmin, lmax;
    int lmax_bytes;                       /* how Logical Maximum was written */
    uint32_t size, count;
    uint8_t id;
};

static uint32_t get_u(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t get_s(const uint8_t *p, int n)
{
    uint32_t v = get_u(p, n);
    if (n == 1)
        return (int8_t)v;
    if (n == 2)
        return (int16_t)v;
    return (int32_t)v;
}

int hid_parse(const uint8_t *d, int len, struct hid_map *m)
{
    struct globals g = { 0 }, stack[4];
    int depth = 0;
    /* where the next input value goes, per report id (static: too big
       for a stack, and descriptors are read one at a time, at start) */
    static uint16_t at[256];
    uint16_t usages[HID_USAGES], upages[HID_USAGES];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    int have_min = 0, have_max = 0;

    memset(m, 0, sizeof(*m));
    memset(at, 0, sizeof(at));
    for (int i = 0; i < len;) {
        uint8_t b = d[i];
        if (b == 0xFE) {                  /* a long item: never used for this */
            if (i + 2 >= len)
                return -1;
            i += 3 + d[i + 1];
            continue;
        }
        int n = (b & 3) == 3 ? 4 : (b & 3);
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + 1 + n > len)
            return -1;
        const uint8_t *data = d + i + 1;
        uint32_t u = get_u(data, n);
        i += 1 + n;

        if (type == 1) {                  /* global */
            switch (tag) {
            case 0: g.page = (uint16_t)u; break;
            case 1: g.lmin = get_s(data, n); break;
            case 2: g.lmax = get_s(data, n); g.lmax_bytes = n; break;
            case 7: g.size = u; break;
            case 8:
                if (u == 0 || u > 255)
                    return -1;
                g.id = (uint8_t)u;
                m->ids = 1;
                break;
            case 9: g.count = u; break;
            case 10:                      /* push */
                if (depth == 4)
                    return -1;
                stack[depth++] = g;
                break;
            case 11:                      /* pop */
                if (depth == 0)
                    return -1;
                g = stack[--depth];
                break;
            }
        } else if (type == 2) {           /* local */
            switch (tag) {
            case 0:
                if (nusages < HID_USAGES) {
                    usages[nusages] = (uint16_t)u;
                    upages[nusages] = n == 4 ? (uint16_t)(u >> 16) : 0;
                    nusages++;
                }
                break;
            case 1: umin = u; have_min = 1; break;
            case 2: umax = u; have_max = 1; break;
            }
        } else if (type == 0) {           /* main */
            if (tag == 8) {               /* Input */
                uint32_t bits = g.size * g.count;
                if (g.size == 0 || g.size > 32 || g.count > 255 ||
                    at[g.id] + bits > 65535)
                    return -1;
                int constant = u & 1, variable = (u >> 1) & 1;
                if (!constant && (nusages || (have_min && have_max)) &&
                    m->nfields < HID_FIELDS) {
                    struct hid_field *f = &m->f[m->nfields++];
                    f->report_id = g.id;
                    f->array = !variable;
                    f->size = (uint8_t)g.size;
                    f->count = (uint8_t)g.count;
                    f->bit = at[g.id];
                    f->page = have_min && (umin >> 16) ? (uint16_t)(umin >> 16)
                                                        : g.page;
                    f->lmin = g.lmin;
                    f->lmax = g.lmax;
                    /* A maximum written as the byte 0xFF means 255 to the
                       people who write descriptors, whatever the
                       specification says about signs - and with a
                       minimum of zero nothing else makes sense */
                    if (g.lmin >= 0 && g.lmax < 0)
                        f->lmax = g.lmax_bytes == 1 ? g.lmax & 0xFF
                                : g.lmax_bytes == 2 ? g.lmax & 0xFFFF
                                                    : 0x7FFFFFFF;
                    if (have_min && have_max) {
                        f->umin = (uint16_t)umin;
                        f->umax = (uint16_t)umax;
                    }
                    f->nusages = (uint8_t)nusages;
                    for (int k = 0; k < nusages; k++) {
                        f->usages[k] = usages[k];
                        f->upage[k] = upages[k];
                    }
                }
                at[g.id] = (uint16_t)(at[g.id] + bits);
            }
            /* Output, Feature, Collection and End Collection lay out
               nothing we read; every main item forgets the locals */
            nusages = 0;
            have_min = have_max = 0;
            umin = umax = 0;
        }
    }
    return 0;
}

int hid_has_page(const struct hid_map *m, uint16_t page)
{
    for (int i = 0; i < m->nfields; i++) {
        if (m->f[i].page == page)
            return 1;
        for (int k = 0; k < m->f[i].nusages; k++)
            if (m->f[i].upage[k] == page)
                return 1;
    }
    return 0;
}

/* A value of 'size' bits from 'bit' on, least significant bit first */
static uint32_t bits_at(const uint8_t *r, int len, int bit, int size)
{
    uint32_t v = 0;
    for (int k = 0; k < size; k++) {
        int b = bit + k;
        if (b / 8 >= len)
            break;
        v |= (uint32_t)((r[b / 8] >> (b % 8)) & 1) << k;
    }
    return v;
}

static int32_t value_at(const struct hid_field *f, const uint8_t *r, int len,
                        int i)
{
    uint32_t v = bits_at(r, len, f->bit + i * f->size, f->size);
    /* Signed only when the range says so */
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1)
        return (int32_t)(v | ~((1u << f->size) - 1));
    return (int32_t)v;
}

static uint32_t usage_of(const struct hid_field *f, int i)
{
    if (f->umax >= f->umin && (f->umin || f->umax)) {
        if (f->umin + i > f->umax)
            return 0;
        return (uint32_t)f->page << 16 | (uint32_t)(f->umin + i);
    }
    if (!f->nusages)
        return 0;
    /* past the list, the last usage goes on standing for the rest */
    int k = i < f->nusages ? i : f->nusages - 1;
    return (uint32_t)(f->upage[k] ? f->upage[k] : f->page) << 16 | f->usages[k];
}

int hid_pressed(const struct hid_map *m, const uint8_t *r, int len,
                uint32_t *out, int max)
{
    int id = 0, n = 0;
    if (m->ids) {
        if (len < 1)
            return 0;
        id = r[0];
        r++;
        len--;
    }
    for (int fi = 0; fi < m->nfields; fi++) {
        const struct hid_field *f = &m->f[fi];
        if (f->report_id != id)
            continue;
        if (f->bit + f->size * f->count > len * 8)
            continue;                     /* a report shorter than said */
        for (int i = 0; i < f->count && n < max; i++) {
            int32_t v = value_at(f, r, len, i);
            uint32_t usage;
            if (f->array) {
                if (v < f->lmin || v > f->lmax)
                    continue;             /* nothing in this slot */
                usage = usage_of(f, v - f->lmin);
            } else {
                /* a button: down while its value is not the minimum */
                if (f->lmin != 0 || f->lmax != 1 || !v)
                    continue;
                usage = usage_of(f, i);
            }
            if (usage & 0xFFFF)           /* usage 0 is "nothing" */
                out[n++] = usage;
        }
    }
    return n;
}
