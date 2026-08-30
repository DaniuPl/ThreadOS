/* Budzenie pozostalych procesorow
 *
 * Po wlaczeniu zasilania pracuje jeden rdzen - ten, ktory firmware
 * wybral na rozruchowy. Reszta czeka w miejscu na sygnal, i to jest
 * decyzja sprzetu, nie systemu: procesor obudzony sygnalem SIPI
 * zaczyna w trybie rzeczywistym pod adresem (wektor << 12), czyli
 * dokladnie tak, jak zaczynal pecet czterdziesci lat temu.
 *
 * Ile ich jest, mowi tablica MADT w ACPI. Budzi sie je przez lokalny
 * kontroler przerwan (APIC) para sygnalow: INIT stawia procesor
 * w stanie poczatkowym, SIPI mowi mu, od ktorego adresu ma ruszyc.
 * Zalecenie producenta jest takie, by SIPI wyslac dwa razy - drugi
 * jest nieszkodliwy, gdy pierwszy wystarczyl.
 *
 * Obudzony procesor nie zostaje juz jednak sam z niczym. Bierze
 * wspolna tablice segmentow i wspolna tablice wyjatkow jadra - do tej
 * chwili kazdy jego blad konczyl sie potrojnym bledem i cichym resetem
 * maszyny - a potem wlacza sobie WLASNY zegar w lokalnym APIC.
 * Przerwanie z PIT idzie przez kontroler PIC do jednego procesora,
 * wiec bez tego rdzen wtorny nie dostalby nigdy zadnego.
 *
 * Zadan wciaz nie prowadzi: scheduler i lista procesow pisane byly dla
 * jednego rdzenia i cicho na to licza. Ma za to wszystko, czego taki
 * scheduler bedzie potrzebowal - tablice wyjatkow, zegar i wlasny
 * numer.
 */

#include <stdint.h>
#include "include/smp.h"
#include "include/acpi.h"
#include "include/heap.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/task.h"
#include "include/pit.h"


/* Rejestry APIC leza wysoko, poza pamiecia opisana przy starcie -
   doklejenie ich do mapowania robi w kazdym trybie inny kod. Rdzen
   wtorny w trybie dlugim potrzebuje przy okazji adresu tablic stron,
   bo buduje sobie droge do trybu dlugiego sam. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#include "../kernel64/gdt64.h"
#include "../kernel64/task64.h"
#include "../kernel64/syscall64.h"
#include "../kernel64/idt64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#define IDENTITY_TOP()   paging64_identity_top()
#define LOAD_GDT()    gdt64_load()
#define LOAD_IDT()    idt64_load()
#define LOAD_TSS()    gdt64_load_tss()
#define TSS_SEL_BASE     GDT64_TSS
#define TSS_SEL_STEP     16
#define SYSCALL_ENTRY() syscall64_init()
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#define IDENTITY_TOP()   paging_identity_limit()
#include "include/gdt.h"
#include "include/idt.h"
#define LOAD_GDT()    gdt_load()
#define LOAD_IDT()    idt_load()
#define LOAD_TSS()    gdt_load_tss()
#define TSS_SEL_BASE     GDT_TSS
#define TSS_SEL_STEP     8
#define SYSCALL_ENTRY() do { } while (0)   /* i386 wchodzi bramka int 0x80 */
#endif

#define AP_BASE       0x8000        /* dokad kopiujemy trampoline */
#define AP_STACK      8192          /* stos na kazdy obudzony rdzen */
#define MAX_CPU       SMP_CORES

/* Rejestry lokalnego APIC (przesuniecia od jego bazy) */
#define APIC_ID       0x020
#define APIC_SVR      0x0F0         /* rejestr przerwania widmowego */
#define APIC_EOI      0x0B0         /* potwierdzenie obsluzenia */
#define APIC_ICR_LO   0x300
#define APIC_ICR_HI   0x310
#define APIC_LVT_TIM  0x320         /* wpis zegara w tablicy przerwan APIC */
#define APIC_TIM_INIT 0x380         /* wartosc poczatkowa licznika */
#define APIC_TIM_CUR  0x390         /* biezaca wartosc licznika */
#define APIC_TIM_DIV  0x3E0         /* dzielnik zegara */

