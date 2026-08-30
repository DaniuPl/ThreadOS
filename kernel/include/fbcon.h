#ifndef PROTONOS_FBCON_H
#define PROTONOS_FBCON_H

#include <stdint.h>

/* A text console drawn on a framebuffer
 *
 * The character buffer at 0xB8000 is a BIOS invention. Under UEFI it
 * does not exist, and the only thing the firmware gives us is a
 * framebuffer - the characters have to be drawn by hand.
 *
 * This code knows nothing about paging, PCI or tasks: it is given a
 * buffer address and draws cells in it. That is why both kernels use
 * it - the 32-bit one and the one built for long mode.
 */

/* Remembers the buffer supplied by the bootloader. 0 = accepted. */
int fbcon_set_framebuffer(uint32_t addr, uint32_t pitch,
                          int w, int h, int bpp);

/* Is there a buffer at all (and which) - for gluing it onto the mapping */
int fbcon_have(void);
uint32_t fbcon_addr(void);
uint32_t fbcon_bytes(void);
int fbcon_width(void);
int fbcon_height(void);
int fbcon_pitch(void);
int fbcon_bpp(void);

/* Turns console drawing on (clears the screen). 1 = enabled. */
int fbcon_enable(void);
int fbcon_active(void);

/* Draws one 80x25 cell: a character plus a text-mode colour attribute */
void fbcon_cell(int col, int row, char ch, uint8_t attr);

#endif
