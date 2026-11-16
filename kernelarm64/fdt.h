#ifndef THREADOS_ARM64_FDT_H
#define THREADOS_ARM64_FDT_H

#include <stdint.h>

/* The flattened device tree - how an ARM board describes itself.
 *
 * A PC is found out by asking it: PCI configuration space, ACPI tables,
 * the BIOS memory map. An ARM board mostly cannot be asked; instead the
 * firmware (here QEMU) hands the kernel a tree of nodes - memory, cores,
 * the interrupt controller, every device and its addresses - in a
 * compact big-endian blob. What this kernel needs from it so far: */
struct fdt_info {
    uint64_t mem_base;       /* where the first memory range starts */
    uint64_t mem_total;      /* every memory range, added up */
    int cpus;                /* cores the board says it has */
    uint64_t cpu_id[8];      /* the first eight cores' MPIDRs, in order */
    int psci;                /* how to call the firmware: 1 hvc, 2 smc, 0 none */
    int gic_version;         /* the interrupt controller: 2, 3, 0 = none */
    uint64_t gic_dist;       /* its distributor */
    uint64_t gic_cpu;        /* its CPU interface (v2) */
    const char *bootargs;    /* /chosen/bootargs, inside the tree; or 0 */
};

/* Whether a valid tree header is at this address */
int fdt_valid(const void *blob);
/* Its total size in bytes, from the header */
uint32_t fdt_size(const void *blob);
/* Walks the tree, reading no more than 'room' bytes from its start -
   the header's own idea of the size is the tree's word and is checked
   against this, not trusted. 0 = read; -1 = it does not add up (larger
   than the room, an offset past its end, a name with no end, a
   property longer than its block). */
int fdt_scan(const void *blob, uint32_t room, struct fdt_info *out);

#endif
