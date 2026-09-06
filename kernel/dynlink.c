/* The shared library: symbol lookup and relocations
 *
 * This whole file is ELF reading - and of two of them at once. From
 * the library it takes the dynamic symbol table: a function name ->
 * the address that function lies at. From the program it takes the
 * relocation list: the address of an empty slot in its jump table ->
 * the name of the function that belongs there. Putting one together
 * with the other is the whole of binding.
 *
 * The library is linked at a FIXED address (USER_LIB_BASE) rather than
 * an arbitrary one. That simplifies the matter by a whole layer: the
 * addresses in its symbol table are already final, so the library
 * itself has no relocations to settle. There are two prices for this
 * and both are acceptable: there can be only one library and it is
 * always visible at the same address. A real system does not do this,
 * because it wants many of them and wants to randomise the addresses -
 * but a real system has a linker in user space for that.
 *
 * ELF structures differ between processor modes in field width but not
 * in layout - hence one set of names and an #ifdef over the
 * definitions.
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

/* --- ELF headers ------------------------------------------------------ */

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

/* Relocations with addends - long mode uses only these */
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

/* Relocations without addends - i386 uses only these */
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

/* --- state ------------------------------------------------------------ */

#define SEGMENTS  4
#define PAGES_MAX  128                  /* half a megabyte of library is enough */

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

/* Frames of the library's read-only pages - one set for the whole system */
static dyn_addr shared_vaddr[PAGES_MAX];
static uint32_t shared_frame[PAGES_MAX];
static uint32_t shared_pages;

static int ready;

/* --- reading the ELF --------------------------------------------------- */

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
    /* The section table has to fit inside the file - without it we will
       find neither symbols nor relocations, and rubbish cannot be
       trusted */
    if (!eh->e_shnum || eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize
                        > size)
        return 0;
    return 1;
}

/* --- the library ------------------------------------------------------ */

int dyn_present(void)
{
    return ready;
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

/* The frames of the read-only segments are created ONCE and live until
   the system stops - every process gets the same ones. We fill them in
   straight away, so that there is no need to remember for every
   process where to get the content from. */
static int setup_frames(void)
{
    for (int s = 0; s < nsegments; s++) {
        if (segmenty[s].right & DYN_WRITE)
            continue;                   /* everyone gets their own writable data */
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

            /* Copy the part of the file that falls on this page */
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
        return 0;                       /* there is none - programs carry their own */

    const uint8_t *image = file;
    if (!elf_sane(image, size, ET_DYN)) {
        printk("[kernel] %s is not a shared library\n", LIB_FILE);
        return 0;
    }

    const struct ehdr *eh = (const struct ehdr *)image;
    const struct phdr *ph = (const struct phdr *)(image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum && nsegments < SEGMENTS; i++) {
        if (ph[i].p_type != PT_LOAD || !ph[i].p_memsz)
            continue;
        /* The library is linked at a fixed address; had it landed
           somewhere else, its own references would point into thin
           air - better to say so now */
        if (ph[i].p_vaddr < LIB_BASE) {
            printk("[kernel] %s has a segment at %x - outside the area "
                   "the library\n", LIB_FILE, (uint32_t)ph[i].p_vaddr);
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

    /* The dynamic symbol table - it is the one that answers the
       question "where does the function with this name lie" */
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
        printk("[kernel] %s has no dynamic symbol table\n", LIB_FILE);
        return 0;
    }

    /* The library must have NOTHING left to resolve in itself.
     *
     * Calls between its own functions are bound at link time
     * (-Bsymbolic); if one of them were left to be resolved at load
     * time, it would jump through the library's own jump table, which
     * here has nobody to fill it in - and the program would fall into
     * the void on first use.
     *
     * Relocations that shift an address by the library's position
     * (RELATIVE) are something else and are harmless: the library lies
     * exactly where it was linked, so the shift is zero, and the slots
     * themselves were already filled in with the final value by the
     * linker. */
    for (int i = 0; i < eh->e_shnum; i++) {
        const struct shdr *sh = section(image, i);
        if (sh->sh_type != SHT_REL_OURS || !sh->sh_entsize)
            continue;
        uint32_t count = (uint32_t)(sh->sh_size / sh->sh_entsize);
        const struct rel *r = (const struct rel *)(image + sh->sh_offset);
        for (uint32_t k = 0; k < count; k++) {
            uint32_t typ = REL_TYPE(r[k].r_info);
            if (typ == R_JUMP || typ == R_DATA) {
                printk("[kernel] %s has unresolved references of its own - "
                       "-Bsymbolic is missing at link time\n", LIB_FILE);
                nsegments = 0;
                return 0;
            }
        }
    }

    lib_image = image;
    lib_size = size;
    if (setup_frames() < 0) {
        printk("[kernel] no memory for the library %s\n", LIB_FILE);
        nsegments = 0;
        return 0;
    }

    ready = 1;
    return 1;
}

/* The address of a function with a given name in the library; 0 when
   there is none */
static dyn_addr find(const char *name)
{
    for (uint32_t i = 1; i < lib_nsyms; i++) {
        if (!lib_sym[i].st_shndx)       /* an undefined symbol - not ours */
            continue;
        if (!strcmp(lib_str + lib_sym[i].st_name, name))
            return (dyn_addr)lib_sym[i].st_value;
    }
    return 0;
}

/* --- the program's relocations ----------------------------------------- */

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
    if (!ready || !elf_sane(image, size, ET_EXEC))
        return -1;

    const struct ehdr *eh = (const struct ehdr *)image;
    for (int i = 0; i < eh->e_shnum; i++) {
        const struct shdr *sh = section(image, i);
        if (sh->sh_type != SHT_REL_OURS || !sh->sh_entsize)
            continue;

        /* Every relocation table points at its own symbol table, and
           that at its own string table - nothing has to be guessed */
        const struct shdr *ss = section(image, (int)sh->sh_link);
        if (ss->sh_type != SHT_DYNSYM || !ss->sh_entsize)
            continue;
        const struct sym *syms_tab = (const struct sym *)(image + ss->sh_offset);
        const struct shdr *st = section(image, (int)ss->sh_link);
        const char *texts = (const char *)(image + st->sh_offset);

        uint32_t count = (uint32_t)(sh->sh_size / sh->sh_entsize);
        const struct rel *r = (const struct rel *)(image + sh->sh_offset);
        for (uint32_t k = 0; k < count; k++) {
            uint32_t typ = REL_TYPE(r[k].r_info);
            uint32_t nr = REL_SYM(r[k].r_info);
            if (typ != R_JUMP && typ != R_DATA) {
                if (typ == R_COPY) {
                    /* Copying DATA from the library into the program.
                       The library deliberately exports no variables -
                       if one ended up there, every process would have
                       a copy of its own and nobody would know. */
                    printk("[kernel] a copy relocation for the data '%s' - "
                           "a library is to expose functions only\n",
                           texts + syms_tab[nr].st_name);
                    return -1;
                }
                continue;               /* none of our business */
            }
            if (nr >= (uint32_t)(ss->sh_size / ss->sh_entsize))
                return -1;

            const char *name = texts + syms_tab[nr].st_name;
            dyn_addr dst = find(name);
            if (!dst) {
                printk("[kernel] no symbol '%s' in %s\n", name, LIB_FILE);
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
