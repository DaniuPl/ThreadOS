/* Wątki jądra i scheduler (round-robin z wywłaszczaniem)
 *
 * Każdy wątek ma własny stos 8 KiB. Przełączenie kontekstu (switch.S)
 * zapisuje rejestry callee-saved i ESP - resztę stanu trzyma stos:
 * wątek wywłaszczony przerwaniem ma na stosie pełną ramkę przerwania
 * i wznowi się przez iret, wątek po task_sleep/yield wróci ze
 * zwykłego wywołania schedule().
 *
 * Lista wątków jest cykliczna; "main" (dawne kmain) nigdy nie śpi
 * i pełni rolę wątku bezczynności (hlt), więc scheduler zawsze ma
 * kogo wybrać. Zakończone wątki (zombie) są sprzątane przy kolejnych
 * wywołaniach schedule() - nigdy nie sprzątamy bieżącego.
 */

#include <stddef.h>
#include "include/task.h"
#include "include/heap.h"
#include "include/fs.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"
#include "include/gdt.h"
#include "include/paging.h"
#include "include/kfile.h"
#include "include/pipe.h"
#include "include/signal.h"
#include "include/exec.h"
#include "include/spinlock.h"
#include "include/smp.h"
#include "include/biglock.h"

#define STACK_SIZE 8192
#define NAME_MAX   16

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED,
                  TASK_ZOMBIE };

struct task {
    uint32_t esp;
    uint32_t kstack_top;        /* dla TSS.esp0 (przerwania z ring 3) */
    uint32_t cr3;               /* przestrzeń adresowa zadania */
    int owns_dir;               /* czy katalog stron zwolnić przy reap */
    uint32_t brk;               /* koniec sterty procesu (sbrk); 0 = wątek jądra */
    uint32_t brk_base;          /* poczatek sterty - stad zaczyna sie obszar
                                   przydzielany dopiero przy pierwszym dotknieciu */
    uint32_t mmap_next;         /* nastepny wolny adres mmap (0 = wątek jądra) */
    /* Pliki odwzorowane w pamieci. Numery blokow ustalamy raz, przy
       zakladaniu odwzorowania, i czytamy je potem warstwa blokowa
       z pominieciem kodu PFS - z tego samego powodu, co przy wymianie
       stron: PFS ma jeden wspolny bufor na blok, a blad strony potrafi
       trafic w srodku dowolnej operacji na plikach. */
    struct file_map {
        uint32_t start, end;  /* zakres adresow (0 = wolny wpis) */
        uint32_t size;        /* ile bajtow ma sam plik */
        uint32_t nblocks_of;
        uint32_t *blocks;
        uint32_t write;          /* 1 = zmiany wracaja do pliku */
    } maps[MMAP_FILES];
    int cwd;                     /* katalog roboczy (slot PFS, -1 = root) */
    int parent;                  /* pid rodzica (-1 = brak, sierota) */
    int vc;                      /* konsola, na ktorej zadanie pisze */
    uint32_t sig_pending;        /* maska oczekujacych sygnalow */
    uint32_t sig_handler[NSIG];  /* SIG_DFL / SIG_IGN / adres procedury */
    uint32_t sig_tramp;          /* trampolina powrotu (adres w ring 3) */
    uint32_t sig_frame;          /* ramka przerwanego stanu na stosie */
    int in_handler;              /* czy procedura obslugi wlasnie dziala */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;           /* tyknięcie, przy którym obudzić */
    int id;
    int priority;               /* 0=wysoki, 1=normalny, 2=niski (idle) */
    /* Gdzie wolno prowadzic to zadanie.
       'wszedzie' = 1 znaczy, ze moze je wziac kazdy rdzen. Domyslnie
       jest zero i tak ma byc: zadanie, ktore siega po dysk, siec albo
       cokolwiek innego pisanego dla jednego rdzenia, ma zostac tam,
       gdzie bylo. Rdzen wtorny bierze tylko to, co ktos jawnie na to
       pozwolil.
       'bezczynny_rdzenia' >= 0 znaczy, ze to zadanie bezczynne TEGO
       rdzenia - nikt inny go nie prowadzi, bo stoi na jego stosie. */
    int anycore;
    /* 'samodzielny' = 1 znaczy, ze watek nie bierze wielkiego zamka
       jadra: ten, kto o niego poprosil, wzial na siebie, ze watek
       dotyka wylacznie rzeczy znoszacych dwa rdzenie naraz. Proces
       ring 3 tego NIE ma - jego strona jadrowa to zwykly kod jadra,
       ktory ma byc chroniony jak kazdy inny. */
    int nolock;
    int idle_for_core;
    int exit_code;              /* kod zwrocony przez task_exit */
    int killed;                 /* oczekujace zabicie (kill) */
    char name[NAME_MAX];
    void *stack;                /* NULL dla wątku main (stos z boot.S) */
    struct file_desc fds[TASK_MAX_FD];
    struct task *next;
};

/* Stos rozruchowy z boot.S - wątek main kontynuuje na nim */
extern char stack_top[];

extern void context_switch(uint32_t *old_esp_store, uint32_t new_esp);

/* --- Co rdzen ma swojego ---------------------------------------------
 *
 * Przez cala historie tego jadra biezace zadanie bylo JEDNA zmienna
 * globalna - i wlasnie dlatego wiecej niz jeden rdzen nie mial jak
 * prowadzic zadan. Teraz jest ich tyle, ile rdzeni, a ktore z nich
 * jest moje, mowi sam procesor (numer w jego lokalnym APIC).
 *
 * Makro 'current' zostaje, bo caly kod jadra mowi tym slowem i nie ma
 * powodu, zeby przestal. Zmienilo sie tylko to, na co pokazuje.
 */
