/* Watki jadra w trybie dlugim
 *
 * Lista jest cykliczna, a watek glowny - ten, ktory przyszedl prosto
 * z rozruchu - nigdy nie spi i pelni role watku bezczynnosci. Dzieki
 * temu scheduler zawsze ma kogo wybrac i nie musi znac przypadku
 * "nie ma nikogo gotowego".
 *
 * Zakonczone watki zostaja na liscie jako zombie i znikaja przy
 * kolejnym przelaczeniu - nigdy nie sprzatamy tego, na ktorego stosie
 * wlasnie stoimy.
 */

#include <stddef.h>
#include <stdint.h>
#include "task64.h"
#include "paging64.h"
#include "syscall64.h"
#include "elf64.h"
#include "fpu64.h"
#include "../kernel/include/fs.h"
#include "pipe64.h"
#include "proc64.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/zamekjadra.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/io.h"

#define STACK_SIZE 16384
#define NAME_MAX   16

/* Deskryptory otwartych plikow.
 *
 * Trzy pierwsze numery sa zajete od zawsze i nie odnosza sie do
 * zadnego pliku: zero to klawiatura, jeden to konsola, dwa zostaje
 * na bledy. Reszta to pliki na dysku - kazdy z wlasnym miejscem
 * w pliku, bo dwa procesy moga czytac ten sam plik w innym tempie. */
#define FD_MAX   8
#define FD_FIRST 3

/* Rodzaje siedza w naglowku - programy i syscalle tez o nich wiedza.
   Wolny numer 0, 1 albo 2 znaczy "domyslne wejscie i wyjscie":
   klawiatura i konsola. Dopiero przekierowanie wstawia tam cos innego. */
struct fd_entry {
    int kind;
    int slot;                    /* i-wezel w PFS albo numer potoku */
    uint32_t pos;                /* gdzie stoi ten konkretny czytelnik */
};

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_ZOMBIE };

struct task64 {
    uint64_t rsp;                /* wierzcholek stosu, gdy watek nie biegnie */
    void *stack;                 /* NULL dla watku glownego - ma stos z rozruchu */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;
    uint64_t space;              /* wlasna przestrzen adresowa (0 = jadra) */
    uint64_t kstack_top;         /* stos jadra dla wejsc z ring 3 */
    int id;
    int parent;                  /* kto je stworzyl - dla czekania na koniec */
    int sig_pending;             /* sygnal czekajacy na dostarczenie */
    uint64_t sig_handler;        /* procedura programu (0 = zabij) */
    uint64_t sig_restorer;       /* powrot z procedury do jadra */
    struct fd_entry fds[FD_MAX];
    void *fpu;                   /* stan rejestrow zmiennoprzecinkowych */
    uint64_t brk_base;           /* poczatek sterty (za obrazem programu) */
    uint64_t brk;                /* dokad program o nia poprosil */
    uint64_t mmap;               /* nastepny wolny adres na mapowania */
    struct mapa_pliku {
        uint64_t start, koniec;  /* zakres adresow (0 = wolny wpis) */
        uint32_t rozmiar;        /* ile bajtow ma sam plik */
        uint32_t ile_blokow;
        uint32_t *bloki;         /* numery blokow PFS, po kolei */
        uint32_t zapis;          /* 1 = zmiany wracaja do pliku */
    } mapy[MMAP_PLIKOW];
    int cwd;                     /* katalog roboczy (i-wezel, FS_ROOT = /) */
    int vc;                      /* konsola wirtualna, na ktorej pisze */
    /* Gdzie wolno prowadzic to zadanie.
       'wszedzie' = 1 znaczy, ze moze je wziac kazdy rdzen. Domyslnie
       jest zero i tak ma byc: zadanie siegajace po dysk, siec albo
       cokolwiek innego pisanego dla jednego rdzenia zostaje tam, gdzie
       bylo. 'bezczynny_rdzenia' >= 0 znaczy, ze to zadanie bezczynne
       TEGO rdzenia - stoi na jego stosie i nikt inny go nie prowadzi. */
    int wszedzie;
    /* 'samodzielny' = 1 znaczy, ze watek nie bierze wielkiego zamka
       jadra: ten, kto o niego poprosil, wzial na siebie, ze watek
       dotyka wylacznie rzeczy znoszacych dwa rdzenie naraz. Proces
       ring 3 tego NIE ma - jego strona jadrowa to zwykly kod jadra,
       ktory ma byc chroniony jak kazdy inny. */
    int samodzielny;
    int bezczynny_rdzenia;
    char name[NAME_MAX];
    struct task64 *next;
};

extern void context_switch64(uint64_t *old_rsp, uint64_t new_rsp);

/* --- Co rdzen ma swojego ---------------------------------------------
 *
 * Przez cala historie tego jadra biezace zadanie bylo JEDNA zmienna
 * globalna - i wlasnie dlatego wiecej niz jeden rdzen nie mial jak
 * prowadzic zadan. Teraz jest ich tyle, ile rdzeni, a ktore z nich
 * jest moje, mowi sam procesor (numer w jego lokalnym APIC).
 */
struct rdzen {
    struct task64 *biezace;
    struct task64 *bezczynne;
};
static struct rdzen rdzenie[SMP_RDZENI];

#define current   (rdzenie[smp_rdzen()].biezace)
#define idle_task (rdzenie[smp_rdzen()].bezczynne)

/* Lista zadan jest jedna dla calej maszyny, wiec i zamek jest jeden.
   Trzyma sie go PRZEZ przelaczenie kontekstu, a zwalnia go ten, kto po
   drugiej stronie przyjdzie - dopiero wtedy stos poprzedniego zadania
   jest naprawde wolny. */
