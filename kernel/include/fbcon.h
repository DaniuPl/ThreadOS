#ifndef PROTONOS_FBCON_H
#define PROTONOS_FBCON_H

#include <stdint.h>

/* Konsola tekstowa rysowana na buforze obrazu
 *
 * Bufor znakow pod 0xB8000 to wynalazek BIOS-owy. Pod UEFI go nie ma
 * i jedyne, co dostajemy od firmware, to bufor obrazu - znaki trzeba
 * narysowac samemu.
 *
 * Ten kod nie wie nic ani o stronicowaniu, ani o PCI, ani o zadaniach:
 * dostaje adres bufora i rysuje w nim komorki. Dzieki temu uzywaja go
 * oba jadra - 32-bitowe i to budowane na tryb dlugi.
 */

/* Zapamietuje bufor podany przez bootloader. 0 = przyjety. */
int fbcon_set_framebuffer(uint32_t addr, uint32_t pitch,
                          int w, int h, int bpp);

/* Czy bufor w ogole jest (i jaki) - do doklejenia go do mapowania */
int fbcon_have(void);
uint32_t fbcon_addr(void);
uint32_t fbcon_bytes(void);
int fbcon_width(void);
int fbcon_height(void);
int fbcon_pitch(void);

/* Wlacza rysowanie konsoli (czysci ekran). 1 = wlaczona. */
int fbcon_enable(void);
int fbcon_active(void);

/* Rysuje jedna komorke 80x25: znak plus atrybut koloru trybu tekstowego */
void fbcon_cell(int col, int row, char ch, uint8_t attr);

#endif
