/* Kontroler USB (UHCI)
 *
 * Sterowanie idzie przez porty I/O, a przesylki opisuje sie strukturami
 * w pamieci: deskryptor przesylki (TD) mowi, w ktora strone i ile
 * bajtow, kolejka (QH) laczy je w ciag, a lista ramek - 1024 wskazniki
 * czytane przez kontroler co milisekunde - wskazuje na te kolejke.
 *
 * Czekamy odpytywaniem, bez przerwan. Klawiature odpytuje i tak watek
 * co kilkanascie milisekund, a brak przerwan oznacza, ze ten sam kod
 * zadziala rowniez tam, gdzie przerwania sa wylaczone.
 *
 * Wszystkie struktury musza lezec w pamieci ciaglej fizycznie i pod
 * adresem, ktory kontroler rozumie wprost - a wiec w mapowaniu
 * identycznosciowym, gdzie adres wirtualny rowna sie fizycznemu.
 */

#include <stdint.h>
#include "include/uhci.h"
#include "include/pci.h"
#include "include/pmm.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"

/* Rejestry (przesuniecia od bazy portow I/O) */
#define REG_CMD      0x00
#define REG_STS      0x02
#define REG_INTR     0x04
#define REG_FRNUM    0x06
#define REG_FLBASE   0x08
#define REG_SOF      0x0C
#define REG_PORTSC1  0x10

#define CMD_RS       0x0001         /* kontroler pracuje */
#define CMD_HCRESET  0x0002
#define CMD_GRESET   0x0004
#define CMD_MAXP     0x0080         /* pakiety do 64 B */

#define STS_HALTED   0x0020         /* kontroler stanal */

#define PORT_CONNECT     0x0001
#define PORT_CONNECT_CHG 0x0002
#define PORT_ENABLE      0x0004
#define PORT_ENABLE_CHG  0x0008
#define PORT_LOWSPEED    0x0100
#define PORT_RESET       0x0200

/* Deskryptor przesylki - 32 B, wyrownany do 16 */
struct td {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t pad[4];
} __attribute__((packed, aligned(16)));

struct qh {
    volatile uint32_t head;
    volatile uint32_t element;
    uint32_t pad[2];
} __attribute__((packed, aligned(16)));

#define LINK_TERMINATE 0x00000001u
#define LINK_QH        0x00000002u

#define TD_ACTIVE      (1u << 23)
#define TD_STALLED     (1u << 22)
#define TD_LOWSPEED    (1u << 26)
#define TD_ERRCNT      (3u << 27)
#define TD_ACTLEN_MASK 0x7FFu

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

#define TD_MAX 16                   /* dosc na przesylke sterujaca */

static uint16_t base;               /* baza portow I/O kontrolera */
static int nports;
static uint32_t *frame_list;
static struct qh *queue;
static struct td *tds;
static int armed;          /* czy deskryptor odczytu czeka w kolejce */

static uint16_t rd16(int off)        { return inw(base + off); }
static void     wr16(int off, uint16_t v) { outw(base + off, v); }
static void     wr32(int off, uint32_t v) { outl(base + off, v); }

/* Kontroler potrzebuje czasu na reset portu; PIT tyka co 10 ms */
static void wait_ms(uint32_t ms)
{
    uint32_t until = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < until)
        io_wait();
}

int uhci_ports(void)
{
    return nports;
}

int uhci_port_lowspeed(int port)
{
    return (rd16(REG_PORTSC1 + port * 2) & PORT_LOWSPEED) ? 1 : 0;
}

int uhci_port_reset(int port)
{
    int off = REG_PORTSC1 + port * 2;
    if (!(rd16(off) & PORT_CONNECT))
        return 0;

    wr16(off, PORT_RESET);
    wait_ms(50);
    wr16(off, 0);
    wait_ms(10);

    /* Wlaczenie portu potrafi wymagac kilku prob - urzadzenie dochodzi
       do siebie po resecie we wlasnym tempie */
    for (int i = 0; i < 10; i++) {
        uint16_t st = rd16(off);
        if (!(st & PORT_CONNECT))
            return 0;
        if (st & (PORT_CONNECT_CHG | PORT_ENABLE_CHG)) {
            wr16(off, st & ~PORT_RESET);   /* skasuj zgloszenia zmiany */
            continue;
        }
        if (st & PORT_ENABLE)
            return 1;
        wr16(off, st | PORT_ENABLE);
        wait_ms(10);
    }
    return 0;
}

