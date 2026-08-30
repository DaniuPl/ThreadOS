/* Stronicowanie czteropoziomowe
 *
 * Adres wirtualny to cztery indeksy po dziewiec bitow i przesuniecie
 * w stronie. Kazdy poziom to jedna ramka 4 KiB z 512 wpisami po 64 bity.
 *
 * Pamiec opisujemy stronami po 2 MiB - jeden wpis zamiast 512, wiec
 * caly stumegabajtowy obszar mieści sie w kilku tablicach zamiast
 * w setkach. Drobniejsze mapowania robi paging64_map(), i to wlasnie
 * one beda potrzebne procesom.
 *
 * Doszedl bit, ktorego stronicowanie 32-bitowe nie mialo: zakaz
 * wykonywania. Strona z danymi moze byc od teraz oznaczona tak, ze
 * procesor odmowi wykonania czegokolwiek, co w niej lezy - bufor
 * obrazu czy sterta nie sa juz potencjalnym miejscem na cudzy kod.
 */

#include <stdint.h>
#include "paging64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/swap.h"
#include "../kernel/include/shm.h"

#define PTE_PRESENT 0x001
#define PTE_WRITE   0x002
#define PTE_HUGE    0x080
#define PTE_ACCESSED 0x020  /* ustawia procesor przy uzyciu strony */
#define PTE_DIRTY    0x040  /* ustawia procesor przy zapisie do strony */
#define ADDR_MASK   0x000FFFFFFFFFF000ULL

#define HUGE_SIZE   (2u * 1024 * 1024)

static uint64_t *pml4;             /* przestrzen jadra - wzorzec dla reszty */
static uint64_t identity_top;
static int nx_on;
static uint32_t cow_splits;        /* ile stron rozdzielilo kopiowanie */

/* Przestrzen, na ktorej procesor pracuje w tej chwili.
 *
 * Nie trzymamy jej w zmiennej: jedyna prawda jest rejestr CR3, a jego
 * zawartosc jest adresem fizycznym - ktory przy mapowaniu
 * identycznosciowym jest zarazem adresem wirtualnym tablicy. */
static uint64_t *root_now(void)
{
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(uintptr_t)(cr3 & ADDR_MASK);
}

static uint64_t *table_at(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(entry & ADDR_MASK);
}

/* Zwraca tablice nizszego poziomu, tworzac ja, gdy jeszcze nie ma.
 *
 * 'extra' to prawa, ktore musza byc otwarte po drodze. Procesor liczy
 * dostep jako iloczyn wpisow ze WSZYSTKICH poziomow, wiec zamkniety
 * wpis posredni uniewaznia najhojniejszy wpis konca sciezki - strona
 * oznaczona jako dostepna dla uzytkownika i tak byla dla niego
 * niewidoczna, bo tablica po drodze nie miala tego bitu.
 *
 * Otwieramy je na wyrost i to jest bezpieczne: o dostepie rozstrzyga
 * i tak najbardziej restrykcyjny wpis, a tym jest zawsze ten ostatni. */
static uint64_t *ensure(uint64_t *table, int idx, uint64_t extra)
{
    if (!(table[idx] & PTE_PRESENT)) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame)
            return (uint64_t *)0;
        memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
        table[idx] = (uint64_t)frame | PTE_PRESENT | PTE_WRITE | extra;
    } else {
        table[idx] |= extra;        /* dolozenie praw do istniejacej sciezki */
    }
    return table_at(table[idx]);
}

static void invlpg(uint64_t addr)
{
    __asm__ volatile("invlpg (%0)" : : "r"((void *)(uintptr_t)addr) : "memory");
}

static int map_in(uint64_t *top, uint64_t virt, uint64_t phys,
                  uint64_t flags)
{
    if (!top)
        return -1;

    /* Bit uzytkownika musi isc przez cala sciezke - inaczej ring 3
       nie zobaczy strony, choc jej wlasny wpis na to pozwala */
    uint64_t path = flags & PG_USER;

    uint64_t *pdpt = ensure(top, (int)((virt >> 39) & 0x1FF), path);
    if (!pdpt)
        return -1;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF), path);
    if (!pd)
        return -1;

    int i2 = (int)((virt >> 21) & 0x1FF);
    if (pd[i2] & PTE_HUGE)
        return -1;              /* obszar opisany juz strona 2 MiB */
    uint64_t *pt = ensure(pd, i2, path);
    if (!pt)
        return -1;

    pt[(virt >> 12) & 0x1FF] = (phys & ADDR_MASK) | PTE_PRESENT | flags;
    return 0;
}

