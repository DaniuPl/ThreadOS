/* Reading the flattened device tree
 *
 * The blob is a header, a block of structure and a block of strings.
 * The structure is a stream of 32-bit big-endian tokens: BEGIN_NODE
 * (followed by the node's name), PROP (a length, an offset of its name
 * in the strings block, and the value), END_NODE, and END. Nothing in
 * it says where a node is - like a HID descriptor, it has to be walked.
 *
 * Every read is checked against the blocks the header declares: the
 * tree is handed over by firmware, and on a real board that firmware
 * is somebody else's.
 */

#include <stdint.h>
#include "fdt.h"
#include "../kernel/include/string.h"

#define FDT_MAGIC      0xD00DFEEDu
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

#define MAX_DEPTH 32

/* Byte by byte: the MMU is still off, all memory is Device memory, and
   there an access that is not aligned to its size is a fault */
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

int fdt_valid(const void *blob)
{
    return be32(blob) == FDT_MAGIC;
}

uint32_t fdt_size(const void *blob)
{
    return be32((const uint8_t *)blob + 4);
}

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

/* A value of 'cells' 32-bit cells, as one number */
static uint64_t cells_value(const uint8_t *p, uint32_t cells)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++)
        v = v << 32 | be32(p + 4 * i);
    return v;
}

/* Whether a string list (a 'compatible' value: strings one after
   another, each with its NUL) holds this one */
static int list_has(const uint8_t *val, uint32_t len, const char *want)
{
    uint32_t i = 0;
    while (i < len) {
        uint32_t start = i;
        while (i < len && val[i])
            i++;
        if (i == len)
            return 0;                     /* the last one has no end */
        if (strcmp((const char *)val + start, want) == 0)
            return 1;
        i++;
    }
    return 0;
}

/* The other devices this kernel looks for in the tree */
enum { DEV_OTHER, DEV_FWCFG, DEV_PL031, DEV_VIRTIO };

/* Which GIC a 'compatible' names: 2, 3, or 0 for something else */
static int gic_kind(const uint8_t *val, uint32_t len)
{
    if (list_has(val, len, "arm,cortex-a15-gic") ||
        list_has(val, len, "arm,gic-400"))
        return 2;
    if (list_has(val, len, "arm,gic-v3"))
        return 3;
    return 0;
}

