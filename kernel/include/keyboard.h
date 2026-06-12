#ifndef PROTONOS_KEYBOARD_H
#define PROTONOS_KEYBOARD_H

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard_init(void);

/* Rejestruje odbiorcę znaków z klawiatury (np. shell) */
typedef void (*keyboard_char_handler_t)(char c);
void keyboard_set_handler(keyboard_char_handler_t handler);

#endif
