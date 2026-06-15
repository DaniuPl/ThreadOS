/* Sterownik trybu graficznego VGA 13h (320x200, 256 kolorow)
 *
 * W trybie chronionym nie mamy BIOS-u, wiec tryb ustawiamy bezposrednio
 * programujac rejestry VGA standardowymi wartosciami dla trybu 13h
 * (Misc, Sequencer, CRTC, Graphics Controller, Attribute Controller).
 * Framebuffer to liniowy obszar 320*200 bajtow pod 0xA0000 - kazdy bajt
 * to indeks koloru w palecie DAC. 0xA0000 lezy w pierwszym MiB, wiec
 * jest juz w mapowaniu identycznosciowym jadra.
 */

#include "include/video.h"
#include "include/io.h"

static volatile uint8_t *const fb = (uint8_t *)0xA0000;

/* Standardowe wartosci rejestrow trybu 13h: Misc, 5x SEQ, 25x CRTC,
   9x GC, 21x AC (kolejnosc jak w write_regs ponizej). */
static uint8_t mode13[] = {
    /* MISC */
    0x63,
    /* SEQ */
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    /* GC */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    /* AC */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};

static void write_regs(uint8_t *r)
{
    outb(0x3C2, *r++);                       /* Misc output */

    for (uint8_t i = 0; i < 5; i++) {        /* Sequencer */
        outb(0x3C4, i);
        outb(0x3C5, *r++);
    }

    /* Odblokuj rejestry CRTC (wyczysc bit ochrony w CR11) */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);

    for (uint8_t i = 0; i < 25; i++) {       /* CRTC */
        outb(0x3D4, i);
        outb(0x3D5, *r++);
    }

    for (uint8_t i = 0; i < 9; i++) {        /* Graphics Controller */
        outb(0x3CE, i);
        outb(0x3CF, *r++);
    }

    for (uint8_t i = 0; i < 21; i++) {       /* Attribute Controller */
        inb(0x3DA);                          /* reset zatrzasku indeks/dane */
        outb(0x3C0, i);
        outb(0x3C0, *r++);
    }

    inb(0x3DA);                              /* odblokuj palete, wlacz obraz */
    outb(0x3C0, 0x20);
}

void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    outb(0x3C8, idx);
    outb(0x3C9, r);                          /* skladowe 6-bitowe (0..63) */
    outb(0x3C9, g);
    outb(0x3C9, b);
}

void video_init(void)
{
    write_regs(mode13);

    /* Podstawowa paleta: kilka czytelnych kolorow na ustalonych indeksach */
    video_set_palette(0, 0, 0, 0);           /* czarny */
    video_set_palette(1, 63, 0, 0);          /* czerwony */
    video_set_palette(2, 0, 63, 0);          /* zielony */
    video_set_palette(3, 0, 0, 63);          /* niebieski */
    video_set_palette(4, 63, 63, 63);        /* bialy */
    video_set_palette(5, 63, 63, 0);         /* zolty */

    video_clear(0);
}

void video_clear(uint8_t color)
{
    for (int i = 0; i < VIDEO_W * VIDEO_H; i++)
        fb[i] = color;
}

void video_putpixel(int x, int y, uint8_t color)
{
    if ((unsigned)x < VIDEO_W && (unsigned)y < VIDEO_H)
        fb[y * VIDEO_W + x] = color;
}

void video_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            video_putpixel(x + i, y + j, color);
}
