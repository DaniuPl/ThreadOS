/* Putting the kernel's HID report descriptor reader through damaged
 * descriptors and reports
 *
 * A report descriptor comes from whatever was plugged in, and so do the
 * reports read by it; kernel/hid.c has to survive any bytes. The
 * kernel's own source is compiled here as it is (kstub/hid.c is a link
 * to it), with the sanitizers watching.
 *
 * The file: two bytes of the descriptor's length (little-endian), the
 * descriptor, then reports, each a length byte and that many bytes.
 *
 * Usage: hid_harness <file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hid.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    static unsigned char in[1 << 16];
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(in, 1, sizeof(in), f);
    fclose(f);
    if (len < 2)
        return 0;
    size_t dlen = in[0] | (size_t)in[1] << 8;
    if (dlen > len - 2)
        dlen = len - 2;

    /* Buffers of exactly their lengths: a read past the end is a read
       past an allocation, and the sanitizer sees it */
    unsigned char *desc = malloc(dlen ? dlen : 1);
    memcpy(desc, in + 2, dlen);
    static struct hid_map map;
    int ok = hid_parse(desc, (int)dlen, &map);
    free(desc);
    if (ok < 0)
        return 0;
    if (map.nfields < 0 || map.nfields > HID_FIELDS)
        abort();
    hid_has_page(&map, HID_PAGE_CONSUMER);

    for (size_t at = 2 + dlen; at < len;) {
        size_t n = in[at++];
        if (n > len - at)
            n = len - at;
        unsigned char *r = malloc(n ? n : 1);
        memcpy(r, in + at, n);
        uint32_t out[16];
        int got = hid_pressed(&map, r, (int)n, out, 16);
        if (got < 0 || got > 16)
            abort();
        free(r);
        at += n;
    }
    return 0;
}