struct core {
    struct task *running;
    struct task *idle;      /* to, do ktorego rdzen wraca bez pracy */
};
static struct core cores[SMP_CORES];

#define current (cores[smp_core()].running)

/* Lista zadan jest jedna dla calej maszyny, wiec i zamek jest jeden.
 *
 * Trzyma sie go PRZEZ przelaczenie kontekstu, a zwalnia go ten, kto po
 * drugiej stronie przyjdzie. Inaczej zostawaloby okno, w ktorym zadanie
 * jest juz oznaczone jako gotowe, a jego stos wciaz uzywany - i drugi
 * rdzen wskoczylby na stos, po ktorym pierwszy jeszcze chodzi. */
static struct spinlock lock = SPINLOCK_FREE;

static int next_id;
static int scheduler_up;

/* Tablica kodow wyjscia zakonczonych procesow - wait je stad odczytuje.
   Kazdy wpis pamieta tez rodzica, bo zadanie znika z listy szybciej,
   niz rodzic zdazy po niego przyjsc (task_wait_any pyta wlasnie tu).
   Maly bufor cykliczny; przy przepelnieniu nadpisuje najstarszy wpis. */
#define EXIT_SLOTS 16
static struct { int pid; int parent; int code; int valid; } exit_codes[EXIT_SLOTS];
static int exit_next;

static void exit_record(int pid, int parent, int code)
{
    exit_codes[exit_next].pid = pid;
    exit_codes[exit_next].parent = parent;
    exit_codes[exit_next].code = code;
    exit_codes[exit_next].valid = 1;
    exit_next = (exit_next + 1) % EXIT_SLOTS;
}

/* Pobiera (i kasuje) zapamietany kod wyjscia danego pid; 0 gdy brak */
static int exit_take(int pid)
{
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (exit_codes[i].valid && exit_codes[i].pid == pid) {
            exit_codes[i].valid = 0;
            return exit_codes[i].code;
        }
    }
    return 0;
}