int paging64_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    int rc = map_in(root_now(), virt, phys, flags);
    if (rc == 0)
        invlpg(virt);               /* tylko biezaca przestrzen ma co
                                       uniewazniac w buforze tlumaczen */
    return rc;
}

/* Jeden wpis na dwa megabajty - tak opisujemy pamiec i bufor obrazu */
static int map_huge(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pdpt = ensure(pml4, (int)((virt >> 39) & 0x1FF), 0);
    if (!pdpt)
        return -1;
    uint64_t *pd = ensure(pdpt, (int)((virt >> 30) & 0x1FF), 0);
    if (!pd)
        return -1;
    pd[(virt >> 21) & 0x1FF] = (phys & ~(uint64_t)(HUGE_SIZE - 1)) |
                               PTE_PRESENT | PTE_HUGE | flags;
    return 0;
}

uint64_t paging64_lookup(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    if (e & PTE_HUGE)
        return (e & ADDR_MASK) + (virt & (HUGE_SIZE - 1));
    e = table_at(e)[(virt >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    return (e & ADDR_MASK) + (virt & 0xFFF);
}

/* Wskaznik na wpis tablicy stron danego adresu albo NULL. Zwracamy
   wpis, a nie ramke: dzwoniacy chce tez bitow. */
static uint64_t *page_entry(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return (uint64_t *)0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return (uint64_t *)0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return (uint64_t *)0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return (uint64_t *)0;
    return &table_at(e)[(virt >> 12) & 0x1FF];
}

/* Czy strona zapisywalnego odwzorowania byla ruszana od ostatniego
   odeslania na dysk? Odpowiada za nas procesor - bit "brudna" ustawia
   sam przy kazdym zapisie, wiec odsylamy tylko to, co sie zmienilo. */
int paging64_file_dirty(uint64_t virt)
{
    uint64_t *w = page_entry(virt);
    if (!w)
        return 0;
    return (*w & PTE_PRESENT) && (*w & PG_FILE) && (*w & PTE_DIRTY);
}

/* Gasi bit "brudna" po odeslaniu strony do pliku */
void paging64_file_clean(uint64_t virt)
{
    uint64_t *w = page_entry(virt);
    if (!w)
        return;
    *w &= ~(uint64_t)PTE_DIRTY;
    invlpg(virt);
}

/* Zdejmuje mapowanie strony i zwraca ramke, ktora pod nia lezala
   (0 = nic tam nie bylo). Tablic posrednich nie zwalniamy: kosztuja
   po jednej ramce na dwa megabajty i zaraz sie przydadza. */
uint64_t paging64_unmap(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    uint64_t *pdpt = table_at(e);
    e = pdpt[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    uint64_t *pd = table_at(e);
    e = pd[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    if (!(pt[i] & PTE_PRESENT))
        return 0;
    uint64_t phys = pt[i] & ADDR_MASK;
    pt[i] = 0;
    invlpg(virt);
    return phys;
}

/* Czy caly zakres jest widoczny dla ring 3?
 *
 * Jadro nie ma prawa uwierzyc we wskaznik podany przez program:
 * proces moglby podsunac adres wlasnych tablic stron albo pamieci
 * jadra i kazac je sobie wypisac. Prawa dostepu sa iloczynem po
 * wszystkich czterech poziomach, wiec bit USER musi byc na kazdym
 * z nich - dokladnie tak, jak sprawdza to procesor. */
int paging64_user_range(uint64_t virt, uint64_t len)
{
    uint64_t *top = root_now();
    if (!top || len == 0)
        return len == 0;
    if (virt + len < virt)                   /* przekrecenie licznika */
        return 0;

    for (uint64_t va = virt & ~0xFFFULL; va < virt + len; va += 4096) {
        uint64_t e = top[(va >> 39) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        e = table_at(e)[(va >> 30) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        e = table_at(e)[(va >> 21) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
        if (e & PTE_HUGE)
            continue;
        e = table_at(e)[(va >> 12) & 0x1FF];
        if (!(e & PTE_PRESENT) || !(e & PG_USER))
            return 0;
    }
    return 1;
}

/* --- Przestrzenie adresowe procesow ------------------------------
 *
 * Kazdy proces dostaje wlasny korzen tablic, ale pamiec jadra musi byc
 * widoczna z kazdego z nich - inaczej pierwsze przerwanie po
 * przelaczeniu CR3 wykonaloby sie w prozni.
 *
 * Podzial wychodzi sam z rozkladu adresow. Najwyzszy poziom dzieli
 * przestrzen na kawalki po 512 GiB, a drugi - po gigabajcie. Cala
 * pamiec fizyczna, jadro i bufor obrazu mieszcza sie w kawalkach
 * innych niz ten drugi gigabajt, w ktorym leza programy. Wystarczy
 * wiec dac procesowi wlasna tablice drugiego poziomu i wyczyscic
 * w niej ten jeden wpis: reszta zostaje WSPOLNA, bo wskazuje na te
 * same ramki, co u jadra. Mapowanie dolozone przez jadro po starcie
 * widac natychmiast we wszystkich przestrzeniach naraz.
 */

uint64_t paging64_kernel_space(void)
{
    return (uint64_t)(uintptr_t)pml4;
}

uint64_t paging64_new_space(void)
{
    if (!pml4)
        return 0;

    uint32_t root = pmm_alloc_frame();
    if (!root)
        return 0;
    uint32_t mid = pmm_alloc_frame();
    if (!mid) {
        pmm_free_frame(root);
        return 0;
    }

    uint64_t *top = (uint64_t *)(uintptr_t)root;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)mid;

    memcpy(top, pml4, PAGE_SIZE);                 /* wszystko od jadra */
    memcpy(pdpt, table_at(pml4[0]), PAGE_SIZE);   /* pierwsze 512 GiB */
    pdpt[PAGING64_USER_PDPT] = 0;                 /* gigabajt programu - pusty */

    /* Bit uzytkownika na tej sciezce otwiera ja tylko z gory: o dostepie
       rozstrzyga i tak najbardziej restrykcyjny wpis, a ten lezy na
       koncu - w tablicy strony */
    top[0] = (uint64_t)mid | PTE_PRESENT | PTE_WRITE | PG_USER;
    return (uint64_t)root;
}

/* Powiela przestrzen procesu - bez kopiowania pamieci.
 *
 * Rodzic i dziecko dostaja te same ramki. Strona, ktora byla
 * zapisywalna, traci u OBU prawo zapisu i dostaje znacznik COW: przy
 * pierwszej probie zapisu procesor zglosi blad strony, a jadro dopiero
 * wtedy zrobi kopie - i to tylko tej jednej strony.
 *
 * Strony tylko do odczytu (kod, dane stale) nie potrzebuja nawet tego:
 * skoro nikt ich nie zmieni, moga zostac wspolne na zawsze. Pilnuje
 * tego licznik wlascicieli w PMM - ramka wraca do puli dopiero, gdy
 * zwolni ja ostatni.
 *
 * Zysk jest wprost proporcjonalny do rozmiaru programu: fork przestaje
 * kosztowac tyle, ile proces zajmuje pamieci, a zaczyna kosztowac tyle,
 * ile ma tablic stron.
 *
 * Zwraca korzen nowej przestrzeni albo 0. */
uint64_t paging64_clone_space(uint64_t src)
{
    uint64_t dst = paging64_new_space();
    if (!dst)
        return 0;

    uint64_t *stop = (uint64_t *)(uintptr_t)src;
    uint64_t *dtop = (uint64_t *)(uintptr_t)dst;
    uint64_t e = table_at(stop[0])[PAGING64_USER_PDPT];
    if (!(e & PTE_PRESENT))
        return dst;                     /* pusty proces - nic do dzielenia */

    uint64_t *pd = table_at(e);
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
            continue;
        uint64_t *pt = table_at(pd[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pt[j] & PTE_PRESENT))
                continue;

            uint64_t va = ((uint64_t)PAGING64_USER_PDPT << 30) |
                          ((uint64_t)i << 21) | ((uint64_t)j << 12);
            uint64_t frame = pt[j] & ADDR_MASK;
            uint64_t flags = pt[j] &
                             (PG_WRITE | PG_USER | PG_NX | PG_COW | PG_FILE);

            /* Zapisywalna strona idzie do obu jako tylko-do-odczytu
               ze znacznikiem - inaczej rodzic pisalby dziecku po
               pamieci, nie wiedzac o tym. Wyjatkiem jest zapisywalne
               odwzorowanie pliku: tam rodzic i dziecko MAJA pisac po
               tym samym, bo pisza do jednego pliku. */
            if ((flags & PG_WRITE) && !(flags & PG_FILE)) {
                flags = (flags & ~(uint64_t)PG_WRITE) | PG_COW;
                pt[j] = frame | PTE_PRESENT | flags;
                invlpg(va);
            }

            pmm_frame_ref((uint32_t)frame);
            if (map_in(dtop, va, frame, flags) < 0) {
                pmm_free_frame((uint32_t)frame);
                paging64_free_space(dst);
                return 0;
            }
        }
    }
    return dst;
}

/* Zapis do strony wspoldzielonej po fork-u.
 *
 * Jesli ramka ma juz tylko jednego wlasciciela, nie ma czego
 * rozdzielac - wystarczy oddac prawo zapisu. To czesty przypadek:
 * druga strona fork-a zdazyla sie skonczyc albo sama zapisala te
 * strone wczesniej.
 *
 * Zwraca 1, gdy strona zostala rozdzielona i program moze powtorzyc
 * zapis; 0 znaczy, ze to nie byl blad tego rodzaju. */
int paging64_handle_cow(uint64_t virt)
{
    uint64_t *top = root_now();
    if (!top)
        return 0;

    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;

    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    if (!(pt[i] & PTE_PRESENT) || !(pt[i] & PG_COW))
        return 0;

    uint64_t old = pt[i] & ADDR_MASK;
    uint64_t flags = (pt[i] & (PG_USER | PG_NX)) | PG_WRITE;

    if (pmm_frame_refs((uint32_t)old) <= 1) {
        pt[i] = old | PTE_PRESENT | flags;      /* jestesmy sami */
        invlpg(virt);
        return 1;
    }
    cow_splits++;

    uint32_t fresh = pmm_alloc_frame();
    if (!fresh)
        return 0;
    memcpy((void *)(uintptr_t)fresh, (const void *)(uintptr_t)old, PAGE_SIZE);
    pmm_free_frame((uint32_t)old);              /* jeden wlasciciel mniej */

    pt[i] = (uint64_t)fresh | PTE_PRESENT | flags;
    invlpg(virt);
    return 1;
}

uint32_t paging64_cow_splits(void)
{
    return cow_splits;
}

uint64_t paging64_current_space(void)
{
    uint64_t cr3;
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    return cr3 & ADDR_MASK;
}

void paging64_switch(uint64_t space)
{
    __asm__ volatile("movq %0, %%cr3" : : "r"(space) : "memory");
}

/* Zwalnia przestrzen procesu: strony programu, jego tablice i korzen.
   Tablic jadra nie dotyka - sa wspolne i zyja dalej. */
void paging64_free_space(uint64_t space)
{
    if (!space || space == (uint64_t)(uintptr_t)pml4)
        return;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    uint64_t *pdpt = table_at(top[0]);
    uint64_t e = pdpt[PAGING64_USER_PDPT];

    if (e & PTE_PRESENT) {
        uint64_t *pd = table_at(e);
        for (int i = 0; i < 512; i++) {
            if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
                continue;
            uint64_t *pt = table_at(pd[i]);
            for (int j = 0; j < 512; j++) {
                if (pt[j] & PTE_PRESENT)
                    pmm_free_frame((uint32_t)(pt[j] & ADDR_MASK));
                else if (pt[j] & PG_SWAPPED)
                    swap_free_slot((int)((pt[j] & ADDR_MASK) >> 12));
            }
            pmm_free_frame((uint32_t)(pd[i] & ADDR_MASK));
        }
        pmm_free_frame((uint32_t)(e & ADDR_MASK));
    }
    pmm_free_frame((uint32_t)((uint64_t)(uintptr_t)pdpt & ADDR_MASK));
    pmm_free_frame((uint32_t)space);
}

/* --- Wymiana stron na dysk ------------------------------------------
 *
 * Wpis strony wyrzuconej na dysk: bez bitu obecnosci, ze znacznikiem
 * i numerem miejsca w gornych bitach. Procesor takiego wpisu nie
 * oglada - dla niego to po prostu strona, ktorej nie ma.
 *
 * Prawa dostepu jada z nia razem. Nie wolno ich zgadnac przy powrocie:
 * strona kodu programu jest tylko do odczytu i wykonywalna, a strona
 * danych odwrotnie. Oddanie wszystkim jednych i tych samych praw konczy
 * sie tym, ze program po sciagnieciu wlasnego kodu z dysku dostaje
 * wyjatek przy pierwszej instrukcji - bo kod wrocil z zakazem
 * wykonywania.
 */
static uint64_t swap_pte(int slot, uint64_t right)
{
    return ((uint64_t)slot << 12) | PG_SWAPPED |
           (right & (PG_WRITE | PG_USER | PG_NX));
}

/* Wskaznik zegara: gdzie skonczylismy ostatnie szukanie ofiary. Bez
   niego kazde szukanie zaczynaloby sie od poczatku i wyrzucalo w kolko
   te same, najnizej lezace strony. */
static int clock_pd, clock_pt;

/* Wyrzuca na dysk najwyzej 'want' stron z podanej przestrzeni.
 *
 * Algorytm drugiej szansy: procesor sam zaznacza w kazdym uzytym wpisie
 * bit "odwiedzona". Strone z tym bitem zostawiamy, tylko go gasimy -
 * jesli do nastepnego obchodu nikt jej nie tknie, poleci. Zabieramy
 * wiec te, ktore naprawde lezaly odlogiem.
 *
 * Nie ruszamy stron wspoldzielonych (shm), stron oddanych na kopiowanie
 * przy zapisie ani ramek nalezacych do wiecej niz jednej przestrzeni:
 * z nimi wymiana wymagalaby liczenia wlascicieli takze na dysku.
 *
 * Chodzimy wylacznie po gigabajcie programu (PAGING64_USER_PDPT).
 * Pamiec jadra na dysk nie idzie - a nawet gdyby, to nie ma jej kto
 * sciagnac z powrotem, bo sciaga ja obsluga bledu strony. */
int paging64_reclaim_pages(uint64_t space, int want)
{
    if (!space || space == (uint64_t)(uintptr_t)pml4)
        return 0;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    if (!(top[0] & PTE_PRESENT))
        return 0;
    uint64_t e = table_at(top[0])[PAGING64_USER_PDPT];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    uint64_t *pd = table_at(e);
    int freed = 0;

    /* Dwa obchody: pierwszy gasi bity "odwiedzona", drugi zabiera */
    for (int pass = 0; pass < 2 && freed < want; pass++) {
        for (int n = 0; n < 512 && freed < want; n++) {
            int i = (clock_pd + n) % 512;
            if (!(pd[i] & PTE_PRESENT) || (pd[i] & PTE_HUGE))
                continue;
            uint64_t *pt = table_at(pd[i]);
            /* Wewnatrz tablicy tez zaczynamy tam, gdzie skonczylismy -
               inaczej kazde szukanie przemiataloby od nowa strony,
               ktore juz dawno poszly na dysk */
            int start = (n == 0) ? clock_pt : 0;
            for (int k = 0; k < 512 && freed < want; k++) {
                int j = (start + k) % 512;
                uint64_t w = pt[j];
                if (!(w & PTE_PRESENT) || !(w & PG_USER) || (w & PG_COW))
                    continue;
                /* Strony zapisywalnego odwzorowania nie ma po co
                   wyrzucac na dysk wymiany - jej miejscem na dysku
                   jest sam odwzorowany plik */
                if (w & PG_FILE)
                    continue;
                uint64_t frame = w & ADDR_MASK;
                if (shm_is_shared((uint32_t)frame) ||
                    pmm_frame_refs((uint32_t)frame) != 1)
                    continue;
                if (w & PTE_ACCESSED) {
                    pt[j] = w & ~(uint64_t)PTE_ACCESSED;   /* druga szansa */
                    continue;
                }

                int slot = swap_alloc_slot();
                if (slot < 0)
                    goto end;              /* plik wymiany pelny */
                if (swap_write(slot, (const void *)(uintptr_t)frame) < 0) {
                    swap_free_slot(slot);
                    goto end;
                }
                pt[j] = swap_pte(slot, w);
                pmm_free_frame((uint32_t)frame);
                clock_pd = i;
                clock_pt = j + 1;
                freed++;
            }
        }
        /* Po pierwszym obchodzie wszystkie bity sa juz zgaszone -
           drugi obchod znajdzie ofiary, o ile w ogole jest co brac */
    }
end:
    if (freed && space == (uint64_t)(uintptr_t)root_now())
        __asm__ volatile("movq %%cr3, %%rax; movq %%rax, %%cr3"
                         : : : "rax", "memory");
    return freed;
}

/* Sciaga z dysku strone, ktora program wlasnie probowal dotknac.
   Zwraca 1, gdy strona wrocila i mozna wznowic instrukcje. */
int paging64_swap_in(uint64_t space, uint64_t virt)
{
    if (!space)
        return 0;

    uint64_t *top = (uint64_t *)(uintptr_t)space;
    uint64_t e = top[(virt >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT))
        return 0;
    e = table_at(e)[(virt >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;
    e = table_at(e)[(virt >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
        return 0;

    uint64_t *pt = table_at(e);
    int i = (int)((virt >> 12) & 0x1FF);
    uint64_t w = pt[i];
    if ((w & PTE_PRESENT) || !(w & PG_SWAPPED))
        return 0;

    /* Adres wycinamy maska: gorne bity wpisu niosa prawa dostepu */
    int slot = (int)((w & ADDR_MASK) >> 12);
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                       /* brak pamieci nawet na powrot */
    if (swap_read(slot, (void *)(uintptr_t)frame) < 0) {
        pmm_free_frame(frame);
        return 0;
    }
    pt[i] = (uint64_t)frame | PTE_PRESENT |
            (w & (PG_WRITE | PG_USER | PG_NX));
    swap_free_slot(slot);
    invlpg(virt);
    return 1;
}

/* Doklejenie do mapowania obszaru lezacego poza pamiecia.
 *
 * Rejestry urzadzen leza wysoko - grubo ponad tym, co opisuje
 * mapowanie zbudowane przy starcie, bo tamto siega tylko szczytu
 * pamieci zarzadzanej przez PMM. Bez tego pierwszy odczyt z BAR-a
 * konczy sie bledem strony.
 *
 * Zapis wolno, wykonywanie nie: w rejestrach urzadzenia nie ma kodu. */
int paging64_map_phys(uint64_t phys, uint64_t len)
{
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + len + PAGE_SIZE - 1) & ~0xFFFULL;

    for (uint64_t a = start; a < end; a += PAGE_SIZE)
        if (paging64_lookup(a) != a &&
            paging64_map(a, a, PG_WRITE | PG_NX) < 0)
            return -1;
    return 0;
}

uint64_t paging64_identity_top(void)
{
    return identity_top;
}

int paging64_nx_enabled(void)
{
    return nx_on;
}

int paging64_init(void)
{
    /* Zakaz wykonywania trzeba najpierw wlaczyc w EFER - inaczej bit 63
       wpisu jest zarezerwowany i jego ustawienie konczy sie bledem */
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    low |= (1u << 11);                       /* NXE */
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080));
    nx_on = 1;

    uint32_t root = pmm_alloc_frame();
    if (!root)
        return -1;
    memset((void *)(uintptr_t)root, 0, PAGE_SIZE);
    pml4 = (uint64_t *)(uintptr_t)root;

    /* Cala pamiec, ktora zarzadza PMM - identycznosciowo. Jadro,
       jego stos i wszystkie tablice stron leza w tym obszarze, wiec
       po przelaczeniu CR3 nic nam nie znika spod nog. */
    uint64_t top = (pmm_managed_top() + HUGE_SIZE - 1) & ~(uint64_t)(HUGE_SIZE - 1);
    for (uint64_t a = 0; a < top; a += HUGE_SIZE)
        if (map_huge(a, a, PTE_WRITE | PG_NX) < 0)
            return -1;
    identity_top = top;

    /* Kod jadra musi zostac wykonywalny - pierwsze dwa megabajty nad
       megabajtem to obraz jadra, wiec temu jednemu wpisowi zdejmujemy
       zakaz wykonywania */
    if (map_huge(0, 0, PTE_WRITE) < 0 ||
        map_huge(HUGE_SIZE, HUGE_SIZE, PTE_WRITE) < 0)
        return -1;

    /* Bufor obrazu lezy wysoko, poza pamiecia - i na pewno nie ma
       w nim kodu do wykonania */
    if (fbcon_have()) {
        uint64_t fb = fbcon_addr();
        uint64_t end = fb + fbcon_bytes();
        for (uint64_t a = fb & ~(uint64_t)(HUGE_SIZE - 1); a < end;
             a += HUGE_SIZE)
            if (map_huge(a, a, PTE_WRITE | PG_NX) < 0)
                return -1;
    }

    __asm__ volatile("movq %0, %%cr3" : : "r"((uint64_t)root) : "memory");
    return 0;
}
