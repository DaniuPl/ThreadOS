#ifndef PROTONOS_VIDEO_H
#define PROTONOS_VIDEO_H

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

/* Dimensions of the current mode */
int video_w(void);
int video_h(void);

/* Moves a finished frame from the working buffer onto the screen
   (double buffering). Does nothing in a mode without a working buffer. */
void video_present(void);

void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void video_clear(uint8_t color);
void video_putpixel(int x, int y, uint8_t color);
void video_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Text in the 8x8 font (foreground pixels only - the background is
   left alone) */
void video_draw_char(int x, int y, char c, uint8_t color);
void video_draw_string(int x, int y, const char *s, uint8_t color);

#endif
