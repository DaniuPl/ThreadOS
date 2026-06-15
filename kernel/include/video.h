#ifndef PROTONOS_VIDEO_H
#define PROTONOS_VIDEO_H

#include <stdint.h>

#define VIDEO_W 320
#define VIDEO_H 200

/* Przelacza karte VGA w tryb graficzny 13h (320x200, 256 kolorow,
   framebuffer pod 0xA0000) i ustawia podstawowa palete. */
void video_init(void);

void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void video_clear(uint8_t color);
void video_putpixel(int x, int y, uint8_t color);
void video_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Tekst czcionka 8x8 (tylko piksele pierwszoplanowe - tlo bez zmian) */
void video_draw_char(int x, int y, char c, uint8_t color);
void video_draw_string(int x, int y, const char *s, uint8_t color);

#endif
