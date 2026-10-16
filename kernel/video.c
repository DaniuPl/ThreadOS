/* Graphics mode driver: VGA 13h (320x200x8) or a linear VESA mode
 * (e.g. 640x480x32) through the Bochs/QEMU DISPI interface.
 *
 * So that the desktop code stays independent of the mode, it draws
 * with palette indices (0..255). In 8bpp mode an index goes straight
 * into the buffer and into the DAC; in 32bpp mode we keep a software
 * index->RGB palette and write a 32-bit colour. The 13h framebuffer is
 * at 0xA0000 (within the kernel mapping), while a linear VESA one is
 * at an address from PCI (mapped separately).
 */

#include "include/video.h"
#include "include/font8x8.h"
#include "include/fbcon.h"
#include "include/pci.h"

/* The framebuffer lies outside the memory the kernel manages - it has
   to be glued onto the mapping, and different code does that in each
   mode. This is the only place in this file that knows about the
   processor mode at all. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n) paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n) paging_map_phys((a), (n))
#endif
#include "include/pmm.h"
#include "include/string.h"
#include "include/io.h"

/* The state of the current mode. We draw into 'fb' (the working
   buffer); 'front' is the visible framebuffer. When they differ
   (double buffering), the finished frame is moved by video_present() -
   which removes flicker and tearing. */
static volatile uint8_t *fb = (uint8_t *)(uintptr_t)0xA0000;
static volatile uint8_t *front = (uint8_t *)(uintptr_t)0xA0000;
/* A copy in RAM of what was last put on the screen. It is here for one
   reason: writing to a framebuffer is far slower than writing to
   memory - it goes across the bus to the card - and the compositor
   redraws the whole frame thirty times a second whether anything moved
   or not. Comparing against this says which rows really changed, and
   an idle desktop then writes nothing at all to the card. The
   comparison is RAM against RAM; reading the framebuffer back to do it
   would cost more than the writes it saved. */
static volatile uint8_t *shadow;
static int shadow_valid;
static uint32_t present_rows;
/* Two sizes, and the difference between them is the whole of what
   "scaling the interface" means here.
 *
 * Everything above this file - the compositor, every program, the
 * mouse - draws in LOGICAL pixels and asks video_w()/video_h() how
 * many there are. The screen has its own, physical, number of them.
 * When a panel is large enough that an eight pixel letter would be a
 * millimetre and a half tall, the logical screen is made half the
 * physical one and every logical pixel is put on the screen as a
 * square of four.
 *
 * It is pixel doubling, and it is worth saying plainly what that costs
 * and what it does not. It does not sharpen anything: the letters are
 * the same eight-by-eight letters, drawn twice as large, and a
 * photograph shown on such a screen has half the detail the panel
 * could carry. What it buys is that not one line of layout anywhere in
 * this system had to change - and a machine whose every window, icon
 * and status line is placed in absolute pixels would otherwise have
 * had all of them rewritten, and every test with them. */
static int vid_w = 320, vid_h = 200;
static int vid_pitch = 320;          /* bytes per row */
static int phys_w = 320, phys_h = 200;
static int phys_pitch = 320;
static int scale = 1;                /* logical pixels per screen pixel */
static int scale_wish;               /* 0 = decide from the panel */
static int vid_bpp = 8;
static int vid_bypp = 1;             /* bytes per pixel: 1, 3 or 4 */
static uint32_t palette_rgb[256];    /* the software palette (0x00RRGGBB) */

int video_w(void) { return vid_w; }
int video_h(void) { return vid_h; }

/* --- Mode 13h (VGA registers) --- */
static uint8_t mode13[] = {
    0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};

static void write_regs(uint8_t *r)
{
    outb(0x3C2, *r++);
    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        outb(0x3C5, *r++);
    }
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);
    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, *r++);
    }
    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, *r++);
    }
    for (uint8_t i = 0; i < 21; i++) {
        inb(0x3DA);
        outb(0x3C0, i);
        outb(0x3C0, *r++);
    }
    inb(0x3DA);
    outb(0x3C0, 0x20);
}

/* Components are 0..255 - what every other piece of the machine
   means by a colour. The VGA DAC is the odd one out: it takes six
   bits, so only that path narrows them. */
