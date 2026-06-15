#ifndef PROTONOS_TASK_H
#define PROTONOS_TASK_H

#include <stdint.h>

struct task;

/* Tablica deskryptorow plikow procesu. fd 0/1/2 = klawiatura/konsola,
   wyzsze fd = pliki na dysku albo konce potoku. */
#define TASK_MAX_FD 8

enum fd_kind {
    FD_CONSOLE,           /* klawiatura (read) / ekran (write) */
    FD_FILE,              /* plik na dysku PFS */
    FD_PIPE,              /* koniec potoku miedzyprocesowego */
};

struct file_desc {
    int used;
    enum fd_kind kind;
    int inode;            /* FD_FILE: indeks i-wezla PFS */
    uint32_t off;         /* FD_FILE: biezacy offset */
    int pipe;             /* FD_PIPE: indeks potoku */
    int readable;
    int writable;
};

/* Tablica fd biezacego zadania (do warstwy syscalli plikowych) */
struct file_desc *task_fds(void);

/* Katalog roboczy biezacego zadania (slot PFS, -1 = root) */
int task_cwd(void);
void task_set_cwd(int slot);

/* Zamienia bieżący tok wykonania (kmain) w pierwszy wątek systemu */
void scheduler_init(void);

/* Tworzy wątek jądra z własnym stosem; zacznie działać od następnego
   tyknięcia timera */
struct task *task_create(const char *name, void (*entry)(void *), void *arg);

void task_yield(void);              /* oddaj dobrowolnie procesor */
void task_sleep(uint32_t ms);       /* uśpij bieżący wątek */
void task_exit(int code);           /* zakończ bieżący wątek z kodem */
int  task_wait(int pid);            /* czekaj na proces, zwróć jego kod */
int  task_kill(int pid);            /* zaznacz proces do zabicia (0/-1) */
void task_kill_pending(void);       /* dorecz zabicie przy powrocie do ring 3 */
void task_block(void);              /* śpij, aż ktoś obudzi */
void task_unblock(struct task *t);  /* obudź (wołać z IF=0) */

struct task *task_current(void);
int task_id_of(struct task *t);
int task_current_id(void);
int task_alive(int id);             /* czy zadanie o tym id wciąż żyje */

/* Wybiera następny wątek i przełącza kontekst. Wołać z IF=0. */
void schedule(void);

/* Wypisuje listę wątków (komenda 'ps' w shellu) */
void task_dump(void);

const char *task_current_name(void);

/* Bieżące zadanie przechodzi na własną przestrzeń adresową;
   katalog stron zostanie zwolniony przy sprzątaniu zombie */
void task_use_address_space(uint32_t dir_phys);

uint32_t task_current_cr3(void);    /* CR3 bieżącego zadania */
uint32_t task_brk(void);            /* koniec sterty procesu (0 = wątek jądra) */
void task_set_brk(uint32_t brk);
uint32_t task_mmap_next(void);      /* nastepny wolny adres mmap */
void task_set_mmap_next(uint32_t addr);

#endif
