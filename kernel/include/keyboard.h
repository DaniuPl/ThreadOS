#ifndef PROTONOS_KEYBOARD_H
#define PROTONOS_KEYBOARD_H

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard_init(void);

/* Rejestruje odbiorcę znaków z klawiatury (np. shell) */
typedef void (*keyboard_char_handler_t)(char c);
void keyboard_set_handler(keyboard_char_handler_t handler);
keyboard_char_handler_t keyboard_get_handler(void);

/* Blokujący odczyt dla procesów (syscall read). Bieżące zadanie
   przejmuje konsolę do swojej śmierci; zwraca >=1 przeczytanych
   znaków (bez echa - echo robi czytający). */
int keyboard_read(char *buf, uint32_t len);

#endif
