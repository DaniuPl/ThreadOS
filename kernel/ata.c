/* Sterownik dysku ATA (IDE) w trybie PIO z odpytywaniem
 *
 * Obsluguje pierwsza magistrale (porty 0x1F0-0x1F7), dysk master,
 * adresowanie LBA28. To najprostszy mozliwy sterownik: bez przerwan,
 * bez DMA - czytamy/zapisujemy 256 slow przez port danych, czekajac
 * w petli az kontroler zglosi gotowosc. Dla zabawkowego systemu i
 * dyskow QEMU w zupelnosci wystarczy.
 */

#include "include/ata.h"
#include "include/io.h"

#define ATA_DATA      0x1F0
#define ATA_FEATURES  0x1F1
#define ATA_SECCOUNT  0x1F2
#define ATA_LBA_LO    0x1F3
#define ATA_LBA_MID   0x1F4
#define ATA_LBA_HI    0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_STATUS    0x1F7   /* odczyt: status; zapis: komenda */
#define ATA_CMD       0x1F7
#define ATA_ALTSTATUS 0x3F6   /* status bez kasowania zadania przerwania */

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define CMD_READ  0x20
#define CMD_WRITE 0x30
#define CMD_FLUSH 0xE7

int ata_present(void)
{
    /* Pusta magistrala zwraca 0xFF (linie w stanie wysokim) */
    return inb(ATA_STATUS) != 0xFF;
}

/* Odczyt alt-status 4x daje wymagane ~400 ns na ustabilizowanie BSY
   po wyborze dysku lub wydaniu komendy (i nie kasuje zadania IRQ) */
static void ata_delay(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_ALTSTATUS);
}

/* Czeka az BSY opadnie i DRQ wstanie; -1 gdy kontroler zglosi blad */
static int ata_wait_drq(void)
{
    for (int i = 0; i < 1000000; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st & (ST_ERR | 0x20))   /* ERR albo DF (device fault) */
            return -1;
        if (!(st & ST_BSY) && (st & ST_DRQ))
            return 0;
    }
    return -1;
}

static int ata_wait_not_busy(void)
{
    for (int i = 0; i < 1000000; i++)
        if (!(inb(ATA_STATUS) & ST_BSY))
            return 0;
    return -1;
}

/* Wybor dysku master + zaladowanie adresu LBA28, licznik sektorow = 1.
   Wybor dysku MUSI poprzedzac komende, a po nim trzeba odczekac. */
static int ata_setup(uint32_t lba)
{
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));  /* master, LBA */
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

    /* Wymus zrzut bufora kontrolera na talerz - bez tego zapis moze
       nie przetrwac restartu */
    outb(ATA_CMD, CMD_FLUSH);
    ata_delay();
    return ata_wait_not_busy();
}
