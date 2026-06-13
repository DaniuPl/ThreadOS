/* Warstwa deskryptorow plikow (model uniksowy)
 *
 * Spina trzy swiaty pod jednym interfejsem read/write:
 *   fd 0       - klawiatura (blokujacy odczyt)
 *   fd 1, 2    - konsola (VGA + port szeregowy)
 *   fd >= 3    - pliki na dysku PFS (z wlasnym offsetem)
 *
 * Dzieki temu program pisze tak samo na ekran (write(1,...)) jak do
 * pliku (write(fd,...)) - rozni je tylko numer deskryptora.
 */

#include "include/kfile.h"
#include "include/task.h"
#include "include/fs.h"
#include "include/keyboard.h"
#include "include/printk.h"

static void console_write(const char *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        printk("%c", buf[i]);
}

int file_open(const char *name, int flags)
{
    struct file_desc *fds = task_fds();

    int fd = -1;
    for (int i = 3; i < TASK_MAX_FD; i++) {
        if (!fds[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0)
        return -1;                       /* brak wolnego deskryptora */

    int idx = fs_open(name, flags & O_CREATE);
    if (idx < 0)
        return -1;
    if (flags & O_TRUNC)
        fs_truncate(idx);

    fds[fd].used = 1;
    fds[fd].is_file = 1;
    fds[fd].inode = idx;
    fds[fd].off = 0;
    fds[fd].readable = (flags & O_READ) ? 1 : 0;
    fds[fd].writable = (flags & O_WRITE) ? 1 : 0;
    return fd;
}

int file_close(int fd)
{
    struct file_desc *fds = task_fds();
    if (fd < 3 || fd >= TASK_MAX_FD || !fds[fd].used)
        return -1;                       /* 0/1/2 zostaja otwarte */
    fds[fd].used = 0;
    return 0;
}

int file_read(int fd, void *buf, uint32_t len)
{
    struct file_desc *fds = task_fds();
    if (fd < 0 || fd >= TASK_MAX_FD || !fds[fd].used || !fds[fd].readable)
        return -1;

    if (!fds[fd].is_file)
        return keyboard_read(buf, len);  /* fd 0: klawiatura */

    int n = fs_read_at(fds[fd].inode, fds[fd].off, buf, len);
    if (n > 0)
        fds[fd].off += (uint32_t)n;
    return n;
}

int file_write(int fd, const void *buf, uint32_t len)
{
    struct file_desc *fds = task_fds();
    if (fd < 0 || fd >= TASK_MAX_FD || !fds[fd].used || !fds[fd].writable)
        return -1;

    if (!fds[fd].is_file) {              /* fd 1/2: konsola */
        console_write(buf, len);
        return (int)len;
    }

    int n = fs_write_at(fds[fd].inode, fds[fd].off, buf, len);
    if (n > 0)
        fds[fd].off += (uint32_t)n;
    return n;
}

int file_seek(int fd, uint32_t off)
{
    struct file_desc *fds = task_fds();
    if (fd < 3 || fd >= TASK_MAX_FD || !fds[fd].used)
        return -1;
    fds[fd].off = off;
    return (int)off;
}
