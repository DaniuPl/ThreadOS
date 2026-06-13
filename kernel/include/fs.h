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

/* --- Operacje po indeksie i-wezla (warstwa deskryptorow plikow) --- */

/* Otwiera plik po nazwie; gdy create=1 tworzy go, jesli nie istnieje.
   Zwraca indeks i-wezla (>=0) albo -1. */
int fs_open(const char *name, int create);

int fs_size(int idx);                 /* rozmiar pliku albo -1 */
int fs_truncate(int idx);             /* zeruje rozmiar pliku */

/* Odczyt/zapis len bajtow od offsetu. Zapis rozszerza plik.
   Zwracaja liczbe przetworzonych bajtow albo -1. */
int fs_read_at(int idx, uint32_t off, void *buf, uint32_t len);
int fs_write_at(int idx, uint32_t off, const void *buf, uint32_t len);

/* Enumeracja katalogu: dla slotu 0..FS_MAX_FILES-1 wpisuje nazwe do
   name (>=FS_NAME_MAX+1) i zwraca rozmiar pliku; -1 gdy slot pusty. */
int fs_get(int slot, char *name, uint32_t *size);

/* Usuwa plik po nazwie. Zwraca 0 albo -1 (brak pliku). */
int fs_delete(const char *name);

/* Wypisuje liste plikow (komenda 'fls') */
void fs_list(void);

#endif
