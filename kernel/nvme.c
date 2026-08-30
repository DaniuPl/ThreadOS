/* Sterownik NVMe - dysk podlaczony wprost do PCI Express
 *
 * Cale dziedzictwo ATA tu znika. Nie ma rejestru "wybierz dysk", nie
 * ma ramek FIS, nie ma nawet pojecia sektora w sensie sprzetowym -
 * jest za to para KOLEJEK lezacych w pamieci:
 *
 *   kolejka zgloszen (SQ)  - my wkladamy tu polecenia po 64 bajty,
 *   kolejka ukonczen (CQ)  - kontroler odklada tu odpowiedzi po 16.
 *
 * Kto gdzie doszedl, mowia dwa wskazniki na kolejke: ogon zgloszen
 * przesuwamy my, glowe ukonczen tez my - oba przez "dzwonki"
 * (rejestry doorbell) w obszarze rejestrow kontrolera. Ze odpowiedz
 * jest nowa, poznajemy po BICIE FAZY: kontroler odwraca go przy
 * kazdym okrazeniu kolejki, wiec nie trzeba niczego zerowac.
 *
 * Kolejek jest u nas dwie pary: administracyjna, ktora tworzy sie
 * sama przy starcie kontrolera, i jedna do danych, ktora zamawiamy
 * poleceniem na tej pierwszej.
 *
 * Czekamy odpytywaniem, bez przerwan - z tego samego powodu, co przy
 * AHCI: dysk obslugujemy takze z wnetrza obslugi bledu strony, gdzie
 * przerwania sa wylaczone i nikt nas nie obudzi.
 */

#include <stdint.h>
#include "include/nvme.h"
#include "include/pci.h"
#include "include/printk.h"
#include "include/string.h"

/* Rejestry kontrolera leza wysoko, poza pamiecia opisana z gory -
   doklejenie ich do mapowania robi w kazdym trybie inny kod i to
   jedyne miejsce w tym pliku, ktore o trybie w ogole wie. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n) paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n) paging_map_phys((a), (n))
#endif

#define SECTOR 512

/* Rejestry kontrolera (przesuniecia od bazy) */
#define REG_CAP    0x00        /* mozliwosci, 64 bity */
#define REG_CC     0x14        /* konfiguracja */
#define REG_CSTS   0x1C        /* stan */
#define REG_AQA    0x24        /* rozmiary kolejek administracyjnych */
#define REG_ASQ    0x28        /* adres kolejki zgloszen admin, 64 bity */
#define REG_ACQ    0x30        /* adres kolejki ukonczen admin, 64 bity */
#define REG_DB     0x1000      /* pierwszy dzwonek */

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)   /* kontroler sie poddal */

/* Polecenia administracyjne */
#define ADM_CREATE_SQ 0x01
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY  0x06

/* Polecenia danych */
#define IO_WRITE 0x01
#define IO_READ  0x02

#define QSIZE     8            /* pozycji w kazdej kolejce */
#define IO_QID    1

/* Polecenie: 64 bajty o ukladzie narzuconym przez standard */
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

/* Odpowiedz: 16 bajtow, z bitem fazy w polu stanu */
struct completion {
    uint32_t result;
    uint32_t reserve;
    uint16_t sq_head;
    uint16_t id_sq;
    uint16_t cid;
    uint16_t stan;             /* bit 0 = faza, bity 1-15 = kod bledu */
} __attribute__((packed));

/* Kolejki i bufor musza lezec w pamieci ciaglej fizycznie i byc
   wyrownane do strony - kontroler siega po nie sam, a adresy podaje
   sie mu wskaznikami PRP, ktore opisuja strony, a nie dowolne
   kawalki. */
static struct command adm_sq[QSIZE] __attribute__((aligned(4096)));
static struct completion adm_cq[QSIZE] __attribute__((aligned(4096)));
static struct command io_sq[QSIZE] __attribute__((aligned(4096)));
static struct completion io_cq[QSIZE] __attribute__((aligned(4096)));

/* Bufor posredniczacy. Wskaznik PRP opisuje strone, a wolajacy podaje
   nam bufory lezace gdziekolwiek - przepisanie przez wlasna, wyrownana
   strone jest tansze niz tlumaczenie kazdego takiego bufora na liste
   stron. Osiem sektorow to dokladnie jedna strona. */
static uint8_t dma_buf[4096] __attribute__((aligned(4096)));
#define BUF_SECTORS (4096 / SECTOR)

static volatile uint8_t *base;
static int present;
static uint32_t nsectors;
static uint32_t db_krok;        /* odstep miedzy dzwonkami */

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

/* Dzwonek: dwa na kolejke (ogon zgloszen, glowa ukonczen) */
static void doorbell_sq(int qid, uint16_t tail)
{
    wr32(REG_DB + (uint32_t)(2 * qid) * db_krok, tail);
}

static void doorbell_cq(int qid, uint16_t head)
{
    wr32(REG_DB + (uint32_t)(2 * qid + 1) * db_krok, head);
}

