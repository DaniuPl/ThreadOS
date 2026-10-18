/* UEFI runtime services - the firmware, called from a running system
 *
 * A UEFI machine's boot menu is not on the disk: it is a set of
 * variables in the firmware's flash - Boot0000, Boot0001, ... each a
 * name and a path to a loader, and BootOrder listing them. Installing
 * ThreadOS beside Windows writes its loader where the firmware would
 * never look by itself, and a laptop's firmware often has no screen
 * for adding an entry by hand (Lenovo's InsydeH2O does not). So the
 * installer adds the entry itself, the way every other system's
 * installer does - through the one door the firmware leaves open after
 * the loader has gone: its runtime services, SetVariable among them.
 *
 * What it takes:
 *
 *   the system table's address and the firmware's memory map, which a
 *   UEFI loader passes as Multiboot 2 tags (GRUB does, threadboot
 *   does; kernel/mb2.c keeps them)
 *
 *   the firmware's runtime regions reachable at their physical
 *   addresses - nobody called SetVirtualAddressMap, so that is where
 *   they still expect to be. They are identity-mapped into the
 *   kernel's own space (code executable, device registers uncached),
 *   and the call runs on that space
 *
 *   the firmware's calling convention (Microsoft's: ms_abi), with
 *   interrupts off - runtime services are not reentrant - and the
 *   current program's SSE registers saved around it, since the
 *   firmware may use them
 *
 * Nothing here is needed to run; it is only ever used to change the
 * boot menu, when an installation asks for it.
 */

#include <stddef.h>
#include <stdint.h>
#include "../kernel/include/efirt.h"
#include "../kernel/include/mb2.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/io.h"
#include "paging64.h"

#define MS __attribute__((ms_abi))

typedef uint64_t MS (*get_variable_fn)(uint16_t *name, const void *guid,
                                       uint32_t *attrs, uint64_t *size,
                                       void *data);
typedef uint64_t MS (*set_variable_fn)(uint16_t *name, const void *guid,
                                       uint32_t attrs, uint64_t size,
                                       const void *data);

struct efi_runtime {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
    void *get_time, *set_time, *get_wakeup_time, *set_wakeup_time;
    void *set_virtual_address_map, *convert_pointer;
    get_variable_fn get_variable;
    void *get_next_variable_name;
    set_variable_fn set_variable;
};

struct efi_system_table {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
    void *vendor;
    uint32_t vendor_revision, pad;
    void *con_in_handle, *con_in, *con_out_handle, *con_out;
    void *stderr_handle, *stderr;
    struct efi_runtime *runtime;
    void *boot;
    uint64_t tables;
    void *table;
};

#define SIG_SYSTAB  0x5453595320494249ULL  /* "IBI SYST" */
#define SIG_RUNTIME 0x56524553544E5552ULL  /* "RUNTSERV" */

#define EFI_MEMORY_RUNTIME (1ULL << 63)
#define T_RUNTIME_CODE 5
#define T_MMIO         11
#define T_MMIO_PORT    12

#define NV_BS_RT 7                         /* non-volatile, both phases */
#define EFI_NOT_FOUND 0x800000000000000EULL

static const uint8_t global_guid[16] = {
    0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
    0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C };

static struct efi_runtime *rt;
static int ready = -1;                     /* -1 = not tried yet */
static uint8_t fpu_area[512] __attribute__((aligned(16)));

static int setup(void)
{
    uint64_t systab;
    const uint8_t *map;
    uint32_t map_size, desc_size;

    if (ready >= 0)
        return ready;
    ready = 0;
    if (!mb2_efi(&systab, &map, &map_size, &desc_size))
        return 0;                          /* not a UEFI start */

    /* Every region the firmware keeps for its runtime, where it is */
    for (uint32_t off = 0; off + desc_size <= map_size; off += desc_size) {
        uint32_t type = *(const uint32_t *)(map + off);
        uint64_t start = *(const uint64_t *)(map + off + 8);
        uint64_t pages = *(const uint64_t *)(map + off + 24);
        uint64_t attr = *(const uint64_t *)(map + off + 32);
        if (!(attr & EFI_MEMORY_RUNTIME))
            continue;
        if (paging64_firmware_map(start, pages * 4096,
                                  type == T_RUNTIME_CODE,
                                  type == T_MMIO || type == T_MMIO_PORT) < 0)
            return 0;
    }
    /* The tables themselves lie in runtime data - mapped above; and if
       a firmware put them elsewhere, a page of each is enough to read */
    paging64_firmware_map(systab, sizeof(struct efi_system_table), 0, 0);
    const struct efi_system_table *st =
        (const struct efi_system_table *)(uintptr_t)systab;
    if (st->signature != SIG_SYSTAB || !st->runtime)
        return 0;
    paging64_firmware_map((uint64_t)(uintptr_t)st->runtime,
                          sizeof(struct efi_runtime), 0, 0);
    if (st->runtime->signature != SIG_RUNTIME)
        return 0;
    rt = st->runtime;
    ready = 1;
    return 1;
}

int efirt_available(void)
{
    return setup();
}

/* Around every call: interrupts off, the kernel's own space, the SSE
   registers of whoever was running kept */
static uint64_t enter(void)
{
    uint64_t flags, cr3;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile("fxsave (%0)" : : "r"(fpu_area) : "memory");
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %0, %%cr3" : : "r"(paging64_kernel_space())
                     : "memory");
    fpu_area[510] = 0;                     /* (unused bytes of the area) */
    *(uint64_t *)&fpu_area[496] = flags;
    *(uint64_t *)&fpu_area[504] = cr3;
    return flags;
}

