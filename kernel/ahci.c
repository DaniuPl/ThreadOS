/* Sterownik AHCI (SATA)
 *
 * Kontroler dostaje od nas liste polecen w pamieci i sam po nia siega
 * przez DMA - procesor nie przepisuje juz danych slowo po slowie, jak
 * przy ATA PIO. Na jedno polecenie skladaja sie trzy struktury:
 *
 *   naglowek polecenia  - w liscie 32 pozycji (rejestr CLB portu),
 *                         mowi gdzie lezy tablica polecenia i ile ma
 *                         opisow obszarow danych,
 *   tablica polecenia   - ramka FIS z rozkazem ATA plus tablica PRDT
 *                         (adres i dlugosc kazdego kawalka bufora),
 *   obszar odbiorczy FIS - tam kontroler odklada odpowiedzi (rejestr FB).
 *
 * Wszystko to musi lezec w pamieci ciaglej fizycznie i pod adresem,
 * ktory kontroler rozumie wprost - a wiec w naszym mapowaniu
 * identycznosciowym, gdzie adres wirtualny rowna sie fizycznemu.
 *
 * Czekamy odpytywaniem, bez przerwan: dysk obslugujemy takze z wnetrza
 * obslugi bledu strony (wymiana stron), gdzie przerwania sa wylaczone
 * i nikt nas nie obudzi.
 */

#include <stdint.h>
#include "include/ahci.h"
#include "include/pci.h"
#include "include/pmm.h"
#include "include/paging.h"
#include "include/printk.h"
#include "include/string.h"

#define SECTOR 512

/* Rejestry portu (0x100 + port * 0x80 od poczatku obszaru kontrolera) */
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
    uint16_t flags;              /* bity 0-4: dlugosc FIS w slowach, 6: zapis */
    uint16_t prdtl;              /* ile opisow obszarow danych */
    volatile uint32_t prdbc;     /* ile bajtow kontroler przeniosl */
    uint32_t ctba, ctbau;        /* adres tablicy polecenia */
    uint32_t rsv[4];
} __attribute__((packed));

struct prdt_entry {
    uint32_t dba, dbau, rsv;
    uint32_t dbc;                /* bity 0-21: liczba bajtow - 1 */
} __attribute__((packed));

#define PRDT_MAX 8

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct prdt_entry prdt[PRDT_MAX];
} __attribute__((packed));

