#ifndef PROTONOS_TASK64_H
#define PROTONOS_TASK64_H

#include <stdint.h>

/* Watki jadra w trybie dlugim
 *
 * Ten sam pomysl, co w jadrze 32-bitowym: lista cykliczna, wywlaszczanie
 * co tykniecie timera, watek glowny nigdy nie spi i pelni role watku
 * bezczynnosci. Rozni sie tylko przelaczanie kontekstu - szesc
 * rejestrow zamiast czterech i osiem bajtow na kazdy.
 *
 * Nie ma tu jeszcze procesow: ani przestrzeni adresowych, ani
 * deskryptorow, ani sygnalow. To przyjdzie razem z ring 3.
 */

struct task64;

void scheduler64_init(void);
struct task64 *task64_create(const char *name, void (*entry)(void *),
                             void *arg);
void task64_yield(void);
void task64_sleep(uint32_t ms);
void task64_exit(void);
int  task64_id(const struct task64 *t);
int  task64_count(void);            /* ile zadan jest na liscie */
void task64_dump(void);

#endif