static void task_set_name(struct task *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* Czysta tablica sygnalow: wszystko domyslne, nic nie czeka */
static void task_init_signals(struct task *t)
{
    t->sig_pending = 0;
    t->sig_tramp = 0;
    t->sig_frame = 0;
    t->in_handler = 0;
    for (int i = 0; i < NSIG; i++)
        t->sig_handler[i] = SIG_DFL;
}

/* Standardowe strumienie: 0=klawiatura (read), 1/2=konsola (write) */
static void task_init_fds(struct task *t)
{
    memset(t->fds, 0, sizeof(t->fds));         /* kind=FD_CONSOLE (=0) */
    t->fds[0].used = 1; t->fds[0].readable = 1;   /* stdin  */
    t->fds[1].used = 1; t->fds[1].writable = 1;   /* stdout */
    t->fds[2].used = 1; t->fds[2].writable = 1;   /* stderr */
}

struct file_desc *task_fds(void)
{
    return current->fds;
}

int task_cwd(void)
{
    return current->cwd;
}

void task_set_cwd(int slot)
{
    current->cwd = slot;
}

/* Pierwszy kod każdego nowego wątku - context_switch "wraca" tutaj */
static void task_trampoline(void)
{
    /* Zamek listy zadan trzyma jeszcze ten, kto nas tu przelaczyl -
       i to my mamy go zwolnic. Nowe zadanie nie wraca z przelaczenia
       kontekstu jak wszystkie inne, wiec musi zrobic to samo tutaj. */
    spin_unlock(&lock);
    /* Nowe zadanie zaczyna w kodzie jadra, wiec bierze wielki zamek.
     *
     * Nie bierze go tylko to, ktore wolno prowadzic kazdemu rdzeniowi -
     * bo ten, kto o nie poprosil, wzial juz na siebie, ze jego watek
     * dotyka wylacznie rzeczy znoszacych dwa rdzenie naraz. Gdyby
     * i takie zadanie bralo wielki zamek, cztery watki liczace
     * rownolegle liczylyby sie po kolei - a po to ich nie ma. */
    if (!current->nolock)
        biglock_enter();
    __asm__ volatile("sti");
    current->entry(current->arg);
    task_exit(0);
}

void scheduler_init(void)
{
    struct task *main_task = kmalloc(sizeof(*main_task));
    main_task->id = next_id++;
    task_set_name(main_task, "main");
    main_task->state = TASK_RUNNING;
    main_task->stack = NULL;
    main_task->kstack_top = (uint32_t)stack_top;
    main_task->cr3 = paging_kernel_cr3();
    main_task->owns_dir = 0;
    main_task->brk = 0;
    main_task->brk_base = 0;
    main_task->mmap_next = 0;
    main_task->cwd = -1;        /* katalog glowny */
    main_task->parent = -1;     /* korzen drzewa procesow */
    main_task->vc = 0;
    main_task->killed = 0;
    main_task->priority = 2;    /* watek glowny = idle, najnizszy priorytet */
    task_init_fds(main_task);
    task_init_signals(main_task);
    main_task->anycore = 0;             /* watek glowny zostaje na swoim */
    main_task->nolock = 0;
    main_task->idle_for_core = 0;    /* to on jest bezczynnym rdzenia 0 */
    main_task->next = main_task;
    cores[0].running = main_task;
    cores[0].idle = main_task;
    scheduler_up = 1;

    /* Watek glowny konczy rozruch, wiec od tej chwili jest zwyklym
       zadaniem w kodzie jadra. Zamek odda dopiero wtedy, gdy stanie sie
       zadaniem bezczynnym (koniec kmain). */
    biglock_enter();
}

int task_scheduler_ready(void)
{
    return scheduler_up;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct task *t = kmalloc(sizeof(*t));
    t->stack = kmalloc(STACK_SIZE);
    t->kstack_top = (uint32_t)t->stack + STACK_SIZE;
    t->cr3 = paging_kernel_cr3();
    t->owns_dir = 0;
    t->brk = 0;
    t->brk_base = 0;
    t->mmap_next = 0;
    for (int i = 0; i < MMAP_FILES; i++)
        t->maps[i].start = t->maps[i].end = 0;
    t->cwd = current ? current->cwd : -1;  /* dziecko dziedziczy cwd */
    t->parent = current ? current->id : -1;
    t->vc = current ? current->vc : 0;   /* dziecko pisze tam, co rodzic */
    t->killed = 0;
    t->priority = 1;            /* domyslnie normalny priorytet */
    t->anycore = 0;            /* dopoki ktos nie powie inaczej */
    t->nolock = 0;
    t->idle_for_core = -1;
    task_init_fds(t);
    task_init_signals(t);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->id = next_id++;
    task_set_name(t, name);

    /* Spreparuj stos tak, by context_switch zdjął zerowe rejestry
       i "wrócił" do trampoliny */
    uint32_t *sp = (uint32_t *)((uint32_t)t->stack + STACK_SIZE);
    *--sp = (uint32_t)task_trampoline;  /* adres powrotu z context_switch */
    *--sp = 0;                          /* ebp */
    *--sp = 0;                          /* ebx */
    *--sp = 0;                          /* esi */
    *--sp = 0;                          /* edi */
    t->esp = (uint32_t)sp;

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = current->next;
    current->next = t;
    spin_unlock_irq(&lock, flags);
    return t;
}

/* Zadanie, ktore wolno prowadzic KAZDEMU rdzeniowi.
 *
 * Zwykle zadanie jadra zostaje na rdzeniu rozruchowym i tak ma byc:
 * siega po sterowniki, system plikow i cala reszte pisana dla jednego
 * rdzenia. Tutaj wolajacy bierze na siebie, ze jego watek dotyka tylko
 * tego, co zniesie dwa rdzenie naraz. */
struct task *task_create_anycore(const char *name, void (*entry)(void *),
                                  void *arg)
{
    struct task *t = task_create(name, entry, arg);
    t->anycore = 1;
    t->nolock = 1;
    return t;
}

/* Oznacza BIEZACE zadanie jako takie, ktore wolno prowadzic kazdemu
   rdzeniowi. Uzywa tego proces ring 3: jego strona jadrowa zostaje
   zwyklym kodem jadra pod wielkim zamkiem, ale samo liczenie w ring 3
   moze isc rownolegle z innymi. */
void task_set_anycore(void)
{
    if (current)
        current->anycore = 1;
}

/* Tworzy zadanie-watek dzielace przestrzen adresowa (cr3) biezacego
   procesu. owns_dir=1, ale przestrzen zniszczy dopiero ostatni watek
   (reap sprawdza cr3_in_use). Dziedziczy brk/mmap_next procesu. */
struct task *task_create_thread(const char *name, void (*entry)(void *),
                                void *arg, uint32_t cr3)
{
    struct task *t = task_create(name, entry, arg);
    uint32_t flags = irq_save();
    t->cr3 = cr3;
    t->owns_dir = 1;
    t->brk = current->brk;
    t->brk_base = current->brk_base;
    t->mmap_next = current->mmap_next;
    task_mmap_inherit(t);
    irq_restore(flags);
    return t;
}

/* --- sygnaly -------------------------------------------------------- */

/* Wysyla sygnal do procesu o danym pid. SIGKILL idzie stara droga
   (flaga 'killed'), zeby nie dalo sie go przechwycic ani zignorowac.
   Spiacego adresata budzimy - inaczej sygnal poczekalby do rana. */
int task_signal_send(int pid, int sig)
{
    if (sig <= 0 || sig >= NSIG)
        return -1;
    if (sig == SIGKILL)
        return task_kill(pid);

    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int found = 0;
    do {
        if (t->id == pid && t->state != TASK_ZOMBIE) {
            t->sig_pending |= 1u << sig;
            if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                t->state = TASK_READY;
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

/* Ustawia procedure obslugi sygnalu dla biezacego procesu.
   'tramp' to adres trampoliny powrotu w ring 3 (z usys.h). */
int task_signal_set(int sig, uint32_t handler, uint32_t tramp)
{
    if (sig <= 0 || sig >= NSIG || sig == SIGKILL)
        return -1;                  /* SIGKILL-a nie da sie przechwycic */
    uint32_t flags = irq_save();
    current->sig_handler[sig] = handler;
    if (tramp)
        current->sig_tramp = tramp;
    irq_restore(flags);
    return 0;
}

/* Zdejmuje najnizszy oczekujacy sygnal (0 = zaden nie czeka) */
int task_signal_take(void)
{
    uint32_t flags = irq_save();
    int sig = 0;
    for (int i = 1; i < NSIG; i++) {
        if (current->sig_pending & (1u << i)) {
            current->sig_pending &= ~(1u << i);
            sig = i;
            break;
        }
    }
    irq_restore(flags);
    return sig;
}

/* Czy czeka jakis sygnal, ktorego doreczenie cos zmieni? Pyta o to
   blokujacy odczyt - inaczej proces czekalby na klawisz w nieskonczonosc,
   a sygnal doreczylby sie dopiero po jego nadejsciu. */
int task_signal_pending(void)
{
    uint32_t flags = irq_save();
    uint32_t p = current->sig_pending;
    int any = 0;
    for (int i = 1; i < NSIG; i++)
        if ((p & (1u << i)) && current->sig_handler[i] != SIG_IGN) {
            any = 1;
            break;
        }
    irq_restore(flags);
    return any;
}

uint32_t task_signal_handler(int sig)
{
    return (sig > 0 && sig < NSIG) ? current->sig_handler[sig] : SIG_DFL;
}

uint32_t task_signal_tramp(void)
{
    return current->sig_tramp;
}

int task_signal_in_handler(void)
{
    return current->in_handler;
}

void task_signal_enter(uint32_t frame)
{
    current->sig_frame = frame;
    current->in_handler = 1;
}

uint32_t task_signal_leave(void)
{
    uint32_t frame = current->in_handler ? current->sig_frame : 0;
    current->in_handler = 0;
    current->sig_frame = 0;
    return frame;
}

/* Odzyskiwanie pamieci: obchodzi przestrzenie adresowe procesow
   i prosi kazda o oddanie jednej strony na dysk. Zaczyna od nastepnej
   po ostatnio odwiedzonej, zeby nie obdzierac w kolko tego samego
   procesu. Zwraca 1, gdy udalo sie zwolnic ramke.

   Watki jadra (cr3 jadra) pomijamy - pamiec jadra na dysk nie idzie.

   Cala robota idzie przy wylaczonych przerwaniach i nie jest to tylko
   ostroznosc wobec listy zadan: miedzy zgaszeniem wpisu strony
   a zapisaniem jej na dysk nikt nie moze dostac procesora. Inaczej
   watek dzielacy te przestrzen trafilby na strone "juz na dysku",
   ktorej tam jeszcze nie ma. */
int task_reclaim_page(void)
{
    static struct task *cursor;
    if (!current)                   /* scheduler jeszcze nie wystartowal */
        return 0;
    /* Wyrzucanie stron na dysk zostaje na rdzeniu rozruchowym. Siega
       po plik wymiany i po warstwe blokowa, a te pisane byly dla
       jednego rdzenia; rdzen wtorny, ktoremu zabraknie pamieci, ma
       powiedziec o tym wprost, a nie po cichu ruszyc dysk. */
    if (smp_core() != 0)
        return 0;
    uint32_t flags = irq_save();

    struct task *start = cursor && cursor->state != TASK_ZOMBIE
                             ? cursor : current;
    struct task *t = start;
    uint32_t kernel_cr3 = paging_kernel_cr3();
    int freed = 0;

    do {
        t = t->next;
        /* Partiami: samo przemiatanie tablic stron kosztuje wiecej niz
           zapis jednej strony, wiec nie oplaca sie chodzic po nich
           od nowa dla kazdej brakujacej ramki */
        if (t->cr3 != kernel_cr3 && t->state != TASK_ZOMBIE &&
            paging_reclaim_pages(t->cr3, 32)) {
            cursor = t;
            freed = 1;
            break;
        }
    } while (t != start);

    irq_restore(flags);
    return freed;
}

/* Zmiana nazwy biezacego zadania - exec podmienia program, wiec 'ps'
   ma pokazywac nowy, a nie ten, ktory sie nim stal */
void task_rename(const char *name)
{
    uint32_t flags = irq_save();
    task_set_name(current, name);
    /* Nowy obraz procesu nie ma juz kodu starych procedur obslugi -
       sygnaly wracaja do reakcji domyslnych (jak w uniksie po exec) */
    for (int i = 0; i < NSIG; i++)
        if (current->sig_handler[i] != SIG_IGN)
            current->sig_handler[i] = SIG_DFL;
    current->sig_tramp = 0;
    current->sig_frame = 0;
    current->in_handler = 0;
    irq_restore(flags);
}

/* Rozwidlenie procesu (fork): dziecko dostaje gotowa kopie przestrzeni
   adresowej (cr3, zob. paging_clone_address_space), a poza tym wszystko
   po rodzicu - sterte, obszar mmap, katalog roboczy, priorytet i tablice
   deskryptorow. Konce potokow dziedziczone przez dziecko dostaja
   dodatkowego wlasciciela, inaczej zamkniecie u dziecka dawaloby
   przedwczesny EOF rodzicowi. */
struct task *task_fork(void (*entry)(void *), void *arg, uint32_t cr3)
{
    struct task *t = task_create(current->name, entry, arg);
    uint32_t flags = irq_save();
    t->cr3 = cr3;
    t->owns_dir = 1;
    t->brk = current->brk;
    t->brk_base = current->brk_base;
    t->mmap_next = current->mmap_next;
    task_mmap_inherit(t);
    t->cwd = current->cwd;
    t->priority = current->priority;
    /* Dziecko dziedziczy prawo do biegania na kazdym rdzeniu: to
       wlasnosc programu, a nie pojedynczego zadania */
    t->anycore = current->anycore;
    /* Procedury obslugi sygnalow dziedziczy sie po rodzicu; oczekujace
       sygnaly - nie, to sprawy rodzica. Trampolina jest w tym samym
       miejscu kodu, bo dziecko ma kopie tego samego obrazu. */
    t->sig_tramp = current->sig_tramp;
    for (int i = 0; i < NSIG; i++)
        t->sig_handler[i] = current->sig_handler[i];
    for (int i = 0; i < TASK_MAX_FD; i++) {
        t->fds[i] = current->fds[i];
        if (t->fds[i].used && t->fds[i].kind == FD_PIPE) {
            if (t->fds[i].readable)
                pipe_add_reader(t->fds[i].pipe);
            if (t->fds[i].writable)
                pipe_add_writer(t->fds[i].pipe);
        }
    }
    irq_restore(flags);
    return t;
}

/* Czy przestrzeni adresowej uzywa jeszcze jakis inny watek? Pyta o to
   exec przed zniszczeniem starego obrazu procesu. */
int task_cr3_shared(uint32_t cr3)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int shared = 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->cr3 == cr3 && t->state != TASK_ZOMBIE) {
            shared = 1;
            break;
        }
        t = t->next;
    }
    spin_unlock_irq(&lock, flags);
    return shared;
}

/* Czy jakies inne zadanie wciaz uzywa tej przestrzeni adresowej (CR3)?
   Watki procesu wspoldziela CR3 - przestrzen niszczymy dopiero, gdy
   konczy sie ostatni z nich. */
static int cr3_in_use(uint32_t cr3, struct task *except)
{
    struct task *t = current;
    do {
        if (t != except && t->cr3 == cr3)
            return 1;
        t = t->next;
    } while (t != current);
    return 0;
}

static void reap_zombies(void)
{
    struct task *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task *z = t->next;
            t->next = z->next;
            /* Niszcz przestrzen adresowa tylko, gdy zaden inny watek
               (procesu) jej juz nie uzywa. CR3 zombie nie jest aktywny. */
            if (z->owns_dir && !cr3_in_use(z->cr3, z))
                paging_destroy_address_space(z->cr3);
            for (int i = 0; i < MMAP_FILES; i++)
                if (z->maps[i].blocks)
                    kfree(z->maps[i].blocks);
            kfree(z->stack);
            kfree(z);
        } else {
            t = t->next;
        }
    }
}