static struct spinlock zamek = SPINLOCK_WOLNY;

static int next_id;
static int scheduler_wstal;

static void set_name(struct task64 *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* Pierwszy kod kazdego nowego watku - context_switch64 "wraca" tutaj */
static void trampoline(void)
{
    /* Zamek listy zadan trzyma jeszcze ten, kto nas tu przelaczyl -
       i to my mamy go zwolnic. Nowe zadanie nie wraca z przelaczenia
       kontekstu jak wszystkie inne, wiec musi zrobic to samo tutaj. */
    spin_unlock(&zamek);
    /* Nowe zadanie zaczyna w kodzie jadra, wiec bierze wielki zamek.
     *
     * Nie bierze go tylko to, ktore wolno prowadzic kazdemu rdzeniowi -
     * bo ten, kto o nie poprosil, wzial juz na siebie, ze jego watek
     * dotyka wylacznie rzeczy znoszacych dwa rdzenie naraz. */
    if (!current->samodzielny)
        jadro_wejdz();
    __asm__ volatile("sti");
    current->entry(current->arg);
    task64_exit();
}

/* Watek bezczynnosci: nie robi nic i nigdy nie spi.
 *
 * Jest po to, zeby scheduler zawsze mial kogo wybrac. Bez niego
 * musialby umiec przypadek "nikt nie jest gotowy" - a jedyne, co
 * moglby wtedy zrobic, to czekac na przerwanie w srodku wlasnego
 * wywolania. Gdyby to wywolanie przyszlo z obslugi przerwania timera,
 * kolejne tykniecie wchodziloby w nie samo, i tak bez konca. */
static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile("hlt");
}

void scheduler64_init(void)
{
    struct task64 *main_task = kmalloc(sizeof(*main_task));
    main_task->stack = NULL;
    main_task->state = TASK_RUNNING;
    main_task->id = next_id++;
    main_task->wake_at = 0;
    main_task->space = 0;
    main_task->kstack_top = 0;
    main_task->parent = -1;
    main_task->sig_pending = 0;
    main_task->sig_handler = 0;
    main_task->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        main_task->fds[i].kind = FDK_WOLNY;
    main_task->brk_base = main_task->brk = 0;
    main_task->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_PLIKOW; i++)
        main_task->mapy[i].start = main_task->mapy[i].koniec = 0;
    main_task->fpu = fpu64_alloc();
    main_task->cwd = FS_ROOT;
    main_task->vc = 0;
    set_name(main_task, "main");
    main_task->wszedzie = 0;
    main_task->samodzielny = 0;
    main_task->bezczynny_rdzenia = -1;
    main_task->next = main_task;
    rdzenie[0].biezace = main_task;

    rdzenie[0].bezczynne = task64_create("bezczynny", idle_loop, 0);
    rdzenie[0].bezczynne->bezczynny_rdzenia = 0;
    scheduler_wstal = 1;

    /* Watek glowny konczy rozruch, wiec od tej chwili jest zwyklym
       zadaniem w kodzie jadra */
    jadro_wejdz();
}

int task_scheduler_gotowy(void)
{
    return scheduler_wstal;
}

struct task64 *task64_create(const char *name, void (*entry)(void *),
                             void *arg)
{
    struct task64 *t = kmalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->stack = kmalloc(STACK_SIZE);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->wake_at = 0;
    t->space = 0;
    t->kstack_top = 0;
    t->parent = current ? current->id : -1;
    t->sig_pending = 0;
    t->sig_handler = 0;
    t->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        t->fds[i].kind = FDK_WOLNY;
    t->brk_base = t->brk = 0;
    t->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_PLIKOW; i++)
        t->mapy[i].start = t->mapy[i].koniec = 0;
    t->fpu = fpu64_alloc();
    t->cwd = current ? current->cwd : FS_ROOT;
    t->vc = current ? current->vc : 0;
    t->id = next_id++;
    t->wszedzie = 0;            /* dopoki ktos nie powie inaczej */
    t->samodzielny = 0;
    t->bezczynny_rdzenia = -1;
    set_name(t, name);

    /* Stos ukladamy tak, by context_switch64 zdjal z niego szesc zer
       i "wrocil" do trampoliny. Jedno slowo zapasu na gorze pilnuje
       wyrownania stosu, ktorego wymaga konwencja wywolan. */
    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack + STACK_SIZE);
    *--sp = 0;                            /* wyrownanie do 16 bajtow */
    *--sp = (uint64_t)(uintptr_t)trampoline;   /* adres powrotu */
    for (int i = 0; i < 6; i++)
        *--sp = 0;                        /* rbp, rbx, r12-r15 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    irqflags_t flags = spin_lock_irq(&zamek);
    t->next = current->next;
    current->next = t;
    spin_unlock_irq(&zamek, flags);
    return t;
}

/* Zadanie, ktore wolno prowadzic KAZDEMU rdzeniowi. Zwykle zostaje na
   rozruchowym: reszta jadra pisana jest dla jednego rdzenia. */
struct task64 *task64_create_wszedzie(const char *name,
                                      void (*entry)(void *), void *arg)
{
    struct task64 *t = task64_create(name, entry, arg);
    if (t) {
        t->wszedzie = 1;
        t->samodzielny = 1;
    }
    return t;
}

/* Oznacza BIEZACE zadanie jako takie, ktore wolno prowadzic kazdemu
   rdzeniowi. Uzywa tego proces ring 3: jego strona jadrowa zostaje
   zwyklym kodem jadra pod wielkim zamkiem, ale samo liczenie w ring 3
   moze isc rownolegle z innymi. */
void task_wszedzie_dla_mnie(void)
{
    if (current)
        current->wszedzie = 1;
}

/* Slad po zakonczonym zadaniu.
 *
 * Samo zadanie znika z listy przy najblizszym przelaczeniu, ale jego
 * rodzic moze chciec poznac kod wyjscia jeszcze po fakcie - i wcale
 * nie musi zdazyc zapytac, zanim dziecko sie skonczy. Zostaje wiec
 * po nim ten jeden wpis, ktory rodzic odbiera przy czekaniu.
 *
 * Gdy nikt po niego nie przyjdzie, najstarszy wpis ustepuje miejsca
 * nowszemu: to jest slad, a nie kolejka, ktora wolno zapchac. */
#define EXIT_SLOTS 16

struct exit_rec {
    int used;
    int id;
    int parent;
    int code;
};

static struct exit_rec finished[EXIT_SLOTS];
static uint32_t exit_next;

static void record_exit(struct task64 *t, int code)
{
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (!finished[i].used) {
            finished[i].used = 1;
            finished[i].id = t->id;
            finished[i].parent = t->parent;
            finished[i].code = code;
            return;
        }
    }
    /* Wszystko zajete - nadpisujemy po kolei, zeby jeden zapominalski
       rodzic nie zablokowal rejestru na zawsze */
    struct exit_rec *r = &finished[exit_next++ % EXIT_SLOTS];
    r->used = 1;
    r->id = t->id;
    r->parent = t->parent;
    r->code = code;
}

/* Czy zadanie ma jeszcze zywe dzieci? Bez tego czekanie na nieistniejace
   dziecko trwaloby wiecznie. */
static int has_children(int id)
{
    struct task64 *t = current;
    do {
        if (t != current && t->parent == id && t->state != TASK_ZOMBIE)
            return 1;
        t = t->next;
    } while (t != current);
    return 0;
}

/* Czeka na koniec dowolnego wlasnego dziecka. Zwraca jego numer
   i wpisuje kod wyjscia; -1 znaczy "nie ma na co czekac" albo
   "czekanie przerwal sygnal".
 *
 * To drugie jest tu rownie wazne, co pierwsze. Sygnal dostarcza sie
 * WYLACZNIE przy powrocie do pierscienia 3, a ta petla nigdy tam nie
 * wraca - wiec bez tego sprawdzenia sygnal wyslany do czekajacego
 * procesu czekalby na dostarczenie tak dlugo, jak dlugo trwa
 * czekanie. Gdy dziecko nie konczy sie samo, znaczy to: na zawsze. */
int task64_wait(int *code)
{
    for (;;) {
        irqflags_t flags = irq_save();
        for (int i = 0; i < EXIT_SLOTS; i++) {
            if (finished[i].used && finished[i].parent == current->id) {
                int id = finished[i].id;
                if (code)
                    *code = finished[i].code;
                finished[i].used = 0;
                irq_restore(flags);
                return id;
            }
        }
        int alive = has_children(current->id);
        int signalled = current->sig_pending != 0;
        irq_restore(flags);

        if (!alive || signalled)
            return -1;
        task64_sleep(10);
    }
}

/* Kto naprawde trzyma konsole: najglebszy zywy potomek zadania.
 *
 * Shell czeka na swoje dziecko, a to moze czekac na swoje - Ctrl+C ma
 * trafic w to, co na koncu tego lancucha faktycznie dziala, a nie
 * w shella, ktory tylko czeka. Inaczej przerwanie programu zabijaloby
 * shella i zostawialo program przy zyciu.
 *
 * Zejscie w dol jest skonczone same z siebie - dziecko dostaje numer
 * wiekszy od rodzica, wiec 'best' rosnie przy kazdym kroku - ale
 * ograniczenie zostaje mimo to. Petla szukajaca konca petli to
 * ostatnie miejsce, w ktorym chce sie polegac na rozumowaniu. */
int task64_foreground(int root)
{
    irqflags_t flags = irq_save();
    int best = root;

    for (int depth = 0; depth < 64; depth++) {
        struct task64 *t = current;
        int found = 0;
        do {
            if (t->parent == best && t->id > best &&
                t->state != TASK_ZOMBIE) {
                best = t->id;
                found = 1;
                break;
            }
            t = t->next;
        } while (t != current);
        if (!found)
            break;
    }
    irq_restore(flags);
    return best;
}

/* Sprzata zakonczone watki - nigdy tego, na ktorego stosie stoimy */
static void reap_zombies(void)
{
    struct task64 *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task64 *z = t->next;
            t->next = z->next;
            fpu64_free(z->fpu);
            kfree(z->stack);
            kfree(z);
        } else {
            t = t->next;
        }
    }
}

/* Obchodzi liste, budzac tych, ktorym minal czas snu, i zwraca
   pierwszego gotowego poza biezacym. Watek bezczynnosci bierzemy
   dopiero wtedy, gdy nie ma nikogo innego - inaczej zjadalby polowe
   czasu procesora, nic nie robiac. */
/* Czy rdzen 'nr' moze wziac to zadanie?
 *
 * Rdzen rozruchowy prowadzi wszystko, tak jak dotad. Rdzen wtorny -
 * tylko to, co ktos jawnie na to pozwolil, bo reszta jadra pisana jest
 * dla jednego rdzenia i cicho na to liczy. Zadanie bezczynne nalezy do
 * swojego rdzenia i do nikogo innego. */
