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
#define MAP_FB(a, n)   paging64_map_phys_wc((a), (n))   /* write-combining */
#else
#include "include/paging.h"
#define MAP_PHYS(a, n) paging_map_phys((a), (n))
#define MAP_FB(a, n)   paging_map_phys((a), (n))
#endif
#include "include/pmm.h"
#include "include/string.h"
#include "include/io.h"
#include "include/printk.h"

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
static volatile int shadow_valid;
static uint32_t present_rows;
/* Two sizes, and the difference between them is the whole of what
 * "scaling the interface" means here.
 *
 * Everything above this file - the compositor, every program, the
 * mouse - lays itself out in LOGICAL pixels and asks video_w() and
 * video_h() how many there are. The panel has its own, physical,
 * number of them, and on a laptop's panel there are more: a 15-inch
 * 1920x1080 screen is a desktop 1280x720 across, every logical pixel
 * one and a half of the panel's.
 *
 * It used to be pixel doubling: the desktop drawn small and every
 * pixel put on the panel as a square of four - an eight pixel letter
 * drawn twice as large, not sharper. Now the buffer the compositor
 * draws into is the panel's own size, and kernel/gfx.c and
 * kernel/uitext.c draw every shape and every letter at that size from
 * logical numbers: an edge worked out in the panel's pixels, a letter
 * rendered from its outline at one and a half times the size. The
 * scale is in sixteenths, so it need not be a whole number.
 *
 * What still writes single pixels through this file - the 8x8 letters,
 * the old palette drawing - gets each logical pixel as the block of
 * panel pixels it covers, which is the doubling it always was. */
static int vid_w = 320, vid_h = 200;  /* logical */
static int vid_pitch = 320;          /* bytes per row of 'fb' (physical) */
static int phys_w = 320, phys_h = 200;
static int phys_pitch = 320;         /* bytes per row of 'front' */
static int k16 = 16;                 /* panel pixels per logical one, /16 */
static int scale_wish;               /* 0 = decide from the panel */
static int edid_mm_w, edid_mm_h;     /* the panel's size, when it says */
static int vid_bpp = 8;
static int vid_bypp = 1;             /* bytes per pixel: 1, 3 or 4 */
static uint32_t palette_rgb[256];    /* the software palette (0x00RRGGBB) */

int video_w(void) { return vid_w; }
int video_h(void) { return vid_h; }
int video_pw(void) { return phys_w; }
int video_ph(void) { return phys_h; }
int video_k(void) { return k16; }

/* A logical coordinate on the panel: rounded down, so that two
   neighbouring logical pixels cover neighbouring panel pixels and
   nothing is left between them */
static inline int to_p(int v)
{
    return k16 == 16 ? v : (int)((long)v * k16 / 16);
}

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
    k16 = 16;                        /* one for one */
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

/* How many panel pixels one logical pixel is, in sixteenths.
 *
 * Asked for (the 'scale' setting: 1 to 4 as multiples, 100 to 400 as a
 * percentage), or decided from the panel. When the panel says how
 * large it is (EDID), the answer is its density against the 96 dots an
 * inch the desktop was laid out for. When it does not, its height
 * stands in for that: a laptop's panel is about as tall in inches
 * whatever its resolution, so 720 rows is taken for one and the rest
 * scaled from it - 1080 is one and a half, 1440 two. Either way to the
 * nearest quarter, and never below one: a desktop smaller than its
 * panel is not a desktop. */
static void decide_scale(void)
{
    int k;
    if (scale_wish >= 100)
        k = (scale_wish * 16 + 50) / 100;
    else if (scale_wish > 0)
        k = scale_wish * 16;
    else if (edid_mm_w >= 100 && edid_mm_h >= 60)
        /* dots an inch = w / (mm / 25.4); against 96, in sixteenths */
        k = (int)(((long)phys_w * 254 * 16 / ((long)edid_mm_w * 960) + 2)
                  / 4 * 4);
    else
        k = (phys_h * 16 / 720 + 2) / 4 * 4;
    if (k < 16)
        k = 16;
    if (k > 64)
        k = 64;
    /* Never a desktop smaller than 640x480: everything here was laid
       out for at least that, and a window larger than the desktop is
       a window that cannot be seen whole */
    while (k > 16 && (phys_w * 16 / k < 640 || phys_h * 16 / k < 480))
        k -= 4;
    k16 = k;
    vid_w = phys_w * 16 / k16;
    vid_h = phys_h * 16 / k16;
}

void video_set_scale(int s)
{
    scale_wish = s;
}

