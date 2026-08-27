/* Stronicowanie
 *
 * Całą wykrytą pamięć fizyczną mapujemy identycznościowo (adres
 * wirtualny = fizyczny) stronami 4 KiB i włączamy bit PG w CR0. Jądro
 * działa dalej pod tymi samymi adresami, ale od teraz każdy dostęp poza
 * mapowany obszar wywoła page fault zamiast cichej korupcji pamięci.
 *
 * Pierwsze 64 MiB opisują tablice statyczne - jądro musi widzieć
 * pamięć, zanim zacznie prosić PMM o ramki na cokolwiek. Dalsze tablice
 * biorą się już z PMM, więc granicę wyznacza ilość RAM-u, a nie rozmiar
 * tablicy w .bss.
 *
 * Mapowanie kończy się najpóźniej na PAGING_USER_BASE: powyżej zaczyna
 * się przestrzeń użytkownika i tam ta sama liczba znaczy już co innego
 * w każdym procesie.
 */

#include <stdint.h>
#include "include/paging.h"
#include "include/pmm.h"
#include "include/string.h"
#include "include/shm.h"
#include "include/swap.h"
#include "include/printk.h"

#define PTE_PRESENT 0x1
#define PTE_WRITE   0x2
#define PTE_USER    0x4
#define PTE_ACCESSED 0x20   /* ustawia procesor przy uzyciu strony */
/* Bity 9 i 10 sa do dyspozycji systemu; sprzet je ignoruje.
   Bit 9  - strona oddana na kopiowanie przy zapisie (fork).
   Bit 10 - strona wyrzucona na dysk; numer miejsca siedzi w gornych
            bitach wpisu, ktore przy nieobecnej stronie i tak nikogo
            nie obchodza. */
#define PTE_COW     0x200
#define PTE_SWAPPED 0x400

#define NUM_TABLES (PAGING_STATIC_LIMIT / (4096u * 1024))

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[NUM_TABLES][1024] __attribute__((aligned(4096)));
static uint32_t identity_limit;   /* dokad naprawde siega mapowanie */

uint32_t paging_identity_limit(void)
{
    return identity_limit;
}

void paging_init(void)
{
    for (uint32_t t = 0; t < NUM_TABLES; t++) {
        for (uint32_t i = 0; i < 1024; i++)
            page_tables[t][i] = ((t * 1024 + i) * 4096) | PTE_PRESENT | PTE_WRITE;
        page_directory[t] = (uint32_t)page_tables[t] | PTE_PRESENT | PTE_WRITE;
    }
    identity_limit = PAGING_STATIC_LIMIT;

    /* Reszta wykrytej pamieci - tablice stron biora sie juz z PMM.
       Wyzej niz do poczatku przestrzeni uzytkownika nie idziemy. */
    uint32_t top = pmm_managed_top();
    if (top > PAGING_USER_BASE)
        top = PAGING_USER_BASE;
    if (top > PAGING_STATIC_LIMIT &&
        paging_map_phys(PAGING_STATIC_LIMIT, top - PAGING_STATIC_LIMIT) == 0)
        identity_limit = top;

    /* PG (bit 31) wlacza stronicowanie, WP (bit 16) sprawia, ze zakaz
       zapisu obowiazuje takze jadro. Bez WP kod jadra pisalby w strony
       tylko-do-odczytu bez pytania i psul kopiowanie przy zapisie -
       syscall zapisujacy do bufora procesu ominalby caly mechanizm. */
    __asm__ volatile(
        "movl %0, %%cr3\n\t"
        "movl %%cr0, %%eax\n\t"
        "orl $0x80010000, %%eax\n\t"
        "movl %%eax, %%cr0\n\t"
        :
        : "r"(page_directory)
        : "eax", "memory");
}

uint32_t paging_kernel_cr3(void)
{
    return (uint32_t)page_directory;
}