static int wolno_wziac(const struct task64 *t, int nr)
{
    if (t->bezczynny_rdzenia >= 0)
        return t->bezczynny_rdzenia == nr;
    return nr == 0 || t->wszedzie;
}

static struct task64 *pick_ready(int nr)
{
    uint32_t now = pit_ticks();
    struct task64 *ja = rdzenie[nr].biezace;
    struct task64 *moje_bezczynne = rdzenie[nr].bezczynne;
    struct task64 *t = ja->next;
    struct task64 *found = NULL;

    while (t != ja) {
        if (t->state == TASK_SLEEPING && now >= t->wake_at)
            t->state = TASK_READY;
        if (t->state == TASK_READY && !found && t != moje_bezczynne &&
            wolno_wziac(t, nr))
            found = t;
        t = t->next;
    }
    if (!found && moje_bezczynne && moje_bezczynne != ja &&
        moje_bezczynne->state == TASK_READY)
        found = moje_bezczynne;
    return found;
}

/* Ustawia procesorowi to, co nalezy do wybranego zadania: jego
   przestrzen adresowa i stos, na ktory ma wejsc jadro przy przerwaniu
   albo wywolaniu systemowym z ring 3.
 *
 * CR3 przeladowujemy tylko wtedy, gdy naprawde sie zmienia - kazde
 * przeladowanie wyrzuca caly bufor tlumaczen adresow, wiec robienie
 * tego przy przelaczaniu miedzy watkami jadra bylo by czysta strata. */
static void apply_context(struct task64 *t, int nr)
{
    uint64_t want = t->space ? t->space : paging64_kernel_space();
    if (paging64_current_space() != want)
        paging64_switch(want);
    /* Stos jadra dla wejsc z ring 3 - w segmencie stanu zadania TEGO
       rdzenia, bo kazdy ma swoj */
    (void)nr;
    if (t->kstack_top)
        syscall64_set_kernel_stack(t->kstack_top);
}

/* Oddaje przestrzen zadania i wraca do przestrzeni jadra.
   Wolane, gdy zadanie umiera - a umiera, stojac jeszcze na wlasnym
   CR3, wiec najpierw trzeba z niego zejsc. */
static void drop_space(struct task64 *t)
{
    if (!t->space)
        return;
    uint64_t doomed = t->space;
    t->space = 0;
    t->kstack_top = 0;
    paging64_switch(paging64_kernel_space());
    paging64_free_space(doomed);
}

/* Bierze przestrzen na wlasnosc i od razu na nia przechodzi.
   Wolane przez watek, ktory wlasnie zaladowal program. */
void task64_enter_space(uint64_t space, uint64_t kstack_top)
{
    irqflags_t flags = irq_save();
    current->space = space;
    current->kstack_top = kstack_top;
    apply_context(current, smp_rdzen());
    irq_restore(flags);
}

/* Podmienia przestrzen na nowa i oddaje stara - do zwolnienia przez
   wolajacego, ale dopiero PO tym, jak procesor z niej zejdzie. */
uint64_t task64_swap_space(uint64_t space, uint64_t kstack_top)
{
    irqflags_t flags = irq_save();
    uint64_t old = current->space;
    current->space = space;
    if (kstack_top)              /* zero = zostaw stos jadra bez zmian */
        current->kstack_top = kstack_top;
    apply_context(current, smp_rdzen());
    irq_restore(flags);
    return old;
}

void task64_rename(const char *name)
{
    irqflags_t flags = irq_save();
    set_name(current, name);
    irq_restore(flags);
}

/* Wybiera nastepny watek i przelacza kontekst. Wolac z IF=0.
 *
 * Zamek listy zadan bierzemy bez ruszania IF: gdyby timer wszedl
 * w srodek sekcji trzymajacej ten zamek, czekalby na niego bez konca,
 * bo zwolnic go moze tylko przerwany wlasnie kod. Trzymamy go PRZEZ
 * przelaczenie, a zwalnia go ten, kto po drugiej stronie przyjdzie -
 * dopiero wtedy stos poprzedniego zadania jest naprawde wolny.
 */
static void przelacz(int nowy_stan)
{
    int nr = smp_rdzen();
    if (!rdzenie[nr].biezace)
        return;

    /* Wielki zamek jadra oddajemy NA CZAS PRZELACZENIA - trzymanie go
       przez zmiane kontekstu zamykaloby jadro dla wszystkich az do
       przebudzenia tego zadania. Glebokosc lezy na stosie TEGO
       zadania, wiec kazde odzyska dokladnie swoja. */
    int glebokosc = jadro_zwolnij_wszystko();

    spin_lock(&zamek);
    /* Stan biezacego zadania zmieniamy juz pod zamkiem, razem z wyborem
       nastepnego: ustawiony wczesniej dawalby okno, w ktorym zadanie
       jest oznaczone jako spiace, a jego stos wciaz uzywany. */
    if (nowy_stan >= 0)
        rdzenie[nr].biezace->state = (enum task_state)nowy_stan;
    reap_zombies();

    struct task64 *next = pick_ready(nr);

    /* Nie ma nikogo innego - zostajemy, o ile sami mozemy biec dalej.
       Watek bezczynnosci sprawia, ze przypadek "nie ma nikogo, a my nie
       mozemy" po prostu nie wystepuje. */
    if (!next)
        next = rdzenie[nr].biezace;

    if (next == rdzenie[nr].biezace) {
        if (nowy_stan < 0)
            next->state = TASK_RUNNING;
        spin_unlock(&zamek);
        jadro_przywroc(glebokosc);
        return;
    }

    struct task64 *prev = rdzenie[nr].biezace;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    rdzenie[nr].biezace = next;

    /* Rejestry zmiennoprzecinkowe sa takim samym stanem zadania, jak
       reszta - z ta roznica, ze procesor zapisuje je jedna instrukcja
       do obszaru 512 bajtow. Bez tego dwa programy liczace rownoczesnie
       nadpisywalyby sobie wyniki, nie wiedzac o tym. */
    fpu64_save(prev->fpu);
    fpu64_load(next->fpu);

    apply_context(next, nr);
    context_switch64(&prev->rsp, next->rsp);
    spin_unlock(&zamek);
    /* Od tej chwili biegnie juz zadanie, ktore tu wrocilo - i to ono
       odbiera swoja glebokosc ze swojego stosu */
    jadro_przywroc(glebokosc);
}