/* Wklada polecenie i czeka na odpowiedz. Zwraca kod bledu (0 = dobrze)
   albo -1, gdy kontroler w ogole nie odpowiedzial. */
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
            *phase ^= 1;            /* okrazenie odwraca faze */
        doorbell_cq(qid, *head);

        if (returned != cid)
            return -1;             /* odpowiedz nie na to pytanie */
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

/* Jedna partia: najwyzej strona danych, bo tyle opisuje jeden PRP */
static int batch(uint32_t lba, uint32_t count, int write)
{
    struct command p;
    memset(&p, 0, sizeof(p));
    p.opcode = write ? IO_WRITE : IO_READ;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)dma_buf;
    p.cdw10 = lba;                 /* mlodsze 32 bity adresu bloku */
    p.cdw11 = 0;
    p.cdw12 = count - 1;           /* liczba blokow, liczona od zera */
    return io(&p) == 0 ? 0 : -1;
}

int nvme_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!present)
        return -1;
    uint8_t *dst = buf;

    while (count) {
        uint32_t ile = count > BUF_SECTORS ? BUF_SECTORS : count;
        if (batch(lba, ile, 0) < 0)
            return -1;
        memcpy(dst, dma_buf, ile * SECTOR);
        dst += ile * SECTOR;
        lba += ile;
        count -= ile;
    }
    return 0;
}

int nvme_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!present)
        return -1;
    const uint8_t *src = buf;

    while (count) {
        uint32_t ile = count > BUF_SECTORS ? BUF_SECTORS : count;
        memcpy(dma_buf, src, ile * SECTOR);
        if (batch(lba, ile, 1) < 0)
            return -1;
        src += ile * SECTOR;
        lba += ile;
        count -= ile;
    }
    return 0;
}

/* Pyta o opis przestrzeni nazw numer 1: ile ma blokow i jak duzych */
static int identify_disk(void)
{
    struct command p;
    memset(&p, 0, sizeof(p));
    p.opcode = ADM_IDENTIFY;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)dma_buf;
    p.cdw10 = 0;                   /* opis przestrzeni nazw */
    if (adm(&p) != 0)
        return 0;

    uint64_t nblocks = 0;
    for (int i = 0; i < 8; i++)
        nblocks |= (uint64_t)dma_buf[i] << (8 * i);

    /* Rozmiar bloku siedzi w tablicy formatow: numer uzywanego formatu
       jest w bajcie 26, a sam format - cztery bajty - od 128. Jego
       bajt 2 mowi, ile to bitow adresu w bloku (potega dwojki). */
    uint32_t format = dma_buf[26] & 0x0F;
    uint32_t bits = dma_buf[128 + format * 4 + 2];
    uint32_t size = bits < 32 ? (1u << bits) : 0;

    if (size != SECTOR) {
        printk("[ !! ] NVMe: blok %u B, a system pracuje na %u B\n",
               size, (uint32_t)SECTOR);
        return 0;
    }
    if (!nblocks)
        return 0;

    nsectors = nblocks > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)nblocks;
    return 1;
}

/* Zamawia pare kolejek na dane. Ukonczen najpierw - kolejka zgloszen
   musi wiedziec, dokad odsylac odpowiedzi. */
static int setup_queues(void)
{
    struct command p;

    memset(&p, 0, sizeof(p));
    p.opcode = ADM_CREATE_CQ;
    p.prp1 = (uint64_t)(uintptr_t)io_cq;
    p.cdw10 = (uint32_t)IO_QID | ((QSIZE - 1) << 16);
    p.cdw11 = 1;                   /* ciagla w pamieci, bez przerwan */
    if (adm(&p) != 0)
        return 0;

    memset(&p, 0, sizeof(p));
    p.opcode = ADM_CREATE_SQ;
    p.prp1 = (uint64_t)(uintptr_t)io_sq;
    p.cdw10 = (uint32_t)IO_QID | ((QSIZE - 1) << 16);
    p.cdw11 = 1 | ((uint32_t)IO_QID << 16);   /* ciagla, odpowiedzi do CQ */
    return adm(&p) == 0;
}

int nvme_init(void)
{
    struct pci_dev dev;
    /* Klasa 01/08/02: pamiec masowa, kontroler NVM, interfejs NVMe */
    if (!pci_find_class(0x01, 0x08, 0x02, &dev))
        return 0;
    if (dev.bar0 & 0x01)           /* oczekujemy BAR-a pamieciowego */
        return 0;

    uint64_t mmio = (uint64_t)(dev.bar0 & ~0xFu);
    if ((dev.bar0 & 0x06) == 0x04)  /* BAR 64-bitowy: gorna polowa w bar1 */
        mmio |= (uint64_t)dev.bar[1] << 32;
    if (!mmio || MAP_PHYS(mmio, 0x2000) < 0)
        return 0;

    pci_enable_bus_master(&dev);
    base = (volatile uint8_t *)(uintptr_t)mmio;

    uint64_t cap = rd64(REG_CAP);
    db_krok = 4u << ((uint32_t)(cap >> 32) & 0x0F);   /* CAP.DSTRD */

    /* Kontroler trzeba zatrzymac, zanim da sie mu cokolwiek ustawic */
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

    /* Strony 4 KiB, polecenia po 64 B, odpowiedzi po 16 B */
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
