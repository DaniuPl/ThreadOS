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
#define ZALADUJ_GDT()    gdt64_zaladuj()
#define ZALADUJ_IDT()    idt64_zaladuj()
#define ZALADUJ_TSS()    gdt64_zaladuj_tss()
#define TSS_SEL_BAZA     GDT64_TSS
#define TSS_SEL_KROK     16
#define WEJSCIE_SYSCALL() syscall64_init()
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#define IDENTITY_TOP()   paging_identity_limit()
#include "include/gdt.h"
#include "include/idt.h"
#define ZALADUJ_GDT()    gdt_zaladuj()
#define ZALADUJ_IDT()    idt_zaladuj()
#define ZALADUJ_TSS()    gdt_zaladuj_tss()
#define TSS_SEL_BAZA     GDT_TSS
#define TSS_SEL_KROK     8
#define WEJSCIE_SYSCALL() do { } while (0)   /* i386 wchodzi bramka int 0x80 */
#endif

#define AP_BASE       0x8000        /* dokad kopiujemy trampoline */
#define AP_STACK      8192          /* stos na kazdy obudzony rdzen */
#define MAX_CPU       SMP_RDZENI

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

#define LVT_OKRESOWY  (1u << 17)    /* licznik startuje od nowa sam */
#define LVT_ZAMASKOWANY (1u << 16)
#define SMP_WEKTOR_ZEGARA 48
#define SMP_WEKTOR_POBUDKI 49
#define ICR_WSZYSTKIM_POZA_MNA 0x000C0000u

#define ICR_INIT      0x00000500
#define ICR_STARTUP   0x00000600
#define ICR_ASSERT    0x00004000
#define ICR_PENDING   0x00001000    /* sygnal jeszcze w drodze */

/* Naglowek wpisu w MADT: kazdy zaczyna sie tak samo */
struct madt_wpis {
    uint8_t typ;
    uint8_t dlugosc;
} __attribute__((packed));

struct madt_lapic {
    struct madt_wpis naglowek;
    uint8_t acpi_id;
    uint8_t apic_id;
    uint32_t flagi;
} __attribute__((packed));

extern uint8_t ap_trampolina[], ap_trampolina_koniec[];
extern volatile uint32_t ap_licznik;
extern volatile uint32_t ap_stos;
extern volatile uint32_t ap_numer;
extern volatile uint32_t ap_cr3;

static volatile uint8_t *apic;
static int cpus, started;
static uint8_t apic_id[MAX_CPU];
static uint32_t zegar_krok;              /* ile tykniec APIC na 10 ms */
static volatile uint32_t tykniec[MAX_CPU];

/* --- Wspolna kolejka pracy ------------------------------------------
 *
 * Caly stan dzielony miedzy rdzeniami to te kilka pol. Numer nastepnej
 * jednostki bierze sie pod zamkiem, a liczy juz bez niego - inaczej
 * rdzenie czekalyby na siebie zamiast pracowac.
 */
static struct spinlock zamek = SPINLOCK_WOLNY;
static void (*praca_fn)(uint32_t, void *);
static void *praca_dane;
static volatile uint32_t praca_ile;      /* ile jednostek w zleceniu */
static volatile uint32_t praca_nastepna; /* pierwsza jeszcze nie wzieta */
static volatile uint32_t praca_zrobione;
static volatile uint32_t udzial[MAX_CPU];

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
int smp_rdzen_apic(void)
{
    if (!apic)
        return 0;
    uint8_t id = (uint8_t)(apic_rd(APIC_ID) >> 24);
    for (int i = 0; i < cpus; i++)
        if (apic_id[i] == id)
            return i;
    return 0;
}

