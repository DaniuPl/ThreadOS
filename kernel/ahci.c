/* The AHCI (SATA) driver
 *
 * The controller gets a list of commands from us in memory and reaches
 * for it itself through DMA - the processor no longer copies data word
 * by word as with ATA PIO. One command is made of three structures:
 *
 *   the command header  - in a list of 32 slots (the port's CLB
 *                         register), it says where the command table
 *                         lies and how many data region descriptors it
 *                         has,
 *   the command table   - a FIS frame with the ATA command plus the
 *                         PRDT table (the address and length of every
 *                         chunk of the buffer),
 *   the FIS receive area - where the controller parks its answers (the
 *                         FB register).
 *
 * All of it has to lie in physically contiguous memory and at an
 * address the controller understands directly - that is, in our
 * identity mapping, where the virtual address equals the physical one.
 *
 * We wait by polling, without interrupts: the disk is also served from
 * inside the page fault handler (swapping), where interrupts are off
 * and nobody would wake us.
 */

#include <stdint.h>
#include "include/ahci.h"
#include "include/pci.h"
#include "include/pmm.h"

/* The controller registers lie high, beyond the memory described up
   front - they have to be glued onto the mapping. Different code does
   that in each mode, and this is the only place in this file that
   knows about the mode at all. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n) paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n) paging_map_phys((a), (n))
#endif
#include "include/printk.h"
#include "include/string.h"

#define SECTOR 512

/* Port registers (0x100 + port * 0x80 from the start of the
   controller's area) */
struct hba_port {
    volatile uint32_t clb, clbu, fb, fbu;
    volatile uint32_t is, ie, cmd, rsv0;
    volatile uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf;
    volatile uint32_t fbs;
    volatile uint32_t rsv1[11];
    volatile uint32_t vendor[4];
};

struct hba_mem {
    volatile uint32_t cap, ghc, is, pi, vs;
    volatile uint32_t ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    volatile uint8_t rsv[0xA0 - 0x2C];
    volatile uint8_t vendor[0x100 - 0xA0];
    struct hba_port ports[32];
};

struct cmd_header {
    uint16_t flags;              /* bits 0-4: FIS length in words, 6: write */
    uint16_t prdtl;              /* how many data region descriptors */
    volatile uint32_t prdbc;     /* how many bytes the controller moved */
    uint32_t ctba, ctbau;        /* the command table's address */
    uint32_t rsv[4];
} __attribute__((packed));

struct prdt_entry {
    uint32_t dba, dbau, rsv;
    uint32_t dbc;                /* bits 0-21: byte count - 1 */
} __attribute__((packed));

#define PRDT_MAX 8

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct prdt_entry prdt[PRDT_MAX];
} __attribute__((packed));

/* A command frame: host -> device, register type */
struct fis_h2d {
    uint8_t type;                /* 0x27 */
    uint8_t pmport_c;            /* bit 7: this is a command, not a register change */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth, icc, control;
    uint8_t rsv[4];
} __attribute__((packed));

#define FIS_TYPE_H2D   0x27
#define CMD_READ_DMA   0x25      /* READ DMA EXT (LBA48) */
#define CMD_WRITE_DMA  0x35      /* WRITE DMA EXT (LBA48) */
#define CMD_IDENTIFY   0xEC

#define PORT_CMD_ST    0x0001    /* start - the controller processes commands */
#define PORT_CMD_FRE   0x0010    /* receiving FIS frames */
#define PORT_CMD_FR    0x4000    /* receiving has really started */
#define PORT_CMD_CR    0x8000    /* processing has really started */
#define PORT_IS_TFES   0x40000000u  /* an error reported by the device */

#define SIG_SATA       0x00000101u

static struct hba_mem *hba;
static struct hba_port *port;
static struct cmd_header *cmd_list;
static struct cmd_table *cmd_tbl;
static uint32_t disk_sectors;

/* The controller can take its time; we have no clock finer than 10 ms,
   so we count loop iterations. The values have plenty of margin. */
static int wait_clear(volatile uint32_t *reg, uint32_t mask)
{
    for (uint32_t i = 0; i < 20000000u; i++)
        if (!(*reg & mask))
            return 0;
    return -1;
}

static void port_stop(struct hba_port *p)
{
    p->cmd &= ~PORT_CMD_ST;
    p->cmd &= ~PORT_CMD_FRE;
    wait_clear(&p->cmd, PORT_CMD_CR | PORT_CMD_FR);
}

static void port_start(struct hba_port *p)
{
    wait_clear(&p->cmd, PORT_CMD_CR);
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
}

/* Assembles a command in slot 0 and waits for it to finish.
   'write' = the direction, 'buf' must be physically contiguous. */
