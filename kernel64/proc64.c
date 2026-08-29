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

/* --- Argumenty na stosie programu --------------------------------
 *
 * Uklad jest ten sam, co wszedzie: najpierw same napisy, nad nimi
 * tablica wskaznikow zakonczona zerem, a program dostaje w rejestrach
 * ich liczbe i adres tablicy. Nic w tym magicznego - to zwykla pamiec
 * programu, ktora akurat jadro wypelnilo przed jego pierwsza
 * instrukcja.
 *
 * Piszemy wprost pod adresy uzytkownika, bo w tej chwili stoimy juz
 * w jego przestrzeni. Zwraca nowy wierzcholek stosu i wpisuje adres
 * tablicy. */
static uint64_t build_args(uint64_t rsp, const struct args64 *args,
                           uint64_t *argv_va)
{
    uint64_t ptrs[ARG_MAX];
    int argc = args ? args->argc : 0;

    if (argc > ARG_MAX)
        argc = ARG_MAX;

    for (int i = 0; i < argc; i++) {
        uint32_t len = 0;
        while (args->v[i][len])
            len++;
        rsp -= len + 1;
        for (uint32_t j = 0; j <= len; j++)
            ((char *)(uintptr_t)rsp)[j] = args->v[i][j];
        ptrs[i] = rsp;
    }

    rsp &= ~0xFULL;
    rsp -= (uint64_t)(argc + 1) * 8;
    uint64_t *tab = (uint64_t *)(uintptr_t)rsp;
    for (int i = 0; i < argc; i++)
        tab[i] = ptrs[i];
    tab[argc] = 0;                   /* tablica konczy sie zerem */

    *argv_va = rsp;

    /* Wierzcholek wyrownany do szesnastu bajtow, minus osiem.
       Program zaczyna sie od funkcji _start napisanej w C, a kompilator
       traktuje ja jak kazda inna: zaklada, ze stos wyglada tak, jak po
       instrukcji call, czyli z adresem powrotu na wierzchu. Na tym
       zalozeniu opiera wyrownanie swoich zmiennych - i gdy sie nie
       zgadza, pierwsza instrukcja SSE zapisujaca szesnascie bajtow
       (movaps) konczy sie wyjatkiem ochrony ogolnej. Tak samo liczy
       stos doreczenie sygnalu, i z tego samego powodu. */
    return (rsp & ~0xFULL) - 8;
}

/* Przepisanie napisu z pamieci programu do jadra.
   Sprawdzamy strone po stronie, bo napis moze konczyc sie tuz przed
   granica tego, co program ma zmapowane. */
static int copy_user_string(uint64_t ptr, char *dst, int max)
{
    for (int i = 0; i < max - 1; i++) {
        if ((ptr & 0xFFF) == 0 || i == 0) {
            if (!paging64_user_range(ptr, 1))
                return -1;
        }
        char c = *(const char *)(uintptr_t)ptr++;
        dst[i] = c;
        if (!c)
            return i;
    }
    dst[max - 1] = '\0';
    return max - 1;
}

/* Zbiera argv podane przez program. Zwraca liczbe argumentow. */
int proc64_copy_args(uint64_t argv_ptr, struct args64 *out)
{
    out->argc = 0;
    if (!argv_ptr)
        return 0;
    if (!paging64_user_range(argv_ptr, (uint64_t)(ARG_MAX + 1) * 8))
        return 0;

    const uint64_t *tab = (const uint64_t *)(uintptr_t)argv_ptr;
    for (int i = 0; i < ARG_MAX && tab[i]; i++) {
        if (copy_user_string(tab[i], out->v[i], ARG_LEN) < 0)
            break;
        out->argc++;
    }
    return out->argc;
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
    if (child) {
        task64_inherit_fds(child);   /* otwarte pliki ida z pamiecia */
        task64_inherit_brk(child);
    }
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
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len,
                    uint64_t argv_ptr)
{
    char kname[16];
    struct args64 args;

    if (len == 0 || len >= sizeof(kname) ||
        !paging64_user_range((uint64_t)(uintptr_t)name, len))
        return -1;
    for (uint64_t i = 0; i < len; i++)
        kname[i] = name[i];
    kname[len] = '\0';

    /* Argumenty przepisujemy do jadra ZANIM zniknie pamiec, w ktorej
       leza - tak samo jak nazwe programu */
    proc64_copy_args(argv_ptr, &args);
    if (args.argc == 0) {
        args.argc = 1;
        for (uint64_t i = 0; i <= len && i < ARG_LEN - 1; i++)
            args.v[0][i] = kname[i];
    }

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

    uint64_t entry, rsp, brk_base;
    if (elf64_load(image, size, &entry, &brk_base) < 0 ||
        !(rsp = elf64_make_stack())) {
        /* Stary obraz juz nie istnieje, a nowy sie nie zaladowal -
           procesowi nie ma do czego wrocic */
        printk("[jadro] exec: '%s' nie da sie zaladowac\n", kname);
        paging64_free_space(old);
        task64_exit_code(-1);
    }
    paging64_free_space(old);
    task64_set_brk_base(brk_base);   /* nowy obraz, nowa sterta */
    task64_rename(kname);

    uint64_t argv_va = 0;
    rsp = build_args(rsp, &args, &argv_va);

    /* Ramka powrotu opisuje teraz nowy program */
    r->rip = entry;
    r->rsp = rsp;
    r->rax = r->rbx = r->rcx = r->rdx = 0;
    r->rbp = 0;
    r->rdi = (uint64_t)args.argc;
    r->rsi = argv_va;
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
    struct args64 args;
    int quiet;
};

static void spawn_thread(void *arg)
{
    struct spawn_req *req = (struct spawn_req *)arg;
    char name[16];
    struct args64 args = req->args;
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

    uint64_t entry, brk_base;
    if (elf64_load(image, size, &entry, &brk_base) < 0)
        task64_exit();
    task64_set_brk_base(brk_base);

    uint64_t rsp = elf64_make_stack();
    if (!rsp) {
        printk("[ !! ] elf64: brak pamieci na stos programu\n");
        task64_exit();
    }

    if (!quiet)
        printk("[ OK ] Program '%s' z initrd: %u B, wejscie %x\n",
               name, size, (uint32_t)entry);

    uint64_t argv_va = 0;
    rsp = build_args(rsp, &args, &argv_va);

    enter_ring3_64(entry, rsp, (uint64_t)args.argc, argv_va);
}

int proc64_spawn(const char *name, const struct args64 *args, int quiet)
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
    req->quiet = quiet;
    if (args) {
        req->args = *args;
    } else {
        /* Bez argumentow program i tak dostaje swoja nazwe - argv[0]
           jest tym, czym jest wszedzie */
        req->args.argc = 1;
        for (int k = 0; k < ARG_LEN - 1 && req->name[k]; k++)
            req->args.v[0][k] = req->name[k];
        req->args.v[0][ARG_LEN - 1] = '\0';
        for (int k = 0; k < ARG_LEN; k++)
            if (!req->name[k]) { req->args.v[0][k] = '\0'; break; }
    }

    struct task64 *t = task64_create(req->name, spawn_thread, req);
    if (!t) {
        kfree(req);
        return -1;
    }
    return task64_id(t);
}
