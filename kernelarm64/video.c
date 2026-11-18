/* The screen on ARM64: QEMU's ramfb, under the shared drawing code
 *
 * QEMU's virt machine has no display of its own; the ramfb device is
 * one a machine describes for itself: the kernel takes memory, fills
 * in a configuration - where, what pixel format, how large - and
 * writes it into the fw_cfg file etc/ramfb. From then on QEMU shows
 * that memory. So the kernel chooses the size, and it chooses a
 * phone's: 720 x 1280, upright, at two pixels to a logical one, as a
 * phone draws - 360 x 640 of layout.
 *
 * The drawing itself is the PC's (kernel/gfx.c, kernel/uitext.c,
 * kernel/mark.c): they ask kernel/include/video.h for rows of pixels
 * and the scale, and this file is what answers on this machine. They
 * draw into a buffer of the kernel's own, and video_present() copies it
 * onto the screen whole, so nothing half drawn is ever shown.
 */

#include <stdint.h>
#include "fwcfg.h"
#include "video64.h"
#include "../kernel/include/video.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/font8x8.h"

#define FOURCC_XRGB8888 0x34325258u    /* 'X' 'R' '2' '4' */

static uint32_t *front, *back;
static int pw, ph, k16 = 16;
static uint16_t ramfb_item;

/* ramfb is told where the screen's memory is and how it is laid out;
   told again, the screen changes - QEMU's window with it */
static int ramfb_describe(int width, int height)
{
    struct {
        uint64_t addr;
        uint32_t fourcc, flags, width, height, stride;
    } __attribute__((packed)) cfg;
    cfg.addr = __builtin_bswap64((uint64_t)(uintptr_t)front);
    cfg.fourcc = __builtin_bswap32(FOURCC_XRGB8888);
    cfg.flags = 0;
    cfg.width = __builtin_bswap32((uint32_t)width);
    cfg.height = __builtin_bswap32((uint32_t)height);
    cfg.stride = __builtin_bswap32((uint32_t)width * 4);
    return fwcfg_write(ramfb_item, &cfg, sizeof(cfg));
}

static void *pages(uint32_t bytes)
{
    uint32_t a = pmm_alloc_contiguous((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    return (void *)(uintptr_t)a;
}

int video_ramfb_init(int width, int height, int scale16)
{
    uint16_t item;
    uint32_t size;
    if (fwcfg_find("etc/ramfb", &item, &size) < 0)
        return -1;
    uint32_t bytes = (uint32_t)width * (uint32_t)height * 4;
    front = pages(bytes);
    back = pages(bytes);
    if (!front || !back)
        return -2;
    memset(front, 0, bytes);
    memset(back, 0, bytes);
    ramfb_item = item;
    if (ramfb_describe(width, height) < 0)
        return -3;
    pw = width;
    ph = height;
    k16 = scale16;
    return 0;
}

/* On its side, or upright: the width and height swapped. A screen the
   kernel describes is described the other way round - QEMU's window
   turns, and the touchscreen, which reports where on that window the
   finger is, with it. (A fixed panel would keep its shape and have the
   picture turned on its way to it instead, and the finger's axes.) */
int video_set_landscape(int on)
{
    if (!front)
        return -1;
    if ((pw > ph) == !!on)
        return 0;
    if (ramfb_describe(ph, pw) < 0)
        return -1;
    int t = pw;
    pw = ph;
    ph = t;
    memset(back, 0, (size_t)pw * (size_t)ph * 4);
    memset(front, 0, (size_t)pw * (size_t)ph * 4);
    return 0;
}

int video_landscape(void)
{
    return pw > ph;
}

void video_set_scale(int scale16)
{
    k16 = scale16 < 16 ? 16 : scale16;
}

/* The screen's brightness, in 256ths: ramfb has no backlight to turn
   down, so the picture itself is dimmed on its way to the screen */
static int bright = 256;

void video_set_brightness(int level)
{
    bright = level < 16 ? 16 : level > 256 ? 256 : level;
}

void video_present(void)
{
    if (!front)
        return;
    uint32_t n = (uint32_t)pw * (uint32_t)ph;
    if (bright >= 256) {
        memcpy(front, back, n * 4);
        return;
    }
    const uint32_t *s = back;
    uint32_t *d = front;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        d[i] = ((((c >> 16) & 0xFF) * (uint32_t)bright >> 8) << 16) |
               ((((c >> 8) & 0xFF) * (uint32_t)bright >> 8) << 8) |
               ((c & 0xFF) * (uint32_t)bright >> 8);
    }
}

/* What kernel/bootsplash.c asks before it draws: whether there is a
   screen to draw on - here, whether ramfb took */
int video_have_loader_lfb(void) { return front != 0; }

void video_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb)
{
    int x0 = x * k16 / 16, y0 = y * k16 / 16;
    int x1 = (x + w) * k16 / 16, y1 = (y + h) * k16 / 16;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pw) x1 = pw;
    if (y1 > ph) y1 = ph;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            back[(long)yy * pw + xx] = rgb & 0xFFFFFF;
}

int video_pw(void) { return pw; }
int video_ph(void) { return ph; }
int video_k(void) { return k16; }
int video_w(void) { return pw * 16 / k16; }
int video_h(void) { return ph * 16 / k16; }

uint32_t *video_prow32(int y)
{
    if (!back || (unsigned)y >= (unsigned)ph)
        return (uint32_t *)0;
    return back + (long)y * pw;
}

uint32_t video_pgetpixel_rgb(int x, int y)
{
    if (!back || (unsigned)x >= (unsigned)pw || (unsigned)y >= (unsigned)ph)
        return 0;
    return back[(long)y * pw + x] & 0xFFFFFF;
}

void video_pputpixel_rgb(int x, int y, uint32_t rgb)
{
    if (!back || (unsigned)x >= (unsigned)pw || (unsigned)y >= (unsigned)ph)
        return;
    back[(long)y * pw + x] = rgb & 0xFFFFFF;
}

/* The screen is always true colour here: a palette index is only what
   the shared code falls back on, and it gets grey levels */
uint32_t video_palette_rgb(uint8_t idx)
{
    return (uint32_t)idx * 0x010101u;
}

uint8_t video_nearest_index(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint8_t)((r + g + b) / 3);
}

/* What uitext.c draws with when there is no font: the 8x8 bitmap, a
   logical pixel to a dot */
void video_draw_string(int x, int y, const char *s, uint8_t color)
{
    uint32_t rgb = video_palette_rgb(color);
    int px = x * k16 / 16, py = y * k16 / 16, dot = k16 / 16;
    for (; *s; s++, px += 8 * dot) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7E)
            continue;
        for (int r = 0; r < 8; r++)
            for (int col = 0; col < 8; col++)
                if (font8x8[c - 0x20][r] & (1u << col))
                    for (int dy = 0; dy < dot; dy++)
                        for (int dx = 0; dx < dot; dx++)
                            video_pputpixel_rgb(px + col * dot + dx,
                                                py + r * dot + dy, rgb);
    }
}