#define LVT_PERIODIC  (1u << 17)    /* licznik startuje od nowa sam */
#define LVT_MASKED (1u << 16)
#define SMP_VEC_TIMER 48
#define SMP_VEC_WAKE 49
#define ICR_ALL_BUT_SELF 0x000C0000u

#define ICR_INIT      0x00000500
#define ICR_STARTUP   0x00000600
#define ICR_ASSERT    0x00004000
#define ICR_PENDING   0x00001000    /* sygnal jeszcze w drodze */

/* Naglowek wpisu w MADT: kazdy zaczyna sie tak samo */
struct madt_wpis {
    uint8_t typ;
    uint8_t len;
} __attribute__((packed));

struct madt_lapic {
    struct madt_wpis header;
    uint8_t acpi_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

extern uint8_t ap_trampoline[], ap_trampoline_end[];
extern volatile uint32_t ap_counter;
extern volatile uint32_t ap_stack;
extern volatile uint32_t ap_number;
extern volatile uint32_t ap_cr3;

static volatile uint8_t *apic;
static int cpus, started;
static uint8_t apic_id[MAX_CPU];
static uint32_t timer_step;              /* ile tykniec APIC na 10 ms */
static volatile uint32_t ticks_of_core[MAX_CPU];

/* --- Wspolna kolejka pracy ------------------------------------------
 *
 * Caly stan dzielony miedzy rdzeniami to te kilka pol. Numer nastepnej
 * jednostki bierze sie pod zamkiem, a liczy juz bez niego - inaczej
 * rdzenie czekalyby na siebie zamiast pracowac.
 */
static struct spinlock lock = SPINLOCK_FREE;
static void (*work_fn)(uint32_t, void *);
static void *work_arg;
static volatile uint32_t work_count;      /* ile jednostek w zleceniu */
static volatile uint32_t work_next; /* pierwsza jeszcze nie wzieta */
static volatile uint32_t work_done;
static volatile uint32_t share[MAX_CPU];

static void apic_wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(apic + reg) = v;
}

static uint32_t apic_rd(uint32_t reg)
{
    return *(volatile uint32_t *)(apic + reg);
}

/* --- Ktory rdzen wlasnie biegnie -------------------------------------
 *
 * Odpowiedz daje sam procesor: numer w jego lokalnym APIC jest ustalony
 * przez sprzet i kazdy czyta wlasny. Tlumaczymy go na numer porzadkowy
 * w naszej tablicy, bo numery od sprzetu nie musza byc kolejne.
 *
 * Przed uruchomieniem APIC (i na maszynie, ktora go nie ma) rdzen jest
 * jeden i ma numer zero.
 */
int smp_core_apic(void)
{
    if (!apic)
        return 0;
    uint8_t id = (uint8_t)(apic_rd(APIC_ID) >> 24);
    for (int i = 0; i < cpus; i++)
        if (apic_id[i] == id)
            return i;
    return 0;
}

int smp_core(void)
{
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr < TSS_SEL_BASE)
        return 0;                    /* segment jeszcze niezaladowany */
    int nr = (tr - TSS_SEL_BASE) / TSS_SEL_STEP;
    return (nr >= 0 && nr < SMP_CORES) ? nr : 0;
}