static void leave(void)
{
    uint64_t flags = *(uint64_t *)&fpu_area[496];
    uint64_t cr3 = *(uint64_t *)&fpu_area[504];
    __asm__ volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
    __asm__ volatile("fxrstor (%0)" : : "r"(fpu_area) : "memory");
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int get_var(uint16_t *name, void *data, uint64_t *size)
{
    uint32_t attrs = 0;
    enter();
    uint64_t st = rt->get_variable(name, global_guid, &attrs, size, data);
    leave();
    if (st == EFI_NOT_FOUND)
        return 0;
    return st == 0 ? 1 : -1;
}

static int set_var(uint16_t *name, const void *data, uint64_t size)
{
    enter();
    uint64_t st = rt->set_variable(name, global_guid, NV_BS_RT, size, data);
    leave();
    return st == 0 ? 0 : -1;
}

static void boot_name(uint16_t *out, unsigned n)
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'B'; out[1] = 'o'; out[2] = 'o'; out[3] = 't';
    for (int i = 0; i < 4; i++)
        out[4 + i] = (uint16_t)hex[(n >> (12 - 4 * i)) & 0xF];
    out[8] = 0;
}

static void wide(uint16_t *out, const char *s)
{
    while (*s)
        *out++ = (uint16_t)(uint8_t)*s++;
    *out = 0;
}

/* Whether entry n carries our description */
static int is_ours(unsigned n, const char *desc)
{
    static uint8_t buf[2048];
    uint16_t name[9];
    uint64_t size = sizeof(buf);
    boot_name(name, n);
    if (get_var(name, buf, &size) != 1 || size < 6)
        return 0;
    const uint16_t *d = (const uint16_t *)(buf + 6);
    for (size_t i = 0;; i++) {
        if (6 + 2 * (i + 1) > size)
            return 0;
        if (d[i] != (uint16_t)(uint8_t)desc[i])
            return 0;
        if (!desc[i])
            return 1;
    }
}

int efirt_add_boot_entry(const char *desc, const struct efirt_partition *p,
                         const char *path, int first, unsigned *number)
{
    static uint8_t opt[1024];
    static uint16_t order[128];
    uint16_t name[9];

    if (!setup())
        return -1;

    /* EFI_LOAD_OPTION: attributes, the path list's length, the
       description, then the path - the partition by its GUID (the form
       efibootmgr writes, which firmware finds wherever the disk sits),
       the file on it, the end */
    size_t at = 0;
    *(uint32_t *)opt = 1;                   /* LOAD_OPTION_ACTIVE */
    at = 6;
    for (size_t i = 0;; i++) {
        *(uint16_t *)(opt + at) = (uint16_t)(uint8_t)desc[i];
        at += 2;
        if (!desc[i])
            break;
    }
    size_t path_start = at;
    uint8_t *hd = opt + at;                 /* HARDDRIVE_DEVICE_PATH */
    hd[0] = 4;
    hd[1] = 1;
    hd[2] = 42;
    hd[3] = 0;
    *(uint32_t *)(hd + 4) = p->number;
    *(uint64_t *)(hd + 8) = p->start;
    *(uint64_t *)(hd + 16) = p->sectors;
    memcpy(hd + 24, p->guid, 16);
    hd[40] = 2;                             /* a GPT partition, */
    hd[41] = 2;                             /* named by a GUID */
    at += 42;
    uint8_t *fp = opt + at;                 /* FILEPATH_DEVICE_PATH */
    size_t plen = 0;
    while (path[plen])
        plen++;
    size_t flen = 4 + 2 * (plen + 1);
    fp[0] = 4;
    fp[1] = 4;
    fp[2] = (uint8_t)flen;
    fp[3] = (uint8_t)(flen >> 8);
    for (size_t i = 0; i <= plen; i++)
        *(uint16_t *)(fp + 4 + 2 * i) =
            path[i] == '/' ? '\\' : (uint16_t)(uint8_t)path[i];
    at += flen;
    opt[at] = 0x7F;
    opt[at + 1] = 0xFF;
    opt[at + 2] = 4;
    opt[at + 3] = 0;
    at += 4;
    *(uint16_t *)(opt + 4) = (uint16_t)(at - path_start);

    /* The order there is, our entry if there is one, else a free number */
    uint16_t order_name[10];
    wide(order_name, "BootOrder");
    uint64_t osize = sizeof(order);
    if (get_var(order_name, order, &osize) != 1)
        osize = 0;
    unsigned n_order = (unsigned)(osize / 2);
    int num = -1;
    for (unsigned i = 0; i < n_order && num < 0; i++)
        if (is_ours(order[i], desc))
            num = order[i];
    for (unsigned n = 0; num < 0 && n < 0x1000; n++) {
        static uint8_t probe[8];
        uint64_t sz = sizeof(probe);
        boot_name(name, n);
        int r = get_var(name, probe, &sz);
        if (r == 0)
            num = (int)n;                   /* (too small to read = it exists) */
    }
    if (num < 0)
        return -1;

    boot_name(name, (unsigned)num);
    if (set_var(name, opt, at) < 0)
        return -1;

    /* In the order: first (the machine boots ThreadOS unless told
       otherwise) or last (it goes on booting what it did, ThreadOS
       one choice in its menu) */
    unsigned out = 0;
    static uint16_t fresh[129];
    if (first)
        fresh[out++] = (uint16_t)num;
    for (unsigned i = 0; i < n_order; i++)
        if (order[i] != (uint16_t)num && out < 128)
            fresh[out++] = order[i];
    if (!first)
        fresh[out++] = (uint16_t)num;
    if (set_var(order_name, fresh, (uint64_t)out * 2) < 0)
        return -1;
    if (number)
        *number = (unsigned)num;
    return 0;
}