int paging_map_phys(uint32_t phys, uint32_t size)
{
    uint32_t start = phys & ~0xFFFu;
    uint32_t end = (phys + size + 0xFFFu) & ~0xFFFu;

    for (uint32_t a = start; a < end; a += 4096) {
        uint32_t pde = a >> 22;
        uint32_t pte = (a >> 12) & 0x3FF;
        if (!(page_directory[pde] & PTE_PRESENT)) {
            uint32_t pt = pmm_alloc_frame();   /* ramka nisko, widoczna wprost */
            if (!pt)
                return -1;
            memset((void *)pt, 0, 4096);
            page_directory[pde] = pt | PTE_PRESENT | PTE_WRITE;
        }
        uint32_t *pt = (uint32_t *)(page_directory[pde] & ~0xFFFu);
        pt[pte] = a | PTE_PRESENT | PTE_WRITE;
        __asm__ volatile("invlpg (%0)" : : "r"(a) : "memory");
    }
    return 0;
}

/* Czy ten wpis katalogu opisuje tablicę jądra?
 *
 * Tablice jądra są współdzielone przez wszystkie przestrzenie adresowe:
 * nie wolno ich niszczyć przy sprzątaniu procesu ani wpisywać do nich
 * stron użytkownika.
 *
 * Porównanie wprost z page_directory[i] nie wystarczy - procesor sam
 * ustawia w używanym wpisie bit "odwiedzony", więc ta sama tablica ma
 * po chwili różne wartości wpisu w różnych katalogach. Rozstrzyga za to
 * bit USER: tablice jądra go nie mają i nigdy mieć nie będą.
 */
static int kernel_table(uint32_t pde)
{
    return (pde & PTE_PRESENT) && !(pde & PTE_USER);
}

/* Każda ramka z PMM leży w obszarze mapowanym identycznościowo, więc
   jądro widzi ją wprost pod jej adresem fizycznym - katalogi i tablice
   stron procesów modyfikujemy bez żadnego okna ani mapowania tymczasowego. */

uint32_t paging_new_address_space(void)
{
    uint32_t dir_phys = pmm_alloc_frame();
    if (!dir_phys)
        return 0;
    uint32_t *dir = (uint32_t *)dir_phys;
    /* Mapowania jądra współdzielone (te same tablice stron, supervisor),
       reszta wpisów pusta - tam zamieszka program użytkownika */
    for (int i = 0; i < 1024; i++)
        dir[i] = page_directory[i];
    return dir_phys;
}

int paging_map_user_in(uint32_t dir_phys, uint32_t vaddr, uint32_t frame)
{
    /* Nie pozwól zamapować strony użytkownika na obszarze jądra */
    if (vaddr < PAGING_USER_BASE)
        return 0;

    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    uint32_t pte = (vaddr >> 12) & 0x3FF;
    uint32_t pt_phys;

    if (!(dir[pde] & PTE_PRESENT)) {
        pt_phys = pmm_alloc_frame();
        if (!pt_phys)
            return 0;
        memset((void *)pt_phys, 0, 4096);
        dir[pde] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    } else {
        if (kernel_table(dir[pde]))   /* tablica jądra - nie ruszaj */
            return 0;
        pt_phys = dir[pde] & ~0xFFFu;
    }

    ((uint32_t *)pt_phys)[pte] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    /* Jeśli mapujemy do aktywnej przestrzeni (sbrk), odśwież TLB */
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

uint32_t paging_get_mapping(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;
    uint32_t pte = ((uint32_t *)(dir[pde] & ~0xFFFu))[(vaddr >> 12) & 0x3FF];
    return pte & PTE_PRESENT ? pte & ~0xFFFu : 0;
}

void paging_destroy_address_space(uint32_t dir_phys)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    for (int i = 0; i < 1024; i++) {
        /* Zwalniaj tylko tablice procesu - jądrowe są współdzielone */
        if (!(dir[i] & PTE_PRESENT) || kernel_table(dir[i]))
            continue;
        uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PTE_PRESENT)) {
                /* Strona lezaca na dysku - oddaj jej miejsce */
                if (pt[j] & PTE_SWAPPED)
                    swap_free_slot((int)(pt[j] >> 12));
                continue;
            }
            uint32_t frame = pt[j] & ~0xFFFu;
            /* Ramek pamieci wspoldzielonej nie zwalniamy - moga ich
               uzywac inne procesy */
            if (!shm_is_shared(frame))
                pmm_free_frame(frame);
        }
        pmm_free_frame((uint32_t)pt);
    }
    pmm_free_frame(dir_phys);
}

