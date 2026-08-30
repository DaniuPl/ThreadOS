/* Sterownik karty Intel 82540EM (e1000, PCI)
 *
 * Karta o pokolenie nowsza niz RTL8139 i pomyslana zupelnie inaczej.
 * Rejestry nie siedza w portach I/O, tylko w pamieci (BAR0), wiec
 * najpierw trzeba je dokleic do mapowania - leza wysoko, poza tym,
 * co jadro opisalo przy starcie.
 *
 * Ruch chodzi przez dwa PIERSCIENIE DESKRYPTOROW, po jednym na
 * kierunek. Deskryptor to szesnascie bajtow: adres bufora, dlugosc
 * i pole statusu. Kto ma deskryptor na wlasnosc, wynika ze wskaznikow
 * glowy i ogona - karta czyta od glowy (RDH/TDH) do ogona (RDT/TDT),
 * a my przesuwamy ogon, oddajac jej kolejne pozycje. Jest to
 * rozwiazanie o tyle wygodniejsze od pierscienia bajtow w RTL8139, ze
 * pakiety nie sklejaja sie ze soba i nie trzeba ich przepisywac
 * z konca bufora na poczatek.
 *
 * Adres MAC czytamy z pamieci EEPROM przez rejestr EERD; gdy karta go
 * stamtad nie oddaje (bywa tak w maszynach wirtualnych), bierzemy go
 * z rejestru filtra adresow, ktory firmware zwykle juz wypelnil.
 */

#include <stdint.h>
#include "include/e1000.h"
#include "include/pci.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/net.h"

/* Rejestry leza w pamieci, a ta w kazdym z jader opisana jest innym
   kodem - to jedyna rzecz w tym pliku zalezna od trybu procesora */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

#define E1000_VENDOR 0x8086
#define E1000_DEVICE 0x100E        /* 82540EM - ta, ktora udaje QEMU */

/* Rejestry (przesuniecia od bazy w pamieci) */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200        /* tablica filtra multicast (128 slow) */
#define REG_RAL      0x5400
#define REG_RAH      0x5404

#define CTRL_SLU     (1u << 6)     /* podnies lacze */
#define CTRL_ASDE    (1u << 5)     /* automatyczne wykrywanie predkosci */
#define CTRL_RST     (1u << 26)

#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)    /* przyjmuj rozglaszanie */
#define RCTL_SECRC   (1u << 26)    /* obetnij CRC - i tak go nie chcemy */

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)     /* dopelniaj krotkie ramki */

#define TXD_CMD_EOP  (1u << 0)     /* koniec pakietu */
#define TXD_CMD_IFCS (1u << 1)     /* dolicz CRC */
#define TXD_CMD_RS   (1u << 3)     /* zglos, gdy skonczysz */
#define TXD_STA_DD   (1u << 0)     /* karta oddala deskryptor */

#define RXD_STA_DD   (1u << 0)
#define RXD_STA_EOP  (1u << 1)

#define ICR_RXT0     (1u << 7)     /* przyszly ramki */

#define RX_DESC      32
#define TX_DESC      16
#define BUF_SIZE     2048

/* Deskryptory maja ustalony przez sprzet uklad pol - to karta je
   czyta i pisze, nie my */
struct rx_desc {
    uint64_t addr;
    uint16_t len;
    uint16_t sum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t len;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* Pierscienie i bufory w .bss: jadro siedzi w mapowaniu
   identycznosciowym, wiec ich adres wirtualny jest zarazem
   fizycznym, ktory podajemy karcie. Wyrownanie do szesnastu bajtow
   wymaga sprzet. */
static struct rx_desc rx_ring[RX_DESC] __attribute__((aligned(16)));
static struct tx_desc tx_ring[TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_bufs[RX_DESC][BUF_SIZE] __attribute__((aligned(16)));
static uint8_t tx_bufs[TX_DESC][BUF_SIZE] __attribute__((aligned(16)));

static volatile uint8_t *base;
static int present;
static uint8_t mac[6];
static uint32_t rx_packets, tx_packets;
static uint32_t rx_current;        /* ktory deskryptor ogladamy */
static uint32_t tx_next;

static void wr(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(base + reg) = value;
}

static uint32_t rd(uint32_t reg)
{
    return *(volatile uint32_t *)(base + reg);
}

int e1000_present(void)          { return present; }
const uint8_t *e1000_mac(void)   { return mac; }
uint32_t e1000_rx_packets(void)  { return rx_packets; }
uint32_t e1000_tx_packets(void)  { return tx_packets; }

/* Slowo z pamieci EEPROM. Zwraca -1, gdy karta nie odpowiada -
   niektore maszyny wirtualne nie udaja tej pamieci w ogole. */
static int32_t eeprom(uint8_t addr)
{
    wr(REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4))                     /* bit gotowosci */
            return (int32_t)((v >> 16) & 0xFFFF);
        io_wait();
    }
    return -1;
}

static void read_mac(void)
{
    int32_t w0 = eeprom(0);
    if (w0 >= 0) {
        int32_t w1 = eeprom(1), w2 = eeprom(2);
        if (w1 >= 0 && w2 >= 0) {
            mac[0] = (uint8_t)w0;
            mac[1] = (uint8_t)(w0 >> 8);
            mac[2] = (uint8_t)w1;
            mac[3] = (uint8_t)(w1 >> 8);
            mac[4] = (uint8_t)w2;
            mac[5] = (uint8_t)(w2 >> 8);
            return;
        }
    }

    /* Bez EEPROM-u zostaje filtr adresow - firmware zwykle wpisuje
       tam adres karty jeszcze przed nami */
    uint32_t low = rd(REG_RAL), high = rd(REG_RAH);
    mac[0] = (uint8_t)low;
    mac[1] = (uint8_t)(low >> 8);
    mac[2] = (uint8_t)(low >> 16);
    mac[3] = (uint8_t)(low >> 24);
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);
}

