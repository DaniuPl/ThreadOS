#ifndef THREADOS_MULTIBOOT_H
#define THREADOS_MULTIBOOT_H

#include <stdint.h>

/* The bootloader information structure (Multiboot 1).
   The fields above mmap_* are of no use to us yet, but they have to be
   present so that the offsets line up. */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;     /* KiB of memory below 1 MiB (flag bit 0) */
    uint32_t mem_upper;     /* KiB of memory above 1 MiB (flag bit 0) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;   /* memory map (flag bit 6) */
    uint32_t mmap_addr;
} __attribute__((packed));

#define MULTIBOOT_FLAG_MEM  (1 << 0)
#define MULTIBOOT_FLAG_CMDLINE (1 << 2)
#define MULTIBOOT_FLAG_MODS (1 << 3)
#define MULTIBOOT_FLAG_MMAP (1 << 6)

/* A module loaded by the bootloader (e.g. an initrd from QEMU's -initrd) */
struct multiboot_mod {
    uint32_t start;
    uint32_t end;
    uint32_t string;
    uint32_t reserved;
} __attribute__((packed));

/* A memory map entry. Note: the size field describes the size of the
   rest of the entry, so the next one lies at +size+4. */
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

#endif
