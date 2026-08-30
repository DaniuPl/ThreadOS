/* Ladowanie programow ELF64 z initrd
 *
 * Program dotychczas dzialajacy w ring 3 lezal wpisany na sztywno
 * w obrazie jadra - kilkanascie bajtow w sekcji .rodata. To dowodzilo,
 * ze przejscie do ring 3 dziala, ale niczego wiecej: system, ktory
 * umie uruchomic tylko wlasny kawalek siebie, nie uruchamia programow.
 *
 * Tutaj program jest juz osobnym plikiem: zbudowanym osobno, wlozonym
 * do initrd i wczytanym przez jadro dopiero po starcie. Jadro nie wie
 * o nim nic ponad to, co przeczyta z naglowka ELF.
 */

#include "elf64.h"
#include "paging64.h"

/* Przestrzen adresowa procesu jest prywatna od granicy jednego
   gigabajta - a ta granica jest jednoczesnie podzialem tablic
   drugiego poziomu. Obie liczby musza sie zgadzac. */
_Static_assert((USER64_BASE >> 30) == PAGING64_USER_PDPT,
               "USER64_BASE musi lezec w gigabajcie PAGING64_USER_PDPT");
#include "../kernel/include/pmm.h"
#include "../kernel/include/dynlink.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

/* Naglowki ELF64 - tylko te pola, ktorych naprawde uzywamy.
   Wzgledem ELF32 zmienil sie nie tylko rozmiar adresow: w naglowku
   segmentu pole z prawami dostepu przenioslo sie tuz za typ, zeby
   szescdziesieciocztero-bitowe adresy lezaly wyrownane. */
struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define ET_EXEC     2
#define EM_X86_64   62
#define ELFCLASS64  2
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2

static int elf_valid(const struct elf64_ehdr *eh, uint32_t size)
{
    if (size < sizeof(*eh))
        return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != ELFCLASS64)
        return 0;
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return 0;
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size)
        return 0;
    return eh->e_type == ET_EXEC && eh->e_machine == EM_X86_64 && eh->e_phnum;
}

/* Ramka pod strone programu: albo juz jest (dwa segmenty potrafia
   siegnac tej samej strony), albo bierzemy nowa i zerujemy - dzieki
   temu .bss nie musi byc niczym wypelniany osobno. */
static uint64_t page_frame_for(uint64_t va, uint64_t flags)
{
    uint64_t frame = paging64_lookup(va);
    if (frame)
        return frame;
    frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
    if (paging64_map(va, frame, flags) < 0) {
        pmm_free_frame((uint32_t)frame);
        return 0;
    }
    return frame;
}

/* --- biblioteka wspoldzielona w przestrzeni procesu ------------------
 *
 * Segmenty bez prawa zapisu (kod i stale) dostaja RAMKI WSPOLNE: te
 * same, ktore widzi kazdy inny proces - i to jest cala oszczednosc.
 * Segment z danymi jest inny i musi byc: biblioteka trzyma w nim swoj
 * stan, a stan jednego procesu nie ma prawa byc widoczny w drugim.
 */
static int map_library(void)
{
    uint64_t vaddr;
    uint32_t memsz, filesz;
    int right;
    const uint8_t *data;

    for (int i = 0; dyn_segment(i, &vaddr, &memsz, &filesz, &right, &data);
         i++) {
        /* Prawa podaje sam ELF - tak samo jak przy segmentach programu.
           Dane biblioteki nie sa wiec wykonywalne, a jej kod nie jest
           zapisywalny, i pilnuje tego procesor. */
        uint64_t flags = PG_USER;
        if (right & DYN_WRITE)
            flags |= PG_WRITE;
        if (!(right & DYN_EXEC))
            flags |= PG_NX;

        uint64_t start = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = vaddr + memsz;
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            if (!(right & DYN_WRITE)) {
                uint64_t frame = dyn_frame(va);
                if (!frame || paging64_map(va, frame, flags) < 0)
                    return -1;
                /* Ramka ma przezyc smierc procesu - nalezy do systemu */
                pmm_frame_ref((uint32_t)frame);
                continue;
            }

            uint64_t frame = page_frame_for(va, flags);
            if (!frame)
                return -1;
            uint64_t od = va > vaddr ? va : vaddr;
            uint64_t file_to = vaddr + filesz;
            if (od < file_to) {
                uint64_t do_ = va + PAGE_SIZE < file_to ? va + PAGE_SIZE
                                                       : file_to;
                memcpy((void *)(uintptr_t)(frame + (od - va)),
                       data + (od - vaddr), (uint32_t)(do_ - od));
            }
        }
    }
    return 0;
}

