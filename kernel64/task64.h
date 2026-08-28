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
void task64_exit_code(int code);    /* to samo, ale z kodem dla rodzica */

/* Czeka na koniec dowolnego wlasnego dziecka. Zwraca jego numer
   i wpisuje kod wyjscia; -1 = nie ma na co czekac. */
int task64_wait(int *code);

/* Najglebszy zywy potomek zadania - to on trzyma konsole i to jemu
   nalezy sie Ctrl+C, a nie shellowi, ktory na niego czeka */
int task64_foreground(int root);

/* Bierze przestrzen adresowa na wlasnosc biezacego zadania i od razu
   na nia przechodzi. Zwalnia sie sama, gdy zadanie sie skonczy. */
void task64_enter_space(uint64_t space, uint64_t kstack_top);

/* Podmienia przestrzen i oddaje stara - do zwolnienia przez wolajacego
   dopiero po tym, jak procesor z niej zejdzie (exec) */
uint64_t task64_swap_space(uint64_t space, uint64_t kstack_top);

/* Zmienia nazwe biezacego zadania (po exec widac, co naprawde biegnie) */
void task64_rename(const char *name);

/* Zabija biezace zadanie (blad programu w ring 3). Nie wraca. */
void task64_kill_current(const char *why);
/* Sygnaly. Wyslanie tylko odklada sygnal i budzi spiace zadanie -
   dostarcza go dopiero powrot do pierscienia 3. */
int task64_signal(int id, int sig);
int task64_signal_pending(void);      /* czy cos czeka na dostarczenie */
int task64_take_signal(void);         /* 0 = nic nie czeka */
void task64_set_sighandler(uint64_t handler, uint64_t restorer);
uint64_t task64_sighandler(void);
uint64_t task64_sigrestorer(void);
int task64_current_id(void);

/* Katalog roboczy zadania - dziecko zaczyna tam, gdzie stal rodzic */
int task64_cwd(void);
void task64_set_cwd(int slot);

/* Sterta programu: jadro przesuwa granice, a strony przydziela dopiero
   obsluga bledu strony, przy pierwszym dotknieciu */
void task64_set_brk_base(uint64_t base);
void task64_set_brk(uint64_t brk);
uint64_t task64_brk(void);
uint64_t task64_brk_base(void);
int task64_in_heap(uint64_t addr);
void task64_inherit_brk(struct task64 *child);

/* Deskryptory otwartych plikow - tablica nalezy do zadania */
int task64_fd_open(int slot);        /* zwraca numer deskryptora albo -1 */
int task64_fd_slot(int fd);          /* i-wezel w PFS albo -1 */
uint32_t task64_fd_pos(int fd);
void task64_fd_advance(int fd, uint32_t n);
int task64_fd_close(int fd);
void task64_inherit_fds(struct task64 *child);

int  task64_id(const struct task64 *t);
int  task64_count(void);            /* ile zadan jest na liscie */
void task64_dump(void);

#endif
