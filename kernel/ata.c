/* ATA (IDE) disk driver in PIO mode with polling
 *
 * It handles the first bus (ports 0x1F0-0x1F7), both drives on it -
 * the master and the slave - and LBA28 addressing. This is the simplest possible driver: no
 * interrupts, no DMA - we read/write 256 words through the data port,
 * waiting in a loop until the controller reports readiness. For a toy
 * system and QEMU disks that is entirely enough.
 */

#include "include/ata.h"
#include "include/io.h"
#include "include/pit.h"

#define ATA_DATA      0x1F0
#define ATA_FEATURES  0x1F1
#define ATA_SECCOUNT  0x1F2
#define ATA_LBA_LO    0x1F3
#define ATA_LBA_MID   0x1F4
#define ATA_LBA_HI    0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_STATUS    0x1F7   /* read: status; write: command */
#define ATA_CMD       0x1F7
#define ATA_ALTSTATUS 0x3F6   /* status without clearing the interrupt request */

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

/* How long the drive may stay busy is a time, not a number of looks at
   its status. Each look is a trip out of a virtual machine, and a flush
   there lasts as long as the host's disk takes to sync its file: on a
   loaded host that ran past a million looks, the write that had worked
   came back as failed, and the file system lost what it wrote next to
   it. ATA allows a drive 30 seconds (to spin up); so do we, measured on
   the time stamp counter, which runs with interrupts off - the file
   system calls in with them off. It is measured against the timer once,
   at the first look at the bus, while the timer still ticks. */
#define ATA_WAIT_MS 30000

static uint64_t tsc_per_ms;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static void ata_calibrate(void)
{
    if (tsc_per_ms)
        return;
    /* the edge of a tick first, then two whole ticks (20 ms); a timer
       that does not tick leaves the old count of looks in charge */
    uint32_t t = pit_ticks();
    for (uint32_t i = 0; pit_ticks() == t; i++) {
        if (i > 200000000u)
            return;
        __asm__ volatile("pause");
    }
    t = pit_ticks();
    uint64_t c0 = rdtsc();
    for (uint32_t i = 0; pit_ticks() - t < 2; i++) {
        if (i > 400000000u)
            return;
        __asm__ volatile("pause");
    }
    tsc_per_ms = (rdtsc() - c0) / 20;
}

/* A wait's clock: started, and asked whether it ran out */
struct ata_wait {
    uint64_t start;
    uint32_t looks;
};

static void wait_start(struct ata_wait *w)
{
    w->start = tsc_per_ms ? rdtsc() : 0;
    w->looks = 0;
}

static int wait_over(struct ata_wait *w)
{
    if (tsc_per_ms)
        return rdtsc() - w->start > (uint64_t)ATA_WAIT_MS * tsc_per_ms;
    return ++w->looks > 1000000;
}

int ata_present(void)
{
    /* An empty bus returns 0xFF (the lines held high) */
    if (inb(ATA_STATUS) == 0xFF)
        return 0;
    ata_calibrate();
    return 1;
}

/* Which of the two drives on the bus the commands are for. One bit in
   the drive register, and it has to be set before every command - so
   it is kept here rather than passed through every call. */
static int unit;

void ata_select(int u)
{
    unit = u ? 1 : 0;
}

int ata_selected(void)
{
    return unit;
}

static int ata_wait_drq(void);

/* Disk capacity: the IDENTIFY DEVICE command returns 256 descriptor
   words, of which words 60-61 are the number of LBA28 addressable
   sectors. */
uint32_t ata_sectors(void)
{
    static uint32_t cached[2];
    if (cached[unit])
        return cached[unit];
    if (!ata_present())
        return 0;

    outb(ATA_DRIVE, (uint8_t)(0xA0 | (unit << 4)));  /* the chosen drive */
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0)           /* no disk */
        return 0;
    if (ata_wait_drq() < 0)
        return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_DATA);

    cached[unit] = ((uint32_t)id[61] << 16) | id[60];
    return cached[unit];
}

/* Reading alt-status 4x gives the required ~400 ns for BSY to settle
   after selecting a drive or issuing a command (and does not clear the
   IRQ request) */
static void ata_delay(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_ALTSTATUS);
}

/* Waits for BSY to fall and DRQ to rise; -1 when the controller
   reports an error */
static int ata_wait_drq(void)
{
    struct ata_wait w;
    wait_start(&w);
    do {
        uint8_t st = inb(ATA_STATUS);
        if (st & (ST_ERR | 0x20))   /* ERR or DF (device fault) */
            return -1;
        if (!(st & ST_BSY) && (st & ST_DRQ))
            return 0;
    } while (!wait_over(&w));
    return -1;
}

static int ata_wait_not_busy(void)
{
    struct ata_wait w;
    wait_start(&w);
    do {
        if (!(inb(ATA_STATUS) & ST_BSY))
            return 0;
    } while (!wait_over(&w));
    return -1;
}

/* Selecting the drive plus loading the LBA28 address, sector count =
   1. The drive selection MUST precede the command, and one has to wait
   after it. */
static int ata_setup(uint32_t lba)
{
    outb(ATA_DRIVE, (uint8_t)(0xE0 | (unit << 4) | ((lba >> 24) & 0x0F)));
    ata_delay();
    if (ata_wait_not_busy() < 0)
        return -1;
    outb(ATA_FEATURES, 0x00);
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HI, (lba >> 16) & 0xFF);
    return 0;
}

int ata_read_sector(uint32_t lba, void *buf)
{
    if (ata_setup(lba) < 0)
        return -1;
    outb(ATA_CMD, CMD_READ);
    ata_delay();

    if (ata_wait_drq() < 0)
        return -1;

    uint16_t *out = buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
        out[i] = inw(ATA_DATA);
    return 0;
}

int ata_write_sector(uint32_t lba, const void *buf)
{
    if (ata_setup(lba) < 0)
        return -1;
    outb(ATA_CMD, CMD_WRITE);
    ata_delay();

    if (ata_wait_drq() < 0)
        return -1;

    const uint16_t *in = buf;
    for (int i = 0; i < ATA_SECTOR_SIZE / 2; i++)
        outw(ATA_DATA, in[i]);

    /* Force the controller's buffer onto the platter - without this a
       write may not survive a restart */
    outb(ATA_CMD, CMD_FLUSH);
    ata_delay();
    return ata_wait_not_busy();
}
