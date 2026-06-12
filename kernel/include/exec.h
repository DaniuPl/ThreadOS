#ifndef PROTONOS_EXEC_H
#define PROTONOS_EXEC_H

/* Uruchamia program ELF z initrd jako proces uzytkownika (ring 3,
   wlasna przestrzen adresowa). Zwraca pid albo -1 (brak pliku). */
int exec_spawn(const char *name);

#endif
