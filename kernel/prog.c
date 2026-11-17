/* Looking for a program: the initrd, then the disk
 *
 * The ELF loader and the filesystem had existed side by side for a
 * long time and were never joined: 'run' looked into the initrd and
 * nowhere else. This file is that joining and nothing more - it
 * decides where to get the bytes from, and what to do with them the
 * loader already knows. There is not one word about the processor
 * here.
 *
 * A file from disk has to be read into kernel memory in full, because
 * the loader wants to see the whole image at once (the headers lie at
 * the start, the segments anywhere). The initrd does not require that:
 * it is in memory already.
 */

#include <stdint.h>
#include "include/prog.h"
#include "include/task.h"
#include "include/initrd.h"
#include "include/fs.h"
#include "include/heap.h"
#include "include/printk.h"
#include "../kernel64/elf64.h"

/* The ceiling for a program from disk read into the kernel's heap
   whole. The heap is not made of rubber; a larger program is read
   into its own pages as it loads (prog_open below). */
#define PROG_MAX (2 * 1024 * 1024)

static const char *from_where = "initrd";

const char *prog_source(void)
{
    return from_where;
}

int prog_exists(const char *name)
{
    uint32_t size = 0;
    if (initrd_find(name, &size))
        return 1;
    if (!fs_ready())
        return 0;
    return fs_open(task_cwd(), name, 0) >= 0;
}

const void *prog_image(const char *name, uint32_t *size_out, void **buf_out)
{
    *buf_out = (void *)0;

    uint32_t size = 0;
    const void *image = initrd_find(name, &size);
    if (image) {
        from_where = "initrd";
        *size_out = size;
        return image;
    }

    if (!fs_ready())
        return (const void *)0;

    /* We look for the program where the task stands - the working
       directory is its property, just as with 'cat' or 'ls' */
    int f = fs_open(task_cwd(), name, 0);
    if (f < 0)
        return (const void *)0;

    int len = fs_size(f);
    if (len <= 0 || len > PROG_MAX) {
        printk("[kernel] '%s' on the disk is %d B - that is not a program\n",
               name, len);
        return (const void *)0;
    }

    void *buf = kmalloc((uint32_t)len);
    if (!buf) {
        printk("[kernel] no memory to read '%s' (%d B)\n", name, len);
        return (const void *)0;
    }
    /* fs_read_at returns the NUMBER of bytes read - an image shorter
       than the file is not the same program */
    if (fs_read_at(f, 0, buf, (uint32_t)len) != len) {
        kfree(buf);
        printk("[kernel] '%s' cannot be read off the disk\n", name);
        return (const void *)0;
    }

    from_where = "the disk";
    *buf_out = buf;
    *size_out = (uint32_t)len;
    return buf;
}

void prog_release(void *buf)
{
    if (buf)
        kfree(buf);
}

/* --- Opening a program and loading it --------------------------------
 *
 * A program from the initrd, or a small one from disk, is in kernel
 * memory whole and loads as it always did. A larger one from disk is
 * not read into the kernel's heap at all: it stays a file until it is
 * loaded, and the loader reads it into the program's own pages
 * (elf64_load_file). That is what a program of tens of megabytes needs
 * - WebKit's processes (apps/webkit) are. */

static int read_slot(void *ctx, uint64_t off, void *dst, uint32_t len)
{
    if (off > 0xFFFFFFFFull)
        return -1;
    return fs_read_at((int)(intptr_t)ctx, (uint32_t)off, dst, len);
}

int prog_open(const char *name, struct prog *p)
{
    p->image = (const void *)0;
    p->buf = (void *)0;
    p->size = 0;
    p->slot = -1;

    uint32_t size = 0;
    const void *image = initrd_find(name, &size);
    if (image) {
        from_where = "initrd";
        p->image = image;
        p->size = size;
        return 0;
    }
    if (!fs_ready())
        return -1;
    int f = fs_open(task_cwd(), name, 0);
    if (f < 0)
        return -1;
    int len = fs_size(f);
    if (len <= 0) {
        printk("[kernel] '%s' on the disk is empty - that is not a program\n",
               name);
        return -1;
    }
    if (len <= PROG_MAX) {
        p->image = prog_image(name, &p->size, &p->buf);
        return p->image ? 0 : -1;
    }
    from_where = "the disk";
    p->slot = f;
    p->size = (uint32_t)len;
    return 0;
}

int prog_load(struct prog *p, uint64_t *entry, uint64_t *brk_base)
{
    if (p->image)
        return elf64_load(p->image, p->size, entry, brk_base);
    if (p->slot < 0)
        return -1;
    struct elf64_file f = { read_slot, (void *)(intptr_t)p->slot, p->size };
    return elf64_load_file(&f, entry, brk_base);
}

void prog_close(struct prog *p)
{
    prog_release(p->buf);
    p->buf = (void *)0;
    p->image = (const void *)0;
    p->slot = -1;
}
