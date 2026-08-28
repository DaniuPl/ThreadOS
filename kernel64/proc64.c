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
#include "../kernel/include/initrd.h"
#include "elf64.h"
#include "syscall64.h"

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

/* Podmiana obrazu procesu (exec)
 *
 * Proces zostaje ten sam - ten sam numer zadania, ten sam rodzic,
 * ten sam stos jadra - a zmienia sie wszystko, co ma w pamieci.
 *
 * Powrot do nowego programu nie wymaga zadnej osobnej sciezki: jestesmy
 * w obsludze wywolania systemowego, a ta wraca do ring 3 przez ramke,
 * ktora dostala. Wystarczy ja przepisac - nowy adres startu, nowy stos,
 * wyzerowane rejestry - i pozwolic jej wrocic. Program po drugiej
 * stronie nie ma jak zauwazyc, ze wrocil gdzie indziej, niz wolal.
 *
 * Kolejnosc jest tu jedyna mozliwa: nazwe trzeba przepisac do jadra,
 * ZANIM zniknie pamiec, w ktorej lezy, a stara przestrzen zwolnic
 * dopiero wtedy, gdy procesor z niej zejdzie.
 */
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len)
{
    char kname[16];

    if (len == 0 || len >= sizeof(kname) ||
        !paging64_user_range((uint64_t)(uintptr_t)name, len))
        return -1;
    for (uint64_t i = 0; i < len; i++)
        kname[i] = name[i];
    kname[len] = '\0';

    uint32_t size = 0;
    const void *image = initrd_find(kname, &size);
    if (!image) {
        printk("[jadro] exec: nie ma programu '%s'\n", kname);
        return -1;
    }

    uint64_t space = paging64_new_space();
    if (!space)
        return -1;

    /* Stos jadra zostaje ten sam - stoimy na nim w tej chwili */
    uint64_t old = task64_swap_space(space, 0);

    uint64_t entry, rsp;
    if (elf64_load(image, size, &entry) < 0 || !(rsp = elf64_make_stack())) {
        /* Stary obraz juz nie istnieje, a nowy sie nie zaladowal -
           procesowi nie ma do czego wrocic */
        printk("[jadro] exec: '%s' nie da sie zaladowac\n", kname);
        paging64_free_space(old);
        task64_exit_code(-1);
    }
    paging64_free_space(old);
    task64_rename(kname);

    /* Ramka powrotu opisuje teraz nowy program */
    r->rip = entry;
    r->rsp = rsp;
    r->rax = r->rbx = r->rcx = r->rdx = 0;
    r->rsi = r->rdi = r->rbp = 0;
    r->r8 = r->r9 = r->r10 = r->r11 = 0;
    r->r12 = r->r13 = r->r14 = r->r15 = 0;
    return 0;
}

/* --- Uruchamianie programu z initrd -------------------------------
 *
 * Nowy proces to nowe zadanie, nowa przestrzen adresowa i program
 * wczytany z pliku. Jadro robi to przy starcie, a shell na zyczenie -
 * i jest to dokladnie ta sama droga.
 */
struct spawn_req {
    char name[16];
    uint64_t arg;
    int quiet;
};

static void spawn_thread(void *arg)
{
    struct spawn_req *req = (struct spawn_req *)arg;
    char name[16];
    uint64_t user_arg = req->arg;
    int quiet = req->quiet;

    for (int i = 0; i < (int)sizeof(name); i++)
        name[i] = req->name[i];
    kfree(req);

    uint32_t size = 0;
    const void *image = initrd_find(name, &size);
    if (!image) {
        printk("[ !! ] initrd: nie ma pliku '%s'\n", name);
        task64_exit();
    }

    /* Wlasna przestrzen adresowa. Od tej chwili gigabajt programu
       nalezy tylko do niego - a pamiec jadra widac dalej, bo jest
       we wszystkich przestrzeniach ta sama. */
    uint64_t space = paging64_new_space();
    if (!space) {
        printk("[ !! ] brak pamieci na przestrzen adresowa\n");
        task64_exit();
    }

    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(space, kstack & ~0xFULL);

    uint64_t entry;
    if (elf64_load(image, size, &entry) < 0)
        task64_exit();

    uint64_t rsp = elf64_make_stack();
    if (!rsp) {
        printk("[ !! ] elf64: brak pamieci na stos programu\n");
        task64_exit();
    }

    if (!quiet)
        printk("[ OK ] Program '%s' z initrd: %u B, wejscie %x\n",
               name, size, (uint32_t)entry);

    enter_ring3_64(entry, rsp, user_arg);
}

int proc64_spawn(const char *name, uint64_t arg, int quiet)
{
    struct spawn_req *req;
    uint32_t size = 0;
    int i;

    if (!initrd_find(name, &size))
        return -1;

    req = kmalloc(sizeof(*req));
    if (!req)
        return -1;
    for (i = 0; i < (int)sizeof(req->name) - 1 && name[i]; i++)
        req->name[i] = name[i];
    req->name[i] = '\0';
    req->arg = arg;
    req->quiet = quiet;

    struct task64 *t = task64_create(req->name, spawn_thread, req);
    if (!t) {
        kfree(req);
        return -1;
    }
    return task64_id(t);
}
