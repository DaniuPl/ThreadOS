/* Putting the kernel's TrueType reader through damaged fonts
 *
 * A font is a file, and files arrive from disks and from the network;
 * kernel/ttf.c reads one with every offset checked, and this is where
 * that claim is tested. The kernel's own source is compiled here as it
 * is (kstub/ttf.c is a link to it), with the sanitizers watching.
 *
 * Everything a font can be asked is asked: the character map for a
 * spread of characters, every glyph's advance, and every glyph drawn
 * at a small and a large size - composites inside composites included,
 * because that is where a loop or a runaway offset would hide.
 *
 * Usage: ttf_harness <font file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ttf.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <font file>\n", argv[0]);
        return 2;
    }

    static unsigned char scratch[1 << 20];
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    size_t len = fread(scratch, 1, sizeof(scratch), f);
    fclose(f);

    /* A buffer of exactly the file's length, so a read past its end is
       a read past an allocation and the sanitizer sees it */
    unsigned char *data = malloc(len ? len : 1);
    memcpy(data, scratch, len);

    struct ttf_font font;
    const char *why;
    if (ttf_open(&font, data, (uint32_t)len, &why) < 0) {
        free(data);
        return 0;                         /* refused - which is fine */
    }

    static uint8_t bitmap[192 * 192];
    struct ttf_bitmap b;
    for (uint32_t c = 0; c < 0x2100; c += 7)
        (void)ttf_glyph(&font, c);
    for (int g = 0; g < font.glyphs && g < 2000; g++) {
        (void)ttf_advance(&font, g, 16);
        (void)ttf_render(&font, g, 11, &b, bitmap, sizeof(bitmap));
        (void)ttf_render(&font, g, 90, &b, bitmap, sizeof(bitmap));
    }
    int32_t a, d, gap;
    ttf_vmetrics(&font, 20, &a, &d, &gap);

    free(data);
    return 0;
}
