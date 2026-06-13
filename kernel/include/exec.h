#ifndef PROTONOS_EXEC_H
#define PROTONOS_EXEC_H

#include <stdint.h>

/* Uruchamia program ELF z initrd jako proces uzytkownika (ring 3,
   wlasna przestrzen adresowa). Argument to linia polecen (program +
   argumenty rozdzielone spacjami). Zwraca pid albo -1. */
int exec_spawn(const char *cmdline);

/* Jak exec_spawn, ale stdout (fd 1) procesu jest przekierowany do
   pliku outfile na dysku (tworzonego/obcinanego). */
int exec_spawn_redirect(const char *cmdline, const char *outfile);

/* Uruchamia wielostopniowy potok 'a | b | c | ...' (procesy rownolegle).
   'pipeline' to cala linia z separatorami '|'. Gdy outfile != NULL,
   stdout ostatniego etapu idzie dodatkowo do pliku. Zwraca pid
   ostatniego etapu albo -1. */
int exec_spawn_pipeline(const char *pipeline, const char *outfile);

/* sbrk: przesuwa koniec sterty biezacego procesu o increment bajtow
   (>=0). Zwraca stary brk albo (uint32_t)-1 przy bledzie. */
uint32_t process_sbrk(int32_t increment);

#endif
