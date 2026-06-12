#ifndef PROTONOS_TASK_H
#define PROTONOS_TASK_H

#include <stdint.h>

struct task;

/* Zamienia bieżący tok wykonania (kmain) w pierwszy wątek systemu */
void scheduler_init(void);

/* Tworzy wątek jądra z własnym stosem; zacznie działać od następnego
   tyknięcia timera */
struct task *task_create(const char *name, void (*entry)(void *), void *arg);

void task_yield(void);              /* oddaj dobrowolnie procesor */
void task_sleep(uint32_t ms);       /* uśpij bieżący wątek */
void task_exit(void);               /* zakończ bieżący wątek */

/* Wybiera następny wątek i przełącza kontekst. Wołać z IF=0. */
void schedule(void);

/* Wypisuje listę wątków (komenda 'ps' w shellu) */
void task_dump(void);

const char *task_current_name(void);

#endif
