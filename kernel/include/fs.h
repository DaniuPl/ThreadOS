#ifndef PROTONOS_FS_H
#define PROTONOS_FS_H

#include <stdint.h>

#define FS_NAME_MAX  23
#define FS_MAX_FILES 16
#define FS_MAX_SIZE  (32 * 512)   /* 16 KiB na plik (32 sektory) */

/* Wykrywa dysk i wczytuje system plikow PFS; formatuje pusty dysk.
   Zwraca 1 gdy gotowy, 0 gdy brak dysku. */
int fs_init(void);

int fs_ready(void);

/* Zapisuje caly plik (nadpisuje). Zwraca 0 albo -1. */
int fs_write(const char *name, const void *data, uint32_t len);

/* Czyta plik do buf (max bajtow). Zwraca liczbe bajtow albo -1. */
int fs_read(const char *name, void *buf, uint32_t max);

/* Wypisuje liste plikow (komenda 'fls') */
void fs_list(void);

#endif