void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    palette_rgb[idx] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (vid_bpp == 8) {
        outb(0x3C8, idx);
        outb(0x3C9, (uint8_t)(r >> 2));
        outb(0x3C9, (uint8_t)(g >> 2));
        outb(0x3C9, (uint8_t)(b >> 2));
    }
}

static void set_default_palette(void)
{
    video_set_palette(0, 0, 0, 0);
    video_set_palette(1, 255, 0, 0);
    video_set_palette(2, 0, 255, 0);
    video_set_palette(3, 0, 0, 255);
    video_set_palette(4, 255, 255, 255);
    video_set_palette(5, 255, 255, 0);
}

void video_init(void)
{
    vid_w = 320; vid_h = 200; vid_pitch = 320; vid_bpp = 8;
    phys_w = 320; phys_h = 200; phys_pitch = 320;
    scale = 1;                       /* nothing to scale down from */
    shadow = (uint8_t *)0;
    shadow_valid = 0;
    fb = front = (uint8_t *)(uintptr_t)0xA0000;  /* no double buffering */
    vid_bypp = 1;
    write_regs(mode13);
    set_default_palette();
    video_clear(0);
}

/* --- Linear VESA mode (Bochs/QEMU DISPI) --- */
#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF

static void dispi(uint16_t idx, uint16_t val)
{
    outw(VBE_INDEX, idx);
    outw(VBE_DATA, val);
}

/* How many screen pixels one logical pixel is drawn as.
 *
 * Asked once, when the mode is set up. A wish of zero means "decide
 * from the panel": a screen nine hundred pixels tall or more is a
 * laptop's own resolution or better, and there an eight pixel letter
 * is about a millimetre and a half - readable through a magnifying
 * glass and not otherwise. Everything below that is left alone,
 * because doubling a 640x480 screen would leave a desktop 320 pixels
 * across, which is not a desktop. */
static void decide_scale(void)
{
    scale = scale_wish > 0 ? scale_wish : (phys_h >= 900 ? 2 : 1);
    if (scale < 1)
        scale = 1;
    if (scale > 4)
        scale = 4;
}

void video_set_scale(int s)
{
    scale_wish = s;
}

int video_scale(void)
{
    return scale;
}

/* The working buffer, and the copy of the screen next to it, are both
   the LOGICAL size - that is what makes the scaling cost nothing
   above this file. Without room for them there is no scaling and no
   double buffering either, and we draw straight onto the screen. */
static void take_buffers(void)
{
    uint32_t bytes = (uint32_t)vid_h * (uint32_t)vid_pitch;
    uint32_t pages = (bytes + 4095) / 4096;
    uint32_t back = pmm_alloc_contiguous(pages);
    if (!back) {
        scale = 1;                   /* nothing to scale into */
        vid_w = phys_w;
        vid_h = phys_h;
        vid_pitch = phys_pitch;
        fb = front;
        shadow = (uint8_t *)0;
        shadow_valid = 0;
        return;
    }
    fb = (uint8_t *)(uintptr_t)back;
    uint32_t sh = pmm_alloc_contiguous(pages);
    shadow = sh ? (uint8_t *)(uintptr_t)sh : (uint8_t *)0;
    shadow_valid = 0;
}

int video_init_vesa(int w, int h)
{
    /* When the loader has already set a mode, we leave it alone: its
       buffer works on any hardware, while the Bochs/QEMU interface
       only works on a virtual machine. We merely add a working
       buffer so that the compositor does not flicker. */
    if (video_have_loader_lfb()) {
        decide_scale();
        vid_w = phys_w / scale;
        vid_h = phys_h / scale;
        vid_pitch = vid_w * vid_bypp;
        take_buffers();
        /* A screen whose height does not divide by the scale has a
           strip at the bottom that no logical row covers. It is
           painted once, here, rather than left showing whatever the
           card powered up with. */
        for (int y = 0; y < phys_h; y++)
            memset((void *)(front + (uint32_t)y * phys_pitch), 0,
                   (uint32_t)phys_pitch);
        video_clear(0);
        video_present();
        return 0;
    }

    uint32_t lfb = pci_find_vga_lfb();
    if (!lfb)
        return -1;
    if (MAP_PHYS(lfb, (uint32_t)w * h * 4) < 0)
        return -1;

    dispi(4, 0x00);          /* disable before changing the mode */
    dispi(1, (uint16_t)w);   /* XRES */
    dispi(2, (uint16_t)h);   /* YRES */
    dispi(3, 32);            /* BPP */
    dispi(4, 0x41);          /* ENABLED | LFB */

    phys_w = w; phys_h = h; phys_pitch = w * 4;
    vid_bpp = 32; vid_bypp = 4;
    front = (uint8_t *)(uintptr_t)lfb;

    decide_scale();
    vid_w = phys_w / scale;
    vid_h = phys_h / scale;
    vid_pitch = vid_w * 4;
    take_buffers();

    set_default_palette();
    video_clear(0);
    video_present();
    return 0;
}