int smp_rdzen(void)
{
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr < TSS_SEL_BAZA)
        return 0;                    /* segment jeszcze niezaladowany */
    int nr = (tr - TSS_SEL_BAZA) / TSS_SEL_KROK;
    return (nr >= 0 && nr < SMP_RDZENI) ? nr : 0;
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
static void zmierz_zegar(void)
{
    /* Czekanie na PIT musi byc ograniczone. Ten pomiar biegnie bardzo
       wczesnie i na maszynie, na ktorej PIT nie tyka (albo jego
       przerwanie jeszcze nigdzie nie dochodzi), petla bez granicy
       zawiesilaby caly rozruch - i to w miejscu, ktore niczego o sobie
       nie mowi. */
#define ZWLOKA_MAX 20000000u

    apic_wr(APIC_TIM_DIV, 0x3);                  /* dzielnik 16 */
    apic_wr(APIC_LVT_TIM, LVT_ZAMASKOWANY);      /* liczy, ale nie zglasza */

    uint32_t start = pit_ticks();
    uint32_t obiegow = 0;
    while (pit_ticks() == start && obiegow < ZWLOKA_MAX)  /* granica tykniecia */
        obiegow++;
    apic_wr(APIC_TIM_INIT, 0xFFFFFFFFu);

    start = pit_ticks();
    obiegow = 0;
    while (pit_ticks() - start < 10 && obiegow < ZWLOKA_MAX)   /* 100 ms */
        obiegow++;
    uint32_t zostalo = apic_rd(APIC_TIM_CUR);
    apic_wr(APIC_TIM_INIT, 0);                   /* stop */

    uint32_t na_100ms = 0xFFFFFFFFu - zostalo;
    zegar_krok = na_100ms / 10;                  /* na dziesiec milisekund */
    /* Gdy pomiar sie nie udal, lepiej dac zegarowi wartosc z sufitu niz
       zadna: rdzen bez przerwan nie da sie nigdy wywlaszczyc. */
    if (obiegow >= ZWLOKA_MAX || zegar_krok < 1000 || zegar_krok > 0x10000000u)
        zegar_krok = 1000000;
#undef ZWLOKA_MAX
}

/* Wlacza lokalny APIC i jego zegar na biezacym rdzeniu */
static void zegar_wlacz(void)
{
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);
    apic_wr(APIC_TIM_DIV, 0x3);
    apic_wr(APIC_LVT_TIM, SMP_WEKTOR_ZEGARA | LVT_OKRESOWY);
    apic_wr(APIC_TIM_INIT, zegar_krok);
}

/* Obsluga tykniecia zegara APIC - wolana z isr_handler dla wektora 48.
 *
 * Kazdy rdzen liczy swoje w osobnym slowie, wiec nie potrzeba tu zamka.
 * Potwierdzenie idzie do WLASNEGO APIC tego rdzenia, a nie do PIC:
 * przerwanie nie przeszlo przez kontroler, wiec nie ma go tam komu
 * zamknac. */
void smp_zegar_tyk(void)
{
    int r = smp_rdzen();
    tykniec[r]++;
    if (apic)
        apic_wr(APIC_EOI, 0);
    /* Potwierdzenie idzie PRZED przelaczeniem zadan: schedule moze nie
       wrocic tu przez dluzszy czas, a lokalny APIC nie ma na co czekac.
       Tak samo robi obsluga timera na rdzeniu rozruchowym. */
    if (task_scheduler_gotowy())
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
static void pobudka_dla_reszty(void)
{
    if (!apic)
        return;
    apic_wr(APIC_ICR_HI, 0);
    apic_wr(APIC_ICR_LO, ICR_WSZYSTKIM_POZA_MNA | SMP_WEKTOR_POBUDKI);
}

void smp_pobudka_odbierz(void)
{
    if (apic)
        apic_wr(APIC_EOI, 0);
}

uint32_t smp_tykniec(int rdzen)
{
    return (rdzen >= 0 && rdzen < MAX_CPU) ? tykniec[rdzen] : 0;
}

uint32_t smp_udzial(int rdzen)
{
    return (rdzen >= 0 && rdzen < MAX_CPU) ? udzial[rdzen] : 0;
}

int smp_pracujacych(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CPU; i++)
        if (udzial[i])
            n++;
    return n;
}

/* Bierze jedna jednostke do policzenia. Zwraca 0, gdy nie ma juz nic.
 *
 * Przerwania gasimy na czas trzymania zamka takze na rdzeniu
 * rozruchowym: gdyby timer wywlaszczyl watek trzymajacy zamek, kazdy
 * inny checny musialby czekac do jego nastepnej kolejki. */
static int wez_jednostke(uint32_t *numer)
{
    uint32_t flags = irq_save();
    spin_lock(&zamek);
    int jest = praca_nastepna < praca_ile;
    if (jest)
        *numer = praca_nastepna++;
    spin_unlock(&zamek);
    irq_restore(flags);
    return jest;
}

/* Petla robocza - ta sama dla rdzenia rozruchowego i dla wtornych */
static void rob_co_jest(int rdzen)
{
    uint32_t numer;
    while (wez_jednostke(&numer)) {
        praca_fn(numer, praca_dane);
        udzial[rdzen]++;
        __atomic_add_fetch(&praca_zrobione, 1, __ATOMIC_SEQ_CST);
    }
}

