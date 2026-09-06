/* The NVMe driver - a disk plugged straight into PCI Express
 *
 * All of the ATA legacy disappears here. There is no "select drive"
 * register, no FIS frames, not even a notion of a sector in the
 * hardware sense - what there is instead is a pair of QUEUES lying in
 * memory:
 *
 *   the submission queue (SQ) - we put 64-byte commands in here,
 *   the completion queue (CQ) - the controller puts 16-byte answers here.
 *
 * How far each side has got is stated by two pointers per queue: the
 * submission tail is moved by us, the completion head by us as well -
 * both through "doorbells" (doorbell registers) in the controller's
 * register area. That an answer is new we tell from the PHASE BIT: the
 * controller flips it on every lap of the queue, so nothing has to be
 * cleared.
 *
 * We have two pairs of queues: the administrative one, which creates
 * itself when the controller starts, and one for data, which we order
 * with a command on the first.
 *
 * We wait by polling, without interrupts - for the same reason as with
 * AHCI: the disk is also served from inside the page fault handler,
 * where interrupts are off and nobody would wake us.
 */

#include <stdint.h>
#include "include/nvme.h"
#include "include/pci.h"
#include "include/printk.h"
#include "include/string.h"

/* The controller registers lie high, beyond the memory described up
   front - gluing them onto the mapping takes different code in each
   mode, and this is the only place in this file that knows about the
   mode at all. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n) paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n) paging_map_phys((a), (n))
#endif

#define SECTOR 512

/* Controller registers (offsets from the base) */
#define REG_CAP    0x00        /* capabilities, 64 bits */
#define REG_CC     0x14        /* configuration */
#define REG_CSTS   0x1C        /* status */
#define REG_AQA    0x24        /* admin queue sizes */
#define REG_ASQ    0x28        /* admin submission queue address, 64 bits */
#define REG_ACQ    0x30        /* admin completion queue address, 64 bits */
#define REG_DB     0x1000      /* the first doorbell */

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)   /* the controller has given up */

/* Administrative commands */
#define ADM_CREATE_SQ 0x01
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY  0x06

/* Data commands */
#define IO_WRITE 0x01
#define IO_READ  0x02

#define QSIZE     8            /* slots in each queue */
#define IO_QID    1