/* --- Zegar rdzenia ---------------------------------------------------
 *
 * Rdzen rozruchowy ma PIT i to on odmierza czas calego systemu. Rdzenie
 * wtorne nie moga z niego korzystac: przerwanie z PIT trafia przez
 * kontroler PIC do jednego procesora, a nie do wszystkich. Kazdy z nich
 * ma za to WLASNY zegar w lokalnym APIC - i to on daje mu przerwania.
 *
 * Czestotliwosci tego zegara nie da sie odczytac; trzeba ja zmierzyc.
 * Robi to rdzen rozruchowy, bo tylko on ma czym mierzyc: puszcza licznik
 * APIC i patrzy, ile zdazy odliczyc przez znana liczbe tykniec PIT.
 */
static void timer_measure(void)
{
    /* Czekanie na PIT musi byc ograniczone. Ten pomiar biegnie bardzo
       wczesnie i na maszynie, na ktorej PIT nie tyka (albo jego
       przerwanie jeszcze nigdzie nie dochodzi), petla bez granicy
       zawiesilaby caly rozruch - i to w miejscu, ktore niczego o sobie
       nie mowi. */
#define DELAY_MAX 20000000u

    apic_wr(APIC_TIM_DIV, 0x3);                  /* dzielnik 16 */
    apic_wr(APIC_LVT_TIM, LVT_MASKED);      /* liczy, ale nie zglasza */

    uint32_t start = pit_ticks();
    uint32_t rounds = 0;
    while (pit_ticks() == start && rounds < DELAY_MAX)  /* granica tykniecia */
        rounds++;
    apic_wr(APIC_TIM_INIT, 0xFFFFFFFFu);

    start = pit_ticks();
    rounds = 0;
    while (pit_ticks() - start < 10 && rounds < DELAY_MAX)   /* 100 ms */
        rounds++;
    uint32_t remaining = apic_rd(APIC_TIM_CUR);
    apic_wr(APIC_TIM_INIT, 0);                   /* stop */

    uint32_t na_100ms = 0xFFFFFFFFu - remaining;
    timer_step = na_100ms / 10;                  /* na dziesiec milisekund */
    /* Gdy pomiar sie nie udal, lepiej dac zegarowi wartosc z sufitu niz
       zadna: rdzen bez przerwan nie da sie nigdy wywlaszczyc. */
    if (rounds >= DELAY_MAX || timer_step < 1000 || timer_step > 0x10000000u)
        timer_step = 1000000;
#undef DELAY_MAX
}

/* Wlacza lokalny APIC i jego zegar na biezacym rdzeniu */
static void timer_start(void)
{
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);
    apic_wr(APIC_TIM_DIV, 0x3);
    apic_wr(APIC_LVT_TIM, SMP_VEC_TIMER | LVT_PERIODIC);
    apic_wr(APIC_TIM_INIT, timer_step);
}

/* Obsluga tykniecia zegara APIC - wolana z isr_handler dla wektora 48.
 *
 * Kazdy rdzen liczy swoje w osobnym slowie, wiec nie potrzeba tu zamka.
 * Potwierdzenie idzie do WLASNEGO APIC tego rdzenia, a nie do PIC:
 * przerwanie nie przeszlo przez kontroler, wiec nie ma go tam komu
 * zamknac. */
void smp_timer_tick(void)
{
    int r = smp_core();
    ticks_of_core[r]++;
    if (apic)
        apic_wr(APIC_EOI, 0);
    /* Potwierdzenie idzie PRZED przelaczeniem zadan: schedule moze nie
       wrocic tu przez dluzszy czas, a lokalny APIC nie ma na co czekac.
       Tak samo robi obsluga timera na rdzeniu rozruchowym. */
    if (task_scheduler_ready())
        schedule();
}

/* Pobudka dla wszystkich pozostalych rdzeni.
 *
 * Rdzen bezczynny stoi na instrukcji hlt - nie kreci sie w kolko, wiec
 * nie zauwazy sam, ze pojawila sie praca. Wyrywa go stad przerwanie,
 * a najprostsze przerwanie, jakie mozna komus poslac, to takie, ktore
 * nic nie znaczy: cala jego tresc to fakt, ze przyszlo.
 *
 * Skrot "wszystkim poza mna" oszczedza wyliczania, kto jeszcze zyje -
 * rdzen, ktory sie nie obudzil, i tak nic z tym nie zrobi. */
