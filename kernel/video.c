/* Sterownik trybu graficznego: VGA 13h (320x200x8) albo VESA liniowy
 * (np. 640x480x32) przez interfejs Bochs/QEMU DISPI.
 *
 * Aby kod pulpitu pozostal niezalezny od trybu, rysuje on indeksami
 * palety (0..255). W trybie 8bpp indeks trafia wprost do bufora i do
 * DAC; w trybie 32bpp trzymamy programowa palete index->RGB i piszemy
 * 32-bitowy kolor. Framebuffer 13h jest pod 0xA0000 (w mapowaniu
 * jadra), a liniowy VESA - pod adresem z PCI (mapowany osobno).
 */

#include "include/video.h"
#include "include/font8x8.h"
#include "include/fbcon.h"
#include "include/pci.h"
#include "include/paging.h"
#include "include/pmm.h"
#include "include/string.h"
#include "include/io.h"

/* Stan biezacego trybu. Rysujemy do 'fb' (bufor roboczy); 'front' to
   widoczny framebuffer. Gdy sa rozne (podwojne buforowanie), gotowa
   klatke przenosi video_present() - to usuwa migotanie i rozrywanie. */
static volatile uint8_t *fb = (uint8_t *)0xA0000;
static volatile uint8_t *front = (uint8_t *)0xA0000;
static int vid_w = 320, vid_h = 200;
static int vid_pitch = 320;          /* bajtow na wiersz */
static int vid_bpp = 8;
static uint32_t palette_rgb[256];    /* programowa paleta (0x00RRGGBB) */

int video_w(void) { return vid_w; }
int video_h(void) { return vid_h; }

/* --- Tryb 13h (rejestry VGA) --- */
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

void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b)
{
    /* skladowe 6-bitowe (0..63) -> 8-bitowe w programowej palecie */
    palette_rgb[idx] = ((uint32_t)((r << 2) | (r >> 4)) << 16) |
                       ((uint32_t)((g << 2) | (g >> 4)) << 8) |
                       (uint32_t)((b << 2) | (b >> 4));
    if (vid_bpp == 8) {
        outb(0x3C8, idx);
        outb(0x3C9, r);
        outb(0x3C9, g);
        outb(0x3C9, b);
    }
}

static void set_default_palette(void)
{
    video_set_palette(0, 0, 0, 0);
    video_set_palette(1, 63, 0, 0);
    video_set_palette(2, 0, 63, 0);
    video_set_palette(3, 0, 0, 63);
    video_set_palette(4, 63, 63, 63);
    video_set_palette(5, 63, 63, 0);
}

void video_init(void)
{
    vid_w = 320; vid_h = 200; vid_pitch = 320; vid_bpp = 8;
    fb = front = (uint8_t *)0xA0000;     /* bez podwojnego buforowania */
    write_regs(mode13);
    set_default_palette();
    video_clear(0);
}

/* --- Tryb VESA liniowy (Bochs/QEMU DISPI) --- */
#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF

static void dispi(uint16_t idx, uint16_t val)
{
    outw(VBE_INDEX, idx);
    outw(VBE_DATA, val);
}

int video_init_vesa(int w, int h)
{
    /* Gdy tryb ustawil juz loader, nie ruszamy go: jego bufor dziala
       na kazdym sprzecie, a interfejs Bochs/QEMU tylko na maszynie
       wirtualnej. Dokladamy jedynie bufor roboczy, zeby kompozytor
       nie migotal. */
    if (video_have_loader_lfb()) {
        uint32_t bytes = (uint32_t)vid_h * vid_pitch;
        uint32_t back = pmm_alloc_contiguous((bytes + 4095) / 4096);
        if (back)
            fb = (uint8_t *)back;
        video_clear(0);
        video_present();
        return 0;
    }

    uint32_t lfb = pci_find_vga_lfb();
    if (!lfb)
        return -1;
    if (paging_map_phys(lfb, (uint32_t)w * h * 4) < 0)
        return -1;

    dispi(4, 0x00);          /* wylacz przed zmiana trybu */
    dispi(1, (uint16_t)w);   /* XRES */
    dispi(2, (uint16_t)h);   /* YRES */
    dispi(3, 32);            /* BPP */
    dispi(4, 0x41);          /* ENABLED | LFB */

    vid_w = w; vid_h = h; vid_pitch = w * 4; vid_bpp = 32;
    front = (uint8_t *)lfb;

    /* Bufor roboczy w RAM (podwojne buforowanie - bez migotania) */
    uint32_t bytes = (uint32_t)w * h * 4;
    uint32_t pages = (bytes + 4095) / 4096;
    uint32_t back = pmm_alloc_contiguous(pages);
    fb = back ? (uint8_t *)back : front;   /* bez RAM - rysuj wprost do LFB */

    set_default_palette();
    video_clear(0);
    video_present();
    return 0;
}

/* --- Bufor obrazu podany przez bootloader ---------------------------
 *
 * Tryb ustawia wtedy loader, a nie my. Ma to jedna wielka zalete: nie
 * wymaga zadnego konkretnego sprzetu. Nasze video_init_vesa siega po
 * interfejs Bochs/QEMU, ktorego na prawdziwej plycie nie ma; bufor
 * podany przez GRUB-a dziala wszedzie - i to jedyna droga pod UEFI,
 * gdzie trybu tekstowego VGA nie ma w ogole.
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
    vid_bpp = 32;
    fb = front = (uint8_t *)addr;
    lfb_addr = addr;
    lfb_bytes = pitch * (uint32_t)h;
    lfb_from_loader = 1;
    set_default_palette();
    return 0;
}

/* Bufor lezy zwykle wysoko, poza pamiecia zarzadzana przez jadro -
   po wlaczeniu stronicowania trzeba go doklejic do mapowania.
   Wolane zaraz po paging_init, zanim cokolwiek na nim napiszemy. */
int video_map_lfb(void)
{
    if (!lfb_from_loader)
        return 0;
    return paging_map_phys(lfb_addr, lfb_bytes);
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

void video_present(void)
{
    if (fb != front)
        memcpy((void *)front, (const void *)fb, (uint32_t)vid_h * vid_pitch);
}

/* --- Rysowanie (niezalezne od trybu) --- */

void video_putpixel(int x, int y, uint8_t color)
{
    if ((unsigned)x >= (unsigned)vid_w || (unsigned)y >= (unsigned)vid_h)
        return;
    if (vid_bpp == 8)
        fb[y * vid_pitch + x] = color;
    else
        *(volatile uint32_t *)(fb + y * vid_pitch + x * 4) = palette_rgb[color];
}

void video_clear(uint8_t color)
{
    if (vid_bpp == 8) {
        for (int i = 0; i < vid_pitch * vid_h; i++)
            fb[i] = color;
    } else {
        uint32_t c = palette_rgb[color];
        volatile uint32_t *p = (volatile uint32_t *)fb;
        for (int i = 0; i < vid_w * vid_h; i++)
            p[i] = c;
    }
}

void video_fill_rect(int x, int y, int w, int h, uint8_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            video_putpixel(x + i, y + j, color);
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