/* Zapisuje rozwiazany adres w tablicy skokow programu. Przestrzen
   procesu jest juz aktywna, wiec piszemy wprost. */
static int write_addr(void *ctx, uint64_t addr, uint64_t value)
{
    (void)ctx;
    if (!paging64_user_range(addr, sizeof(uint64_t)))
        return -1;
    *(uint64_t *)(uintptr_t)addr = value;
    return 0;
}

static int load_segment(const uint8_t *file, const struct elf64_phdr *ph)
{
    if (ph->p_vaddr < USER64_BASE ||
        ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||     /* przekrecenie */
        ph->p_vaddr + ph->p_memsz > USER64_TOP) {
        printk("[jadro] elf64: segment poza zakresem uzytkownika (%x)\n",
               (uint32_t)ph->p_vaddr);
        return -1;
    }

    /* Prawa podaje sam plik. Brak bitu "wykonywalny" znaczy zakaz
       wykonywania - i to procesor egzekwuje, nie my. */
    uint64_t flags = PG_USER;
    if (ph->p_flags & PF_W)
        flags |= PG_WRITE;
    if (!(ph->p_flags & PF_X))
        flags |= PG_NX;

    uint64_t file_end = ph->p_vaddr + ph->p_filesz;

    for (uint64_t va = ph->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
         va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
        uint64_t frame = page_frame_for(va, flags);
        if (!frame)
            return -1;

        uint64_t from = va > ph->p_vaddr ? va : ph->p_vaddr;
        if (from < file_end) {
            uint64_t to = va + PAGE_SIZE < file_end ? va + PAGE_SIZE : file_end;
            memcpy((void *)(uintptr_t)(frame + (from - va)),
                   file + ph->p_offset + (from - ph->p_vaddr),
                   (uint32_t)(to - from));
        }
    }
    return 0;
}

int elf64_load(const void *image, uint32_t size, uint64_t *entry,
               uint64_t *brk_base)
{
    const uint8_t *file = (const uint8_t *)image;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    if (!elf_valid(eh, size)) {
        printk("[jadro] elf64: to nie jest program ELF64 x86-64\n");
        return -1;
    }

    const struct elf64_phdr *ph =
        (const struct elf64_phdr *)(file + eh->e_phoff);
    int loaded = 0;
    uint64_t top = USER64_BASE;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
            continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) {
            printk("[jadro] elf64: segment wystaje poza plik\n");
            return -1;
        }
        if (load_segment(file, &ph[i]) < 0)
            return -1;
        if (ph[i].p_vaddr + ph[i].p_memsz > top)
            top = ph[i].p_vaddr + ph[i].p_memsz;
        loaded++;
    }
    if (!loaded) {
        printk("[jadro] elf64: brak segmentow do zaladowania\n");
        return -1;
    }
    if (eh->e_entry < USER64_BASE || eh->e_entry >= USER64_TOP) {
        printk("[jadro] elf64: punkt wejscia poza programem (%x)\n",
               (uint32_t)eh->e_entry);
        return -1;
    }

    /* Program moze nie niesc kodu, ktorego uzywa, tylko puste miejsca
       na adresy funkcji z biblioteki wspoldzielonej. Trzeba je
       wypelnic TERAZ, przed pierwsza instrukcja - skok przez
       niewypelniona pozycje nie ma dokad prowadzic. */
    if (dyn_is_dynamic(file, size)) {
        if (!dyn_present()) {
            printk("[jadro] elf64: program potrzebuje biblioteki %s, "
                   "ktorej nie ma\n", dyn_name());
            return -1;
        }
        if (map_library() < 0 ||
            dyn_relocate(file, size, (void *)0, write_addr) < 0) {
            printk("[jadro] elf64: nie da sie zwiazac z %s\n", dyn_name());
            return -1;
        }
    }

    *entry = eh->e_entry;
    /* Sterta zaczyna sie za obrazem, na najblizszej granicy strony -
       nizej lezy pamiec programu, wyzej jest wolne az do stosu */
    if (brk_base)
        *brk_base = (top + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return 0;
}

uint64_t elf64_make_stack(void)
{
    for (int i = 1; i <= USER64_STACK_PAGES; i++) {
        uint64_t va = USER64_STACK_TOP - (uint64_t)i * PAGE_SIZE;
        /* Stos jest zapisywalny, ale nigdy wykonywalny */
        if (!page_frame_for(va, PG_USER | PG_WRITE | PG_NX))
            return 0;
    }
    /* Szesnaście bajtow zapasu: wierzcholek stosu ma byc wyrownany,
       a program ma gdzie odlozyc pierwsze wartosci */
    return USER64_STACK_TOP - 16;
}
