/* ACPI - turning the computer off and rebooting it
 *
 * The road to switching the power off leads through three structures:
 *
 *   RSDP - a pointer lying somewhere in memory below one megabyte,
 *          recognised by the string "RSD PTR " and a checksum,
 *   RSDT - the listing of all tables; in it we look for the FADT ("FACP"),
 *   FADT - says which ports to write to and where the DSDT lies.
 *
 * One thing is still missing: the value that has to be written. It
 * sits in the DSDT, in AML bytecode, under the name _S5_ - the "off"
 * state. We do not have a full AML interpreter and do not need one:
 * that single object is a short package that can be read directly.
 *
 * The tables often lie outside the area the kernel manages as memory
 * (the firmware marks it as reserved), so we glue each of them onto
 * the identity mapping before reading it.
 */

#include <stdint.h>
#include "include/acpi.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"

/* The only thing in this file that depends on the processor mode: the
   ACPI tables have to be glued onto the mapping, and the page tables
   are built by different code in each kernel. The road to switching
   the power off itself - RSDP, RSDT, FADT, _S5_ in bytecode - is the
   same. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define IDENTITY_TOP()   paging64_identity_top()
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define IDENTITY_TOP()   paging_identity_limit()
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

struct rsdp {
    char signature[8];          /* "RSD PTR " */
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* A register address in the generic description (ACPI 2.0) - we use it
   only for the reset register */
struct gas {
    uint8_t space;              /* 0 = memory, 1 = I/O ports */
    uint8_t width, offset, size;
    uint32_t addr_low, addr_high;
} __attribute__((packed));

struct fadt {
    struct sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable;
    uint8_t s4bios_req, pstate_cnt;
    uint32_t pm1a_evt_blk, pm1b_evt_blk;
    uint32_t pm1a_cnt_blk, pm1b_cnt_blk;
    uint32_t pm2_cnt_blk, pm_tmr_blk;
    uint32_t gpe0_blk, gpe1_blk;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
    uint8_t gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved2;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t reset_value;
} __attribute__((packed));

#define SLP_EN   (1u << 13)
#define SCI_EN   1

static const struct fadt *fadt;
static uint16_t slp_typa, slp_typb;
static char oem_id[7];
static int ready;

/* The table listing remembered at startup - we look for the MADT in it
   later, once it is known that there is ACPI at all */
static const struct sdt_header *rsdt_spis;

/* ACPI tables usually lie outside the memory the kernel manages -
   they have to be let into the identity mapping first */
static const void *map(uint32_t phys, uint32_t len)
{
    if (!phys)
        return (void *)0;
    if ((uint64_t)phys + len > IDENTITY_TOP() &&
        MAP_PHYS(phys, len) < 0)
        return (void *)0;
    return (const void *)(uintptr_t)phys;
}

/* Reading a single byte from a fixed address in low memory */
static uint8_t low_byte(uint32_t addr)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)addr;
    /* The compiler sees a constant address here and treats it as an
       array of zero length. Passing the pointer through an empty asm
       statement takes that knowledge away from it - the address is
       real, it just comes from outside the language. */
    __asm__("" : "+r"(p));
    return *p;
}

static int checksum_ok(const void *p, uint32_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

/* The RSDP lies either in the BIOS data area or in ROM between 0xE0000
   and 0xFFFFF; always on a 16-byte boundary. */
/* The pointer supplied by the loader (Multiboot 2) - it takes
   precedence over searching on our own, because under UEFI there is
   nowhere to search */
static uint32_t given_rsdp;

void acpi_set_rsdp(uint32_t addr)
{
    given_rsdp = addr;
}

static const struct rsdp *find_rsdp(void)
{
    if (given_rsdp) {
        const struct rsdp *r = (const struct rsdp *)(uintptr_t)given_rsdp;
        if (!memcmp(r->signature, "RSD PTR ", 8) && checksum_ok(r, 20))
            return r;
    }

    /* The address of the BIOS data area lies at the fixed address
       0x40E. We read it byte by byte: for the compiler a constant
       address cast to a pointer is an "array of zero length", and it
       rightly frowns at it. */
    uint32_t ebda = ((uint32_t)low_byte(0x40F) << 8 | low_byte(0x40E)) << 4;
    if (ebda >= 0x400 && ebda < 0xA0000) {
        for (uint32_t a = ebda; a < ebda + 1024; a += 16) {
            const struct rsdp *r = (const struct rsdp *)(uintptr_t)a;
            if (!memcmp(r->signature, "RSD PTR ", 8) &&
                checksum_ok(r, 20))
                return r;
        }
    }
    for (uint32_t a = 0xE0000; a < 0x100000; a += 16) {
        const struct rsdp *r = (const struct rsdp *)(uintptr_t)a;
        if (!memcmp(r->signature, "RSD PTR ", 8) && checksum_ok(r, 20))
            return r;
    }
    return (const struct rsdp *)0;
}

/* Extracts from the AML bytecode the values of the _S5_ state - "off".
 *
 * The object looks like this: the name "_S5_", a package tag (0x12),
 * its length, the element count, and then two numbers - one for each
 * of the two control blocks. The numbers may be written directly
 * (0..7 have tags of their own) or preceded by a byte tag (0x0A).
 */
static int parse_s5(const uint8_t *dsdt, uint32_t len)
{
    for (uint32_t i = 0; i + 8 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0)
            continue;

        const uint8_t *p = dsdt + i + 4;
        if (*p != 0x12)                 /* not a package - keep looking */
            continue;
        p++;
        p += ((*p & 0xC0) >> 6) + 2;    /* package length + element count */

        if (*p == 0x0A)                 /* the "one-byte number" tag */
            p++;
        slp_typa = (uint16_t)(*p++ << 10);
        if (*p == 0x0A)
            p++;
        slp_typb = (uint16_t)(*p << 10);
        return 1;
    }
    return 0;
}

