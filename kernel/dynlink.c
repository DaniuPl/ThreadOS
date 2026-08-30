/* Biblioteka wspoldzielona: wyszukiwanie symboli i relokacje
 *
 * Caly ten plik jest czytaniem ELF-a - i to dwoch naraz. Z biblioteki
 * bierze tablice symboli dynamicznych: nazwa funkcji -> adres, pod
 * ktorym ta funkcja lezy. Z programu bierze liste relokacji: adres
 * pustego miejsca w jego tablicy skokow -> nazwa funkcji, ktora ma sie
 * tam znalezc. Zlozenie jednego z drugim to jest cale wiazanie.
 *
 * Biblioteka jest konsolidowana pod STALYM adresem (USER_LIB_BASE),
 * a nie dowolnym. To upraszcza rzecz o cala warstwe: adresy w jej
 * tablicy symboli sa juz ostateczne, wiec ona sama nie ma zadnych
 * relokacji do zalatwienia. Ceny za to sa dwie i obie akceptowalne:
 * biblioteka moze byc tylko jedna i zawsze widac ja pod tym samym
 * adresem. Prawdziwy system tak nie robi, bo chce ich wiele i chce
 * losowac adresy - ale prawdziwy system ma na to konsolidator
 * w przestrzeni uzytkownika.
 *
 * Struktury ELF-a roznia sie miedzy trybami procesora szerokoscia pol,
 * ale nie ukladem - stad jeden zestaw nazw i #ifdef nad definicjami.
 */

#include <stdint.h>
#include "include/dynlink.h"
#include "include/initrd.h"
#include "include/pmm.h"
#include "include/printk.h"
#include "include/string.h"

#ifdef __x86_64__
#include "../kernel64/elf64.h"
#define LIB_BASE   USER64_LIB_BASE
#else
#include "include/exec.h"
#define LIB_BASE   USER_LIB_BASE
#endif

#define LIB_FILE   "libproton.so"

/* --- naglowki ELF ---------------------------------------------------- */

#ifdef __x86_64__
struct ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

struct shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} __attribute__((packed));

struct sym {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} __attribute__((packed));

/* Relokacje z dopelnieniem - tryb dlugi uzywa tylko takich */
struct rel {
    uint64_t r_offset, r_info;
    int64_t  r_addend;
} __attribute__((packed));

#define REL_SYM(i)   ((uint32_t)((i) >> 32))
#define REL_TYPE(i)   ((uint32_t)((i) & 0xFFFFFFFFu))
#define SHT_REL_OURS 4                  /* SHT_RELA */
#define R_JUMP       7                  /* R_X86_64_JUMP_SLOT */
#define R_DATA       6                  /* R_X86_64_GLOB_DAT */
#define R_COPY      5                  /* R_X86_64_COPY */
#define EM_OURS      62                 /* EM_X86_64 */
#else
struct ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

struct shdr {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
} __attribute__((packed));

struct sym {
    uint32_t st_name, st_value, st_size;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
} __attribute__((packed));

/* Relokacje bez dopelnienia - i386 uzywa tylko takich */
struct rel {
    uint32_t r_offset, r_info;
} __attribute__((packed));

#define REL_SYM(i)   ((i) >> 8)
#define REL_TYPE(i)   ((i) & 0xFFu)
#define SHT_REL_OURS 9                  /* SHT_REL */
#define R_JUMP       7                  /* R_386_JUMP_SLOT */
#define R_DATA       6                  /* R_386_GLOB_DAT */
#define R_COPY      5                  /* R_386_COPY */
#define EM_OURS      3                  /* EM_386 */
#endif

#define ET_EXEC   2
#define ET_DYN    3
#define PT_LOAD   1
#define PF_W      2
#define PF_X      1
#define SHT_DYNSYM 11

/* --- stan ------------------------------------------------------------ */

#define SEGMENTS  4
#define PAGES_MAX  128                  /* pol megabajta biblioteki wystarczy */

struct segment {
    dyn_addr vaddr;
    uint32_t memsz, filesz;
    int right;
    const uint8_t *data;
};

