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
 * Na tym ten krok sie konczy: obudzony procesor przechodzi w tryb
 * chroniony, zglasza sie w liczniku i staje. Dawanie mu pracy
 * wymagaloby schedulera per rdzen i zamkow w kazdej strukturze jadra -
 * a te pisane byly dla jednego rdzenia i cicho na to liczą. Lepiej,
 * zeby procesor stal, niz zeby psul liste zadan.
 */

#include <stdint.h>
#include "include/smp.h"
#include "include/acpi.h"
#include "include/heap.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/spinlock.h"

/* Rejestry APIC leza wysoko, poza pamiecia opisana przy starcie -
   doklejenie ich do mapowania robi w kazdym trybie inny kod. Rdzen
   wtorny w trybie dlugim potrzebuje przy okazji adresu tablic stron,
   bo buduje sobie droge do trybu dlugiego sam. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#define IDENTITY_TOP()   paging64_identity_top()
extern volatile uint32_t ap_cr3;
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#define IDENTITY_TOP()   paging_identity_limit()
#endif

#define AP_BASE       0x8000        /* dokad kopiujemy trampoline */
#define AP_STACK      8192          /* stos na kazdy obudzony rdzen */
#define MAX_CPU       8

/* Rejestry lokalnego APIC (przesuniecia od jego bazy) */
#define APIC_ID       0x020
#define APIC_SVR      0x0F0         /* rejestr przerwania widmowego */
#define APIC_ICR_LO   0x300
#define APIC_ICR_HI   0x310

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

static volatile uint8_t *apic;
static int cpus, started;
static uint8_t apic_id[MAX_CPU];

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

    for (;;) {
        if (praca_ile)
            rob_co_jest((int)numer);
        __asm__ volatile("pause" ::: "memory");
    }
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

static void apic_wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(apic + reg) = v;
}

static uint32_t apic_rd(uint32_t reg)
{
    return *(volatile uint32_t *)(apic + reg);
}

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

#ifdef __x86_64__
    /* Rdzen wtorny wlacza sobie tryb dlugi sam i potrzebuje do tego
       tablic stron - bierze te, po ktorych chodzi jadro */
    uint32_t off_cr3 = (uint32_t)((uint8_t *)&ap_cr3 - ap_trampolina);
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    *(volatile uint32_t *)(uintptr_t)(AP_BASE + off_cr3) = (uint32_t)cr3;
#endif

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