/* Sklada ciag deskryptorow i czeka, az kontroler go przerobi.
   Zwraca 0 albo -1; 'actlen' dostaje liczbe przeniesionych bajtow. */
static int run_chain(int count, uint32_t *actlen)
{
    for (int i = 0; i < count; i++)
        tds[i].link = (i + 1 < count) ? ((uint32_t)&tds[i + 1] | 0x4)
                                      : LINK_TERMINATE;

    queue->element = (uint32_t)&tds[0];

    uint32_t total = 0;
    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        int done = 1;
        for (int i = 0; i < count; i++) {
            uint32_t st = tds[i].status;
            if (st & TD_ACTIVE) {
                done = 0;
                break;
            }
            if (st & TD_STALLED) {
                queue->element = LINK_TERMINATE;
                return -1;
            }
        }
        if (done)
            break;
    }

    queue->element = LINK_TERMINATE;
    for (int i = 0; i < count; i++) {
        if (tds[i].status & TD_ACTIVE)
            return -1;                    /* nie doczekalismy sie */
        if ((tds[i].token & 0xFF) != PID_SETUP) {
            uint32_t len = (tds[i].status + 1) & TD_ACTLEN_MASK;
            total += len;
        }
    }
    if (actlen)
        *actlen = total;
    return 0;
}

static void fill_td(int i, int pid, int addr, int endpoint, int toggle,
                    int len, void *buf, int lowspeed)
{
    /* Domyslnie ostatni w lancuchu. Bez tego pole zostaje z zerem, a
       zero to dla kontrolera POPRAWNY wskaznik na adres 0 - poszedlby
       tam po nastepny deskryptor, wzial smieci za polecenie i sie
       zatrzymal. run_chain podmienia to na wskaznik do nastepnego. */
    tds[i].link = LINK_TERMINATE;
    tds[i].status = TD_ACTIVE | TD_ERRCNT | (lowspeed ? TD_LOWSPEED : 0);
    tds[i].token = (uint32_t)pid | ((uint32_t)addr << 8) |
                   ((uint32_t)endpoint << 15) | ((uint32_t)toggle << 19) |
                   ((uint32_t)((len ? len - 1 : 0x7FF) & 0x7FF) << 21);
    tds[i].buffer = (uint32_t)buf;
}

int uhci_control(int addr, int lowspeed, int maxpacket,
                 const void *setup, void *data, int len, int in)
{
    if (!base || maxpacket <= 0)
        return -1;

    /* Zapytanie idzie z bufora, ktory musi widziec kontroler */
    static uint8_t setup_buf[8] __attribute__((aligned(16)));
    memcpy(setup_buf, setup, 8);

    int n = 0;
    fill_td(n++, PID_SETUP, addr, 0, 0, 8, setup_buf, lowspeed);

    int toggle = 1;
    int left = len;
    uint8_t *p = data;
    while (left > 0 && n < TD_MAX - 1) {
        int chunk = left > maxpacket ? maxpacket : left;
        fill_td(n++, in ? PID_IN : PID_OUT, addr, 0, toggle, chunk, p,
                lowspeed);
        toggle ^= 1;
        p += chunk;
        left -= chunk;
    }

    /* Faza potwierdzenia idzie w druga strone niz dane */
    fill_td(n++, in ? PID_OUT : PID_IN, addr, 0, 1, 0, (void *)0, lowspeed);

    uint32_t got = 0;
    if (run_chain(n, &got) < 0)
        return -1;
    return (int)got;
}