/* --- Kopiowanie przy zapisie (fork) ---------------------------------
 *
 * Klon przestrzeni adresowej nie kopiuje ani bajta pamieci procesu:
 * obie strony dostaja te same ramki fizyczne, tyle ze bez prawa zapisu
 * i ze znacznikiem PTE_COW. Ramka wie, ilu ma wlascicieli (pmm_frame_ref).
 * Pierwszy zapis po ktorejkolwiek stronie wpada w page fault, a
 * paging_handle_cow() robi wtedy prywatna kopie tej jednej strony.
 */

static void flush_if_active(uint32_t dir_phys)
{
    uint32_t active;
    __asm__ volatile("movl %%cr3, %0" : "=r"(active));
    if (active == dir_phys)
        __asm__ volatile("movl %0, %%cr3" : : "r"(active) : "memory");
}

uint32_t paging_clone_address_space(uint32_t src_phys)
{
    uint32_t dst_phys = pmm_alloc_frame();
    if (!dst_phys)
        return 0;
    uint32_t *src = (uint32_t *)src_phys;
    uint32_t *dst = (uint32_t *)dst_phys;
    memset(dst, 0, 4096);

    for (int i = 0; i < 1024; i++) {
        /* Tablice jadra sa wspoldzielone - przepisujemy wpis bez zmian */
        if (!(src[i] & PTE_PRESENT) || kernel_table(src[i])) {
            dst[i] = page_directory[i];
            continue;
        }

        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            paging_destroy_address_space(dst_phys);
            flush_if_active(src_phys);
            return 0;
        }
        uint32_t *spt = (uint32_t *)(src[i] & ~0xFFFu);
        uint32_t *dpt = (uint32_t *)pt_phys;

        for (int j = 0; j < 1024; j++) {
            uint32_t e = spt[j];
            if (!(e & PTE_PRESENT) && (e & PTE_SWAPPED)) {
                /* Strona rodzica lezy na dysku - sciagnij ja, zanim
                   podzielisz ja z dzieckiem (jedno miejsce w pliku
                   wymiany nie moze nalezec do dwoch przestrzeni) */
                uint32_t va = ((uint32_t)i << 22) | ((uint32_t)j << 12);
                if (paging_swap_in(src_phys, va))
                    e = spt[j];
            }
            if (!(e & PTE_PRESENT)) {
                dpt[j] = 0;
                continue;
            }
            uint32_t frame = e & ~0xFFFu;
            /* Pamiec wspoldzielona ma zostac wspolna - nie kopiujemy jej */
            if (shm_is_shared(frame)) {
                dpt[j] = e;
                continue;
            }
            e = (e & ~(uint32_t)PTE_WRITE) | PTE_COW;
            spt[j] = e;
            dpt[j] = e;
            pmm_frame_ref(frame);
        }
        dst[i] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }

    /* Rodzic wlasnie stracil prawo zapisu do swoich stron */
    flush_if_active(src_phys);
    return dst_phys;
}

int paging_handle_cow(uint32_t vaddr, uint32_t err_code)
{
    if (!(err_code & 0x2))              /* nie zapis - nie nasza sprawa */
        return 0;

    uint32_t dir_phys;
    __asm__ volatile("movl %%cr3, %0" : "=r"(dir_phys));
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;

    uint32_t *pt = (uint32_t *)(dir[pde] & ~0xFFFu);
    uint32_t idx = (vaddr >> 12) & 0x3FF;
    uint32_t e = pt[idx];
    if (!(e & PTE_PRESENT) || !(e & PTE_COW))
        return 0;

    uint32_t frame = e & ~0xFFFu;
    if (pmm_frame_refs(frame) <= 1) {
        /* Zostalismy sami - wystarczy oddac prawo zapisu */
        pt[idx] = (e & ~(uint32_t)PTE_COW) | PTE_WRITE;
    } else {
        uint32_t copy = pmm_alloc_frame();
        if (!copy)
            return 0;                   /* brak pamieci - proces zginie */
        memcpy((void *)copy, (const void *)frame, 4096);
        pmm_free_frame(frame);          /* jeden wlasciciel mniej */
        pt[idx] = copy | (e & 0xFFFu & ~(uint32_t)PTE_COW) | PTE_WRITE;
    }
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}

/* --- wymiana stron -------------------------------------------------- */

/* Wpis strony wyrzuconej na dysk: nieobecna, ze znacznikiem i numerem
   miejsca w gornych bitach. Procesor takiego wpisu nie interpretuje. */
static uint32_t swap_pte(int slot)
{
    return ((uint32_t)slot << 12) | PTE_SWAPPED;
}

