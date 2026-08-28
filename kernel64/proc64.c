/* Powielanie procesu (fork) w trybie dlugim
 *
 * Dziecko rozni sie od rodzica dwiema rzeczami i niczym wiecej:
 * ma wlasna kopie pamieci i zero zamiast numeru dziecka w RAX.
 *
 * Cala sztuczka polega na tym, ze jadro ma pod reka pelny stan
 * programu. Stub wywolan systemowych odklada dokladnie taka ramke,
 * jaka odklada przerwanie - wiec wystarczy ja skopiowac i kazac
 * nowemu zadaniu wrocic nia do ring 3. Zadne rejestry nie musza byc
 * przy tym odtwarzane "recznie": robi to ta sama sciezka, ktora
 * wraca z kazdego innego wywolania.
 */

#include <stddef.h>
#include <stdint.h>
#include "proc64.h"
#include "paging64.h"
#include "task64.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/pmm.h"

/* Powrot do ring 3 z ramki podanej wskaznikiem (boot64/syscall64.S) */
extern void return_to_user_64(struct regs *frame);

/* Ile ramek kosztowal ostatni fork. Przy dzieleniu stron powinny to
   byc same tablice stron - pamiec programu nie jest kopiowana wcale. */
static uint32_t last_cost;

uint32_t proc64_last_fork_cost(void)
{
    return last_cost;
}

struct fork_arg {
    struct regs frame;
    uint64_t space;
};

/* Pierwszy kod dziecka. Dziala jeszcze w jadrze, na wlasnym stosie -
   bierze przestrzen na wlasnosc i wraca do ring 3 kopia ramki rodzica. */
static void child_start(void *arg)
{
    struct fork_arg *fa = (struct fork_arg *)arg;
    struct regs frame = fa->frame;
    uint64_t space = fa->space;
    kfree(fa);

    /* Stos jadra dziecka - stad ruszy przy kazdym nastepnym wejsciu
       z ring 3. Bierzemy go, zanim ramka trafi na stos, zeby lezala
       ponizej i nie zostala nadpisana. */
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(space, kstack & ~0xFULL);

    frame.rax = 0;                   /* dziecko poznaje sie po zerze */
    return_to_user_64(&frame);       /* nie wraca */
}

int64_t proc64_fork(struct regs *r)
{
    uint32_t before = pmm_used_frames();
    uint64_t space = paging64_clone_space(paging64_current_space());
    last_cost = pmm_used_frames() - before;
    if (!space) {
        printk("[jadro] fork: brak pamieci na kopie przestrzeni\n");
        return -1;
    }

    struct fork_arg *fa = kmalloc(sizeof(*fa));
    if (!fa) {
        paging64_free_space(space);
        return -1;
    }
    fa->frame = *r;
    fa->space = space;

    struct task64 *child = task64_create("dziecko", child_start, fa);
    if (!child) {
        kfree(fa);
        paging64_free_space(space);
        return -1;
    }
    return task64_id(child);
}
