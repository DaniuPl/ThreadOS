#ifndef PROTONOS_VIDEO_H
#define PROTONOS_VIDEO_H

#include <stdint.h>

#define VIDEO_W 320
#define VIDEO_H 200

/* Przelacza karte VGA w tryb graficzny 13h (320x200, 256 kolorow,
   framebuffer pod 0xA0000) i ustawia podstawowa palete. */
void video_init(void);

/* Tryb VESA liniowy (Bochs/QEMU DISPI), 32 bpp. Zwraca 0 albo -1
   (brak framebuffera) - wtedy warto wrocic do video_init(). */
int video_init_vesa(int w, int h);

/* Przejmuje bufor obrazu ustawiony przez bootloader (Multiboot 2).
   Dziala na kazdym sprzecie - inaczej niz video_init_vesa, ktory siega
   po interfejs Bochs/QEMU. 0 = przejety. */
int video_adopt_lfb(uint32_t addr, uint32_t pitch, int w, int h, int bpp);

/* Doklejenie bufora do mapowania stron - wolac zaraz po paging_init */
int video_map_lfb(void);
int video_have_loader_lfb(void);

/* Konsola tekstowa rysowana na buforze obrazu (gdy nie ma trybu
   tekstowego VGA - np. pod UEFI). 1 = wlaczona. */
int video_console_enable(void);
int video_console_active(void);
void video_console_cell(int col, int row, char ch, uint8_t attr);

/* Wymiary biezacego trybu */
int video_w(void);
int video_h(void);

/* Przenosi gotowa klatke z bufora roboczego na ekran (podwojne
   buforowanie). W trybie bez bufora roboczego nic nie robi. */
void video_present(void);

void video_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void video_clear(uint8_t color);
void video_putpixel(int x, int y, uint8_t color);
void video_fill_rect(int x, int y, int w, int h, uint8_t color);

/* Tekst czcionka 8x8 (tylko piksele pierwszoplanowe - tlo bez zmian) */
void video_draw_char(int x, int y, char c, uint8_t color);
void video_draw_string(int x, int y, const char *s, uint8_t color);

#endif