/* The scale asked for again, with the mode already set: only the
   logical size changes - the panel and its buffers stay what they are */
void video_rescale(void)
{
    decide_scale();
}

/* As a percentage: 100, 125, 150 ... */
int video_scale(void)
{
    return (k16 * 100 + 8) / 16;
}

/* The working buffer, and the copy of the screen next to it, are both
   the panel's size - the drawing is done at that size. Without room for
   them there is no double buffering, and we draw straight onto the
   screen. */
static void take_buffers(void)
{
    vid_pitch = phys_w * vid_bypp;
    uint32_t bytes = (uint32_t)phys_h * (uint32_t)vid_pitch;
    uint32_t pages = (bytes + 4095) / 4096;
    uint32_t back = pmm_alloc_contiguous(pages);
    if (!back) {
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

/* --- The panel's own resolution ---------------------------------------
 *
 * A screen says what it is in a block of 128 bytes, the EDID: who made
 * it, how large it is, and - in its first timing descriptor - the mode
 * it was built for, its native one. Anything else is scaled by the
 * screen itself, and a scaled picture is a soft one.
 *
 * A real card hands the EDID over through its own registers, which
 * this system has no driver for; there the loader's mode is all there
 * is. QEMU's standard VGA carries the block at the start of its second
 * memory window (BAR2), and that is read here: the same thing a driver
 * would do, on the one card this system drives itself.
 */
static int edid_w, edid_h;

static int edid_valid(const volatile uint8_t *e)
{
    static const uint8_t magic[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++)
        if (e[i] != magic[i])
            return 0;
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + e[i]);
    return sum == 0;
}

/* Reads the preferred mode and the size out of a block; 0 when it does
   not hold one */
int video_parse_edid(const uint8_t *e, int *w, int *h, int *mm_w, int *mm_h)
{
    if (!edid_valid(e))
        return 0;
    const uint8_t *d = e + 54;             /* the first descriptor */
    int clock = d[0] | (d[1] << 8);
    if (!clock)
        return 0;                          /* not a timing */
    *w = d[2] | ((d[4] & 0xF0) << 4);
    *h = d[5] | ((d[7] & 0xF0) << 4);
    *mm_w = d[12] | ((d[14] & 0xF0) << 4);
    *mm_h = d[13] | ((d[14] & 0x0F) << 8);
    if (*mm_w < 10 || *mm_h < 10) {         /* only in centimetres, then */
        *mm_w = e[21] * 10;
        *mm_h = e[22] * 10;
    }
    return *w > 0 && *h > 0;
}

static void read_edid(void)
{
    struct pci_dev d;
    if (pci_find_device(0x1234, 0x1111, &d) < 0)
        return;                            /* not the standard VGA */
    uint32_t bar2 = d.bar[2];
    if (!bar2 || (bar2 & 1))
        return;
    bar2 &= 0xFFFFFFF0u;
    if (MAP_PHYS(bar2, 4096) < 0)
        return;
    uint8_t e[128];
    const volatile uint8_t *m = (const volatile uint8_t *)(uintptr_t)bar2;
    for (int i = 0; i < 128; i++)
        e[i] = m[i];
    int w, h, mw, mh;
    if (!video_parse_edid(e, &w, &h, &mw, &mh))
        return;
    edid_w = w;
    edid_h = h;
    edid_mm_w = mw;
    edid_mm_h = mh;
}

/* The panel's size, said by the loader from the EDID it could read and
   we cannot (kernel/mb2.c) */
void video_panel_mm(int w, int h)
{
    edid_mm_w = w;
    edid_mm_h = h;
}

void video_native(int *w, int *h)
{
    *w = edid_w;
    *h = edid_h;
}

static int dispi_mode(uint32_t lfb, int w, int h);

int video_init_vesa(int w, int h)
{
    /* When the loader has already set a mode, we leave it alone: its
       buffer works on any hardware, while the Bochs/QEMU interface
       only works on a virtual machine. We merely add a working
       buffer so that the compositor does not flicker. */
    if (video_have_loader_lfb()) {
        decide_scale();
        take_buffers();
        /* A panel whose size does not divide by the scale has a strip
           at the edge that no logical pixel covers. It is painted once,
           here, rather than left showing whatever the card powered up
           with. */
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
    read_edid();
    return dispi_mode(lfb, w, h);
}

/* Sets a mode through the Bochs/QEMU interface: w x h, or for 0x0 the
   panel's own mode when it says what that is, and 640x480 when it does
   not */
static uint32_t dispi_lfb;          /* 0 = the mode is not ours to change */

static int dispi_mode(uint32_t lfb, int w, int h)
{
    if (w <= 0 || h <= 0) {
        w = edid_w ? edid_w : 640;
        h = edid_h ? edid_h : 480;
    }
    /* The card's memory holds what it holds - 16 MiB by default, which
       is 2560x1600 and a little more. Past that, the largest mode of
       the same shape that fits. */
    while ((uint32_t)w * h * 4 > 16u * 1024 * 1024 && w > 640) {
        h = h * 7 / 8;
        w = w * 7 / 8;
    }
    w &= ~7;
    if (MAP_FB(lfb, (uint32_t)w * h * 4) < 0)
        return -1;

    dispi(4, 0x00);          /* disable before changing the mode */
    dispi(1, (uint16_t)w);   /* XRES */
    dispi(2, (uint16_t)h);   /* YRES */
    dispi(3, 32);            /* BPP */
    dispi(4, 0x41);          /* ENABLED | LFB */

    phys_w = w; phys_h = h; phys_pitch = w * 4;
    vid_bpp = 32; vid_bypp = 4;
    front = (uint8_t *)(uintptr_t)lfb;
    dispi_lfb = lfb;

    decide_scale();
    take_buffers();

    set_default_palette();
    video_clear(0);
    video_present();
    return 0;
}

/* The buffers a mode had, given back */
static void drop_buffers(void)
{
    uint32_t bytes = (uint32_t)phys_h * (uint32_t)vid_pitch;
    if (fb != front)
        for (uint32_t a = (uint32_t)(uintptr_t)fb, n = (bytes + 4095) / 4096;
             n; n--, a += 4096)
            pmm_free_frame(a);
    if (shadow)
        for (uint32_t a = (uint32_t)(uintptr_t)shadow,
                      n = (bytes + 4095) / 4096; n; n--, a += 4096)
            pmm_free_frame(a);
    fb = front;
    shadow = (uint8_t *)0;
    shadow_valid = 0;
}

/* Another mode while the screen runs - where the mode is ours to set
   (the Bochs/QEMU interface); a mode the loader set stays what it is.
   1 = changed, 0 = it already was, -1 = it cannot be. */
int video_set_mode(int w, int h)
{
    if (!dispi_lfb)
        return -1;
    int nw = w > 0 && h > 0 ? w : (edid_w ? edid_w : 640);
    int nh = w > 0 && h > 0 ? h : (edid_h ? edid_h : 480);
    if ((nw & ~7) == phys_w && nh == phys_h)
        return 0;
    drop_buffers();
    if (dispi_mode(dispi_lfb, w, h) < 0)
        return -1;
    return 1;
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
    k16 = 16;
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
    return MAP_FB(lfb_addr, lfb_bytes);
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
    if (!dst || y < 0 || y >= phys_h)
        return -1;
    int n = phys_w < max ? phys_w : max;

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

/* One row of the drawing buffer onto the screen - the same size both
   sides, since the drawing is done at the panel's own size */
static void put_row(int y, const uint8_t *src)
{
    memcpy((void *)(front + (uint32_t)y * phys_pitch), src,
           (uint32_t)phys_w * vid_bypp);
}

void video_present(void)
{
    if (fb == front)
        return;                        /* nothing to move: no back buffer */

    uint32_t pitch = (uint32_t)vid_pitch;
    if (!shadow || !shadow_valid) {
        /* Valid from BEFORE the copy: somebody writing to the screen
           behind our back while it is being made says so again, and
           the next frame is sent whole once more */
        if (shadow)
            shadow_valid = 1;
        for (int y = 0; y < phys_h; y++)
            put_row(y, (const uint8_t *)(fb + (uint32_t)y * pitch));
        if (shadow)
            memcpy((void *)shadow, (const void *)fb, (uint32_t)phys_h * pitch);
        present_rows = (uint32_t)phys_h;
        return;
    }

    /* A row is the unit: finer than that and the bookkeeping costs
       more than the writes it saves. */
    uint32_t rows = 0;
    for (int y = 0; y < phys_h; y++) {
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

/* --- in the panel's own pixels -------------------------------------- */

uint32_t video_pgetpixel_rgb(int x, int y)
{
    if ((unsigned)x >= (unsigned)phys_w || (unsigned)y >= (unsigned)phys_h)
        return 0;
    volatile uint8_t *p = fb + y * vid_pitch + x * vid_bypp;
    if (vid_bpp == 8)
        return palette_rgb[*p];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void video_pputpixel_rgb(int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= (unsigned)phys_w || (unsigned)y >= (unsigned)phys_h)
        return;
    if (vid_bpp == 8)
        fb[y * vid_pitch + x] = video_nearest_index(rgb);
    else
        put_px(fb + y * vid_pitch + x * vid_bypp, rgb);
}

/* The drawing buffer's row as words, for code that works on many
   pixels at once (kernel/gfx.c). NULL unless the screen is four bytes a
   pixel - every other format goes through the pixel calls. */
uint32_t *video_prow32(int y)
{
    if (vid_bypp != 4 || (unsigned)y >= (unsigned)phys_h)
        return (uint32_t *)0;
    return (uint32_t *)(uintptr_t)(fb + (uint32_t)y * vid_pitch);
}

/* A rectangle of the panel's pixels, clipped once and then whole rows */
static void pfill(int x, int y, int w, int h, uint32_t rgb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > phys_w) w = phys_w - x;
    if (y + h > phys_h) h = phys_h - y;
    if (w <= 0 || h <= 0)
        return;
    if (vid_bypp == 4) {
        for (int j = 0; j < h; j++) {
            uint32_t *row = video_prow32(y + j) + x;
            for (int i = 0; i < w; i++)
                row[i] = rgb;
        }
        return;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            video_pputpixel_rgb(x + i, y + j, rgb);
}

/* A rectangle of colours in the panel's own pixels, one for one */
void video_pblit_rgb(int x, int y, int w, int h, const uint32_t *src,
                     int stride)
{
    if (!src)
        return;
    for (int j = 0; j < h; j++) {
        int dy = y + j;
        if ((unsigned)dy >= (unsigned)phys_h)
            continue;
        const uint32_t *row = src + (long)j * stride;
        if (vid_bypp == 4 && x >= 0 && x + w <= phys_w) {
            memcpy((void *)(fb + (uint32_t)dy * vid_pitch + (uint32_t)x * 4),
                   row, (uint32_t)w * 4);
            continue;
        }
        for (int i = 0; i < w; i++)
            video_pputpixel_rgb(x + i, dy, row[i]);
    }
}

/* --- in logical pixels: each one the block of the panel it covers ----- */

void video_putpixel(int x, int y, uint8_t color)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return;
    if (k16 != 16) {
        pfill(to_p(x), to_p(y), to_p(x + 1) - to_p(x), to_p(y + 1) - to_p(y),
              palette_rgb[color]);
        return;
    }
    if (vid_bpp == 8)
        fb[y * vid_pitch + x] = color;
    else
        put_px(fb + y * vid_pitch + x * vid_bypp, palette_rgb[color]);
}

void video_clear(uint8_t color)
{
    if (vid_bpp == 8) {
        for (int i = 0; i < vid_pitch * phys_h; i++)
            fb[i] = color;
    } else {
        pfill(0, 0, phys_w, phys_h, palette_rgb[color]);
    }
}

void video_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    if (vid_bypp == 4) {
        video_fill_rect_rgb(x, y, w, h, palette_rgb[color]);
        return;
    }
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
    return video_pgetpixel_rgb(to_p(x), to_p(y));
}

void video_putpixel_rgb(int x, int y, uint32_t rgb)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return;
    if (k16 != 16)
        pfill(to_p(x), to_p(y), to_p(x + 1) - to_p(x), to_p(y + 1) - to_p(y),
              rgb);
    else
        video_pputpixel_rgb(x, y, rgb);
}

/* A logical row as words - only where logical and panel pixels are the
   same; at any other scale a logical row is not a row of anything */
uint32_t *video_row32(int y)
{
    if (k16 != 16 || (unsigned)y >= (unsigned)vid_h)
        return (uint32_t *)0;
    return video_prow32(y);
}

void video_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb)
{
    if (k16 != 16) {
        int x0 = to_p(x), y0 = to_p(y);
        pfill(x0, y0, to_p(x + w) - x0, to_p(y + h) - y0, rgb);
        return;
    }
    pfill(x, y, w, h, rgb);
}

void video_blit_rgb(int x, int y, int w, int h, const uint32_t *src,
                    int stride)
{
    if (!src)
        return;
    if (k16 != 16) {
        /* Each source pixel the block it covers - a picture the
           compositor did not draw itself, and has no outline of */
        for (int j = 0; j < h; j++) {
            int py0 = to_p(y + j), py1 = to_p(y + j + 1);
            const uint32_t *row = src + (long)j * stride;
            for (int i = 0; i < w; i++) {
                int px0 = to_p(x + i), px1 = to_p(x + i + 1);
                pfill(px0, py0, px1 - px0, py1 - py0, row[i]);
            }
        }
        return;
    }
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