int acpi_init(void)
{
    const struct rsdp *rsdp = find_rsdp();
    if (!rsdp)
        return 0;

    for (int i = 0; i < 6; i++)
        oem_id[i] = rsdp->oem[i];
    oem_id[6] = '\0';

    const struct sdt_header *rsdt = map(rsdp->rsdt, sizeof(*rsdt));
    if (!rsdt || memcmp(rsdt->signature, "RSDT", 4) != 0)
        return 0;
    if (!map(rsdp->rsdt, rsdt->length))
        return 0;

    rsdt_spis = rsdt;
    uint32_t count = (rsdt->length - sizeof(*rsdt)) / 4;
    const uint32_t *entries = (const uint32_t *)(rsdt + 1);
    for (uint32_t i = 0; i < count; i++) {
        const struct sdt_header *h = map(entries[i], sizeof(*h));
        if (!h || memcmp(h->signature, "FACP", 4) != 0)
            continue;
        if (!map(entries[i], h->length))
            continue;
        fadt = (const struct fadt *)h;
        break;
    }
    if (!fadt)
        return 0;

    const struct sdt_header *dsdt = map(fadt->dsdt, sizeof(*dsdt));
    if (dsdt && map(fadt->dsdt, dsdt->length))
        parse_s5((const uint8_t *)(dsdt + 1),
                 dsdt->length - sizeof(*dsdt));

    /* Enabling ACPI: when the firmware kept control to itself, it has
       to be taken away by writing to the SMI port. A virtual machine
       usually hands it over at once and this step is absent. */
    if (fadt->smi_cmd && fadt->acpi_enable && fadt->pm1a_cnt_blk &&
        !(inw((uint16_t)fadt->pm1a_cnt_blk) & SCI_EN)) {
        outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
        for (int i = 0; i < 300; i++) {
            if (inw((uint16_t)fadt->pm1a_cnt_blk) & SCI_EN)
                break;
            io_wait();
        }
    }

    ready = fadt->pm1a_cnt_blk != 0;
    return ready;
}

/* Looks in the listing for a table with the given signature and glues
   it onto the mapping. NULL when there is none. */
const void *acpi_table(const char *sig)
{
    if (!rsdt_spis)
        return (const void *)0;

    uint32_t count = (rsdt_spis->length - sizeof(*rsdt_spis)) / 4;
    const uint32_t *entries = (const uint32_t *)(rsdt_spis + 1);
    for (uint32_t i = 0; i < count; i++) {
        const struct sdt_header *h = map(entries[i], sizeof(*h));
        if (!h || memcmp(h->signature, sig, 4) != 0)
            continue;
        if (!map(entries[i], h->length))
            continue;
        return h;
    }
    return (const void *)0;
}

const char *acpi_oem(void)
{
    return oem_id;
}

void acpi_poweroff(void)
{
    if (!ready)
        return;

    uint32_t flags = irq_save();
    outw((uint16_t)fadt->pm1a_cnt_blk, slp_typa | SLP_EN);
    if (fadt->pm1b_cnt_blk)
        outw((uint16_t)fadt->pm1b_cnt_blk, slp_typb | SLP_EN);

    /* The power does not go out in the same instruction in which we
       ask for it - without this moment's delay we would still manage
       to print that it failed, right before it succeeds. */
    for (volatile uint32_t i = 0; i < 50000000u; i++)
        ;
    irq_restore(flags);
}

int acpi_reboot(void)
{
    /* The reset register only exists from ACPI 2.0 and not everyone fills
   it in */
    if (!fadt || fadt->header.length < sizeof(struct fadt) ||
        !fadt->reset_reg.addr_low)
        return 0;

    if (fadt->reset_reg.space == 1) {           /* an I/O port */
        outb((uint16_t)fadt->reset_reg.addr_low, fadt->reset_value);
        return 1;
    }
    if (fadt->reset_reg.space == 0) {           /* a register in memory */
        if (!map(fadt->reset_reg.addr_low, 4))
            return 0;
        *(volatile uint8_t *)(uintptr_t)fadt->reset_reg.addr_low =
            fadt->reset_value;
        return 1;
    }
    return 0;
}