/* --- The framebuffer supplied by the bootloader ----------------------
 *
 * The mode is then set by the loader rather than by us. That has one
 * great advantage: it requires no particular hardware. Our
 * video_init_vesa reaches for the Bochs/QEMU interface, which a real
 * board does not have; a buffer supplied by GRUB works everywhere -
 * and it is the only road under UEFI, where there is no VGA text mode
 * at all.
 */
static int lfb_from_loader;
static uint32_t lfb_addr, lfb_bytes;

int video_adopt_lfb(uint32_t addr, uint32_t pitch, int w, int h, int bpp)
{
    if (fbcon_set_framebuffer(addr, pitch, w, h, bpp) < 0)
        return -1;
    vid_w = w;
    vid_h = h;
    vid_pitch = (int)pitch;
    phys_w = w;
    phys_h = h;
    phys_pitch = (int)pitch;
    scale = 1;
    /* The depth the loader really gave. Hardcoding thirty-two bits
       here worked while the buffer came from the BIOS - UEFI firmware
       can report 24, and then a full-word write tramples the
       neighbouring pixel. */
    vid_bpp = bpp;
    vid_bypp = bpp / 8;
    fb = front = (uint8_t *)(uintptr_t)addr;
    shadow = (uint8_t *)0;
    shadow_valid = 0;
    lfb_addr = addr;
    lfb_bytes = pitch * (uint32_t)h;
    lfb_from_loader = 1;
    set_default_palette();
    return 0;
}

/* The buffer usually lies high, outside the memory the kernel manages -
   once paging is on it has to be glued onto the mapping. Called right
   after paging_init, before we write anything on it. */
int video_map_lfb(void)
{
    if (!lfb_from_loader)
        return 0;
    return MAP_PHYS(lfb_addr, lfb_bytes);
}

int video_have_loader_lfb(void)
{
    return lfb_from_loader;
}

int video_console_enable(void)
{
    return fbcon_enable();
}

int video_console_active(void)
{
    return fbcon_active();
}

void video_console_cell(int col, int row, char ch, uint8_t attr)
{
    fbcon_cell(col, row, ch, attr);
}

/* Reading the screen back, as palette indices.
 *
 * In eight-bit modes the framebuffer already holds indices and this is
 * a copy. In the twenty-four and thirty-two bit ones it holds colours,
 * and the way back is a search through the palette for the nearest
 * entry. For everything the compositor drew that search is exact - it
 * drew FROM this palette. For a true-colour window it is not, and
 * cannot be: a screenshot is a file of indices, so a photograph in it
 * comes back quantised. That is the format saying what it is, not the
 * reading being wrong.
 *
 * The search would be two hundred and fifty six comparisons per pixel
 * if it were done honestly every time. It is not: a screen is made of
 * runs of one colour, so the last answer is remembered and almost
 * always the right one again. */
int video_read_row(int y, uint8_t *dst, int max)
{
    if (!dst || y < 0 || y >= vid_h)
        return -1;
    int n = vid_w < max ? vid_w : max;

    if (vid_bpp == 8) {
        for (int x = 0; x < n; x++)
            dst[x] = fb[(uint32_t)y * vid_pitch + x];
        return n;
    }

    static uint32_t last_rgb;
    static uint8_t last_idx;
    for (int x = 0; x < n; x++) {
        const volatile uint8_t *p = fb + (uint32_t)y * vid_pitch
                                       + (uint32_t)x * vid_bypp;
        uint32_t rgb = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        if (rgb == last_rgb) {
            dst[x] = last_idx;
            continue;
        }
        uint8_t best = 0;
        uint32_t best_d = 0xFFFFFFFFu;
        for (int i = 0; i < 256; i++) {
            uint32_t c = palette_rgb[i];
            int dr = (int)((c >> 16) & 0xFF) - (int)((rgb >> 16) & 0xFF);
            int dg = (int)((c >> 8) & 0xFF) - (int)((rgb >> 8) & 0xFF);
            int db = (int)(c & 0xFF) - (int)(rgb & 0xFF);
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < best_d) {
                best_d = d;
                best = (uint8_t)i;
                if (!d)
                    break;
            }
        }
        last_rgb = rgb;
        last_idx = best;
        dst[x] = best;
    }
    return n;
}