static const uint8_t *lib_image;
static uint32_t lib_size;
static const struct sym *lib_sym;
static uint32_t lib_nsyms;
static const char *lib_str;
static struct segment segmenty[SEGMENTS];
static int nsegments;

/* Ramki stron biblioteki tylko do odczytu - jedne dla calego systemu */
static dyn_addr shared_vaddr[PAGES_MAX];
static uint32_t shared_frame[PAGES_MAX];
static uint32_t shared_pages;

static int gotowa;

/* --- czytanie ELF ---------------------------------------------------- */

static const struct shdr *section(const uint8_t *image, int i)
{
    const struct ehdr *eh = (const struct ehdr *)image;
    return (const struct shdr *)(image + eh->e_shoff +
                                 (uint32_t)i * eh->e_shentsize);
}

static int elf_sane(const uint8_t *image, uint32_t size, int typ)
{
    if (size < sizeof(struct ehdr))
        return 0;
    const struct ehdr *eh = (const struct ehdr *)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
#ifdef __x86_64__
    if (eh->e_ident[4] != 2)            /* ELFCLASS64 */
        return 0;
#else
    if (eh->e_ident[4] != 1)            /* ELFCLASS32 */
        return 0;
#endif
    if (eh->e_machine != EM_OURS || eh->e_type != typ)
        return 0;
    /* Tablica sekcji musi miescic sie w pliku - bez niej nie znajdziemy
       ani symboli, ani relokacji, a smieciom nie mozna wierzyc */
    if (!eh->e_shnum || eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize
                        > size)
        return 0;
    return 1;
}

/* --- biblioteka ------------------------------------------------------ */

int dyn_present(void)
{
    return gotowa;
}

const char *dyn_name(void)
{
    return LIB_FILE;
}

uint32_t dyn_pages(void)
{
    return shared_pages;
}

int dyn_segment(int i, dyn_addr *vaddr, uint32_t *memsz, uint32_t *filesz,
                int *right, const uint8_t **data)
{
    if (i < 0 || i >= nsegments)
        return 0;
    *vaddr = segmenty[i].vaddr;
    *memsz = segmenty[i].memsz;
    *filesz = segmenty[i].filesz;
    *right = segmenty[i].right;
    *data = segmenty[i].data;
    return 1;
}

uint32_t dyn_frame(dyn_addr vaddr)
{
    for (uint32_t i = 0; i < shared_pages; i++)
        if (shared_vaddr[i] == vaddr)
            return shared_frame[i];
    return 0;
}

/* Ramki segmentow tylko do odczytu powstaja RAZ i zyja do konca pracy
   systemu - kazdy proces dostaje te same. Wypelniamy je od razu, zeby
   nie trzeba bylo pamietac przy kazdym procesie, skad wziac tresc. */
static int setup_frames(void)
{
    for (int s = 0; s < nsegments; s++) {
        if (segmenty[s].right & DYN_WRITE)
            continue;                   /* dane pisemne dostaje kazdy swoje */
        dyn_addr start = segmenty[s].vaddr & ~(dyn_addr)0xFFF;
        dyn_addr end = segmenty[s].vaddr + segmenty[s].memsz;
        for (dyn_addr va = start; va < end; va += 4096) {
            if (dyn_frame(va))
                continue;
            if (shared_pages >= PAGES_MAX)
                return -1;
            uint32_t frame = pmm_alloc_frame();
            if (!frame)
                return -1;
            memset((void *)(uintptr_t)frame, 0, 4096);

            /* Przepisz kawalek pliku przypadajacy na te strone */
            dyn_addr od = va > segmenty[s].vaddr ? va : segmenty[s].vaddr;
            dyn_addr file_to = segmenty[s].vaddr + segmenty[s].filesz;
            if (od < file_to) {
                dyn_addr do_ = va + 4096 < file_to ? va + 4096 : file_to;
                memcpy((void *)(uintptr_t)(frame + (uint32_t)(od - va)),
                       segmenty[s].data + (uint32_t)(od - segmenty[s].vaddr),
                       (uint32_t)(do_ - od));
            }
            shared_vaddr[shared_pages] = va;
            shared_frame[shared_pages] = frame;
            shared_pages++;
        }
    }
    return 0;
}

