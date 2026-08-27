#ifndef PROTONOS_FS_H
#define PROTONOS_FS_H

#include <stdint.h>

/* PFS3: bloki po 1 KiB przydzielane z bitmapy, 64 i-wezly, dluzsze nazwy.
   Plik ma 12 blokow bezposrednich, blok posredni (256 wskaznikow) i blok
   podwojnie posredni (256 x 256) - czyli do 64 MiB, wiecej niz mieszcza
   sie na typowym obrazie dysku. */
#define FS_NAME_MAX  63
#define FS_MAX_FILES 64
#define FS_BLOCK     1024u
#define FS_DIRECT    12
#define FS_INDIRECT  (FS_BLOCK / 4)          /* wskaznikow w bloku posrednim */
#define FS_DINDIRECT (FS_INDIRECT * FS_INDIRECT)   /* przez blok podwojny */
#define FS_MAX_SIZE  ((uint32_t)(FS_DIRECT + FS_INDIRECT + FS_DINDIRECT) \
                      * FS_BLOCK)                   /* ~64 MiB */

#define FS_ROOT (-1)              /* katalog glowny (wirtualny i-wezel) */

/* Wykrywa dysk i wczytuje system plikow PFS; formatuje pusty dysk.
   Zwraca 1 gdy gotowy, 0 gdy brak dysku. */
int fs_init(void);
int fs_ready(void);

/* --- Operacje na sciezkach (wzgledem katalogu roboczego cwd) ---
   cwd to slot katalogu albo FS_ROOT. Sciezka moze byc absolutna ("/a/b"),
   wzgledna ("a/b") i zawierac "." oraz "..". */

/* Otwiera plik; create=1 tworzy go, jesli nie istnieje. Slot albo -1. */
int fs_open(int cwd, const char *path, int create);

/* Tworzy katalog. Zwraca 0 albo -1. */
int fs_mkdir(int cwd, const char *path);

/* Rozwiazuje sciezke do katalogu i zapisuje jego slot w *newcwd.
   Zwraca 0 albo -1 (nie istnieje / nie jest katalogiem). */
int fs_chdir(int cwd, const char *path, int *newcwd);

/* Usuwa plik albo pusty katalog (zwalnia jego bloki). 0 albo -1. */
int fs_unlink(int cwd, const char *path);

/* Wpisuje absolutna sciezke slotu do buf (>= 512 B). */
void fs_getpath(int slot, char *buf);

/* Enumeracja katalogu 'dir' (FS_ROOT albo slot): kolejny wpis o numerze
   index. Wpisuje nazwe, rozmiar i czy to katalog. Zwraca 1 albo 0. */
int fs_readdir(int dir, int index, char *name, uint32_t *size, int *is_dir);

/* --- Operacje po slocie i-wezla --- */
int fs_size(int slot);
int fs_truncate(int slot);
int fs_read_at(int slot, uint32_t off, void *buf, uint32_t len);
/* Blok dysku pod 'index'-tym blokiem pliku (alloc=1 przydziela go);
   0 = blad. Do tego pierwszy sektor ATA danego bloku - swap rezerwuje
   miejsce przez system plikow, a pisze po nim wprost. */
uint32_t fs_file_block(int slot, uint32_t index, int alloc);
uint32_t fs_block_lba(uint32_t block);
int fs_write_at(int slot, uint32_t off, const void *buf, uint32_t len);

/* --- Wygodne operacje dla shella jadra (zawsze w katalogu glownym) --- */
int fs_write(const char *name, const void *data, uint32_t len);
int fs_read(const char *name, void *buf, uint32_t max);
void fs_list(void);

/* Zajetosc dysku: bloki uzyte / wszystkie (komenda 'df') */
void fs_usage(uint32_t *used_blocks, uint32_t *total_blocks);

#endif