void schedule(void)
{
    przelacz(-1);
}

/* --- Rdzen wtorny dolacza do schedulera ------------------------------
 *
 * Kazdy rdzen potrzebuje zadania, na ktore ma wrocic, gdy nie ma nic do
 * roboty - inaczej nie byloby dokad odlozyc jego kontekstu. To zadanie
 * jest jego wlasnoscia: stoi na stosie, ktory rdzen dostal przy
 * obudzeniu, i nikt inny nie ma prawa go prowadzic. Ta funkcja nie
 * wraca.
 */
void task_rdzen_wejdz(int nr)
{
    if (nr <= 0 || nr >= SMP_RDZENI || !scheduler_wstal)
        return;

    struct task64 *t = kmalloc(sizeof(*t));
    if (!t)
        return;
    t->stack = NULL;             /* stos ma z trampoliny rozruchowej */
    t->state = TASK_RUNNING;
    t->id = -nr;                 /* ujemny numer: to nie jest proces */
    t->wake_at = 0;
    t->space = 0;
    t->kstack_top = 0;
    t->parent = -1;
    t->sig_pending = 0;
    t->sig_handler = 0;
    t->sig_restorer = 0;
    for (int i = 0; i < FD_MAX; i++)
        t->fds[i].kind = FDK_WOLNY;
    t->brk_base = t->brk = 0;
    t->mmap = USER64_MMAP_BASE;
    for (int i = 0; i < MMAP_PLIKOW; i++)
        t->mapy[i].start = t->mapy[i].koniec = 0;
    t->fpu = fpu64_alloc();
    t->cwd = FS_ROOT;
    t->vc = 0;
    t->wszedzie = 0;
    t->samodzielny = 1;         /* zadanie bezczynne nie bierze zamka */
    t->bezczynny_rdzenia = nr;
    set_name(t, "bezczynny");
    t->name[9] = (char)('0' + nr);
    t->name[10] = '\0';

    irqflags_t flags = spin_lock_irq(&zamek);
    t->next = rdzenie[0].biezace->next;
    rdzenie[0].biezace->next = t;
    rdzenie[nr].biezace = t;
    rdzenie[nr].bezczynne = t;
    spin_unlock_irq(&zamek, flags);

    __asm__ volatile("sti");
    for (;;) {
        smp_rob_zlecenie(nr);
        task64_yield();
        /* Nic dla mnie nie ma - stoj do najblizszego przerwania */
        __asm__ volatile("hlt");
    }
}

