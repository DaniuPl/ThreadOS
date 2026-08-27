#ifndef PROTONOS_KEYBOARD_H
#define PROTONOS_KEYBOARD_H

/* Klawisze bez odpowiednika w ASCII (strzalki, PgUp/PgDn, Home/End)
   dostarczane sa jako kody sterujace - programy rozpoznaja je po
   wartosci, a shell je ignoruje. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_PGUP  0x15
#define KEY_PGDN  0x16
#define KEY_HOME  0x17
#define KEY_END   0x18

/* Ctrl+C: nie znak, tylko zadanie przerwania. Sterownik zamienia go
   na SIGINT dla procesu trzymajacego konsole (zob. keyboard.c). */
#define KEY_INTR  0x03

/* Instaluje handler IRQ1 i odmaskowuje przerwanie klawiatury */
void keyboard_init(void);

/* Rejestruje odbiorcę znaków z klawiatury (np. shell) */
typedef void (*keyboard_char_handler_t)(char c);
void keyboard_set_handler(keyboard_char_handler_t handler);
keyboard_char_handler_t keyboard_get_handler(void);

/* Blokujący odczyt dla procesów (syscall read). Bieżące zadanie
   przejmuje konsolę do swojej śmierci; zwraca >=1 przeczytanych
   znaków (bez echa - echo robi czytający) albo -1, gdy czekanie
   przerwał sygnał (proces wejdzie zaraz w procedurę obsługi). */
int keyboard_read(char *buf, uint32_t len);

#endif
