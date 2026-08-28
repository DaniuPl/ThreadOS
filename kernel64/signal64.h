#ifndef PROTONOS_SIGNAL64_H
#define PROTONOS_SIGNAL64_H

#include <stdint.h>

struct regs;

#define SIGINT   2      /* przerwanie z klawiatury (Ctrl+C) */
#define SIGKILL  9      /* zabicie nie do przechwycenia */
#define SIGTERM 15      /* prosba o zakonczenie */

/* Dostarcza czekajacy sygnal, jesli jakis jest. Wolane tuz przed
   powrotem do pierscienia 3 - i tylko wtedy, bo tylko wtedy ramka
   w 'r' opisuje program, a nie jadro. */
void signal64_deliver(struct regs *r);

/* Powrot z procedury obslugi: odtwarza ramke odlozona przy dostarczeniu */
int signal64_return(struct regs *r);

#endif