/* Budzi zaspane watki i wybiera nastepne zadanie wedlug priorytetu:
   wyzszy priorytet (mniejsza liczba) wywlaszcza nizszy; rownorzedne
   chodza round-robin; biezace zadanie zostaje, gdy jest scisle wyzsze
   od wszystkich gotowych. */
/* Czy rdzen 'nr' moze wziac to zadanie?
 *
 * Rdzen rozruchowy prowadzi wszystko, tak jak dotad. Rdzen wtorny -
 * tylko to, co ktos jawnie na to pozwolil, bo reszta jadra pisana jest
 * dla jednego rdzenia i cicho na to liczy.
 *
 * Zadanie bezczynne nalezy do swojego rdzenia i do nikogo innego:
 * stoi na stosie, ktory ten rdzen dostal przy rozruchu. */
static int may_run(const struct task *t, int nr)
{
    if (t->idle_for_core >= 0)
        return t->idle_for_core == nr;
    return nr == 0 || t->anycore;
}

static struct task *pick_next(int nr)
{
    uint32_t now = pit_ticks();

    /* Obudz spiochow, ktorych czas minal */
    struct task *t = current->next;
    do {
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_at) >= 0)
            t->state = TASK_READY;
        t = t->next;
    } while (t != current->next);

    /* Najwyzszy priorytet (najmniejsza liczba) wsrod gotowych */
    int best = 1 << 30;
    t = current;
    do {
        if (t->state == TASK_READY && may_run(t, nr) && t->priority < best)
            best = t->priority;
        t = t->next;
    } while (t != current);
    if (best == (1 << 30))
        return NULL;                  /* nikt gotowy - biezace bieży dalej */

    /* Biezace zadanie scisle wyzsze niz cokolwiek gotowego -> zostaje */
    if (current->state == TASK_RUNNING && current->priority < best)
        return NULL;

    /* Round-robin wsrod gotowych o priorytecie 'best', zaczynajac za current */
    t = current->next;
    do {
        if (t->state == TASK_READY && may_run(t, nr) &&
            t->priority == best)
            return t;
        t = t->next;
    } while (t != current->next);

    return NULL;
}

