/* Konsola tekstowa na buforze obrazu
 *
 * Osiem na osiem pikseli na znak, ta sama czcionka, co w reszcie
 * grafiki, szesnascie kolorow trybu tekstowego przeliczonych na RGB.
 * Blok 80x25 laduje na srodku ekranu - reszta zostaje czarna.
 *
 * Rysujemy wprost do widocznego bufora, bez bufora roboczego: konsola
 * i kompozytor pulpitu nigdy nie sa widoczne naraz, a przepisywanie
 * calego ekranu po kazdej literze nie mialoby sensu.
 */

#include <stdint.h>
#include "include/fbcon.h"
#include "include/font8x8.h"

#define CON_COLS 80
#define CON_ROWS 25
#define GLYPH    8

/* Szesnascie kolorow trybu tekstowego VGA w postaci RGB */
static const uint32_t console_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static uint8_t *fb;
static uint32_t fb_addr, fb_pitch;
static int fb_w, fb_h;
static int con_on, con_x0, con_y0;

int fbcon_set_framebuffer(uint32_t addr, uint32_t pitch, int w, int h, int bpp)
{
    if (bpp != 32 || !addr || w <= 0 || h <= 0 || pitch < (uint32_t)w * 4)
        return -1;
    fb_addr = addr;
    fb_pitch = pitch;
    fb_w = w;
    fb_h = h;
    fb = (uint8_t *)(uintptr_t)addr;
    return 0;
}

int fbcon_have(void)     { return fb != 0; }
uint32_t fbcon_addr(void)  { return fb_addr; }
uint32_t fbcon_bytes(void) { return fb_pitch * (uint32_t)fb_h; }
int fbcon_width(void)    { return fb_w; }
int fbcon_height(void)   { return fb_h; }
int fbcon_pitch(void)    { return (int)fb_pitch; }

int fbcon_enable(void)
{
    if (!fb || fb_w < CON_COLS * GLYPH || fb_h < CON_ROWS * GLYPH)
        return 0;

    /* Blok tekstu na srodku ekranu */
    con_x0 = (fb_w - CON_COLS * GLYPH) / 2;
    con_y0 = (fb_h - CON_ROWS * GLYPH) / 2;

    for (int y = 0; y < fb_h; y++) {
        uint32_t *row = (uint32_t *)(fb + (uint32_t)y * fb_pitch);
        for (int x = 0; x < fb_w; x++)
            row[x] = 0;
    }
    con_on = 1;
    return 1;
}

int fbcon_active(void)
{
    return con_on;
}

void fbcon_cell(int col, int row, char ch, uint8_t attr)
{
    if (!con_on || col < 0 || row < 0 || col >= CON_COLS || row >= CON_ROWS)
        return;

    uint32_t fgc = console_rgb[attr & 0x0F];
    uint32_t bgc = console_rgb[(attr >> 4) & 0x0F];
    const uint8_t *glyph = (ch >= 0x20 && (uint8_t)ch <= 0x7E)
                               ? font8x8[ch - 0x20] : font8x8[0];

    int px = con_x0 + col * GLYPH;
    int py = con_y0 + row * GLYPH;
    for (int y = 0; y < GLYPH; y++) {
        uint32_t *dst = (uint32_t *)(fb + (uint32_t)(py + y) * fb_pitch +
                                     (uint32_t)px * 4);
        uint8_t bits = glyph[y];
        for (int x = 0; x < GLYPH; x++)
            dst[x] = (bits & (1 << x)) ? fgc : bgc;
    }
}