/* The colour behind an index, so that a picture saved on this machine
   can carry its palette with it. */
uint32_t video_palette_rgb(uint8_t idx)
{
    return palette_rgb[idx];
}

/* "Somebody else has just drawn on the screen."
 *
 * The copy in RAM is only worth anything while it says what is really
 * on the screen. The framebuffer console does not go through this file
 * at all - it holds the buffer's address of its own and writes text
 * straight onto it - so after that the copy is a lie, and a frame
 * compared against a lie sends the wrong rows. Everything that writes
 * to the screen behind this file's back says so here, and the next
 * frame goes across whole.
 *
 * Without it a message printed while the desktop is running stays on
 * the screen for good: the compositor draws the right thing, compares
 * it against a copy that already matches, and sends nothing. */
void video_front_touched(void)
{
    shadow_valid = 0;
}

uint32_t video_present_rows(void)
{
    return present_rows;
}

/* One logical row onto the screen, as 'scale' screen rows of 'scale'
   pixels each. At a scale of one this is the copy it always was. */
static void put_row(int y, const uint8_t *src)
{
    for (int r = 0; r < scale; r++) {
        int py = y * scale + r;
        if (py >= phys_h)
            return;
        volatile uint8_t *dst = front + (uint32_t)py * phys_pitch;
        if (scale == 1) {
            memcpy((void *)dst, src, (uint32_t)vid_pitch);
            continue;
        }
        if (vid_bypp == 4) {
            const uint32_t *s32 = (const uint32_t *)src;
            volatile uint32_t *d32 = (volatile uint32_t *)dst;
            for (int x = 0; x < vid_w; x++) {
                uint32_t c = s32[x];
                for (int k = 0; k < scale; k++)
                    d32[x * scale + k] = c;
            }
            continue;
        }
        for (int x = 0; x < vid_w; x++)
            for (int k = 0; k < scale; k++)
                for (int b = 0; b < vid_bypp; b++)
                    dst[(x * scale + k) * vid_bypp + b] = src[x * vid_bypp + b];
    }
}

void video_present(void)
{
    if (fb == front)
        return;                        /* nothing to move: no back buffer */

    uint32_t pitch = (uint32_t)vid_pitch;
    if (!shadow || !shadow_valid) {
        for (int y = 0; y < vid_h; y++)
            put_row(y, (const uint8_t *)(fb + (uint32_t)y * pitch));
        if (shadow) {
            memcpy((void *)shadow, (const void *)fb, (uint32_t)vid_h * pitch);
            shadow_valid = 1;
        }
        present_rows = (uint32_t)vid_h;
        return;
    }

    /* A row is the unit: finer than that and the bookkeeping costs
       more than the writes it saves. */
    uint32_t rows = 0;
    for (int y = 0; y < vid_h; y++) {
        uint32_t off = (uint32_t)y * pitch;
        const void *src = (const void *)(fb + off);
        if (!memcmp((const void *)(shadow + off), src, pitch))
            continue;
        memcpy((void *)(shadow + off), src, pitch);
        put_row(y, src);
        rows++;
    }
    present_rows = rows;
}

/* --- Drawing (independent of the mode) --- */

/* One pixel at a ready address. At 24 bits there is no padding byte,
   so a full-word write would trample the neighbour - it has to be
   written as three bytes. */
static inline void put_px(volatile uint8_t *p, uint32_t rgb)
{
    if (vid_bypp == 4) {
        *(volatile uint32_t *)p = rgb;
    } else {
        p[0] = (uint8_t)(rgb & 0xFF);
        p[1] = (uint8_t)((rgb >> 8) & 0xFF);
        p[2] = (uint8_t)((rgb >> 16) & 0xFF);
    }
}

