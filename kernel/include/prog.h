#ifndef THREADOS_PROG_H
#define THREADOS_PROG_H

#include <stdint.h>

/* Where to get a program image from
 *
 * Until now the answer was one: from the initrd. The initrd, though,
 * is a read-only image loaded together with the kernel - a system that
 * has a disk and a filesystem ought to be able to run what lies on
 * that disk too.
 *
 * The order is: initrd first, then disk. The initrd is always there
 * and always works, so a program of the same name shipped with the
 * kernel cannot be shadowed by a file on disk - which means a broken
 * file on disk cannot take the system away.
 */

/* Finds a program image. Returns a pointer to it and its size, or
   NULL. When the image came from disk, *buf_out receives the address
   to hand back through prog_release - an image from the initrd needs
   nothing and leaves zero there. */
const void *prog_image(const char *name, uint32_t *size_out, void **buf_out);
void prog_release(void *buf);

/* A program found, to be loaded into the current address space. One
   from the initrd, or a small one from disk, lies in memory (image);
   a larger one from disk is a file (slot) read as it loads. */
struct prog {
    const void *image;
    uint32_t size;
    void *buf;                  /* for prog_release */
    int slot;                   /* the file, or -1 */
};
int prog_open(const char *name, struct prog *p);
/* elf64_load or elf64_load_file, whichever the program needs */
int prog_load(struct prog *p, uint64_t *entry, uint64_t *brk_base);
void prog_close(struct prog *p);

/* Is a program of this name anywhere at all. A cheap check: it does
   not read the file, only asks the initrd and the filesystem. */
int prog_exists(const char *name);

/* Where the last image found came from - for messages */
const char *prog_source(void);

/* What follows the last '/' of a path - the whole of a bare name */
const char *prog_last_part(const char *path);

#endif