int dyn_init(void)
{
    uint32_t size = 0;
    const void *file = initrd_find(LIB_FILE, &size);
    if (!file)
        return 0;                       /* nie ma - programy niosa swoje */

    const uint8_t *image = file;
    if (!elf_sane(image, size, ET_DYN)) {
        printk("[jadro] %s nie jest biblioteka wspoldzielona\n", LIB_FILE);
        return 0;
    }

    const struct ehdr *eh = (const struct ehdr *)image;
    const struct phdr *ph = (const struct phdr *)(image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum && nsegments < SEGMENTS; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz)
            continue;
        /* Biblioteka jest konsolidowana pod stalym adresem; gdyby
           trafila gdzie indziej, jej wlasne odwolania wskazywalyby
           w prozne miejsce - lepiej powiedziec to teraz */
        if (ph[i].p_vaddr < LIB_BASE) {
            printk("[jadro] %s ma segment pod %x - poza obszarem "
                   "biblioteki\n", LIB_FILE, (uint32_t)ph[i].p_vaddr);
            return 0;
        }
        segmenty[nsegments].vaddr = (dyn_addr)ph[i].p_vaddr;
        segmenty[nsegments].memsz = (uint32_t)ph[i].p_memsz;
        segmenty[nsegments].filesz = (uint32_t)ph[i].p_filesz;
        segmenty[nsegments].right =
            ((ph[i].p_flags & PF_W) ? DYN_WRITE : 0) |
            ((ph[i].p_flags & PF_X) ? DYN_EXEC : 0);
        segmenty[nsegments].data = image + ph[i].p_offset;
        nsegments++;
    }
    if (!nsegments)
        return 0;

    /* Tablica symboli dynamicznych - to ona odpowiada na pytanie
       "gdzie lezy funkcja o tej nazwie" */
    for (int i = 0; i < eh->e_shnum; i++) {
        const struct shdr *sh = section(image, i);
        if (sh->sh_type != SHT_DYNSYM || !sh->sh_entsize)
            continue;
        lib_sym = (const struct sym *)(image + sh->sh_offset);
        lib_nsyms = (uint32_t)(sh->sh_size / sh->sh_entsize);
        const struct shdr *str = section(image, (int)sh->sh_link);
        lib_str = (const char *)(image + str->sh_offset);
        break;
    }
    if (!lib_sym || !lib_str) {
        printk("[jadro] %s nie ma tablicy symboli dynamicznych\n", LIB_FILE);
        return 0;
    }

    /* Biblioteka nie moze miec NIC do rozwiazania u siebie.
     *
     * Wywolania miedzy jej wlasnymi funkcjami wiaza sie przy
     * konsolidacji (-Bsymbolic); gdyby ktores zostalo do rozwiazania
     * przy ladowaniu, skakaloby przez jej wlasna tablice skokow, ktora
     * u nas nie ma komu wypelnic - i program wpadlby w pustke przy
     * pierwszym uzyciu.
     *
     * Relokacje przesuwajace adres o polozenie biblioteki (RELATIVE)
     * sa czym innym i sa nieszkodliwe: biblioteka lezy dokladnie tam,
     * gdzie ja skonsolidowano, wiec przesuniecie wynosi zero, a same
     * miejsca konsolidator wypelnil juz gotowa wartoscia. */
    for (int i = 0; i < eh->e_shnum; i++) {
        const struct shdr *sh = section(image, i);
        if (sh->sh_type != SHT_REL_OURS || !sh->sh_entsize)
            continue;
        uint32_t ile = (uint32_t)(sh->sh_size / sh->sh_entsize);
        const struct rel *r = (const struct rel *)(image + sh->sh_offset);
        for (uint32_t k = 0; k < ile; k++) {
            uint32_t typ = REL_TYPE(r[k].r_info);
            if (typ == R_JUMP || typ == R_DATA) {
                printk("[jadro] %s ma nierozwiazane wlasne odwolania - "
                       "brakuje -Bsymbolic przy konsolidacji\n", LIB_FILE);
                nsegments = 0;
                return 0;
            }
        }
    }

    lib_image = image;
    lib_size = size;
    if (setup_frames() < 0) {
        printk("[jadro] brak pamieci na biblioteke %s\n", LIB_FILE);
        nsegments = 0;
        return 0;
    }

    gotowa = 1;
    return 1;
}

