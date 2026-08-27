#ifndef PROTONOS_SIGNAL_H
#define PROTONOS_SIGNAL_H

#include <stdint.h>

/* Sygnaly - musza zgadzac sie z user/usys.h */
#define SIGINT   2      /* przerwanie z klawiatury (Ctrl+C) */
#define SIGKILL  9      /* zabicie nie do przechwycenia */
#define SIGTERM 15      /* prosba o zakonczenie */
#define SIGCHLD 17      /* dziecko sie skonczylo */
#define NSIG    20

/* Wartosci specjalne zamiast adresu procedury obslugi */
#define SIG_DFL 0       /* domyslna reakcja jadra */
#define SIG_IGN 1       /* zignoruj sygnal */

struct regs;

/* Doreczenie oczekujacego sygnalu przy powrocie do ring 3. Gdy proces
   ma wlasna procedure obslugi, jadro odklada na jego stos ramke
   przerwanego stanu i przestawia eip na handler. */
void signal_deliver(struct regs *r);

/* Powrot z procedury obslugi (syscall sigreturn): odtwarza stan
   zapisany na stosie uzytkownika przy doreczeniu. */
void signal_return(struct regs *r);

/* Nazwa sygnalu do komunikatow jadra */
const char *signal_name(int sig);

#endif