void task64_yield(void)
{
    irqflags_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task64_sleep(uint32_t ms)
{
    irqflags_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;   /* tykniecie = 10 ms */
    przelacz(TASK_SLEEPING);
    irq_restore(flags);
}

void task64_exit_code(int code)
{
    irq_save();
    task64_fd_close_all();
    /* Odeslij na dysk to, co proces napisal w zapisywalnym
       odwzorowaniu, a czego nie zdazyl odeslac sam - musi to sie stac
       TERAZ, dopoki jego przestrzen adresowa jeszcze zyje */
    proc64_msync();
    task64_mmap_zwolnij();
    drop_space(current);
    record_exit(current, code);
    przelacz(TASK_ZOMBIE);
    for (;;)                              /* zombie nie zostanie wybrany */
        __asm__ volatile("hlt");
}

void task64_exit(void)
{
    irq_save();
    task64_fd_close_all();
    /* Odeslij na dysk to, co proces napisal w zapisywalnym
       odwzorowaniu, a czego nie zdazyl odeslac sam - musi to sie stac
       TERAZ, dopoki jego przestrzen adresowa jeszcze zyje */
    proc64_msync();
    task64_mmap_zwolnij();
    drop_space(current);
    record_exit(current, 0);
    przelacz(TASK_ZOMBIE);
    for (;;)                              /* zombie nie zostanie wybrany */
        __asm__ volatile("hlt");
}

/* Zabija biezace zadanie z podanym powodem. Nie wraca.
 *
 * Wolane z obslugi wyjatku, gdy zawinil program w ring 3. Roznica
 * wzgledem task64_exit jest tylko taka, ze tu jest co powiedziec:
 * zadanie nie skonczylo sie samo, tylko zostalo zatrzymane. */
void task64_kill_current(const char *why)
{
    irq_save();
    printk("[jadro] zadanie %d (%s) zabite: %s\n",
           current->id, current->name, why);
    task64_fd_close_all();
    drop_space(current);
    record_exit(current, -1);          /* zabite, czyli nie swoja smiercia */
    przelacz(TASK_ZOMBIE);
    for (;;)
        __asm__ volatile("hlt");
}

/* --- Sygnaly ------------------------------------------------------
 *
 * Zadanie nosi jeden sygnal czekajacy na dostarczenie. Jeden wystarcza:
 * sygnal jest zawiadomieniem, a nie wiadomoscia - drugie Ctrl+C przed
 * dostarczeniem pierwszego nie niesie nowej informacji.
 *
 * Dostarczenie nie moze sie odbyc tutaj: zadanie jest w tej chwili
 * gdzie indziej, moze spiac, moze w srodku wywolania systemowego.
 * Odbierze go dopiero przy powrocie do pierscienia 3 - i to jest
 * jedyny moment, w ktorym da sie podmienic mu ramke. */
int task64_signal(int id, int sig)
{
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    int found = 0;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            t->sig_pending = sig;
            if (t->state == TASK_SLEEPING)
                t->state = TASK_READY;      /* sygnal przerywa czekanie */
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return found;
}

int task64_signal_pending(void)
{
    return current ? current->sig_pending : 0;
}

int task64_take_signal(void)
{
    irqflags_t flags = irq_save();
    int sig = current->sig_pending;
    current->sig_pending = 0;
    irq_restore(flags);
    return sig;
}

void task64_set_sighandler(uint64_t handler, uint64_t restorer)
{
    irqflags_t flags = irq_save();
    current->sig_handler = handler;
    current->sig_restorer = restorer;
    irq_restore(flags);
}

uint64_t task64_sighandler(void)  { return current->sig_handler; }
uint64_t task64_sigrestorer(void) { return current->sig_restorer; }
int task64_current_id(void)       { return current ? current->id : -1; }
int task64_id_current(void)       { return current ? current->id : -1; }

/* Czy zadanie o tym numerze wciaz zyje. Zombie sie nie liczy - kod
   sieciowy pyta o to, zeby posprzatac gniazda po zmarlych. */
int task64_alive(int id)
{
    irqflags_t flags = irq_save();
    struct task64 *t = current;
    int zyje = 0;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            zyje = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return zyje;
}

/* Katalog roboczy jest wlasnoscia zadania, tak samo jak deskryptory:
   dwa procesy moga stac w innych miejscach drzewa i zadne z nich nie
   przeszkadza drugiemu. Dziecko zaczyna tam, gdzie stal rodzic. */
int task64_cwd(void)          { return current ? current->cwd : FS_ROOT; }
void task64_set_cwd(int slot) { if (current) current->cwd = slot; }

/* Konsola wirtualna jest wlasnoscia zadania z tego samego powodu, co
   katalog roboczy: program uruchomiony na drugiej konsoli pisze na niej
   dalej, nawet gdy patrzymy na pierwsza. Przed startem schedulera
   (komunikaty rozruchowe) nie ma jeszcze zadnego zadania - wtedy
   pierwsza konsola. */
/* --- Wymiana stron: kto ma oddac ------------------------------------
 *
 * PMM, gdy zabraknie mu ramek, pyta o to jadro. Chodzimy po zadaniach
 * w kolko i prosimy kazde o oddanie stron na dysk. Zaczynamy od
 * nastepnego po ostatnio odwiedzonym, zeby nie obdzierac w kolko tego
 * samego procesu.
 *
 * Watki jadra pomijamy: ich przestrzen to przestrzen jadra, a pamiec
 * jadra na dysk nie idzie - nie ma jej kto sciagnac z powrotem, bo
 * sciaga ja obsluga bledu strony, ktora sama w jadrze siedzi.
 *
 * Wszystko idzie przy wylaczonych przerwaniach i nie jest to tylko
 * ostroznosc wobec listy zadan: miedzy zgaszeniem wpisu strony
 * a zapisaniem jej na dysk nikt nie moze dostac procesora. */
int task64_reclaim_page(void)
{
    static struct task64 *kursor;
    if (!current)                    /* scheduler jeszcze nie wystartowal */
        return 0;

    irqflags_t flags = irq_save();
    struct task64 *start = (kursor && kursor->state != TASK_ZOMBIE)
                               ? kursor : current;
    struct task64 *t = start;
    int oddane = 0;

    do {
        t = t->next;
        /* Partiami: samo przemiatanie tablic stron kosztuje wiecej niz
           zapis jednej strony, wiec nie oplaca sie chodzic po nich
           od nowa dla kazdej brakujacej ramki */
        if (t->space && t->state != TASK_ZOMBIE &&
            paging64_reclaim_pages(t->space, 32)) {
            kursor = t;
            oddane = 1;
            break;
        }
    } while (t != start);

    irq_restore(flags);
    return oddane;
}

int task64_vc(void)          { return current ? current->vc : 0; }
void task64_set_vc(int vc)   { if (current) current->vc = vc; }

/* --- Sterta programu ----------------------------------------------
 *
 * Jadro nie przydziela tu ani jednej strony. Przesuwa tylko granice
 * i zapamietuje, dokad program o nia poprosil - strony pojawiaja sie
 * dopiero przy pierwszym dotknieciu, w obsludze bledu strony.
 *
 * Roznica jest wieksza, niz wyglada: prosba o megabajt kosztuje zero,
 * a program, ktory poprosil i nie dotknal, nie zabiera nikomu nic. */
void task64_set_brk_base(uint64_t base)
{
    current->brk_base = base;
    current->brk = base;
    /* Nowy obraz programu to nowy uklad pamieci - mapowania z poprzedniego
       zniknely razem z jego przestrzenia */
    current->mmap = USER64_MMAP_BASE;
}

/* --- Obszar mapowan ------------------------------------------------
 *
 * Kolejne mapowania ida jedno za drugim w gore, bez zwalniania: proces
 * i tak zabiera ze soba caly ten obszar, gdy sie konczy. */
int task64_mmap_dodaj(uint64_t start, uint64_t koniec, uint32_t rozmiar,
                      uint32_t *bloki, uint32_t ile_blokow, uint32_t zapis)
{
    if (!current)
        return -1;
    for (int i = 0; i < MMAP_PLIKOW; i++) {
        if (current->mapy[i].koniec)
            continue;
        current->mapy[i].start = start;
        current->mapy[i].koniec = koniec;
        current->mapy[i].rozmiar = rozmiar;
        current->mapy[i].bloki = bloki;
        current->mapy[i].ile_blokow = ile_blokow;
        current->mapy[i].zapis = zapis;
        return 0;
    }
    return -1;
}

/* Wolane z obslugi bledu strony, wiec musi byc krotkie i bez zadnych
   niespodzianek: samo wyszukanie w tablicy czterech wpisow. */
int task64_mmap_znajdz(uint64_t adres, uint32_t *blok_pierwszy,
                       uint32_t *bajtow, uint32_t *zapis)
{
    if (!current)
        return 0;
    uint64_t strona = adres & ~0xFFFULL;

    for (int i = 0; i < MMAP_PLIKOW; i++) {
        struct mapa_pliku *m = &current->mapy[i];
        if (!m->koniec || strona < m->start || strona >= m->koniec)
            continue;

        uint64_t offset = strona - m->start;
        uint32_t indeks = (uint32_t)(offset / FS_BLOCK);
        if (indeks >= m->ile_blokow)
            return 0;
        *blok_pierwszy = indeks;
        /* Ile pliku zostalo od poczatku tej strony - ogon ostatniej
           strony ma byc wyzerowany, a nie wypelniony smieciem
           z dysku */
        *bajtow = (offset < m->rozmiar)
                      ? (uint32_t)(m->rozmiar - offset) : 0;
        *zapis = m->zapis;
        return 1;
    }
    return 0;
}

/* Numer bloku PFS pod danym indeksem odwzorowania obejmujacego adres */
uint32_t task64_mmap_blok(uint64_t adres, uint32_t indeks)
{
    uint64_t strona = adres & ~0xFFFULL;
    for (int i = 0; i < MMAP_PLIKOW; i++) {
        struct mapa_pliku *m = &current->mapy[i];
        if (!m->koniec || strona < m->start || strona >= m->koniec)
            continue;
        return indeks < m->ile_blokow ? m->bloki[indeks] : 0;
    }
    return 0;
}

void task64_mmap_zwolnij(void)
{
    if (!current)
        return;
    for (int i = 0; i < MMAP_PLIKOW; i++) {
        if (current->mapy[i].bloki)
            kfree(current->mapy[i].bloki);
        current->mapy[i].bloki = (uint32_t *)0;
        current->mapy[i].start = current->mapy[i].koniec = 0;
    }
}

/* Dziecko dostaje wlasna kopie opisow: tablice numerow blokow zwalnia
   kazdy u siebie, wiec nie moga byc wspolne. Gdy kopia sie nie uda,
   dziecko po prostu nie widzi tego pliku - a to lepsze niz dwa
   zadania zwalniajace ten sam wskaznik. */
void task64_mmap_dziedzicz(struct task64 *dziecko)
{
    for (int i = 0; i < MMAP_PLIKOW; i++) {
        dziecko->mapy[i] = current->mapy[i];
        dziecko->mapy[i].bloki = (uint32_t *)0;
        if (!current->mapy[i].koniec)
            continue;
        uint32_t n = current->mapy[i].ile_blokow;
        uint32_t *kopia = kmalloc(n * sizeof(uint32_t));
        if (!kopia) {
            dziecko->mapy[i].start = dziecko->mapy[i].koniec = 0;
            continue;
        }
        for (uint32_t k = 0; k < n; k++)
            kopia[k] = current->mapy[i].bloki[k];
        dziecko->mapy[i].bloki = kopia;
    }
}

/* Opis i-tego odwzorowania biezacego procesu - do odsylania stron na
   dysk, ktore musi obejsc kazde po kolei. Zwraca 0, gdy wpis wolny. */
int task64_mmap_opis(int i, uint64_t *start, uint64_t *koniec,
                     uint32_t *rozmiar, uint32_t *zapis)
{
    if (!current || i < 0 || i >= MMAP_PLIKOW || !current->mapy[i].koniec)
        return 0;
    *start = current->mapy[i].start;
    *koniec = current->mapy[i].koniec;
    *rozmiar = current->mapy[i].rozmiar;
    *zapis = current->mapy[i].zapis;
    return 1;
}

uint64_t task64_mmap_next(void)          { return current ? current->mmap : 0; }
void task64_set_mmap_next(uint64_t adr)  { if (current) current->mmap = adr; }

uint64_t task64_brk(void)      { return current->brk; }
uint64_t task64_brk_base(void) { return current->brk_base; }

void task64_set_brk(uint64_t brk)
{
    current->brk = brk;
}

/* Czy adres nalezy do sterty biezacego zadania? Pyta o to obsluga
   bledu strony, zanim uzna go za blad programu. */
int task64_in_heap(uint64_t addr)
{
    return current && current->brk_base &&
           addr >= current->brk_base && addr < current->brk;
}

void task64_inherit_brk(struct task64 *child)
{
    child->brk_base = current->brk_base;
    child->brk = current->brk;
    child->mmap = current->mmap;     /* dziecko widzi ten sam uklad */
    /* Dziecko dziedziczy prawo do biegania na kazdym rdzeniu: to
       wlasnosc programu, a nie pojedynczego zadania */
    child->wszedzie = current->wszedzie;
}

/* --- Deskryptory --------------------------------------------------
 *
 * Tablica nalezy do zadania, wiec deskryptor jednego procesu nie
 * znaczy nic w drugim - i to jest cala roznica miedzy numerem
 * deskryptora a i-wezlem, ktory jest wspolny dla calego systemu. */
int task64_fd_open(int slot)
{
    irqflags_t flags = irq_save();
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX; i++) {
        if (current->fds[i].kind == FDK_WOLNY) {
            current->fds[i].kind = FDK_PLIK;
            current->fds[i].slot = slot;
            current->fds[i].pos = 0;
            fd = i;
            break;
        }
    }
    irq_restore(flags);
    return fd;
}