/* A command: 64 bytes with a layout imposed by the standard */
struct command {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t reserve;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

/* An answer: 16 bytes, with the phase bit in the status field */
struct completion {
    uint32_t result;
    uint32_t reserve;
    uint16_t sq_head;
    uint16_t id_sq;
    uint16_t cid;
    uint16_t stan;             /* bit 0 = phase, bits 1-15 = error code */
} __attribute__((packed));

/* The queues and the buffer have to lie in physically contiguous
   memory and be page-aligned - the controller reaches for them itself,
   and their addresses are given to it as PRP pointers, which describe
   pages rather than arbitrary chunks. */
static struct command adm_sq[QSIZE] __attribute__((aligned(4096)));
static struct completion adm_cq[QSIZE] __attribute__((aligned(4096)));
static struct command io_sq[QSIZE] __attribute__((aligned(4096)));
static struct completion io_cq[QSIZE] __attribute__((aligned(4096)));

/* A bounce buffer. A PRP pointer describes a page, while the caller
   hands us buffers lying anywhere - copying through our own aligned
   page is cheaper than translating every such buffer into a list of
   pages. Eight sectors is exactly one page. */
static uint8_t dma_buf[4096] __attribute__((aligned(4096)));
#define BUF_SECTORS (4096 / SECTOR)

static volatile uint8_t *base;
static int present;
static uint32_t nsectors;
static uint32_t db_stride;        /* the stride between doorbells */

static uint16_t adm_tail, io_tail;
static uint16_t adm_head, io_head;
static uint8_t adm_phase = 1, io_phase = 1;
static uint16_t next_cid = 1;

static uint32_t rd32(uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

static void wr32(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(base + reg) = v;
}

static uint64_t rd64(uint32_t reg)
{
    return (uint64_t)rd32(reg) | ((uint64_t)rd32(reg + 4) << 32);
}

static void wr64(uint32_t reg, uint64_t v)
{
    wr32(reg, (uint32_t)v);
    wr32(reg + 4, (uint32_t)(v >> 32));
}

/* A doorbell: two per queue (submission tail, completion head) */
static void doorbell_sq(int qid, uint16_t tail)
{
    wr32(REG_DB + (uint32_t)(2 * qid) * db_stride, tail);
}

static void doorbell_cq(int qid, uint16_t head)
{
    wr32(REG_DB + (uint32_t)(2 * qid + 1) * db_stride, head);
}

/* Places a command and waits for the answer. Returns the error code
   (0 = fine) or -1 when the controller did not answer at all. */
static int execute(struct command *sq, struct completion *cq, int qid,
                   uint16_t *tail, uint16_t *head, uint8_t *phase,
                   const struct command *co)
{
    uint16_t cid = next_cid++;
    sq[*tail] = *co;
    sq[*tail].cid = cid;

    *tail = (uint16_t)((*tail + 1) % QSIZE);
    doorbell_sq(qid, *tail);

    for (int spin = 0; spin < 20000000; spin++) {
        volatile struct completion *u = &cq[*head];
        if ((u->stan & 1) != *phase)
            continue;

        int code = (u->stan >> 1) & 0x7FF;
        uint16_t returned = u->cid;

        *head = (uint16_t)((*head + 1) % QSIZE);
        if (*head == 0)
            *phase ^= 1;            /* a lap flips the phase */
        doorbell_cq(qid, *head);

        if (returned != cid)
            return -1;             /* an answer to a different question */
        return code;
    }
    return -1;
}

static int adm(const struct command *co)
{
    return execute(adm_sq, adm_cq, 0, &adm_tail, &adm_head, &adm_phase, co);
}

static int io(const struct command *co)
{
    return execute(io_sq, io_cq, IO_QID, &io_tail, &io_head, &io_phase, co);
}

int nvme_present(void)      { return present; }
uint32_t nvme_sectors(void) { return nsectors; }

/* One batch: at most a page of data, because that is what one PRP
       describes */
static int batch(uint32_t lba, uint32_t count, int write)
{
    struct command p;
    memset(&p, 0, sizeof(p));
    p.opcode = write ? IO_WRITE : IO_READ;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)dma_buf;
    p.cdw10 = lba;                 /* the lower 32 bits of the block address */
    p.cdw11 = 0;
    p.cdw12 = count - 1;           /* the block count, counted from zero */
    return io(&p) == 0 ? 0 : -1;
}

int nvme_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!present)
        return -1;
    uint8_t *dst = buf;

    while (count) {
        uint32_t chunk = count > BUF_SECTORS ? BUF_SECTORS : count;
        if (batch(lba, chunk, 0) < 0)
            return -1;
        memcpy(dst, dma_buf, chunk * SECTOR);
        dst += chunk * SECTOR;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

int nvme_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!present)
        return -1;
    const uint8_t *src = buf;

    while (count) {
        uint32_t chunk = count > BUF_SECTORS ? BUF_SECTORS : count;
        memcpy(dma_buf, src, chunk * SECTOR);
        if (batch(lba, chunk, 1) < 0)
            return -1;
        src += chunk * SECTOR;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

/* Asks for the description of namespace number 1: how many blocks it
   has and how large they are */
static int identify_disk(void)
{
    struct command p;
    memset(&p, 0, sizeof(p));
    p.opcode = ADM_IDENTIFY;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)dma_buf;
    p.cdw10 = 0;                   /* the namespace description */
    if (adm(&p) != 0)
        return 0;

    uint64_t nblocks = 0;
    for (int i = 0; i < 8; i++)
        nblocks |= (uint64_t)dma_buf[i] << (8 * i);

    /* The block size sits in the format table: the number of the format
       in use is in byte 26, and the format itself - four bytes - from
       128. Its byte 2 says how many bits of address there are in a
       block (a power of two). */
    uint32_t format = dma_buf[26] & 0x0F;
    uint32_t bits = dma_buf[128 + format * 4 + 2];
    uint32_t size = bits < 32 ? (1u << bits) : 0;

    if (size != SECTOR) {
        printk("[ !! ] NVMe: a %u B block, and the system works on %u B\n",
               size, (uint32_t)SECTOR);
        return 0;
    }
    if (!nblocks)
        return 0;

    nsectors = nblocks > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)nblocks;
    return 1;
}

