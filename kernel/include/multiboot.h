#ifndef PROTONOS_MULTIBOOT_H
#define PROTONOS_MULTIBOOT_H

#include <stdint.h>

/* Struktura informacji od bootloadera (Multiboot 1).
   Pola powyżej mmap_* nie są nam na razie potrzebne, ale muszą
   być obecne, żeby offsety się zgadzały. */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB pamięci poniżej 1 MiB (flaga bit 0) */
    uint32_t mem_upper;     /* KiB pamięci powyżej 1 MiB (flaga bit 0) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;   /* mapa pamięci (flaga bit 6) */
    uint32_t mmap_addr;
} __attribute__((packed));

#define MULTIBOOT_FLAG_MEM  (1 << 0)
#define MULTIBOOT_FLAG_MMAP (1 << 6)

/* Wpis mapy pamięci. Uwaga: pole size opisuje rozmiar reszty wpisu,
   więc następny wpis leży pod adresem +size+4. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif
