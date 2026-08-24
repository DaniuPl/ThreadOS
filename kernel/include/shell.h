#ifndef PROTONOS_SHELL_H
#define PROTONOS_SHELL_H

/* Rejestruje shell jako odbiorcę klawiatury i wypisuje prompt */
void shell_init(void);

/* Odrysowuje prompt po asynchronicznym wypisie jadra */
void shell_refresh(void);

#endif