/* Orders a pair of data queues. The completion one first - the
   submission queue has to know where to send its answers. */
static int setup_queues(void)
{
    struct command p;

    memset(&p, 0, sizeof(p));
    p.opcode = ADM_CREATE_CQ;
    p.prp1 = (uint64_t)(uintptr_t)io_cq;
    p.cdw10 = (uint32_t)IO_QID | ((QSIZE - 1) << 16);
    p.cdw11 = 1;                   /* contiguous in memory, no interrupts */
    if (adm(&p) != 0)
        return 0;

    memset(&p, 0, sizeof(p));
    p.opcode = ADM_CREATE_SQ;
    p.prp1 = (uint64_t)(uintptr_t)io_sq;
    p.cdw10 = (uint32_t)IO_QID | ((QSIZE - 1) << 16);
    p.cdw11 = 1 | ((uint32_t)IO_QID << 16);   /* contiguous, answers into the CQ */
    return adm(&p) == 0;
}

int nvme_init(void)
{
    struct pci_dev dev;
    /* Class 01/08/02: mass storage, NVM controller, NVMe interface */
    if (!pci_find_class(0x01, 0x08, 0x02, &dev))
        return 0;
    if (dev.bar0 & 0x01)           /* we expect a memory BAR */
        return 0;

    uint64_t mmio = (uint64_t)(dev.bar0 & ~0xFu);
    if ((dev.bar0 & 0x06) == 0x04)  /* a 64-bit BAR: the upper half in bar1 */
        mmio |= (uint64_t)dev.bar[1] << 32;
    if (!mmio || MAP_PHYS(mmio, 0x2000) < 0)
        return 0;

    pci_enable_bus_master(&dev);
    base = (volatile uint8_t *)(uintptr_t)mmio;

    uint64_t cap = rd64(REG_CAP);
    db_stride = 4u << ((uint32_t)(cap >> 32) & 0x0F);   /* CAP.DSTRD */

    /* The controller has to be stopped before anything can be set on it */
    wr32(REG_CC, rd32(REG_CC) & ~CC_EN);
    for (int i = 0; i < 1000000 && (rd32(REG_CSTS) & CSTS_RDY); i++)
        ;
    if (rd32(REG_CSTS) & CSTS_RDY)
        return 0;

    memset(adm_sq, 0, sizeof(adm_sq));
    memset(adm_cq, 0, sizeof(adm_cq));
    memset(io_sq, 0, sizeof(io_sq));
    memset(io_cq, 0, sizeof(io_cq));
    adm_tail = adm_head = io_tail = io_head = 0;
    adm_phase = io_phase = 1;

    wr32(REG_AQA, ((QSIZE - 1) << 16) | (QSIZE - 1));
    wr64(REG_ASQ, (uint64_t)(uintptr_t)adm_sq);
    wr64(REG_ACQ, (uint64_t)(uintptr_t)adm_cq);

    /* 4 KiB pages, 64 B commands, 16 B answers */
    wr32(REG_CC, CC_EN | (0u << 4) | (0u << 7) | (6u << 16) | (4u << 20));
    for (int i = 0; i < 1000000; i++) {
        uint32_t st = rd32(REG_CSTS);
        if (st & CSTS_CFS)
            return 0;
        if (st & CSTS_RDY)
            break;
    }
    if (!(rd32(REG_CSTS) & CSTS_RDY))
        return 0;

    if (!identify_disk() || !setup_queues())
        return 0;

    present = 1;
    return 1;
}