/* Numer dla konca potoku. Szukamy od trzeciego, bo zero i jeden maja
   znaczenie umowne i wstawia sie tam tylko przez dup2. */
int task64_fd_pipe(int idx, int czytelnik)
{
    irqflags_t flags = irq_save();
    int fd = -1;
    for (int i = FD_FIRST; i < FD_MAX; i++) {
        if (current->fds[i].kind == FDK_WOLNY) {
            current->fds[i].kind = czytelnik ? FDK_POTOK_R : FDK_POTOK_W;
            current->fds[i].slot = idx;
            current->fds[i].pos = 0;
            fd = i;
            break;
        }
    }
    irq_restore(flags);
    return fd;
}

int task64_fd_kind(int fd)
{
    if (fd < 0 || fd >= FD_MAX)
        return FDK_WOLNY;
    return current->fds[fd].kind;
}

int task64_fd_slot(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind == FDK_WOLNY)
        return -1;
    return current->fds[fd].slot;
}

uint32_t task64_fd_pos(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind != FDK_PLIK)
        return 0;
    return current->fds[fd].pos;
}

void task64_fd_advance(int fd, uint32_t n)
{
    if (fd >= 0 && fd < FD_MAX && current->fds[fd].kind == FDK_PLIK)
        current->fds[fd].pos += n;
}