void video_putpixel(int x, int y, uint8_t color)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return;
    if (vid_bpp == 8)
        fb[y * vid_pitch + x] = color;
    else
        put_px(fb + y * vid_pitch + x * vid_bypp, palette_rgb[color]);
}

void video_clear(uint8_t color)
{
    if (vid_bpp == 8) {
        for (int i = 0; i < vid_pitch * vid_h; i++)
            fb[i] = color;
    } else {
        uint32_t c = palette_rgb[color];
        for (int y = 0; y < vid_h; y++) {
            volatile uint8_t *row = fb + (uint32_t)y * vid_pitch;
            for (int x = 0; x < vid_w; x++)
                put_px(row + x * vid_bypp, c);
        }
    }
}

void video_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            video_putpixel(x + i, y + j, color);
}

/* --- Drawing in real colours ----------------------------------------
 *
 * Everything above this line says which of two hundred and fifty six
 * entries it means; below it, a colour is a colour. On a screen that
 * holds colours the write is direct - the palette is not consulted at
 * all - and on the eight-bit fallback the colour is traded for the
 * nearest entry there is.
 *
 * That trade is a search through the whole palette, so the last answer
 * is kept: a picture is made of runs of one colour, and a window's
 * background is one colour for thousands of pixels in a row. It is a
 * cache of one, and it is nearly always right - which is the same
 * trick, and for the same reason, as reading the screen back.
 */
int video_true_colour(void)
{
    return vid_bpp >= 24;
}

uint8_t video_nearest_index(uint32_t rgb)
{
    static uint32_t last_rgb = 0xFFFFFFFFu;
    static uint8_t last_idx;
    if (rgb == last_rgb)
        return last_idx;

    uint8_t best = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    for (int i = 0; i < 256; i++) {
        uint32_t c = palette_rgb[i];
        int dr = (int)((c >> 16) & 0xFF) - (int)((rgb >> 16) & 0xFF);
        int dg = (int)((c >> 8) & 0xFF) - (int)((rgb >> 8) & 0xFF);
        int db = (int)(c & 0xFF) - (int)(rgb & 0xFF);
        uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
        if (d < best_d) {
            best_d = d;
            best = (uint8_t)i;
            if (!d)
                break;
        }
    }
    last_rgb = rgb;
    last_idx = best;
    return best;
}

/* What is at a pixel now, as RGB - for drawing that is mixed with what
   it lands on (anti-aliased text). Read from the drawing buffer, which
   in a double-buffered mode is ordinary memory. */
uint32_t video_getpixel_rgb(int x, int y)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return 0;
    volatile uint8_t *p = fb + y * vid_pitch + x * vid_bypp;
    if (vid_bpp == 8)
        return palette_rgb[*p];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void video_putpixel_rgb(int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return;
    if (vid_bpp == 8)
        fb[y * vid_pitch + x] = video_nearest_index(rgb);
    else
        put_px(fb + y * vid_pitch + x * vid_bypp, rgb);
}

void video_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            video_putpixel_rgb(x + i, y + j, rgb);
}

void video_blit_rgb(int x, int y, int w, int h, const uint32_t *src,
                    int stride)
{
    if (!src)
        return;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if ((unsigned)dy >= (unsigned)vid_h)
            continue;
        const uint32_t *row = src + (long)j * stride;
        /* The common case - a whole row inside the screen, four bytes a
           pixel both sides - is a straight copy. It is worth saying so:
           this runs for every pixel of every window, every frame. */
        if (vid_bypp == 4 && x >= 0 && x + w <= vid_w) {
            volatile uint32_t *dst = (volatile uint32_t *)
                (fb + (uint32_t)dy * vid_pitch) + x;
            for (int i = 0; i < w; i++)
                dst[i] = row[i];
            continue;
        }
        for (int i = 0; i < w; i++)
            video_putpixel_rgb(x + i, dy, row[i]);
    }
}

void video_draw_char(int x, int y, char c, uint8_t color)
{
    if (c < 0x20 || c > 0x7E)
        return;
    const uint8_t *glyph = font8x8[c - 0x20];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (glyph[row] & (1 << col))
                video_putpixel(x + col, y + row, color);
}

void video_draw_string(int x, int y, const char *s, uint8_t color)
{
    for (int i = 0; s[i]; i++)
        video_draw_char(x + i * 8, y, s[i], color);
}