/* Wybor nastepnego zadania i przelaczenie sie na nie.
 *
 * Wolane ZAWSZE z wygaszonymi przerwaniami - albo z obslugi przerwania,
 * albo spod irq_save. Zamek listy zadan bierzemy bez ruszania IF: gdyby
 * timer wszedl w srodek sekcji trzymajacej ten zamek, czekalby na niego
 * w nieskonczonosc, bo zwolnic go moze tylko przerwany wlasnie kod.
 *
 * Zamek trzymamy PRZEZ przelaczenie i zwalnia go ten, kto po drugiej
 * stronie przyjdzie - bo dopiero wtedy stos poprzedniego zadania jest
 * naprawde wolny. Zadanie, ktore sie konczy, nie wraca tu nigdy i nie
 * zwalnia niczego; zrobi to za nie jego nastepca.
 */
static void switch_to(int new_state)
{
    int nr = smp_core();
    if (!cores[nr].running)   /* timer tyka, zanim scheduler wystartuje */
        return;

    /* Wielki zamek jadra oddajemy NA CZAS PRZELACZENIA. Trzymanie go
       przez zmiane kontekstu znaczyloby, ze zadanie usypiajace w srodku
       wywolania systemowego zamyka jadro dla wszystkich az do swojego
       przebudzenia. Zmienna 'glebokosc' lezy na stosie TEGO zadania,
       wiec kazde odzyska dokladnie swoja. */
    int depth = biglock_release_all();

    spin_lock(&lock);
    /* Stan biezacego zadania zmieniamy JUZ POD ZAMKIEM, razem z wyborem
       nastepnego. Ustawiony wczesniej dawalby okno, w ktorym zadanie
       jest oznaczone jako spiace, a jego stos wciaz uzywany - i drugi
       rdzen moglby je w tym czasie obudzic i na ten stos wejsc. */
    if (new_state >= 0)
        cores[nr].running->state = (enum task_state)new_state;
    reap_zombies();

    struct task *next = pick_next(nr);
    if (!next) {                /* nikt inny nie jest gotowy - biez dalej */
        spin_unlock(&lock);
        biglock_restore(depth);
        return;
    }

    struct task *prev = cores[nr].running;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    cores[nr].running = next;
    /* Przerwanie z ring 3 tego watku ma wyladowac na JEGO stosie jadra,
       i to w segmencie stanu zadania TEGO rdzenia - kazdy ma swoj */
    tss_set_esp0(next->kstack_top);
    /* Zmiana przestrzeni adresowej tylko, gdy trzeba (przeladowanie
       CR3 oproznia TLB - szkoda cykli miedzy watkami jadra) */
    if (next->cr3 != prev->cr3)
        __asm__ volatile("movl %0, %%cr3" : : "r"(next->cr3) : "memory");
    context_switch(&prev->esp, next->esp);
    spin_unlock(&lock);
    /* Od tej chwili biegnie juz zadanie, ktore tu wrocilo - i to ono
       odbiera swoja glebokosc ze swojego stosu */
    biglock_restore(depth);
}

