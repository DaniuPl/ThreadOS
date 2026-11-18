/* QEMU's fw_cfg - files the host hands the machine
 *
 * On the virt machine fw_cfg is three registers in the device range:
 * data, a selector and a DMA address (the device tree's
 * qemu,fw-cfg-mmio). The host fills it with named files; a file is
 * found in the directory (item 0x19) and moved whole by DMA: the
 * machine writes the address of a small descriptor - what to do, how
 * much, where - and QEMU has done it by the time the write returns.
 * Through it come the screen's configuration (etc/ramfb, which this
 * kernel writes) and the initrd (opt/threados/initrd, which it reads).
 *
 * Everything in it is big-endian. RAM is mapped onto itself (mmu.c), so
 * an address the kernel holds is the one the device is given.
 */

#include <stdint.h>
#include "fwcfg.h"
#include "../kernel/include/string.h"

#define REG_DATA    0x00
#define REG_SEL     0x08
#define REG_DMA     0x10

#define ITEM_SIGNATURE 0x0000
#define ITEM_ID        0x0001
#define ITEM_FILE_DIR  0x0019

#define ID_DMA         0x02      /* the interface can move data by DMA */

#define DMA_ERROR      0x01
#define DMA_READ       0x02
#define DMA_SELECT     0x08
#define DMA_WRITE      0x10

static volatile uint8_t *regs;

static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
static uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

static void select_item(uint16_t item)
{
    *(volatile uint16_t *)(regs + REG_SEL) = be16(item);
}

/* The data register hands out the selected item a byte at a time */
static void read_bytes(void *buf, uint32_t n)
{
    uint8_t *b = buf;
    for (uint32_t i = 0; i < n; i++)
        b[i] = regs[REG_DATA];
}

int fwcfg_init(uint64_t base)
{
    if (!base)
        return -1;
    regs = (volatile uint8_t *)(uintptr_t)base;
    char sig[4];
    select_item(ITEM_SIGNATURE);
    read_bytes(sig, 4);
    if (memcmp(sig, "QEMU", 4) != 0) {
        regs = 0;
        return -1;
    }
    uint8_t id[4];
    select_item(ITEM_ID);
    read_bytes(id, 4);
    if (!(id[0] & ID_DMA)) {
        regs = 0;
        return -1;
    }
    return 0;
}

/* The n-th file whose name starts with 'prefix' (fwcfg_find is the
   whole name) */
static int find(const char *name, int prefix, int nth, uint16_t *item,
                uint32_t *size)
{
    if (!regs)
        return -1;
    uint32_t count;
    select_item(ITEM_FILE_DIR);
    read_bytes(&count, 4);
    count = be32(count);
    /* An entry: size, item, two reserved bytes, a name of 56 */
    for (uint32_t i = 0; i < count && i < 4096; i++) {
        struct {
            uint32_t size;
            uint16_t item, reserved;
            char name[56];
        } e;
        read_bytes(&e, sizeof(e));
        e.name[55] = 0;
        if (prefix ? !memcmp(e.name, name, strlen(name)) && nth-- == 0
                   : !strcmp(e.name, name)) {
            *item = be16(e.item);
            *size = be32(e.size);
            return 0;
        }
    }
    return -1;
}

int fwcfg_find(const char *name, uint16_t *item, uint32_t *size)
{
    return find(name, 0, 0, item, size);
}

int fwcfg_find_nth(const char *prefix, int nth, uint16_t *item,
                   uint32_t *size)
{
    return find(prefix, 1, nth, item, size);
}

static int dma(uint16_t item, void *buf, uint32_t len, uint32_t op)
{
    if (!regs)
        return -1;
    static volatile struct {
        uint32_t control, length;
        uint64_t address;
    } __attribute__((aligned(16))) d;
    d.control = be32(((uint32_t)item << 16) | DMA_SELECT | op);
    d.length = be32(len);
    d.address = __builtin_bswap64((uint64_t)(uintptr_t)buf);
    uint64_t at = (uint64_t)(uintptr_t)&d;
    /* The descriptor and the buffer are in memory before the device is
       told; the write of the low half starts the transfer */
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(regs + REG_DMA) = be32((uint32_t)(at >> 32));
    *(volatile uint32_t *)(regs + REG_DMA + 4) = be32((uint32_t)at);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t c;
    while ((c = be32(d.control)) & ~(uint32_t)DMA_ERROR)
        ;
    return (c & DMA_ERROR) ? -1 : 0;
}

int fwcfg_read(uint16_t item, void *buf, uint32_t len)
{
    return dma(item, buf, len, DMA_READ);
}

int fwcfg_write(uint16_t item, const void *buf, uint32_t len)
{
    return dma(item, (void *)(uintptr_t)buf, len, DMA_WRITE);
}
