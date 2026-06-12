/* exec - ładowanie programów ELF z initrd
 *
 * Każdy proces dostaje własny katalog stron (mapowania jądra
 * współdzielone jako supervisor, segmenty programu i stos jako USER).
 * Loader działa w wątku jądra świeżo utworzonego zadania: buduje
 * przestrzeń adresową, kopiuje segmenty PT_LOAD do nowych ramek,
 * mapuje stos pod 0xC0000000 i skacze do ring 3 przez iret.
 */

#include <stdint.h>
#include "include/exec.h"
#include "include/initrd.h"
#include "include/paging.h"
#include "include/pmm.h"
#include "include/task.h"
#include "include/usermode.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/printk.h"

#define USER_STACK_TOP 0xC0000000u

/* Nagłówki ELF32 (tylko pola, których używamy) */
struct elf_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

#define ET_EXEC  2
#define EM_386   3
#define PT_LOAD  1

static int elf_valid(const struct elf_ehdr *eh, uint32_t size)
{
    if (size < sizeof(*eh))
        return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 1)            /* ELFCLASS32 */
        return 0;
    return eh->e_type == ET_EXEC && eh->e_machine == EM_386 && eh->e_phnum;
}

/* Mapuje (lub odnajduje) ramkę dla strony vaddr procesu */
static uint32_t page_frame_for(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = paging_get_mapping(dir, vaddr);
    if (frame)
        return frame;
    frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    memset((void *)frame, 0, PAGE_SIZE);
    if (!paging_map_user_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

static int load_segment(uint32_t dir, const uint8_t *file,
                        const struct elf_phdr *ph)
{
    if (ph->p_vaddr < PAGING_IDENTITY_LIMIT ||
        ph->p_vaddr + ph->p_memsz > USER_STACK_TOP - PAGE_SIZE) {
        printk("exec: segment poza dozwolonym zakresem (%x)\n", ph->p_vaddr);
        return 0;
    }

    uint32_t file_end = ph->p_vaddr + ph->p_filesz;

    for (uint32_t va = ph->p_vaddr & ~0xFFFu;
         va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
        uint32_t frame = page_frame_for(dir, va);
        if (!frame)
            return 0;

        /* Skopiuj fragment pliku przypadający na tę stronę
           (reszta zostaje wyzerowana - to .bss) */
        uint32_t copy_from = va > ph->p_vaddr ? va : ph->p_vaddr;
        if (copy_from < file_end) {
            uint32_t copy_to = va + PAGE_SIZE < file_end
                                   ? va + PAGE_SIZE : file_end;
            memcpy((void *)(frame + (copy_from - va)),
                   file + ph->p_offset + (copy_from - ph->p_vaddr),
                   copy_to - copy_from);
        }
    }
    return 1;
}

/* Wątek jądra nowego procesu: zbuduj przestrzeń adresową i wejdź w ring 3 */
static void exec_task(void *arg)
{
    char *name = arg;
    uint32_t size;
    const uint8_t *file = initrd_find(name, &size);
    if (!file) {
        printk("exec: '%s' zniknal z initrd?\n", name);
        kfree(name);
        return;
    }

    const struct elf_ehdr *eh = (const struct elf_ehdr *)file;
    if (!elf_valid(eh, size)) {
        printk("exec: '%s' nie jest programem ELF32 i386\n", name);
        kfree(name);
        return;
    }

    uint32_t dir = paging_new_address_space();
    if (!dir) {
        printk("exec: brak pamieci na katalog stron\n");
        kfree(name);
        return;
    }

    const struct elf_phdr *ph =
        (const struct elf_phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (!load_segment(dir, file, &ph[i])) {
            paging_destroy_address_space(dir);
            kfree(name);
            return;
        }
    }

    if (!page_frame_for(dir, USER_STACK_TOP - PAGE_SIZE)) {
        printk("exec: brak pamieci na stos\n");
        paging_destroy_address_space(dir);
        kfree(name);
        return;
    }

    printk("[jadro] exec '%s': ELF %u B, entry %x\n", name, size, eh->e_entry);
    uint32_t entry = eh->e_entry;
    kfree(name);

    /* Od teraz to zadanie ma wlasna przestrzen adresowa; jej ramki
       zostana zwolnione przy sprzataniu zombie */
    task_use_address_space(dir);
    enter_ring3(entry, USER_STACK_TOP);
}

int exec_spawn(const char *name)
{
    if (!initrd_find(name, (void *)0)) {
        printk("run: brak pliku '%s' w initrd (sprawdz 'ls')\n", name);
        return -1;
    }

    uint32_t len = strlen(name) + 1;
    char *copy = kmalloc(len);
    memcpy(copy, name, len);
    task_create(name, exec_task, copy);
    return 0;
}