/* Wskaznik zegara: gdzie skonczylismy ostatnie szukanie ofiary. Bez
   niego kazde szukanie zaczynaloby sie od poczatku i wyrzucalo w kolko
   te same, najnizej lezace strony. */
static uint32_t clock_pde = 256;
static uint32_t clock_pte;

/* Wyrzuca na dysk jedna strone z podanej przestrzeni adresowej.
 *
 * Algorytm drugiej szansy: procesor sam zaznacza w kazdym uzytym wpisie
 * bit "odwiedzona". Strone z tym bitem zostawiamy, tylko go gasimy -
 * jesli do nastepnego obchodu nikt jej nie tknie, poleci. Zabieramy
 * wiec te, ktore naprawde lezaly odlogiem.
 *
 * Nie ruszamy stron wspoldzielonych (shm), stron oddanych na kopiowanie
 * przy zapisie ani ramek nalezacych do wiecej niz jednej przestrzeni -
 * z nimi wymiana wymagalaby liczenia wlascicieli takze na dysku.
 */
int paging_reclaim_pages(uint32_t dir_phys, int want)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    int freed = 0;

    /* Dwa obchody: pierwszy gasi bity "odwiedzona", drugi zabiera */
    for (int pass = 0; pass < 2 && freed < want; pass++) {
        /* Przestrzen uzytkownika to wpisy katalogu 256..767 */
        for (uint32_t n = 0; n < 512 && freed < want; n++) {
            uint32_t i = 256 + (clock_pde - 256 + n) % 512;
            if (!(dir[i] & PTE_PRESENT) || kernel_table(dir[i]))
                continue;
            uint32_t *pt = (uint32_t *)(dir[i] & ~0xFFFu);
            /* Wewnatrz tablicy tez zaczynamy tam, gdzie skonczylismy -
               inaczej kazde szukanie przemiatalo by od nowa strony,
               ktore juz dawno poszly na dysk */
            uint32_t start = (n == 0) ? clock_pte : 0;
            for (uint32_t k = 0; k < 1024 && freed < want; k++) {
                uint32_t j = (start + k) % 1024;
                uint32_t e = pt[j];
                if (!(e & PTE_PRESENT) || !(e & PTE_USER) || (e & PTE_COW))
                    continue;
                uint32_t frame = e & ~0xFFFu;
                if (shm_is_shared(frame) || pmm_frame_refs(frame) != 1)
                    continue;
                if (e & PTE_ACCESSED) {
                    pt[j] = e & ~(uint32_t)PTE_ACCESSED;   /* druga szansa */
                    continue;
                }

                int slot = swap_alloc_slot();
                if (slot < 0)
                    goto done;              /* plik wymiany pelny */
                if (swap_write(slot, (const void *)frame) < 0) {
                    swap_free_slot(slot);
                    goto done;
                }
                pt[j] = swap_pte(slot);
                pmm_free_frame(frame);
                clock_pde = i;
                clock_pte = j + 1;
                freed++;
            }
        }
        /* Po pierwszym obchodzie wszystkie bity sa juz zgaszone -
           drugi obchod znajdzie ofiary, o ile w ogole jest co brac */
    }
done:
    if (freed)
        flush_if_active(dir_phys);
    return freed;
}

/* Sciaga z dysku strone, ktora proces wlasnie probowal dotknac.
   Zwraca 1, gdy strona wrocila (mozna wznowic instrukcje). */
int paging_swap_in(uint32_t dir_phys, uint32_t vaddr)
{
    uint32_t *dir = (uint32_t *)dir_phys;
    uint32_t pde = vaddr >> 22;
    if (!(dir[pde] & PTE_PRESENT) || kernel_table(dir[pde]))
        return 0;

    uint32_t *pt = (uint32_t *)(dir[pde] & ~0xFFFu);
    uint32_t idx = (vaddr >> 12) & 0x3FF;
    uint32_t e = pt[idx];
    if ((e & PTE_PRESENT) || !(e & PTE_SWAPPED))
        return 0;

    int slot = (int)(e >> 12);
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                       /* brak pamieci nawet na powrot */
    if (swap_read(slot, (void *)frame) < 0) {
        pmm_free_frame(frame);
        return 0;
    }
    pt[idx] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    swap_free_slot(slot);
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
    return 1;
}