/* Zamkniecie konca potoku zmniejsza liczbe wlascicieli - i to wlasnie
   ono sprawia, ze czytajacy kiedykolwiek zobaczy koniec danych */
static void zwolnij(struct task64 *t, int fd)
{
    if (t->fds[fd].kind == FDK_POTOK_R)
        pipe64_unref(t->fds[fd].slot, 1);
    else if (t->fds[fd].kind == FDK_POTOK_W)
        pipe64_unref(t->fds[fd].slot, 0);
    t->fds[fd].kind = FDK_WOLNY;
}

int task64_fd_close(int fd)
{
    if (fd < 0 || fd >= FD_MAX || current->fds[fd].kind == FDK_WOLNY)
        return -1;
    irqflags_t flags = irq_save();
    zwolnij(current, fd);
    irq_restore(flags);
    return 0;
}

/* Przepisanie numeru: to tedy przekierowuje sie wejscie i wyjscie.
   Cel jest najpierw zamykany, bo zwykle jest to zero albo jeden. */
int task64_fd_dup2(int stary, int nowy)
{
    if (stary < 0 || stary >= FD_MAX || nowy < 0 || nowy >= FD_MAX)
        return -1;
    if (current->fds[stary].kind == FDK_WOLNY)
        return -1;
    if (stary == nowy)
        return nowy;

    irqflags_t flags = irq_save();
    if (current->fds[nowy].kind != FDK_WOLNY)
        zwolnij(current, nowy);
    current->fds[nowy] = current->fds[stary];
    if (current->fds[nowy].kind == FDK_POTOK_R)
        pipe64_ref(current->fds[nowy].slot, 1);
    else if (current->fds[nowy].kind == FDK_POTOK_W)
        pipe64_ref(current->fds[nowy].slot, 0);
    irq_restore(flags);
    return nowy;
}

/* Zamyka wszystko, co zadanie trzymalo - wolane, gdy sie konczy.
   Bez tego potok nigdy nie zobaczylby konca piszacych. */
void task64_fd_close_all(void)
{
    for (int i = 0; i < FD_MAX; i++)
        if (current->fds[i].kind != FDK_WOLNY)
            zwolnij(current, i);
}

/* Dziecko dziedziczy otwarte pliki razem z miejscem, w ktorym stoja -
   tak samo, jak dziedziczy pamiec. Potoki dostaja przy tym kolejnego
   wlasciciela, bo od teraz trzyma je dwoch. */
void task64_inherit_fds(struct task64 *child)
{
    irqflags_t flags = irq_save();
    for (int i = 0; i < FD_MAX; i++) {
        child->fds[i] = current->fds[i];
        if (child->fds[i].kind == FDK_POTOK_R)
            pipe64_ref(child->fds[i].slot, 1);
        else if (child->fds[i].kind == FDK_POTOK_W)
            pipe64_ref(child->fds[i].slot, 0);
    }
    irq_restore(flags);
}

int task64_id(const struct task64 *t)
{
    return t ? t->id : -1;
}

int task64_count(void)
{
    irqflags_t flags = irq_save();
    int n = 0;
    struct task64 *t = current;
    do {
        n++;
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return n;
}

void task64_dump(void)
{
    static const char *state_names[] = { "gotowy", "dziala", "spi", "zombie" };
    irqflags_t flags = irq_save();
    printk("  ID  STAN    NAZWA\n");
    struct task64 *t = current;
    do {
        printk("  %d   %s  %s%s\n", t->id, state_names[t->state], t->name,
               t == current ? " (biezacy)" : "");
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}
