#ifndef PROTONOS_SHELL_H
#define PROTONOS_SHELL_H

/* Rejestruje shell jako odbiorcę klawiatury, wypisuje prompt i startuje
   wątek, w którym wykonują się komendy (samo przerwanie tylko buforuje) */
void shell_init(void);

/* Odrysowuje prompt po asynchronicznym wypisie jadra */
void shell_refresh(void);

#endif