/* Ramka rozkazu: host -> urzadzenie, rejestrowa */
struct fis_h2d {
    uint8_t type;                /* 0x27 */
    uint8_t pmport_c;            /* bit 7: to rozkaz, nie zmiana rejestrow */
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

#define PORT_CMD_ST    0x0001    /* start - kontroler przetwarza polecenia */
#define PORT_CMD_FRE   0x0010    /* odbieranie ramek FIS */
#define PORT_CMD_FR    0x4000    /* odbieranie faktycznie ruszylo */
#define PORT_CMD_CR    0x8000    /* przetwarzanie faktycznie ruszylo */
#define PORT_IS_TFES   0x40000000u  /* blad zglaszony przez urzadzenie */

#define SIG_SATA       0x00000101u

static struct hba_mem *hba;
static struct hba_port *port;
static struct cmd_header *cmd_list;
static struct cmd_table *cmd_tbl;
static uint32_t disk_sectors;

/* Kontroler potrafi czekac; nie mamy zegara z rozdzielczoscia mniejsza
   niz 10 ms, wiec odliczamy obrotami petli. Wartosci z duzym zapasem. */
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

/* Sklada polecenie w slocie 0 i czeka na jego koniec.
   'write' = kierunek, 'buf' musi byc ciagly fizycznie. */
static int run_command(uint8_t command, uint32_t lba, uint32_t count,
                       void *buf, uint32_t bytes, int write)
{
    if (!port || bytes == 0 || bytes > PRDT_MAX * 4u * 1024 * 1024)
        return -1;

    port->is = (uint32_t)-1;                 /* zgas stare zgloszenia */

    cmd_list[0].flags = (uint16_t)((sizeof(struct fis_h2d) / 4) |
                                   (write ? (1u << 6) : 0));
    cmd_list[0].prdtl = 1;
    cmd_list[0].prdbc = 0;
    cmd_list[0].ctba = (uint32_t)cmd_tbl;
    cmd_list[0].ctbau = 0;

    memset(cmd_tbl, 0, sizeof(*cmd_tbl));
    cmd_tbl->prdt[0].dba = (uint32_t)buf;
    cmd_tbl->prdt[0].dbau = 0;
    cmd_tbl->prdt[0].dbc = bytes - 1;        /* licznik liczony od zera */

    struct fis_h2d *fis = (struct fis_h2d *)cmd_tbl->cfis;
    fis->type = FIS_TYPE_H2D;
    fis->pmport_c = 0x80;
    fis->command = command;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->device = 0x40;                      /* tryb LBA */
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    /* Urzadzenie musi byc gotowe: ani zajete, ani z danymi do oddania */
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
        return -1;                           /* nie doczekalismy sie */
    return (port->is & PORT_IS_TFES) ? -1 : 0;
}

static void read_capacity(void)
{
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return;
    memset((void *)frame, 0, SECTOR);
    if (run_command(CMD_IDENTIFY, 0, 0, (void *)frame, SECTOR, 0) == 0) {
        const uint16_t *id = (const uint16_t *)frame;
        /* Slowa 100-103: pojemnosc LBA48. Starsza polowa nas nie
           obchodzi - i tak zarzadzamy dyskiem 32-bitowym adresem. */
        uint32_t lba48 = (uint32_t)id[100] | ((uint32_t)id[101] << 16);
        uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
        disk_sectors = lba48 ? lba48 : lba28;
    }
    pmm_free_frame(frame);
}

int ahci_init(void)
{
    struct pci_dev dev;
    /* Klasa 0x01 (pamiec masowa), podklasa 0x06 (SATA), interfejs 0x01 (AHCI) */
    if (!pci_find_class(0x01, 0x06, 0x01, &dev))
        return 0;

    uint32_t abar = dev.bar[5] & 0xFFFFFFF0u;
    if (!abar || (dev.bar[5] & 0x01))        /* BAR portowy - nie nasz przypadek */
        return 0;

    pci_enable_bus_master(&dev);
    if (paging_map_phys(abar, 0x2000) < 0)   /* rejestry lezą wysoko */
        return 0;

    hba = (struct hba_mem *)abar;
    hba->ghc |= (1u << 31);                  /* tryb AHCI, nie zgodnosci */

    /* Pierwszy port z podlaczonym dyskiem SATA */
    uint32_t implemented = hba->pi;
    for (int i = 0; i < 32; i++) {
        if (!(implemented & (1u << i)))
            continue;
        struct hba_port *p = &hba->ports[i];
        if ((p->ssts & 0x0F) != 3)           /* brak lacza z urzadzeniem */
            continue;
        if (p->sig != SIG_SATA)              /* np. naped optyczny */
            continue;

        port_stop(p);

        /* Jedna ramka na liste polecen (1 KiB) i obszar odbiorczy FIS
           (256 B) - ramka jest wyrownana do 4 KiB, wiec spelnia
           wymagania obu. Druga ramka na tablice polecenia. */
        uint32_t area = pmm_alloc_frame();
        uint32_t table = pmm_alloc_frame();
        if (!area || !table) {
            printk("[ !! ] AHCI: brak pamieci na struktury sterownika\n");
            return 0;
        }
        memset((void *)area, 0, PAGE_SIZE);
        memset((void *)table, 0, PAGE_SIZE);

        p->clb = area;
        p->clbu = 0;
        p->fb = area + 1024;
        p->fbu = 0;
        p->serr = (uint32_t)-1;              /* skasuj stare bledy lacza */
        p->is = (uint32_t)-1;

        port_start(p);

        port = p;
        cmd_list = (struct cmd_header *)area;
        cmd_tbl = (struct cmd_table *)table;
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