int e1000_send(const void *frame, uint16_t len)
{
    if (!present || len > BUF_SIZE)
        return -1;

    uint32_t flags = irq_save();
    uint32_t i = tx_next;

    /* Deskryptor jest nasz, gdy karta odeslala go z bitem DD. Swiezy,
       nigdy nieuzywany ma status zero i tez jest wolny - dlatego
       czekamy tylko wtedy, gdy cokolwiek w nim polozylismy. */
    if (tx_ring[i].len && !(tx_ring[i].status & TXD_STA_DD)) {
        int free_one = 0;
        for (int spin = 0; spin < 200000; spin++) {
            if (tx_ring[i].status & TXD_STA_DD) {
                free_one = 1;
                break;
            }
            io_wait();
        }
        if (!free_one) {
            irq_restore(flags);
            return -1;
        }
    }

    memcpy(tx_bufs[i], frame, len);
    tx_ring[i].addr = (uint64_t)(uintptr_t)tx_bufs[i];
    tx_ring[i].len = len;
    tx_ring[i].cso = 0;
    tx_ring[i].css = 0;
    tx_ring[i].special = 0;
    tx_ring[i].status = 0;
    /* Dopelnianie krotkich ramek i doliczanie CRC zalatwia karta -
       stad PSP w TCTL i bit IFCS tutaj */
    tx_ring[i].command = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    tx_next = (i + 1) % TX_DESC;
    wr(REG_TDT, tx_next);          /* przesuniecie ogona = "jedz" */
    tx_packets++;

    irq_restore(flags);
    return 0;
}

static void e1000_receive(void)
{
    while (rx_ring[rx_current].status & RXD_STA_DD) {
        struct rx_desc *d = &rx_ring[rx_current];
        uint16_t len = d->len;

        if ((d->status & RXD_STA_EOP) && !d->errors && len && len <= BUF_SIZE) {
            rx_packets++;
            net_rx(rx_bufs[rx_current], len);
        }

        d->status = 0;                 /* deskryptor znowu nasz */
        /* Ogon wskazuje OSTATNI oddany karcie, wiec zawsze o jeden
           w tyle za tym, ktory wlasnie ogladamy */
        wr(REG_RDT, rx_current);
        rx_current = (rx_current + 1) % RX_DESC;
    }
}

static void e1000_irq(struct regs *r)
{
    (void)r;
    uint32_t icr = rd(REG_ICR);        /* odczyt kasuje zgloszenia */
    if (icr & ICR_RXT0)
        e1000_receive();
}

int e1000_init(void)
{
    struct pci_dev dev;
    if (!pci_find_device(E1000_VENDOR, E1000_DEVICE, &dev))
        return 0;
    if (dev.bar0 & 0x01)               /* oczekujemy BAR-a pamieciowego */
        return 0;

    uint32_t mmio = dev.bar0 & ~0xFu;
    if (!mmio || MAP_PHYS(mmio, 0x20000) < 0)
        return 0;

    pci_enable_bus_master(&dev);
    base = (volatile uint8_t *)(uintptr_t)mmio;

    wr(REG_IMC, 0xFFFFFFFF);           /* cisza na czas ustawiania */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 100000 && (rd(REG_CTRL) & CTRL_RST); i++)
        io_wait();
    if (rd(REG_CTRL) & CTRL_RST)
        return 0;                      /* karta nie wstala */
    wr(REG_IMC, 0xFFFFFFFF);           /* reset odsloni przerwania */

    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    read_mac();

    /* Filtr multicast na zero: nie chcemy niczyjej grupy */
    for (int i = 0; i < 128; i++)
        wr(REG_MTA + i * 4, 0);

    /* Wlasny adres do filtra, z bitem waznosci (31) */
    wr(REG_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) |
                ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24));
    wr(REG_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));

    /* Pierscien odbioru: kazdy deskryptor od razu z buforem i oddany
       karcie. Ogon na ostatnim - wszystkie sa jej. */
    for (int i = 0; i < RX_DESC; i++) {
        rx_ring[i].addr = (uint64_t)(uintptr_t)rx_bufs[i];
        rx_ring[i].status = 0;
    }
    wr(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_DESC * (uint32_t)sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_DESC - 1);
    rx_current = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* bufor 2 KiB = 00 */

    /* Pierscien nadawania: pusty, ogon rowny glowie */
    memset(tx_ring, 0, sizeof(tx_ring));
    wr(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, TX_DESC * (uint32_t)sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_next = 0;
    /* Prog kolizji i odstep miedzy ramkami - wartosci z dokumentacji
       karty dla trybu pelnego dupleksu */
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));
    wr(REG_TIPG, 0x0060200A);

    wr(REG_IMS, ICR_RXT0);
    rd(REG_ICR);                       /* skasuj to, co juz sie zebralo */

    irq_install_handler(dev.irq, e1000_irq);
    pic_unmask(dev.irq);

    present = 1;
    return 1;
}
