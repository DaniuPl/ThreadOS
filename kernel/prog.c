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

static const char *skad = "initrd";

const char *prog_skad(void)
{
    return skad;
}

int prog_istnieje(const char *nazwa)
{
    uint32_t size = 0;
    if (initrd_find(nazwa, &size))
        return 1;
    if (!fs_ready())
        return 0;
    return fs_open(task_cwd(), nazwa, 0) >= 0;
}

const void *prog_obraz(const char *nazwa, uint32_t *rozmiar, void **bufor)
{
    *bufor = (void *)0;

    uint32_t size = 0;
    const void *obraz = initrd_find(nazwa, &size);
    if (obraz) {
        skad = "initrd";
        *rozmiar = size;
        return obraz;
    }

    if (!fs_ready())
        return (const void *)0;

    /* Program szukamy tam, gdzie stoi zadanie - katalog roboczy jest
       jego wlasnoscia, tak samo jak przy 'cat' czy 'ls' */
    int f = fs_open(task_cwd(), nazwa, 0);
    if (f < 0)
        return (const void *)0;

    int len = fs_size(f);
    if (len <= 0 || len > PROG_MAX) {
        printk("[jadro] '%s' na dysku ma %d B - to nie jest program\n",
               nazwa, len);
        return (const void *)0;
    }

    void *buf = kmalloc((uint32_t)len);
    if (!buf) {
        printk("[jadro] brak pamieci na wczytanie '%s' (%d B)\n", nazwa, len);
        return (const void *)0;
    }
    /* fs_read_at oddaje LICZBE przeczytanych bajtow - obraz krotszy
       niz plik to nie jest ten sam program */
    if (fs_read_at(f, 0, buf, (uint32_t)len) != len) {
        kfree(buf);
        printk("[jadro] '%s' nie da sie odczytac z dysku\n", nazwa);
        return (const void *)0;
    }

    skad = "dysku";
    *bufor = buf;
    *rozmiar = (uint32_t)len;
    return buf;
}

void prog_zwolnij(void *bufor)
{
    if (bufor)
        kfree(bufor);
}