static int run_command(uint8_t command, uint32_t lba, uint32_t count,
                       void *buf, uint32_t bytes, int write)
{
    if (!port || bytes == 0 || bytes > PRDT_MAX * 4u * 1024 * 1024)
        return -1;

    port->is = (uint32_t)-1;                 /* clear the old notifications */

    cmd_list[0].flags = (uint16_t)((sizeof(struct fis_h2d) / 4) |
                                   (write ? (1u << 6) : 0));
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba = (uint32_t)(uintptr_t)cmd_tbl;
    cmd_list[0].ctbau = 0;

    memset(cmd_tbl, 0, sizeof(*cmd_tbl));
    cmd_tbl->prdt[0].dba = (uint32_t)(uintptr_t)buf;
    cmd_tbl->prdt[0].dbau = 0;
    cmd_tbl->prdt[0].dbc = bytes - 1;        /* the count is taken from zero */

    struct fis_h2d *fis = (struct fis_h2d *)cmd_tbl->cfis;
    fis->type = FIS_TYPE_H2D;
    fis->pmport_c = 0x80;
    fis->command = command;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 0x40;                      /* LBA mode */
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    /* The device has to be ready: neither busy nor with data to hand over */
    if (wait_clear(&port->tfd, 0x88) < 0)
        return -1;

    port->ci = 1;                            /* slot 0 */

    for (uint32_t i = 0; i < 20000000u; i++) {
        if (!(port->ci & 1))
            break;
        if (port->is & PORT_IS_TFES)
            return -1;
    }
    if (port->ci & 1)
        return -1;                           /* nothing arrived in time */
    return (port->is & PORT_IS_TFES) ? -1 : 0;
}

static void read_capacity(void)
{
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return;
    memset((void *)(uintptr_t)frame, 0, SECTOR);
    if (run_command(CMD_IDENTIFY, 0, 0, (void *)(uintptr_t)frame,
                    SECTOR, 0) == 0) {
        const uint16_t *id = (const uint16_t *)(uintptr_t)frame;
        /* Words 100-103: the LBA48 capacity. The upper half is of no
           concern to us - we address the disk with 32 bits anyway. */
        uint32_t lba48 = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
        uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        disk_sectors = lba48 ? lba48 : lba28;
    }
    pmm_free_frame(frame);
}

int ahci_init(void)
{
    struct pci_dev dev;
    /* Class 0x01 (mass storage), subclass 0x06 (SATA), interface 0x01 (AHCI) */
    if (!pci_find_class(0x01, 0x06, 0x01, &dev))
        return 0;

    uint32_t abar = dev.bar[5] & 0xFFFFFFF0u;
    if (!abar || (dev.bar[5] & 0x01))        /* an I/O port BAR - not our case */
        return 0;

    pci_enable_bus_master(&dev);
    if (MAP_PHYS(abar, 0x2000) < 0)          /* the registers lie high */
        return 0;

    hba = (struct hba_mem *)(uintptr_t)abar;
    hba->ghc |= (1u << 31);                  /* AHCI mode, not compatibility */

    /* The first port with a SATA disk attached */
    uint32_t implemented = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(implemented & (1u << i)))
            continue;
        struct hba_port *p = &hba->ports[i];
        if ((p->ssts & 0x0F) != 3)           /* no link to a device */
            continue;
        if (p->sig != SIG_SATA)              /* e.g. an optical drive */
            continue;

        port_stop(p);

        /* One frame for the command list (1 KiB) and the FIS receive area
           (256 B) - the frame is 4 KiB aligned, so it satisfies both
           requirements. A second frame for the command table. */
        uint32_t area = pmm_alloc_frame();
        uint32_t table = pmm_alloc_frame();
        if (!area || !table) {
            printk("[ !! ] AHCI: brak pamieci na struktury sterownika\n");
            return 0;
        }
        memset((void *)(uintptr_t)area, 0, PAGE_SIZE);
        memset((void *)(uintptr_t)table, 0, PAGE_SIZE);

        p->clb = area;
        p->clbu = 0;
        p->fb = area + 1024;
        p->fbu = 0;
        p->serr = (uint32_t)-1;              /* clear the old link errors */
        p->is = (uint32_t)-1;

        port_start(p);

        port = p;
        cmd_list = (struct cmd_header *)(uintptr_t)area;
        cmd_tbl = (struct cmd_table *)(uintptr_t)table;
        read_capacity();
        return 1;
    }
    return 0;
}

uint32_t ahci_sectors(void)
{
    return disk_sectors;
}

int ahci_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!port || !count)
        return -1;
    return run_command(CMD_READ_DMA, lba, count, buf, count * SECTOR, 0);
}

int ahci_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!port || !count)
        return -1;
    return run_command(CMD_WRITE_DMA, lba, count, (void *)buf,
                       count * SECTOR, 1);
}