static void wake_others(void)
{
    if (!apic)
        return;
    apic_wr(APIC_ICR_HI, 0);
    apic_wr(APIC_ICR_LO, ICR_ALL_BUT_SELF | SMP_VEC_WAKE);
}

void smp_wakeup_recv(void)
{
    if (apic)
        apic_wr(APIC_EOI, 0);
}

uint32_t smp_ticks(int core)
{
    return (core >= 0 && core < MAX_CPU) ? ticks_of_core[core] : 0;
}

uint32_t smp_share(int core)
{
    return (core >= 0 && core < MAX_CPU) ? share[core] : 0;
}

int smp_workers(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CPU; i++)
        if (share[i])
            n++;
    return n;
}

/* Bierze jedna jednostke do policzenia. Zwraca 0, gdy nie ma juz nic.
 *
 * Przerwania gasimy na czas trzymania zamka takze na rdzeniu
 * rozruchowym: gdyby timer wywlaszczyl watek trzymajacy zamek, kazdy
 * inny checny musialby czekac do jego nastepnej kolejki. */
static int take_unit(uint32_t *number)
{
    uint32_t flags = irq_save();
    spin_lock(&lock);
    int jest = work_next < work_count;
    if (jest)
        *number = work_next++;
    spin_unlock(&lock);
    irq_restore(flags);
    return jest;
}

/* Petla robocza - ta sama dla rdzenia rozruchowego i dla wtornych */
static void run_available(int core)
{
    uint32_t number;
    while (take_unit(&number)) {
        work_fn(number, work_arg);
        share[core]++;
        __atomic_add_fetch(&work_done, 1, __ATOMIC_SEQ_CST);
    }
}

/* Wejscie rdzenia wtornego z trampoliny. Nie wraca.
 *
 * Ten rdzen nie ma tablicy wyjatkow ani przerwan - kazdy blad konczy
 * sie potrojnym bledem i resetem maszyny. Dlatego robi tu jedna
 * rzecz: czeka na zlecenie i liczy to, co dostal. */
void smp_ap_main(uint32_t number)
{
    if (number >= MAX_CPU)
        number = MAX_CPU - 1;

    /* Trampolina zostawila ten rdzen na wlasnej, tymczasowej tablicy
       segmentow i bez zadnej tablicy wyjatkow - kazdy blad konczyl sie
       potrojnym bledem i cichym resetem calej maszyny. Obie tablice sa
       jedne dla systemu i po zbudowaniu nikt ich nie zmienia, wiec
       wystarczy je tutaj wskazac. */
    LOAD_GDT();
    LOAD_IDT();
    LOAD_TSS();
    /* Rejestry wejscia przez instrukcje syscall sa wlasnoscia rdzenia -
       ten, ktory ich nie ustawi, nie przyjmie jej wcale */
    SYSCALL_ENTRY();

    /* Wlasny zegar. Przerwanie z PIT idzie przez kontroler PIC do
       JEDNEGO procesora - ten go nie zobaczy nigdy. */
    timer_start();
    __asm__ volatile("sti");

    /* Scheduler wstaje pozniej niz rdzenie - do tej chwili ten rdzen
       umie tylko policzyc, co mu zlecono */
    while (!task_scheduler_ready()) {
        if (work_count)
            run_available((int)number);
        else
            __asm__ volatile("hlt");
    }

    /* Odtad rdzen jest pelnoprawnym procesorem jadra: ma wlasne
       zadanie bezczynne i bierze z listy to, co wolno mu prowadzic.
       Ta funkcja nie wraca. */
    task_core_enter((int)number);

    for (;;) {
        if (work_count) {
            run_available((int)number);
            continue;
        }
        /* Bezczynny rdzen ma STAC, a nie krecic sie w kolko.
         *
         * Wirowanie kosztuje tyle samo, co praca - na maszynie
         * wirtualnej dokladnie tyle samo, bo kazdy obieg trzeba
         * naprawde wykonac. Trzy rdzenie kreciace sie bez celu zabieraly
         * rdzeniowi rozruchowemu trzy czwarte maszyny i zwykly zapis na
         * dysk trwal cztery razy dluzej.
         *
         * Instrukcja hlt zatrzymuje rdzen do najblizszego przerwania,
         * a te przychodza teraz sto razy na sekunde z jego wlasnego
         * zegara. Zlecenie zlozone tuz po sprawdzeniu warunku poczeka
         * wiec najwyzej jedno tykniecie - a nie ma sie do czego
         * spieszyc, skoro chodzi o prace liczona w setkach milisekund. */
        __asm__ volatile("hlt");
    }
}