/* Wejscie rdzenia wtornego z trampoliny. Nie wraca.
 *
 * Ten rdzen nie ma tablicy wyjatkow ani przerwan - kazdy blad konczy
 * sie potrojnym bledem i resetem maszyny. Dlatego robi tu jedna
 * rzecz: czeka na zlecenie i liczy to, co dostal. */
void smp_ap_glowna(uint32_t numer)
{
    if (numer >= MAX_CPU)
        numer = MAX_CPU - 1;

    /* Trampolina zostawila ten rdzen na wlasnej, tymczasowej tablicy
       segmentow i bez zadnej tablicy wyjatkow - kazdy blad konczyl sie
       potrojnym bledem i cichym resetem calej maszyny. Obie tablice sa
       jedne dla systemu i po zbudowaniu nikt ich nie zmienia, wiec
       wystarczy je tutaj wskazac. */
    ZALADUJ_GDT();
    ZALADUJ_IDT();
    ZALADUJ_TSS();
    /* Rejestry wejscia przez instrukcje syscall sa wlasnoscia rdzenia -
       ten, ktory ich nie ustawi, nie przyjmie jej wcale */
    WEJSCIE_SYSCALL();

    /* Wlasny zegar. Przerwanie z PIT idzie przez kontroler PIC do
       JEDNEGO procesora - ten go nie zobaczy nigdy. */
    zegar_wlacz();
    __asm__ volatile("sti");

    /* Scheduler wstaje pozniej niz rdzenie - do tej chwili ten rdzen
       umie tylko policzyc, co mu zlecono */
    while (!task_scheduler_gotowy()) {
        if (praca_ile)
            rob_co_jest((int)numer);
        else
            __asm__ volatile("hlt");
    }

    /* Odtad rdzen jest pelnoprawnym procesorem jadra: ma wlasne
       zadanie bezczynne i bierze z listy to, co wolno mu prowadzic.
       Ta funkcja nie wraca. */
    task_rdzen_wejdz((int)numer);

    for (;;) {
        if (praca_ile) {
            rob_co_jest((int)numer);
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

void smp_rob_zlecenie(int rdzen)
{
    if (praca_ile)
        rob_co_jest(rdzen);
}

void smp_praca(void (*funkcja)(uint32_t numer, void *dane), void *dane,
               uint32_t ile)
{
    if (!funkcja || !ile)
        return;

    for (int i = 0; i < MAX_CPU; i++)
        udzial[i] = 0;

    praca_fn = funkcja;
    praca_dane = dane;
    praca_nastepna = 0;
    praca_zrobione = 0;
    __atomic_store_n(&praca_ile, ile, __ATOMIC_SEQ_CST);

    /* Zlecenie jest juz widoczne - teraz trzeba obudzic tych, ktorzy
       na nie czekaja spiac. Kolejnosc nie jest dowolna: pobudka wyslana
       przed zapisem zastalaby jeszcze pusta kolejke. */
    pobudka_dla_reszty();

    /* Rdzen rozruchowy liczy razem z pozostalymi: na maszynie
       jednordzeniowej nie policzylby inaczej nikt */
    rob_co_jest(0);

    /* Czekamy na tych, ktorzy wzieli jednostke tuz przed nami */
    while (__atomic_load_n(&praca_zrobione, __ATOMIC_SEQ_CST) < ile)
        __asm__ volatile("pause" ::: "memory");

    __atomic_store_n(&praca_ile, 0, __ATOMIC_SEQ_CST);
}

int smp_cpus(void)    { return cpus ? cpus : 1; }
int smp_started(void) { return started; }

/* Zwloka liczona portem 0x80 - PIT moze byc jeszcze zajety czym
   innym, a tu chodzi o rzad wielkosci, nie o dokladnosc */
static void zwloka(uint32_t petli)
{
    for (uint32_t i = 0; i < petli; i++)
        io_wait();
}

static void czekaj_na_wyslanie(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(apic_rd(APIC_ICR_LO) & ICR_PENDING))
            return;
        io_wait();
    }
}

/* INIT, a po nim dwa SIPI - tak, jak kaze dokumentacja procesora */
static void obudz(uint8_t id, uint8_t wektor)
{
    apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
    apic_wr(APIC_ICR_LO, ICR_INIT | ICR_ASSERT);
    czekaj_na_wyslanie();
    zwloka(20000);                        /* rzedu dziesieciu milisekund */

    for (int i = 0; i < 2; i++) {
        apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
        apic_wr(APIC_ICR_LO, ICR_STARTUP | wektor);
        czekaj_na_wyslanie();
        zwloka(600);                      /* rzedu dwustu mikrosekund */
    }
}

int smp_init(void)
{
    const uint8_t *madt = acpi_table("APIC");
    if (!madt)
        return 1;

    uint32_t dlugosc = (uint32_t)madt[4] | ((uint32_t)madt[5] << 8) |
                       ((uint32_t)madt[6] << 16) | ((uint32_t)madt[7] << 24);
    uint32_t apic_baza = (uint32_t)madt[36] | ((uint32_t)madt[37] << 8) |
                         ((uint32_t)madt[38] << 16) |
                         ((uint32_t)madt[39] << 24);
    if (dlugosc < 44 || !apic_baza)
        return 1;

    /* Rejestry APIC leza wysoko, poza pamiecia opisana przy starcie */
    if ((uint64_t)apic_baza + 0x1000 > IDENTITY_TOP() &&
        MAP_PHYS(apic_baza, 0x1000) < 0)
        return 1;
    apic = (volatile uint8_t *)(uintptr_t)apic_baza;

    /* Spis procesorow zaczyna sie za naglowkiem tablicy */
    for (uint32_t off = 44; off + 2 <= dlugosc && cpus < MAX_CPU; ) {
        const struct madt_wpis *w = (const struct madt_wpis *)(madt + off);
        if (!w->dlugosc)
            break;
        if (w->typ == 0 && w->dlugosc >= sizeof(struct madt_lapic)) {
            const struct madt_lapic *l = (const struct madt_lapic *)w;
            if (l->flagi & 1)                 /* procesor wlaczony */
                apic_id[cpus++] = l->apic_id;
        }
        off += w->dlugosc;
    }
    if (cpus <= 1)
        return cpus ? cpus : 1;

    /* Wlacz lokalny APIC procesora rozruchowego: bez tego nie ma czym
       wyslac sygnalu. Wektor widmowy 0xFF i bit wlaczenia. */
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);

    /* Zmierz zegar APIC, dopoki mamy czym: PIT jest tylko tutaj */
    zmierz_zegar();

    uint32_t rozmiar = (uint32_t)(ap_trampolina_koniec - ap_trampolina);
    if (rozmiar > 4096)
        return cpus;
    memcpy((void *)(uintptr_t)AP_BASE, ap_trampolina, rozmiar);

    /* Licznik i wskaznik stosu leza w skopiowanym bloku, nie w jadrze -
       procesor w trybie rzeczywistym nie siegnie tam, gdzie oryginal */
    uint32_t off_licznik =
        (uint32_t)((uint8_t *)&ap_licznik - ap_trampolina);
    uint32_t off_stos = (uint32_t)((uint8_t *)&ap_stos - ap_trampolina);
    uint32_t off_numer = (uint32_t)((uint8_t *)&ap_numer - ap_trampolina);
    volatile uint32_t *licznik =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_licznik);
    volatile uintptr_t *stos =
        (volatile uintptr_t *)(uintptr_t)(AP_BASE + off_stos);
    volatile uint32_t *numer =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_numer);
    *licznik = 0;

    /* Tablice stron rdzen wtorny bierze te same, po ktorych chodzi
       jadro: w trybie dlugim bez nich nie ruszy wcale, a w trybie
       chronionym ruszylby, tyle ze adresy programow uzytkownika nie
       znaczylyby dla niego nic. */
    uint32_t off_cr3 = (uint32_t)((uint8_t *)&ap_cr3 - ap_trampolina);
    uintptr_t cr3;
#ifdef __x86_64__
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
#else
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
#endif
    *(volatile uint32_t *)(uintptr_t)(AP_BASE + off_cr3) = (uint32_t)cr3;

    uint8_t moj = (uint8_t)(apic_rd(APIC_ID) >> 24);

    /* Budzimy po jednym i czekamy na zgloszenie: stos jest jeden,
       wiec dwa procesory naraz zadeptalyby sobie ramki */
    for (int i = 0; i < cpus; i++) {
        if (apic_id[i] == moj)
            continue;

        void *pamiec = kmalloc(AP_STACK);
        if (!pamiec)
            break;
        *stos = (uintptr_t)pamiec + AP_STACK;
        /* Rdzen rozruchowy ma numer zero, wtorne dostaja kolejne -
           kazdy liczy swoj udzial w osobnym slowie, wiec nie musza
           sie o nie bic */
        *numer = (uint32_t)(started + 1);

        uint32_t bylo = *licznik;
        obudz(apic_id[i], AP_BASE >> 12);

        for (int spin = 0; spin < 400000 && *licznik == bylo; spin++)
            io_wait();
        if (*licznik > bylo)
            started++;
    }

    return cpus;
}
