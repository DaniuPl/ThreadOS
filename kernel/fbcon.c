/* A text console on a framebuffer
 *
 * Eight by eight pixels per character, the same font as in the rest of
 * the graphics, sixteen text-mode colours converted to RGB. The 80x25
 * block lands in the middle of the screen - the rest stays black.
 *
 * We draw straight into the visible buffer, without a working buffer:
 * the console and the desktop compositor are never visible at the same
 * time, and copying the whole screen after every letter would make no
 * sense.
 *
 * We handle 32 and 24 bits per pixel. Thirty-two is what a BIOS gives
 * through VBE, twenty-four is what UEFI firmware can report - the only
 * difference being that in the second case a pixel has no padding byte
 * and must not be written as a word, because it would trample its
 * neighbour.
 */

#include <stdint.h>
#include "include/fbcon.h"
#include "include/font8x8.h"
#include "include/video.h"

#define CON_COLS 80
#define CON_ROWS 25
#define GLYPH    8

/* The sixteen VGA text-mode colours in RGB form */
static const uint32_t console_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static uint8_t *fb;
static uint32_t fb_addr, fb_pitch;
static int fb_w, fb_h, fb_bpp, fb_bypp;

/* One pixel. The byte order is the same for both formats: blue,
   green, red - and at 32 bits there is an extra padding byte which
   means nothing anyway. */
static inline void put_pixel(uint8_t *p, uint32_t rgb)
{
    if (fb_bypp == 4) {
        *(uint32_t *)p = rgb;
    } else {
        p[0] = (uint8_t)(rgb & 0xFF);
        p[1] = (uint8_t)((rgb >> 8) & 0xFF);
        p[2] = (uint8_t)((rgb >> 16) & 0xFF);
    }
}

static int con_on, con_x0, con_y0;

int fbcon_set_framebuffer(uint32_t addr, uint32_t pitch, int w, int h, int bpp)
{
    int bypp;

    if (bpp == 32)      bypp = 4;
    else if (bpp == 24) bypp = 3;
    else                return -1;

    if (!addr || w <= 0 || h <= 0 || pitch < (uint32_t)w * (uint32_t)bypp)
        return -1;
    fb_addr = addr;
    fb_pitch = pitch;
    fb_w = w;
    fb_h = h;
    fb_bpp = bpp;
    fb_bypp = bypp;
    fb = (uint8_t *)(uintptr_t)addr;
    return 0;
}

int fbcon_have(void)     { return fb != 0; }
uint32_t fbcon_addr(void)  { return fb_addr; }
uint32_t fbcon_bytes(void) { return fb_pitch * (uint32_t)fb_h; }
int fbcon_width(void)    { return fb_w; }
int fbcon_height(void)   { return fb_h; }
int fbcon_pitch(void)    { return (int)fb_pitch; }
int fbcon_bpp(void)      { return fb_bpp; }

int fbcon_enable(void)
{
    if (!fb || fb_w < CON_COLS * GLYPH || fb_h < CON_ROWS * GLYPH)
        return 0;

    /* The text block in the middle of the screen */
    con_x0 = (fb_w - CON_COLS * GLYPH) / 2;
    con_y0 = (fb_h - CON_ROWS * GLYPH) / 2;

    for (int y = 0; y < fb_h; y++) {
        uint8_t *row = fb + (uint32_t)y * fb_pitch;
        for (int x = 0; x < fb_w; x++)
            put_pixel(row + (uint32_t)x * (uint32_t)fb_bypp, 0);
    }
    con_on = 1;
    video_front_touched();
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

    /* This writes onto the visible buffer directly, so whatever keeps
       a copy of it has to be told the copy is now stale - and told
       again once the glyph is there: the compositor may be sending a
       frame on another core in between, and the copy it makes then is
       of a screen without the glyph. */
    video_front_touched();

    uint32_t fgc = console_rgb[attr & 0x0F];
    uint32_t bgc = console_rgb[(attr >> 4) & 0x0F];
    const uint8_t *glyph = (ch >= 0x20 && (uint8_t)ch <= 0x7E)
                               ? font8x8[ch - 0x20] : font8x8[0];

    int px = con_x0 + col * GLYPH;
    int py = con_y0 + row * GLYPH;
    for (int y = 0; y < GLYPH; y++) {
        uint8_t *dst = fb + (uint32_t)(py + y) * fb_pitch +
                       (uint32_t)px * (uint32_t)fb_bypp;
        uint8_t bits = glyph[y];
        for (int x = 0; x < GLYPH; x++)
            put_pixel(dst + (uint32_t)x * (uint32_t)fb_bypp,
                      (bits & (1 << x)) ? fgc : bgc);
    }
    video_front_touched();
}