void smp_run_work(int core)
{
    if (work_count)
        run_available(core);
}

void smp_work(void (*fn)(uint32_t number, void *data), void *data,
               uint32_t ile)
{
    if (!fn || !ile)
        return;

    for (int i = 0; i < MAX_CPU; i++)
        share[i] = 0;

    work_fn = fn;
    work_arg = data;
    work_next = 0;
    work_done = 0;
    __atomic_store_n(&work_count, ile, __ATOMIC_SEQ_CST);

    /* Zlecenie jest juz widoczne - teraz trzeba obudzic tych, ktorzy
       na nie czekaja spiac. Kolejnosc nie jest dowolna: pobudka wyslana
       przed zapisem zastalaby jeszcze pusta kolejke. */
    wake_others();

    /* Rdzen rozruchowy liczy razem z pozostalymi: na maszynie
       jednordzeniowej nie policzylby inaczej nikt */
    run_available(0);

    /* Czekamy na tych, ktorzy wzieli jednostke tuz przed nami */
    while (__atomic_load_n(&work_done, __ATOMIC_SEQ_CST) < ile)
        __asm__ volatile("pause" ::: "memory");

    __atomic_store_n(&work_count, 0, __ATOMIC_SEQ_CST);
}

int smp_cpus(void)    { return cpus ? cpus : 1; }
int smp_started(void) { return started; }

/* Zwloka liczona portem 0x80 - PIT moze byc jeszcze zajety czym
   innym, a tu chodzi o rzad wielkosci, nie o dokladnosc */
static void delay(uint32_t loops)
{
    for (uint32_t i = 0; i < loops; i++)
        io_wait();
}

static void wait_sent(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(apic_rd(APIC_ICR_LO) & ICR_PENDING))
            return;
        io_wait();
    }
}

/* INIT, a po nim dwa SIPI - tak, jak kaze dokumentacja procesora */
static void wake_cpu(uint8_t id, uint8_t vector)
{
    apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
    apic_wr(APIC_ICR_LO, ICR_INIT | ICR_ASSERT);
    wait_sent();
    delay(20000);                        /* rzedu dziesieciu milisekund */

    for (int i = 0; i < 2; i++) {
        apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
        apic_wr(APIC_ICR_LO, ICR_STARTUP | vector);
        wait_sent();
        delay(600);                      /* rzedu dwustu mikrosekund */
    }
}

