#ifndef THREADOS_VIDEO_H
#define THREADOS_VIDEO_H

#include <stdint.h>

#define VIDEO_W 320
#define VIDEO_H 200

/* Switches the VGA card into graphics mode 13h (320x200, 256 colours,
   framebuffer at 0xA0000) and sets a basic palette. */
void video_init(void);

/* Linear VESA mode (Bochs/QEMU DISPI), 32 bpp. Returns 0 or -1 (no
   framebuffer) - in which case falling back to video_init() is wise. */
int video_init_vesa(int w, int h);

/* Adopts the framebuffer set up by the bootloader (Multiboot 2). Works
   on any hardware - unlike video_init_vesa, which reaches for the
   Bochs/QEMU interface. 0 = adopted. */
int video_adopt_lfb(uint32_t addr, uint32_t pitch, int w, int h, int bpp);

/* Gluing the buffer onto the page mapping - call right after paging_init */
int video_map_lfb(void);
int video_have_loader_lfb(void);

/* A text console drawn on the framebuffer (when there is no VGA text
   mode - e.g. under UEFI). 1 = enabled. */
int video_console_enable(void);
int video_console_active(void);
void video_console_cell(int col, int row, char ch, uint8_t attr);

/* Dimensions of the current mode, in LOGICAL pixels - which is what
   everything above this file draws in. On a panel large enough that an
   eight pixel letter would be a millimetre and a half tall, one
   logical pixel is put on the screen as a square of four, and these
   report half of what the panel has. */
int video_w(void);
int video_h(void);
/* The panel's own size, and how many of its pixels a logical one is,
   in sixteenths (24 = one and a half). Drawing through kernel/gfx.c
   is done at the panel's size from logical numbers. */
int video_pw(void);
int video_ph(void);
int video_k(void);
uint32_t *video_prow32(int y);
uint32_t video_pgetpixel_rgb(int x, int y);
void video_pputpixel_rgb(int x, int y, uint32_t rgb);
void video_pblit_rgb(int x, int y, int w, int h, const uint32_t *src,
                     int stride);
/* The mode the panel was made for, from its EDID; 0x0 when unknown.
   video_init_vesa(0, 0) asks for it. */
void video_native(int *w, int *h);
/* The panel's size in millimetres, when the loader knows it (0 = not) */
void video_panel_mm(int w, int h);
int video_parse_edid(const uint8_t *e, int *w, int *h, int *mm_w, int *mm_h);
/* How many panel pixels one logical pixel is: asked for before the mode
   is set up - 1 to 4 times, or 100 to 400 per cent; 0 means "decide
   from the panel" - and said afterwards, in per cent. */
void video_set_scale(int s);
/* ... and taken up at once, while the screen runs (the logical size
   changes; the panel does not) */
void video_rescale(void);
int video_scale(void);

/* Moves a finished frame from the working buffer onto the screen
   (double buffering). Does nothing in a mode without a working buffer. */
void video_present(void);
/* Said by anything that writes to the visible framebuffer without
   going through this file - the framebuffer console does. */
void video_front_touched(void);
/* How many rows the last video_present() actually wrote to the screen.
   On an idle desktop this is zero: the frame was drawn again and came
   out identical, so nothing crossed the bus. */
uint32_t video_present_rows(void);

/* One row of the screen read back as palette indices (see video.c).
   Returns how many pixels were written, or -1. */
int video_read_row(int y, uint8_t *dst, int max);
/* The colour an index stands for, as 0x00RRGGBB */
uint32_t video_palette_rgb(uint8_t idx);

/* Sets what an index stands for. Components are 0..255; the VGA DAC,
   which takes six bits, is the only place they are narrowed. */
void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);

/* --- Drawing in real colours (0x00RRGGBB) ---------------------------
 *
 * The calls above speak palette indices, which is what this machine
 * drew with while its screen had two hundred and fifty six colours to
 * give. The framebuffer a loader hands over today holds colours, so
 * these say one directly - and in the eight-bit fallback they trade it
 * for the nearest index the palette has, which is all such a screen
 * can do.
 *
 * video_true_colour() is how the compositor knows which of the two is
 * worth doing: there is no point keeping a window's content in four
 * bytes a pixel on a screen that cannot show them. */
int video_true_colour(void);
uint8_t video_nearest_index(uint32_t rgb);
void video_putpixel_rgb(int x, int y, uint32_t rgb);
uint32_t video_getpixel_rgb(int x, int y);
void video_fill_rect_rgb(int x, int y, int w, int h, uint32_t rgb);
/* One row of the drawing buffer as 0x00RRGGBB words, or NULL when the
   screen is not four bytes a pixel - or when logical pixels are not the
   panel's (video_prow32 is the panel's own row) */
uint32_t *video_row32(int y);
/* One rectangle of colours, 'stride' pixels to a source row */
void video_blit_rgb(int x, int y, int w, int h, const uint32_t *src,
                    int stride);
void video_clear(uint8_t color);
void video_putpixel(int x, int y, uint8_t color);
void video_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Text in the 8x8 font (foreground pixels only - the background is
   left alone) */
void video_draw_char(int x, int y, char c, uint8_t color);
void video_draw_string(int x, int y, const char *s, uint8_t color);

#endif
