/* PMM - zarządzanie pamięcią fizyczną
 *
 * Bitmapa ramek 4 KiB: bit ustawiony = ramka zajęta. Na podstawie mapy
 * pamięci z Multiboot oznaczamy dostępne obszary jako wolne, po czym
 * rezerwujemy pierwszy 1 MiB (BIOS, VGA) i obraz jądra.
 *
 * Zarządzamy maksymalnie pierwszymi 64 MiB - tyle stronicowanie mapuje
 * identycznościowo (zob. paging.c). Pamięć powyżej zostanie podpięta,
 * gdy powstanie pełny menedżer pamięci wirtualnej.
 */

#include "include/pmm.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/io.h"

#define MANAGED_LIMIT (64u * 1024 * 1024)
#define MAX_FRAMES    (MANAGED_LIMIT / PAGE_SIZE)

/* Symbole z linker.ld wyznaczające obraz jądra w pamięci */
extern char _kernel_start[], _kernel_end[];

static uint32_t bitmap[MAX_FRAMES / 32];
static uint32_t total_frames;   /* ramki dostępne dla PMM */
static uint32_t used_frames;
static uint32_t ram_kib;        /* cała wykryta pamięć */

static void frame_set(uint32_t frame)   { bitmap[frame / 32] |=  (1u << (frame % 32)); }
static void frame_clear(uint32_t frame) { bitmap[frame / 32] &= ~(1u << (frame % 32)); }
static int  frame_test(uint32_t frame)  { return bitmap[frame / 32] & (1u << (frame % 32)); }

static void reserve_range(uint32_t start, uint32_t end)
{
    for (uint32_t f = start / PAGE_SIZE; f < (end + PAGE_SIZE - 1) / PAGE_SIZE; f++) {
        if (f >= MAX_FRAMES)
            break;
        if (!frame_test(f)) {
            frame_set(f);
            used_frames++;
        }
    }
}

void pmm_init(const struct multiboot_info *mbi)
{
    /* Wszystko zajęte, dopóki mapa pamięci nie powie inaczej */
    memset(bitmap, 0xFF, sizeof(bitmap));

    if (!(mbi->flags & MULTIBOOT_FLAG_MMAP)) {
        printk("PMM: brak mapy pamieci od bootloadera!\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    uint32_t entry_addr = mbi->mmap_addr;
    uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;

    while (entry_addr < mmap_end) {
        const struct multiboot_mmap_entry *e =
            (const struct multiboot_mmap_entry *)entry_addr;

        if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
            ram_kib += (uint32_t)(e->len / 1024);

            uint64_t start = e->addr;
            uint64_t end = e->addr + e->len;
            if (start < MANAGED_LIMIT) {
                if (end > MANAGED_LIMIT)
                    end = MANAGED_LIMIT;
                /* tylko całe ramki w obrębie obszaru */
                uint32_t f_start = ((uint32_t)start + PAGE_SIZE - 1) / PAGE_SIZE;
                uint32_t f_end = (uint32_t)(end / PAGE_SIZE);
                for (uint32_t f = f_start; f < f_end; f++) {
                    frame_clear(f);
                    total_frames++;
                }
            }
        }
        entry_addr += e->size + 4;
    }

    used_frames = 0;

    /* Pierwszy 1 MiB: wektory BIOS, bufor VGA, struktury bootloadera */
    reserve_range(0, 1024 * 1024);
    /* Obraz jądra (kod + dane + bss) */
    reserve_range((uint32_t)_kernel_start, (uint32_t)_kernel_end);

    /* Moduły bootloadera (initrd) - nie wolno ich nadpisać */
    if (mbi->flags & MULTIBOOT_FLAG_MODS) {
        const struct multiboot_mod *mods =
            (const struct multiboot_mod *)mbi->mods_addr;
        reserve_range(mbi->mods_addr,
                      mbi->mods_addr + mbi->mods_count * sizeof(*mods));
        for (uint32_t i = 0; i < mbi->mods_count; i++)
            reserve_range(mods[i].start, mods[i].end);
    }
}

uint32_t pmm_alloc_frame(void)
{
    return pmm_alloc_contiguous(1);
}

/* Alokacje i zwalnianie biegna tez z kontekstu IRQ (exec, sprzatanie
   przestrzeni adresowych zombie) - stad sekcje krytyczne. */

uint32_t pmm_alloc_contiguous(uint32_t n)
{
    uint32_t flags = irq_save();
    uint32_t run = 0;
    for (uint32_t f = 0; f < MAX_FRAMES; f++) {
        if (frame_test(f)) {
            run = 0;
            continue;
        }
        if (++run == n) {
            uint32_t first = f - n + 1;
            for (uint32_t i = first; i <= f; i++)
                frame_set(i);
            used_frames += n;
            irq_restore(flags);
            return first * PAGE_SIZE;
        }
    }
    irq_restore(flags);
    return 0;
}

void pmm_free_frame(uint32_t addr)
{
    uint32_t flags = irq_save();
    uint32_t frame = addr / PAGE_SIZE;
    if (frame < MAX_FRAMES && frame_test(frame)) {
        frame_clear(frame);
        used_frames--;
    }
    irq_restore(flags);
}

uint32_t pmm_total_frames(void) { return total_frames; }
uint32_t pmm_used_frames(void)  { return used_frames; }
uint32_t pmm_ram_kib(void)      { return ram_kib; }
