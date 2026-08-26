#ifndef PROTONOS_EXEC_H
#define PROTONOS_EXEC_H

#include <stdint.h>

/* Uruchamia program ELF z initrd jako proces uzytkownika (ring 3,
   wlasna przestrzen adresowa). Argument to linia polecen (program +
   argumenty rozdzielone spacjami). Zwraca pid albo -1. */
int exec_spawn(const char *cmdline);

/* Jak exec_spawn, ale stdout (fd 1) procesu jest przekierowany do
   pliku outfile. append=0 obcina plik, append=1 dopisuje (>>). */
int exec_spawn_redirect(const char *cmdline, const char *outfile, int append);

/* Uruchamia wielostopniowy potok 'a | b | c | ...' (procesy rownolegle).
   'pipeline' to cala linia z separatorami '|'. Gdy outfile != NULL,
   stdout ostatniego etapu idzie dodatkowo do pliku. Zwraca pid
   ostatniego etapu albo -1. */
int exec_spawn_pipeline(const char *pipeline, const char *outfile);

/* fork: rozwidla biezacy proces. Dziecko dostaje klon przestrzeni
   adresowej rodzica (kopiowanie przy zapisie) i wraca z syscalla
   w tym samym miejscu programu. Zwraca pid dziecka (u rodzica),
   zero (u dziecka) albo -1. 'r' to ramka rejestrow syscalla. */
struct regs;
int process_fork(struct regs *r);

/* sbrk: przesuwa koniec sterty biezacego procesu o increment bajtow
   (>=0). Zwraca stary brk albo (uint32_t)-1 przy bledzie. */
uint32_t process_sbrk(int32_t increment);

/* mmap: mapuje 'size' bajtow swiezej pamieci procesu, zwraca adres albo 0 */
uint32_t process_mmap(uint32_t size);

/* shm: mapuje region pamieci wspoldzielonej (po kluczu); adres albo 0 */
uint32_t process_shm(int key, uint32_t size);

/* Tworzy watek w przestrzeni biezacego procesu; zwraca tid albo -1 */
int process_thread(uint32_t entry, uint32_t arg);

#endif