void schedule(void)
{
    switch_to(-1);
}

/* --- Rdzen wtorny dolacza do schedulera ------------------------------
 *
 * Kazdy rdzen potrzebuje zadania, na ktore ma wrocic, gdy nie ma nic do
 * roboty - inaczej nie byloby dokad odlozyc jego kontekstu. To zadanie
 * jest jego wlasnoscia: stoi na stosie, ktory rdzen dostal przy
 * obudzeniu, i nikt inny nie ma prawa go prowadzic.
 *
 * Ta funkcja nie wraca. Rdzen ma odtad dwie rzeczy do roboty: policzyc,
 * co mu zlecono, i poprowadzic zadanie, jesli jakies jest dla niego.
 */
void task_core_enter(int nr)
{
    if (nr <= 0 || nr >= SMP_CORES || !scheduler_up)
        return;

    struct task *t = kmalloc(sizeof(*t));
    if (!t)
        return;
    t->id = -nr;                 /* ujemny numer: to nie jest proces */
    task_set_name(t, "bezczynny");
    t->name[9] = (char)('0' + nr);
    t->name[10] = '\0';
    t->state = TASK_RUNNING;
    t->stack = NULL;             /* stos ma z trampoliny rozruchowej */
    t->kstack_top = 0;
    t->cr3 = paging_kernel_cr3();
    t->owns_dir = 0;
    t->brk = t->brk_base = t->mmap_next = 0;
    for (int i = 0; i < MMAP_FILES; i++)
        t->maps[i].start = t->maps[i].end = 0;
    t->cwd = -1;
    t->parent = -1;
    t->vc = 0;
    t->killed = 0;
    t->priority = 2;
    t->anycore = 0;
    t->nolock = 1;         /* zadanie bezczynne nie bierze zamka */
    t->idle_for_core = nr;
    task_init_fds(t);
    task_init_signals(t);

    irqflags_t flags = spin_lock_irq(&lock);
    t->next = cores[0].running->next;
    cores[0].running->next = t;
    cores[nr].running = t;
    cores[nr].idle = t;
    spin_unlock_irq(&lock, flags);

    __asm__ volatile("sti");
    for (;;) {
        smp_run_work(nr);
        task_yield();
        /* Nic dla mnie nie ma - stoj do najblizszego przerwania.
           Zegar tego rdzenia tyka sto razy na sekunde, a pobudka od
           rdzenia rozruchowego przychodzi od razu. */
        __asm__ volatile("hlt");
    }
}

void task_yield(void)
{
    uint32_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task_sleep(uint32_t ms)
{
    uint32_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;  /* tyknięcie = 10 ms */
    switch_to(TASK_SLEEPING);
    irq_restore(flags);
}

/* Blokowanie bez terminu - ktoś musi obudzić przez task_unblock */
void task_block(void)
{
    uint32_t flags = irq_save();
    switch_to(TASK_BLOCKED);
    irq_restore(flags);
}

/* Wołane z IF=0 (kontekst IRQ albo sekcja krytyczna) */
void task_unblock(struct task *t)
{
    if (t->state == TASK_BLOCKED)
        t->state = TASK_READY;
}

struct task *task_current(void)
{
    return current;
}

int task_id_of(struct task *t)
{
    return t->id;
}

int task_current_id(void)
{
    return current->id;
}

int task_alive(int id)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int alive = 0;
    struct task *t = current;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            alive = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return alive;
}

