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
struct polecenie {
    uint8_t opcode;
    uint8_t flagi;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rezerwa;
    uint64_t metadane;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed));

/* Odpowiedz: 16 bajtow, z bitem fazy w polu stanu */
struct ukonczenie {
    uint32_t wynik;
    uint32_t rezerwa;
    uint16_t glowa_sq;
    uint16_t id_sq;
    uint16_t cid;
    uint16_t stan;             /* bit 0 = faza, bity 1-15 = kod bledu */
} __attribute__((packed));

/* Kolejki i bufor musza lezec w pamieci ciaglej fizycznie i byc
   wyrownane do strony - kontroler siega po nie sam, a adresy podaje
   sie mu wskaznikami PRP, ktore opisuja strony, a nie dowolne
   kawalki. */
static struct polecenie adm_sq[QSIZE] __attribute__((aligned(4096)));
static struct ukonczenie adm_cq[QSIZE] __attribute__((aligned(4096)));
static struct polecenie io_sq[QSIZE] __attribute__((aligned(4096)));
static struct ukonczenie io_cq[QSIZE] __attribute__((aligned(4096)));

/* Bufor posredniczacy. Wskaznik PRP opisuje strone, a wolajacy podaje
   nam bufory lezace gdziekolwiek - przepisanie przez wlasna, wyrownana
   strone jest tansze niz tlumaczenie kazdego takiego bufora na liste
   stron. Osiem sektorow to dokladnie jedna strona. */
static uint8_t bufor[4096] __attribute__((aligned(4096)));
#define BUF_SEKTOROW (4096 / SECTOR)

static volatile uint8_t *baza;
static int present;
static uint32_t sektorow;
static uint32_t db_krok;        /* odstep miedzy dzwonkami */

static uint16_t adm_ogon, io_ogon;
static uint16_t adm_glowa, io_glowa;
static uint8_t adm_faza = 1, io_faza = 1;
static uint16_t nastepny_cid = 1;

static uint32_t rd32(uint32_t reg)
{
    return *(volatile uint32_t *)(baza + reg);
}

static void wr32(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(baza + reg) = v;
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
static void dzwon_sq(int qid, uint16_t ogon)
{
    wr32(REG_DB + (uint32_t)(2 * qid) * db_krok, ogon);
}

static void dzwon_cq(int qid, uint16_t glowa)
{
    wr32(REG_DB + (uint32_t)(2 * qid + 1) * db_krok, glowa);
}

/* Wklada polecenie i czeka na odpowiedz. Zwraca kod bledu (0 = dobrze)
   albo -1, gdy kontroler w ogole nie odpowiedzial. */
static int wykonaj(struct polecenie *sq, struct ukonczenie *cq, int qid,
                   uint16_t *ogon, uint16_t *glowa, uint8_t *faza,
                   const struct polecenie *co)
{
    uint16_t cid = nastepny_cid++;
    sq[*ogon] = *co;
    sq[*ogon].cid = cid;

    *ogon = (uint16_t)((*ogon + 1) % QSIZE);
    dzwon_sq(qid, *ogon);

    for (int spin = 0; spin < 20000000; spin++) {
        volatile struct ukonczenie *u = &cq[*glowa];
        if ((u->stan & 1) != *faza)
            continue;

        int kod = (u->stan >> 1) & 0x7FF;
        uint16_t wrocil = u->cid;

        *glowa = (uint16_t)((*glowa + 1) % QSIZE);
        if (*glowa == 0)
            *faza ^= 1;            /* okrazenie odwraca faze */
        dzwon_cq(qid, *glowa);

        if (wrocil != cid)
            return -1;             /* odpowiedz nie na to pytanie */
        return kod;
    }
    return -1;
}

static int adm(const struct polecenie *co)
{
    return wykonaj(adm_sq, adm_cq, 0, &adm_ogon, &adm_glowa, &adm_faza, co);
}

static int io(const struct polecenie *co)
{
    return wykonaj(io_sq, io_cq, IO_QID, &io_ogon, &io_glowa, &io_faza, co);
}

int nvme_present(void)      { return present; }
uint32_t nvme_sectors(void) { return sektorow; }

/* Jedna partia: najwyzej strona danych, bo tyle opisuje jeden PRP */
static int partia(uint32_t lba, uint32_t count, int zapis)
{
    struct polecenie p;
    memset(&p, 0, sizeof(p));
    p.opcode = zapis ? IO_WRITE : IO_READ;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)bufor;
    p.cdw10 = lba;                 /* mlodsze 32 bity adresu bloku */
    p.cdw11 = 0;
    p.cdw12 = count - 1;           /* liczba blokow, liczona od zera */
    return io(&p) == 0 ? 0 : -1;
}

int nvme_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!present)
        return -1;
    uint8_t *cel = buf;

    while (count) {
        uint32_t ile = count > BUF_SEKTOROW ? BUF_SEKTOROW : count;
        if (partia(lba, ile, 0) < 0)
            return -1;
        memcpy(cel, bufor, ile * SECTOR);
        cel += ile * SECTOR;
        lba += ile;
        count -= ile;
    }
    return 0;
}

int nvme_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!present)
        return -1;
    const uint8_t *zrodlo = buf;

    while (count) {
        uint32_t ile = count > BUF_SEKTOROW ? BUF_SEKTOROW : count;
        memcpy(bufor, zrodlo, ile * SECTOR);
        if (partia(lba, ile, 1) < 0)
            return -1;
        zrodlo += ile * SECTOR;
        lba += ile;
        count -= ile;
    }
    return 0;
}

/* Pyta o opis przestrzeni nazw numer 1: ile ma blokow i jak duzych */
static int poznaj_dysk(void)
{
    struct polecenie p;
    memset(&p, 0, sizeof(p));
    p.opcode = ADM_IDENTIFY;
    p.nsid = 1;
    p.prp1 = (uint64_t)(uintptr_t)bufor;
    p.cdw10 = 0;                   /* opis przestrzeni nazw */
    if (adm(&p) != 0)
        return 0;

    uint64_t blokow = 0;
    for (int i = 0; i < 8; i++)
        blokow |= (uint64_t)bufor[i] << (8 * i);

    /* Rozmiar bloku siedzi w tablicy formatow: numer uzywanego formatu
       jest w bajcie 26, a sam format - cztery bajty - od 128. Jego
       bajt 2 mowi, ile to bitow adresu w bloku (potega dwojki). */
    uint32_t format = bufor[26] & 0x0F;
    uint32_t bity = bufor[128 + format * 4 + 2];
    uint32_t rozmiar = bity < 32 ? (1u << bity) : 0;

    if (rozmiar != SECTOR) {
        printk("[ !! ] NVMe: blok %u B, a system pracuje na %u B\n",
               rozmiar, (uint32_t)SECTOR);
        return 0;
    }
    if (!blokow)
        return 0;

    sektorow = blokow > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)blokow;
    return 1;
}

/* Zamawia pare kolejek na dane. Ukonczen najpierw - kolejka zgloszen
   musi wiedziec, dokad odsylac odpowiedzi. */
static int zaloz_kolejki(void)
{
    struct polecenie p;

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
    baza = (volatile uint8_t *)(uintptr_t)mmio;

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
    adm_ogon = adm_glowa = io_ogon = io_glowa = 0;
    adm_faza = io_faza = 1;

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

    if (!poznaj_dysk() || !zaloz_kolejki())
        return 0;

    present = 1;
    return 1;
}
