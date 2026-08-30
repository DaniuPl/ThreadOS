/* Loading ELF64 programs from the initrd
 *
 * The program that has run in ring 3 so far lay hard-coded in the
 * kernel image - a dozen or so bytes in the .rodata section. That
 * proved the switch to ring 3 works, but nothing more: a system that
 * can only run a piece of itself does not run programs.
 *
 * Here the program is already a separate file: built separately, put
 * into the initrd and read by the kernel only after startup. The
 * kernel knows nothing about it beyond what it reads from the ELF
 * header.
 */

#include "elf64.h"
#include "paging64.h"

/* A process's address space is private from the one-gigabyte
   boundary - and that boundary is at the same time the split of the
   second-level tables. The two numbers have to agree. */
_Static_assert((USER64_BASE >> 30) == PAGING64_USER_PDPT,
               "USER64_BASE musi lezec w gigabajcie PAGING64_USER_PDPT");
#include "../kernel/include/pmm.h"
#include "../kernel/include/dynlink.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

/* ELF64 headers - only the fields we really use. Compared to ELF32
   more than the size of addresses changed: in the segment header the
   access-permission field moved to just after the type, so that the
   sixty-four-bit addresses would lie aligned. */
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

/* A frame for a program page: either it is already there (two
   segments can reach the same page) or we take a new one and zero
   it - thanks to which .bss does not have to be filled in
   separately. */
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

/* --- a shared library in the process's space ------------------------
 *
 * Segments with no write permission (code and constants) get SHARED
 * FRAMES: the same ones every other process sees - and that is the
 * whole saving. The data segment is different and has to be: the
 * library keeps its state in it, and one process's state must not be
 * visible in another.
 */
static int map_library(void)
{
    uint64_t vaddr;
    uint32_t memsz, filesz;
    int right;
    const uint8_t *data;

    for (int i = 0; dyn_segment(i, &vaddr, &memsz, &filesz, &right, &data);
         i++) {
        /* The permissions are supplied by the ELF itself - just as with the
           program's segments. So the library's data is not executable
           and its code is not writable, and the processor sees to
           that. */
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
                /* The frame is to outlive the process's death - it belongs to the
   system */
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

/* Writes the resolved address into the program's jump table. The
   process's space is already active, so we write straight to it. */
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
        ph->p_vaddr + ph->p_memsz < ph->p_vaddr ||     /* overflow */
        ph->p_vaddr + ph->p_memsz > USER64_TOP) {
        printk("[jadro] elf64: segment poza zakresem uzytkownika (%x)\n",
               (uint32_t)ph->p_vaddr);
        return -1;
    }

    /* The permissions are supplied by the file itself. A missing
       "executable" bit means execution is forbidden - and it is the
       processor that enforces that, not us. */
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

    /* A program may not carry the code it uses, only empty slots for
       the addresses of functions from a shared library. They have to be
       filled in NOW, before the first instruction - a jump through an
       unfilled slot has nowhere to lead. */
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
    /* The heap starts past the image, at the nearest page boundary -
       below lies the program's memory, above it is free all the way up
       to the stack */
    if (brk_base)
        *brk_base = (top + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    return 0;
}

uint64_t elf64_make_stack(void)
{
    for (int i = 1; i <= USER64_STACK_PAGES; i++) {
        uint64_t va = USER64_STACK_TOP - (uint64_t)i * PAGE_SIZE;
        /* The stack is writable but never executable */
        if (!page_frame_for(va, PG_USER | PG_WRITE | PG_NX))
            return 0;
    }
    /* Sixteen bytes of slack: the stack top is to be aligned and the
       program is to have somewhere to put its first values */
    return USER64_STACK_TOP - 16;
}