int smp_init(void)
{
    const uint8_t *madt = acpi_table("APIC");
    if (!madt)
        return 1;

    uint32_t len = (uint32_t)madt[4] | ((uint32_t)madt[5] << 8) |
                       ((uint32_t)madt[6] << 16) | ((uint32_t)madt[7] << 24);
    uint32_t apic_base = (uint32_t)madt[36] | ((uint32_t)madt[37] << 8) |
                         ((uint32_t)madt[38] << 16) |
                         ((uint32_t)madt[39] << 24);
    if (len < 44 || !apic_base)
        return 1;

    /* Rejestry APIC leza wysoko, poza pamiecia opisana przy starcie */
    if ((uint64_t)apic_base + 0x1000 > IDENTITY_TOP() &&
        MAP_PHYS(apic_base, 0x1000) < 0)
        return 1;
    apic = (volatile uint8_t *)(uintptr_t)apic_base;

    /* Spis procesorow zaczyna sie za naglowkiem tablicy */
    for (uint32_t off = 44; off + 2 <= len && cpus < MAX_CPU; ) {
        const struct madt_wpis *w = (const struct madt_wpis *)(madt + off);
        if (!w->len)
            break;
        if (w->typ == 0 && w->len >= sizeof(struct madt_lapic)) {
            const struct madt_lapic *l = (const struct madt_lapic *)w;
            if (l->flags & 1)                 /* procesor wlaczony */
                apic_id[cpus++] = l->apic_id;
        }
        off += w->len;
    }
    if (cpus <= 1)
        return cpus ? cpus : 1;

    /* Wlacz lokalny APIC procesora rozruchowego: bez tego nie ma czym
       wyslac sygnalu. Wektor widmowy 0xFF i bit wlaczenia. */
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);

    /* Zmierz zegar APIC, dopoki mamy czym: PIT jest tylko tutaj */
    timer_measure();

    uint32_t size = (uint32_t)(ap_trampoline_end - ap_trampoline);
    if (size > 4096)
        return cpus;
    memcpy((void *)(uintptr_t)AP_BASE, ap_trampoline, size);

    /* Licznik i wskaznik stosu leza w skopiowanym bloku, nie w jadrze -
       procesor w trybie rzeczywistym nie siegnie tam, gdzie oryginal */
    uint32_t off_counter =
        (uint32_t)((uint8_t *)&ap_counter - ap_trampoline);
    uint32_t off_stack = (uint32_t)((uint8_t *)&ap_stack - ap_trampoline);
    uint32_t off_number = (uint32_t)((uint8_t *)&ap_number - ap_trampoline);
    volatile uint32_t *counter =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_counter);
    volatile uintptr_t *stack =
        (volatile uintptr_t *)(uintptr_t)(AP_BASE + off_stack);
    volatile uint32_t *number =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_number);
    *counter = 0;

    /* Tablice stron rdzen wtorny bierze te same, po ktorych chodzi
       jadro: w trybie dlugim bez nich nie ruszy wcale, a w trybie
       chronionym ruszylby, tyle ze adresy programow uzytkownika nie
       znaczylyby dla niego nic. */
    uint32_t off_cr3 = (uint32_t)((uint8_t *)&ap_cr3 - ap_trampoline);
    uintptr_t cr3;
#ifdef __x86_64__
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
#else
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
#endif
    *(volatile uint32_t *)(uintptr_t)(AP_BASE + off_cr3) = (uint32_t)cr3;

    uint8_t mine = (uint8_t)(apic_rd(APIC_ID) >> 24);

    /* Budzimy po jednym i czekamy na zgloszenie: stos jest jeden,
       wiec dwa procesory naraz zadeptalyby sobie ramki */
    for (int i = 0; i < cpus; i++) {
        if (apic_id[i] == mine)
            continue;

        void *memory = kmalloc(AP_STACK);
        if (!memory)
            break;
        *stack = (uintptr_t)memory + AP_STACK;
        /* Rdzen rozruchowy ma numer zero, wtorne dostaja kolejne -
           kazdy liczy swoj udzial w osobnym slowie, wiec nie musza
           sie o nie bic */
        *number = (uint32_t)(started + 1);

        uint32_t was = *counter;
        wake_cpu(apic_id[i], AP_BASE >> 12);

        for (int spin = 0; spin < 400000 && *counter == was; spin++)
            io_wait();
        if (*counter > was)
            started++;
    }

    return cpus;
}
