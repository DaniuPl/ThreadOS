#ifndef PROTONOS_KFILE_H
#define PROTONOS_KFILE_H

#include <stdint.h>

/* Flagi dla file_open (zgodne z usys.h) */
#define O_READ   1
#define O_WRITE  2
#define O_CREATE 4
#define O_TRUNC  8

/* Warstwa deskryptorow plikow procesu. fd 0/1/2 = klawiatura/konsola,
   wyzsze fd odnosza sie do plikow PFS. Operuja na tablicy fd
   biezacego zadania. */
int  file_open(const char *name, int flags);
int  file_close(int fd);
int  file_read(int fd, void *buf, uint32_t len);
int  file_write(int fd, const void *buf, uint32_t len);
int  file_seek(int fd, uint32_t off);

/* Podmienia stdout (fd 1) biezacego zadania na plik. append=0 obcina
   plik, append=1 dopisuje na koncu. Uzywane przy 'cmd > plik' / '>>'. */
int  file_redirect_stdout(const char *name, int append);

/* Ustawia podany fd jako koniec potoku (writable=1 -> zapis/stdout,
   0 -> odczyt/stdin). Uzywane przy budowie potoku 'cmd1 | cmd2'. */
void file_set_pipe(int fd, int pipe_idx, int writable);

/* Zwalnia wszystkie deskryptory biezacego zadania (konce potokow) */
void file_close_all(void);

#endif