void task_exit(int code)
{
    /* Odeslij na dysk to, co proces napisal w zapisywalnym
       odwzorowaniu pliku, a czego jeszcze nie zdazyl odeslac sam.
       Musi to sie stac TUTAJ, dopoki jego przestrzen adresowa jest
       zywa: sprzatacz zombie oglada te strony juz z cudzego CR3. */
    process_msync();

    /* Zamknij deskryptory - konce potokow musza zwolnic druga strone
       (czytelnik dostanie EOF, pisarz - przerwany potok) */
    file_close_all();

    irqflags_t flags = spin_lock_irq(&lock);
    current->exit_code = code;
    /* Zapamietaj kod od razu - wait widzi 'nieaktywny' juz przy ZOMBIE,
       a reaper moze sprzatnac zadanie zanim wait zdazy je odczytac */
    exit_record(current->id, current->parent, code);

    /* Dzieci, ktore nas przezyly, trafiaja pod opieke wlasnego dziadka
       (a w ostateczonosci watku glownego) - inaczej nikt nigdy nie
       odebralby ich kodu wyjscia. */
    /* Rodzic dostaje SIGCHLD - domyslnie zignoruje, ale moze chciec
       wiedziec od razu, ze ma co zbierac przez wait */
    if (current->parent >= 0) {
        struct task *p = current->next;
        while (p != current) {
            if (p->id == current->parent && p->state != TASK_ZOMBIE) {
                p->sig_pending |= 1u << SIGCHLD;
                break;
            }
            p = p->next;
        }
    }

    int grandparent = current->parent >= 0 ? current->parent : 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->parent == current->id)
            t->parent = grandparent;
        t = t->next;
    }
    for (int i = 0; i < EXIT_SLOTS; i++)
        if (exit_codes[i].valid && exit_codes[i].parent == current->id)
            exit_codes[i].parent = grandparent;
    /* Zamek zwalniamy PRZED przelaczeniem: przelacz bierze go sam,
       a wziecie go dwa razy na tym samym rdzeniu nie skonczyloby sie
       nigdy. Przerwania zostaja wygaszone - to zadanie i tak nie ma
       juz dokad wrocic. */
    spin_unlock(&lock);
    switch_to(TASK_ZOMBIE);
    (void)flags;
    /* zombie nigdy nie zostanie wybrany ponownie */
    for (;;)
        __asm__ volatile("hlt");
}

/* Czeka na zakonczenie procesu o danym pid i zwraca jego kod wyjscia */
int task_wait(int pid)
{
    while (task_alive(pid))
        task_sleep(20);
    return exit_take(pid);
}

/* Czy biezace zadanie ma jeszcze zywe dzieci? */
static int has_children(void)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int any = 0;
    struct task *t = current->next;
    while (t != current) {
        if (t->parent == current->id && t->state != TASK_ZOMBIE) {
            any = 1;
            break;
        }
        t = t->next;
    }
    spin_unlock_irq(&lock, flags);
    return any;
}

/* Zdejmuje kod wyjscia dowolnego zakonczonego dziecka; -1 gdy zadne
   jeszcze nie skonczylo */
static int take_child_exit(int *code)
{
    irqflags_t flags = spin_lock_irq(&lock);
    int pid = -1;
    for (int i = 0; i < EXIT_SLOTS; i++) {
        if (exit_codes[i].valid && exit_codes[i].parent == current->id) {
            exit_codes[i].valid = 0;
            pid = exit_codes[i].pid;
            *code = exit_codes[i].code;
            break;
        }
    }
    spin_unlock_irq(&lock, flags);
    return pid;
}

/* Czeka na dowolne wlasne dziecko i zwraca jego pid (kod wyjscia do
   *code). -1 oznacza, ze nie ma juz na co czekac - zadnych dzieci. */
int task_wait_any(int *code)
{
    for (;;) {
        int pid = take_child_exit(code);
        if (pid >= 0)
            return pid;
        if (!has_children())
            return -1;
        task_sleep(20);
    }
}

/* Zaznacza proces o danym pid do zabicia. Faktyczne zakonczenie nastapi,
   gdy ten proces wroci do trybu uzytkownika (task_kill_pending).
   Budzi go, gdy spi/czeka, by szybciej trafil w punkt powrotu. */
int task_kill(int pid)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int found = 0;
    do {
        if (t->id == pid && t->state != TASK_ZOMBIE) {
            t->killed = 1;
            if (t->state == TASK_SLEEPING || t->state == TASK_BLOCKED)
                t->state = TASK_READY;
            found = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return found ? 0 : -1;
}

/* Zabija pierwsze zyjace zadanie o podanej nazwie i zwraca jego pid
   (albo -1). Przydatne, gdy pid nie jest znany - np. aplikacja zostala
   odpalona z menu pulpitu, a nie z shella. */
/* Czy biezace zadanie zostalo zaznaczone do zabicia? Pozwala dlugim
   oczekiwaniom w jadrze (np. odbior z gniazda) przerwac czekanie
   zamiast trzymac proces przy zyciu az do wlasnego timeoutu. */
int task_killed(void)
{
    return current->killed;
}

int task_kill_name(const char *name)
{
    int pid = task_find_name(name);
    return pid >= 0 && task_kill(pid) == 0 ? pid : -1;
}

/* Pid zywego zadania o danej nazwie (-1 = nie ma takiego). Rozdzielone
   od zabijania, bo shell umie wyslac pod nazwe dowolny sygnal. */
int task_find_name(const char *name)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct task *t = current;
    int pid = -1;
    do {
        if (t->state != TASK_ZOMBIE && !strcmp(t->name, name)) {
            pid = t->id;
            break;
        }
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
    return pid;
}

/* Wolane przy powrocie do ring 3: jesli proces ma oczekujace zabicie,
   konczy go tutaj (w jego wlasnym kontekscie). */
void task_kill_pending(void)
{
    if (current->killed) {
        printk("[jadro] proces '%s' (pid %d) zabity (kod -9)\n",
               current->name, current->id);
        task_exit(-9);
    }
}

void task_use_address_space(uint32_t dir_phys)
{
    uint32_t flags = irq_save();
    current->cr3 = dir_phys;
    current->owns_dir = 1;
    __asm__ volatile("movl %0, %%cr3" : : "r"(dir_phys) : "memory");
    irq_restore(flags);
}

uint32_t task_current_cr3(void)
{
    return current->cr3;
}

/* Konsola biezacego zadania. Przed startem schedulera nie ma jeszcze
   zadnego - wypisy rozruchowe ida na pierwsza. */
int task_vc(void)
{
    return current ? current->vc : 0;
}

void task_set_vc(int vc)
{
    current->vc = vc;
}

uint32_t task_brk(void)
{
    return current->brk;
}

void task_set_brk(uint32_t brk)
{
    current->brk = brk;
}

uint32_t task_brk_base(void)
{
    return current->brk_base;
}

/* Poczatek sterty ustawia exec razem z jej koncem - obszar miedzy nimi
   nalezy do procesu, ale ramki dostaje dopiero przy pierwszym dotknieciu */
void task_set_brk_base(uint32_t base)
{
    current->brk_base = base;
}

/* --- Pliki odwzorowane w pamieci ---------------------------------- */

int task_mmap_add(uint32_t start, uint32_t end, uint32_t size,
                    uint32_t *blocks, uint32_t nblocks_of, uint32_t write)
{
    if (!current)
        return -1;
    for (int i = 0; i < MMAP_FILES; i++) {
        if (current->maps[i].end)
            continue;
        current->maps[i].start = start;
        current->maps[i].end = end;
        current->maps[i].size = size;
        current->maps[i].blocks = blocks;
        current->maps[i].nblocks_of = nblocks_of;
        current->maps[i].write = write;
        return 0;
    }
    return -1;
}

/* Wolane z obslugi bledu strony, wiec krotkie i bez niespodzianek */
int task_mmap_find(uint32_t addr, uint32_t *first_block,
                     uint32_t *nbytes, uint32_t *write)
{
    if (!current)
        return 0;
    uint32_t page = addr & ~0xFFFu;

    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;

        uint32_t offset = page - m->start;
        uint32_t index = offset / FS_BLOCK;
        if (index >= m->nblocks_of)
            return 0;
        *first_block = index;
        /* Ile pliku zostalo od poczatku tej strony - ogon ostatniej ma
           zostac wyzerowany, a nie wypelniony smieciem z dysku */
        *nbytes = offset < m->size ? m->size - offset : 0;
        *write = m->write;
        return 1;
    }
    return 0;
}

