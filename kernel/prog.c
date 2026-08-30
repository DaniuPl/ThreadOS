/* Szukanie programu: initrd, potem dysk
 *
 * Loader ELF i system plikow istnialy obok siebie od dawna i nigdy
 * nie byly polaczone: 'run' zagladal wylacznie do initrd. Ten plik
 * jest tym polaczeniem i niczym wiecej - decyduje, skad wziac bajty,
 * a co z nimi zrobic, wie juz loader. Dlatego uzywaja go oba jadra:
 * o trybie procesora nie ma tu ani slowa.
 *
 * Plik z dysku trzeba wczytac w calosci do pamieci jadra, bo loader
 * chce widziec caly obraz naraz (naglowki leza na poczatku, segmenty
 * gdziekolwiek). Initrd tego nie wymaga: on juz jest w pamieci.
 */

#include <stdint.h>
#include "include/prog.h"
#include "include/task.h"
#include "include/initrd.h"
#include "include/fs.h"
#include "include/heap.h"
#include "include/printk.h"

/* Sufit na program z dysku. Sterta jadra nie jest z gumy, a program
   wiekszy niz to i tak nie zmiescilby sie w pamieci wraz z wlasna
   przestrzenia. */
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

    /* Program szukamy tam, gdzie stoi zadanie - katalog roboczy jest
       jego wlasnoscia, tak samo jak przy 'cat' czy 'ls' */
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
    /* fs_read_at oddaje LICZBE przeczytanych bajtow - obraz krotszy
       niz plik to nie jest ten sam program */
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