/* Odczyt z punktu koncowego przerwaniowego.
 *
 * Deskryptor uzbrajamy raz i NIGDY go nie porzucamy. To sedno: gdyby
 * przy braku danych zdejmowac go z kolejki, kontroler moglby dokonczyc
 * przesylke juz po naszej decyzji. Raport wpadlby wtedy do bufora,
 * ktorego nikt nie czyta, a urzadzenie przestawiloby przelacznik
 * danych - i od tej chwili nie zgadzalby sie z naszym juz na zawsze.
 * Objawialo sie to tak, jak takie bledy zwykle: dzialalo, dzialalo,
 * po czym klawiatura milkla na dobre.
 *
 * Uzbrojony deskryptor kontroler ponawia sam co ramke, dopoki
 * urzadzenie nie ma nic do powiedzenia. My tylko zagladamy, czy juz
 * sie skonczyl - i nie czekamy ani chwili. Raporty odbieramy wiec po
 * jednym na obieg, ale zaden nie ginie.
 */
int uhci_interrupt_in(int addr, int lowspeed, int endpoint, int maxpacket,
                      void *data, int len, int *toggle)
{
    if (!base || len <= 0 || len > maxpacket)
        return -1;

    /* Gdyby kontroler sie zatrzymal (blad przetwarzania listy),
       sam wypis o tym nikomu nie pomoze - podnosimy go z powrotem */
    if (rd16(REG_STS) & STS_HALTED) {
        wr16(REG_STS, 0x3F);
        wr16(REG_CMD, CMD_RS | CMD_MAXP);
        armed = 0;
    }

    if (!armed) {
        fill_td(0, PID_IN, addr, endpoint, toggle ? *toggle : 0, len, data,
                lowspeed);
        queue->element = (uint32_t)&tds[0];
        armed = 1;
        return 0;                        /* odpowiedz przyjdzie za chwile */
    }

    uint32_t st = tds[0].status;
    if (st & TD_ACTIVE)
        return 0;                        /* urzadzenie milczy - czekamy dalej */

    /* Deskryptor zuzyty: kontroler sam przestawil juz wskaznik kolejki
       na to, co bylo za nim, czyli na koniec listy */
    armed = 0;
    if (st & TD_STALLED)
        return -1;
    if (toggle)
        *toggle ^= 1;
    return (int)((st + 1) & TD_ACTLEN_MASK);
}

int uhci_init(void)
{
    struct pci_dev dev;
    /* Klasa 0x0C (magistrala szeregowa), podklasa 0x03 (USB),
       interfejs 0x00 (UHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x00, &dev))
        return 0;

    /* UHCI siedzi w przestrzeni portow I/O - u niego to BAR4 */
    if (!(dev.bar[4] & 0x01))
        return 0;
    base = (uint16_t)(dev.bar[4] & 0xFFFC);
    if (!base)
        return 0;

    pci_enable_bus_master(&dev);
    /* Wylacz przechwytywanie starszego typu (BIOS udaje klawiature PS/2
       na urzadzeniu USB) - inaczej kontroler nie jest nasz */
    pci_config_write(dev.bus, dev.slot, dev.func, 0xC0, 0x8F00);

    wr16(REG_CMD, CMD_HCRESET);
    for (int i = 0; i < 100 && (rd16(REG_CMD) & CMD_HCRESET); i++)
        io_wait();
    wr16(REG_INTR, 0);                   /* bez przerwan - odpytujemy */

    uint32_t mem = pmm_alloc_frame();
    uint32_t work = pmm_alloc_frame();
    if (!mem || !work) {
        base = 0;
        return 0;
    }
    frame_list = (uint32_t *)mem;
    queue = (struct qh *)work;
    tds = (struct td *)(work + 64);
    memset((void *)work, 0, PAGE_SIZE);

    queue->head = LINK_TERMINATE;
    queue->element = LINK_TERMINATE;

    /* Ta sama kolejka w kazdej ramce: kontroler zaglada do niej co
       milisekunde, a my wkladamy tam przesylke, gdy jest co wlozyc */
    for (int i = 0; i < 1024; i++)
        frame_list[i] = (uint32_t)queue | LINK_QH;

    wr32(REG_FLBASE, mem);
    wr16(REG_FRNUM, 0);
    outb(base + REG_SOF, 0x40);
    wr16(REG_STS, 0x3F);                 /* skasuj stare zgloszenia */
    wr16(REG_CMD, CMD_RS | CMD_MAXP);

    nports = 2;                          /* UHCI ma zawsze dwa porty */
    return 1;
}
