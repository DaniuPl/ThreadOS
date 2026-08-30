/* Looking for a program: the initrd, then the disk
 *
 * The ELF loader and the filesystem had existed side by side for a
 * long time and were never joined: 'run' looked into the initrd and
 * nowhere else. This file is that joining and nothing more - it
 * decides where to get the bytes from, and what to do with them the
 * loader already knows. That is why both kernels use it: there is not
 * one word about the processor mode here.
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

/* The ceiling for a program from disk. The kernel heap is not made of
   rubber, and a program larger than that would not fit in memory
   together with its own address space anyway. */
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
        printk("[jadro] '%s' na dysku ma %d B - to nie jest program\n",
               name, len);
        return (const void *)0;
    }

    void *buf = kmalloc((uint32_t)len);
    if (!buf) {
        printk("[jadro] brak pamieci na wczytanie '%s' (%d B)\n", name, len);
        return (const void *)0;
    }
    /* fs_read_at returns the NUMBER of bytes read - an image shorter
       than the file is not the same program */
    if (fs_read_at(f, 0, buf, (uint32_t)len) != len) {
        kfree(buf);
        printk("[jadro] '%s' nie da sie odczytac z dysku\n", name);
        return (const void *)0;
    }

    from_where = "dysku";
    *buf_out = buf;
    *size_out = (uint32_t)len;
    return buf;
}

void prog_release(void *buf)
{
    if (buf)
        kfree(buf);
}