/* Adres funkcji o danej nazwie w bibliotece; 0 gdy nie ma takiej */
static dyn_addr find(const char *name)
{
    for (uint32_t i = 1; i < lib_nsyms; i++) {
        if (!lib_sym[i].st_shndx)       /* symbol nieokreslony - nie nasz */
            continue;
        if (!strcmp(lib_str + lib_sym[i].st_name, name))
            return (dyn_addr)lib_sym[i].st_value;
    }
    return 0;
}

/* --- relokacje programu ---------------------------------------------- */

int dyn_is_dynamic(const uint8_t *image, uint32_t size)
{
    if (!elf_sane(image, size, ET_EXEC))
        return 0;
    const struct ehdr *eh = (const struct ehdr *)image;
    for (int i = 0; i < eh->e_shnum; i++)
        if (section(image, i)->sh_type == SHT_REL_OURS)
            return 1;
    return 0;
}

int dyn_relocate(const uint8_t *image, uint32_t size, void *ctx,
                int (*store)(void *ctx, dyn_addr addr, dyn_addr value))
{
    if (!gotowa || !elf_sane(image, size, ET_EXEC))
        return -1;

    const struct ehdr *eh = (const struct ehdr *)image;
    for (int i = 0; i < eh->e_shnum; i++) {
        const struct shdr *sh = section(image, i);
        if (sh->sh_type != SHT_REL_OURS || !sh->sh_entsize)
            continue;

        /* Kazda tablica relokacji wskazuje swoja tablice symboli,
           a ta swoja tablice napisow - nie trzeba niczego zgadywac */
        const struct shdr *ss = section(image, (int)sh->sh_link);
        if (ss->sh_type != SHT_DYNSYM || !ss->sh_entsize)
            continue;
        const struct sym *syms_tab = (const struct sym *)(image + ss->sh_offset);
        const struct shdr *st = section(image, (int)ss->sh_link);
        const char *texts = (const char *)(image + st->sh_offset);

        uint32_t ile = (uint32_t)(sh->sh_size / sh->sh_entsize);
        const struct rel *r = (const struct rel *)(image + sh->sh_offset);
        for (uint32_t k = 0; k < ile; k++) {
            uint32_t typ = REL_TYPE(r[k].r_info);
            uint32_t nr = REL_SYM(r[k].r_info);
            if (typ != R_JUMP && typ != R_DATA) {
                if (typ == R_COPY) {
                    /* Kopiowanie DANYCH z biblioteki do programu.
                       Biblioteka celowo nie wystawia zmiennych - gdyby
                       ktoras sie tam znalazla, kazdy proces mialby
                       wlasna kopie i nikt by o tym nie wiedzial. */
                    printk("[jadro] relokacja kopiujaca dane '%s' - "
                           "biblioteka ma wystawiac tylko funkcje\n",
                           texts + syms_tab[nr].st_name);
                    return -1;
                }
                continue;               /* nie nasza sprawa */
            }
            if (nr >= (uint32_t)(ss->sh_size / ss->sh_entsize))
                return -1;

            const char *name = texts + syms_tab[nr].st_name;
            dyn_addr dst = find(name);
            if (!dst) {
                printk("[jadro] brak symbolu '%s' w %s\n", name, LIB_FILE);
                return -1;
            }
            if (store(ctx, (dyn_addr)r[k].r_offset, dst) < 0)
                return -1;
        }
    }
    (void)lib_size;
    (void)lib_image;
    return 0;
}