uint32_t task_mmap_block(uint32_t addr, uint32_t index)
{
    uint32_t page = addr & ~0xFFFu;
    for (int i = 0; i < MMAP_FILES; i++) {
        struct file_map *m = &current->maps[i];
        if (!m->end || page < m->start || page >= m->end)
            continue;
        return index < m->nblocks_of ? m->blocks[index] : 0;
    }
    return 0;
}

void task_mmap_release(void)
{
    if (!current)
        return;
    for (int i = 0; i < MMAP_FILES; i++) {
        if (current->maps[i].blocks)
            kfree(current->maps[i].blocks);
        current->maps[i].blocks = (uint32_t *)0;
        current->maps[i].start = current->maps[i].end = 0;
    }
}

/* Dziecko dostaje wlasna kopie opisow: tablice numerow blokow zwalnia
   kazdy u siebie, wiec nie moga byc wspolne. */
void task_mmap_inherit(struct task *child)
{
    for (int i = 0; i < MMAP_FILES; i++) {
        child->maps[i] = current->maps[i];
        child->maps[i].blocks = (uint32_t *)0;
        if (!current->maps[i].end)
            continue;
        uint32_t n = current->maps[i].nblocks_of;
        uint32_t *copy = kmalloc(n * sizeof(uint32_t));
        if (!copy) {
            child->maps[i].start = child->maps[i].end = 0;
            continue;
        }
        for (uint32_t k = 0; k < n; k++)
            copy[k] = current->maps[i].blocks[k];
        child->maps[i].blocks = copy;
    }
}

/* Opis i-tego odwzorowania biezacego procesu - do odsylania stron na
   dysk, ktore musi obejsc kazde po kolei. Zwraca 0, gdy wpis wolny. */
int task_mmap_info(int i, uint32_t *start, uint32_t *end,
                   uint32_t *size, uint32_t *write)
{
    if (!current || i < 0 || i >= MMAP_FILES || !current->maps[i].end)
        return 0;
    *start = current->maps[i].start;
    *end = current->maps[i].end;
    *size = current->maps[i].size;
    *write = current->maps[i].write;
    return 1;
}

uint32_t task_mmap_next(void)
{
    return current->mmap_next;
}

void task_set_mmap_next(uint32_t addr)
{
    current->mmap_next = addr;
}

void task_set_priority(int prio)
{
    if (prio < 0)
        prio = 0;
    if (prio > 2)
        prio = 2;
    current->priority = prio;
}

const char *task_current_name(void)
{
    return current ? current->name : "(brak)";
}

void task_dump(void)
{
    static const char *state_names[] = { "gotowy", "dziala", "spi",
                                         "czeka", "zombie" };

    irqflags_t flags = spin_lock_irq(&lock);
    printk("  ID  RODZIC  STAN    NAZWA\n");
    struct task *t = current;
    do {
        printk("  %d   ", t->id);
        if (t->parent >= 0)
            printk("%d", t->parent);
        else
            printk("-");
        printk("       %s", state_names[t->state]);
        for (int pad = 8 - (int)strlen(state_names[t->state]); pad > 0; pad--)
            printk(" ");
        /* Gwiazdka: zadanie, ktore wolno prowadzic kazdemu rdzeniowi */
        printk("%s%s%s\n", t->name, t->anycore ? " *" : "",
               t == current ? " (biezacy)" : "");
        t = t->next;
    } while (t != current);
    spin_unlock_irq(&lock, flags);
}