int fdt_scan(const void *blob, uint32_t room, struct fdt_info *out)
{
    const uint8_t *b = blob;
    out->mem_base = 0;
    out->mem_total = 0;
    out->cpus = 0;
    out->gic_version = 0;
    out->gic_dist = 0;
    out->gic_cpu = 0;
    out->bootargs = 0;
    out->psci = 0;
    out->fwcfg = 0;
    out->rtc = 0;
    out->virtio_n = 0;
    for (int i = 0; i < 8; i++)
        out->cpu_id[i] = 0;
    if (room < 40 || !fdt_valid(blob))
        return -1;

    /* A damaged header could claim gigabytes, and every check below
       would then pass against a size that is not there */
    uint32_t total = be32(b + 4);
    if (total < 40 || total > room)
        return -1;
    uint32_t off_struct = be32(b + 8), off_strings = be32(b + 12);
    uint32_t size_strings = be32(b + 32), size_struct = be32(b + 36);
    if (off_struct > total || size_struct > total - off_struct ||
        off_strings > total || size_strings > total - off_strings)
        return -1;
    const uint8_t *st = b + off_struct, *strs = b + off_strings;

    /* The root says how many cells an address and a size take in its
       children's 'reg'. These are the specification's defaults. */
    uint32_t addr_cells = 2, size_cells = 1;
    int depth = 0;
    /* The node at depth 2 (a child of the root) now open */
    int in_memory = 0, in_cpus = 0, in_chosen = 0, in_psci = 0, gic = 0;
    /* ... and what else it is, by its 'compatible', and its interrupt */
    int kind = DEV_OTHER;
    const uint8_t *irq = 0;
    uint32_t irq_len = 0;
    const uint8_t *reg = 0;
    uint32_t reg_len = 0;
    int found_memory = 0;
    /* /cpus says how many cells a core's 'reg' - its MPIDR - takes; and
       the core whose node is open, by its place in the list */
    uint32_t cpu_cells = 1;
    int this_cpu = -1;

    for (uint32_t p = 0; p + 4 <= size_struct;) {
        uint32_t tok = be32(st + p);
        p += 4;
        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)st + p;
            uint32_t n = 0;
            while (p + n < size_struct && st[p + n])
                n++;
            if (p + n >= size_struct)
                return -1;                 /* a name with no end */
            p += (n + 1 + 3) & ~3u;
            if (++depth > MAX_DEPTH)
                return -1;
            if (depth == 2) {
                in_memory = starts_with(name, "memory");
                in_cpus = n == 4 && starts_with(name, "cpus");
                in_chosen = n == 6 && starts_with(name, "chosen");
                in_psci = n == 4 && starts_with(name, "psci");
                gic = 0;
                kind = DEV_OTHER;
                irq = 0;
                irq_len = 0;
                reg = 0;
                reg_len = 0;
            } else if (depth == 3 && in_cpus && starts_with(name, "cpu@")) {
                this_cpu = out->cpus++;
            }
        } else if (tok == FDT_END_NODE) {
            if (depth == 2 && in_memory && reg) {
                /* Every (address, size) pair is a range of memory */
                uint32_t pair = 4 * (addr_cells + size_cells);
                for (uint32_t i = 0; pair && i + pair <= reg_len; i += pair) {
                    uint64_t base = cells_value(reg + i, addr_cells);
                    uint64_t size = cells_value(reg + i + 4 * addr_cells,
                                                size_cells);
                    if (!found_memory) {
                        out->mem_base = base;
                        found_memory = 1;
                    }
                    out->mem_total += size;
                }
            }
            /* The first interrupt controller: v2 has its distributor
               and its CPU interface as the first two ranges; v3 only
               the distributor matters here */
            if (depth == 2 && gic && reg && !out->gic_version) {
                uint32_t pair = 4 * (addr_cells + size_cells);
                if (pair && reg_len >= pair) {
                    out->gic_version = gic;
                    out->gic_dist = cells_value(reg, addr_cells);
                    /* On a v3 the second range is the redistributors,
                       which are something else entirely */
                    if (gic == 2 && reg_len >= 2 * pair)
                        out->gic_cpu = cells_value(reg + pair, addr_cells);
                }
            }
            /* The devices this kernel has drivers for, by their first
               range - and a virtio transport's interrupt: a shared
               peripheral one (type 0) is the GIC's number plus 32 */
            if (depth == 2 && kind != DEV_OTHER && reg &&
                reg_len >= 4 * addr_cells) {
                uint64_t at = cells_value(reg, addr_cells);
                if (kind == DEV_FWCFG && !out->fwcfg)
                    out->fwcfg = at;
                else if (kind == DEV_PL031 && !out->rtc)
                    out->rtc = at;
                else if (kind == DEV_VIRTIO && out->virtio_n < FDT_MAX_VIRTIO &&
                         irq && irq_len >= 12) {
                    out->virtio_base[out->virtio_n] = at;
                    out->virtio_irq[out->virtio_n] =
                        be32(irq + 4) + (be32(irq) == 0 ? 32 : 16);
                    out->virtio_n++;
                }
            }
            if (depth == 2)
                in_memory = in_cpus = in_chosen = in_psci = gic = 0;
            if (depth == 3)
                this_cpu = -1;
            if (depth > 0)
                depth--;
        } else if (tok == FDT_PROP) {
            if (p + 8 > size_struct)
                return -1;
            uint32_t len = be32(st + p), nameoff = be32(st + p + 4);
            p += 8;
            if (len > size_struct - p || nameoff >= size_strings)
                return -1;
            const uint8_t *val = st + p;
            const char *pname = (const char *)strs + nameoff;
            /* The name must end inside the strings block */
            uint32_t k = nameoff;
            while (k < size_strings && strs[k])
                k++;
            if (k == size_strings)
                return -1;
            if (depth == 1 && len == 4 && strcmp(pname, "#address-cells") == 0)
                addr_cells = be32(val);
            else if (depth == 1 && len == 4 &&
                     strcmp(pname, "#size-cells") == 0)
                size_cells = be32(val);
            else if (depth == 2 && strcmp(pname, "device_type") == 0 &&
                     len >= 7 && memcmp(val, "memory", 7) == 0)
                in_memory = 1;
            else if (depth == 2 && strcmp(pname, "reg") == 0) {
                reg = val;
                reg_len = len;
            } else if (depth == 2 && strcmp(pname, "compatible") == 0) {
                gic = gic_kind(val, len);
                kind = list_has(val, len, "qemu,fw-cfg-mmio") ? DEV_FWCFG
                       : list_has(val, len, "arm,pl031")      ? DEV_PL031
                       : list_has(val, len, "virtio,mmio")    ? DEV_VIRTIO
                                                              : DEV_OTHER;
            } else if (depth == 2 && strcmp(pname, "interrupts") == 0) {
                irq = val;
                irq_len = len;
            }
            else if (depth == 2 && in_chosen &&
                     strcmp(pname, "bootargs") == 0 && len > 0 &&
                     val[len - 1] == 0)
                out->bootargs = (const char *)val;
            else if (depth == 2 && in_psci && strcmp(pname, "method") == 0 &&
                     len == 4 && val[3] == 0)
                out->psci = memcmp(val, "hvc", 3) == 0   ? 1
                            : memcmp(val, "smc", 3) == 0 ? 2 : 0;
            else if (depth == 2 && in_cpus && len == 4 &&
                     strcmp(pname, "#address-cells") == 0)
                cpu_cells = be32(val);
            else if (depth == 3 && this_cpu >= 0 && this_cpu < 8 &&
                     strcmp(pname, "reg") == 0 && cpu_cells >= 1 &&
                     cpu_cells <= 2 && len >= 4 * cpu_cells)
                out->cpu_id[this_cpu] = cells_value(val, cpu_cells);
            p += (len + 3) & ~3u;
        } else if (tok == FDT_NOP) {
            continue;
        } else if (tok == FDT_END) {
            break;
        } else {
            return -1;
        }
        /* A number of cells this kernel cannot hold in 64 bits would
           read as nonsense; better to say the tree does not add up */
        if (addr_cells > 2 || size_cells > 2)
            return -1;
    }
    return 0;
}
