/* Kontroler USB (xHCI)
 *
 * Trzeci kontroler USB i jedyny na nowszych plytach. Rozni sie od UHCI
 * nie szczegolami, tylko podzialem pracy.
 *
 * W UHCI sterownik ukladal deskryptory przesylek w liscie ramek, ktora
 * kontroler przemiatal co milisekunde, i sam nadawal urzadzeniom
 * adresy. Tu jest odwrotnie: z kontrolerem rozmawia sie POLECENIAMI
 * wkladanymi do pierscienia polecen, a odpowiedzi przychodza drugim
 * pierscieniem - zdarzen. Adresy urzadzen nadaje kontroler, a my
 * dostajemy numer slotu.
 *
 * Pierscien to tablica szesnastobajtowych TRB, ktorej ostatni element
 * jest skokiem na poczatek. O tym, ktore TRB sa juz nasze, a ktore
 * kontrolera, rozstrzyga jeden bit - bit cyklu. Przy kazdym obrocie
 * pierscienia zmienia on znaczenie na przeciwne, dzieki czemu nie
 * trzeba niczego zerowac ani liczyc: kontroler przetwarza TRB tak
 * dlugo, jak dlugo ich bit cyklu zgadza sie z jego wlasnym.
 *
 * Czekamy odpytywaniem, bez przerwan - z tego samego powodu, co przy
 * UHCI: klawiature i tak odpytuje watek, a jedna droga mniej to jedno
 * miejsce mniej, w ktorym cos moze pojsc nie tak.
 *
 * Struktury musza lezec w pamieci ciaglej fizycznie i pod adresem,
 * ktory kontroler rozumie wprost - a wiec w mapowaniu
 * identycznosciowym, gdzie adres wirtualny rowna sie fizycznemu.
 */

#include <stdint.h>
#include "include/xhci.h"
#include "include/pci.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/pit.h"
#include "include/io.h"

/* Rejestry siedza w pamieci, wiec trzeba je najpierw zmapowac. Adres
   bierze sie z BAR-a, a nie z niczego stalego - to jedyna rzecz w tym
   pliku zalezna od trybu procesora. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#endif

/* --- rejestry ------------------------------------------------------- */

/* Czesc opisowa (od bazy) */
#define CAP_CAPLENGTH   0x00        /* 8 bitow: gdzie zaczyna sie czesc robocza */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* Czesc robocza (od bazy + CAPLENGTH) */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18
#define OP_DCBAAP       0x30
#define OP_CONFIG       0x38
#define OP_PORTSC       0x400       /* co 0x10 na port */

#define CMD_RS          0x0001
#define CMD_HCRST       0x0002
#define CMD_INTE        0x0004

#define STS_HCH         0x0001      /* kontroler stoi */
#define STS_CNR         0x0800      /* kontroler jeszcze nie gotowy */

#define PORT_CCS        (1u << 0)   /* cos podpiete */
#define PORT_PED        (1u << 1)   /* port wlaczony */
#define PORT_PR         (1u << 4)   /* reset portu */
#define PORT_PP         (1u << 9)   /* zasilanie portu */
#define PORT_SPEED_SH   10
#define PORT_ZMIANY     0x00FE0000u /* bity zgloszen: kasuje sie je jedynka */

/* Czesc czasu rzeczywistego (od bazy + RTSOFF), przerywacz numer 0 */
#define RT_IMAN         0x20
#define RT_IMOD         0x24
#define RT_ERSTSZ       0x28
#define RT_ERSTBA       0x30
#define RT_ERDP         0x38

/* Rodzaje TRB */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_EV_TRANSFER 32         /* zdarzenie: przesylka gotowa */
#define TRB_EV_COMMAND 33         /* zdarzenie: polecenie wykonane */

#define TRB_IOC         (1u << 5)   /* daj znac, gdy skonczone */
#define TRB_IDT         (1u << 6)   /* dane siedza w samym TRB */
#define TRB_ISP         (1u << 2)   /* krotszy pakiet tez konczy przesylke */
#define TRB_TC          (1u << 1)   /* skok przewraca bit cyklu */

#define CODE_OK      1
#define CODE_SHORT      13          /* przyszlo mniej, niz prosilismy - to nie blad */

#define SLOTS          4           /* tyle urzadzen naraz nam wystarczy */
#define TRB_CHAIN      16          /* z czego ostatni to skok na poczatek */
#define CTX_BYTES      2112        /* najwiekszy mozliwy zestaw opisow */

struct trb {
    volatile uint32_t p0, p1, status, control;
};

/* Pierscien od strony piszacego: gdzie wlozyc nastepny TRB i z jakim
   bitem cyklu */
struct ring {
    struct trb *trb;
    uint32_t index;
    uint32_t cycle;
};

static uintptr_t base;              /* czesc opisowa */
static uintptr_t op;                /* czesc robocza */
static uintptr_t rt;                /* czesc czasu rzeczywistego */
static volatile uint32_t *doorbells;
static int nports;
static int ctx_size = 32;        /* 32 albo 64 B na opis - mowi o tym HCCPARAMS1 */

static uint64_t dcbaa[SLOTS + 1] __attribute__((aligned(64)));
static struct trb cmd_trb[TRB_CHAIN] __attribute__((aligned(64)));
static struct trb evt_trb[TRB_CHAIN] __attribute__((aligned(64)));
static uint32_t erst[4] __attribute__((aligned(64)));
static uint8_t dev_ctx[SLOTS][CTX_BYTES] __attribute__((aligned(64)));
static uint8_t in_ctx[SLOTS][CTX_BYTES] __attribute__((aligned(64)));
/* Pierscienie przesylek: [0] punkt zerowy, [1] przerwaniowy */
static struct trb ep_trb[SLOTS][2][TRB_CHAIN] __attribute__((aligned(64)));

static struct ring cmd_pier;
static struct ring ep_pier[SLOTS][2];
static uint32_t evt_index, evt_cycle;
static int armed[SLOTS];       /* czy w punkcie przerwaniowym czeka odczyt */

/* Bufor na dane przesylek sterujacych: kontroler siega po niego sam */
static uint8_t buf[512] __attribute__((aligned(64)));

/* --- dostep do rejestrow -------------------------------------------- */

static uint32_t rd(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static void wr(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

/* Rejestry 64-bitowe wolno zapisywac dwoma polowkami i tak wlasnie
   robimy - inaczej ten sam kod nie zadzialalby w trybie 32-bitowym */
static void wr64(uintptr_t addr, uint64_t v)
{
    wr(addr, (uint32_t)v);
    wr(addr + 4, (uint32_t)(v >> 32));
}

static uint64_t fiz(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static void wait_ms(uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az)
        io_wait();
}

static uintptr_t port_reg(int port)
{
    return op + OP_PORTSC + (uintptr_t)port * 0x10;
}

/* Zapis do PORTSC musi omijac bity, ktore zapisem sie KASUJA: zgloszenia
   zmian i samo wlaczenie portu. Inaczej odczyt-modyfikacja-zapis
   wylaczalby port przy okazji zwyklego ustawienia czegokolwiek. */
static uint32_t portsc_safe(uint32_t v)
{
    return v & ~(PORT_ZMIANY | PORT_PED);
}

/* --- pierscienie ---------------------------------------------------- */

static void ring_setup(struct ring *p, struct trb *trb)
{
    memset(trb, 0, sizeof(struct trb) * TRB_CHAIN);
    p->trb = trb;
    p->index = 0;
    p->cycle = 1;

    /* Ostatni TRB jest skokiem na poczatek. Bit "przewroc cykl" sprawia,
       ze przy kazdym obrocie znaczenie bitu cyklu zmienia sie na
       przeciwne - i dlatego pierscienia nigdy nie trzeba czyscic. */
    struct trb *jump = &trb[TRB_CHAIN - 1];
    jump->p0 = (uint32_t)fiz(trb);
    jump->p1 = (uint32_t)(fiz(trb) >> 32);
    jump->status = 0;
    jump->control = ((uint32_t)TRB_LINK << 10) | TRB_TC;
}

static void insert(struct ring *p, uint64_t param, uint32_t status,
                 uint32_t control)
{
    struct trb *t = &p->trb[p->index];
    t->p0 = (uint32_t)param;
    t->p1 = (uint32_t)(param >> 32);
    t->status = status;
    /* Bit cyklu na samym koncu: dopiero on oddaje TRB kontrolerowi,
       wiec reszta pol musi juz w nim byc */
    t->control = control | p->cycle;

    p->index++;
    if (p->index == TRB_CHAIN - 1) {
        struct trb *jump = &p->trb[TRB_CHAIN - 1];
        jump->control = (jump->control & ~1u) | p->cycle;
        p->index = 0;
        p->cycle ^= 1;
    }
}

/* Zdejmuje jedno zdarzenie z pierscienia zdarzen. 1 = bylo. */
static int event(struct trb *result)
{
    struct trb *t = &evt_trb[evt_index];
    if ((t->control & 1u) != evt_cycle)
        return 0;                        /* to jeszcze nie nasze */

    result->p0 = t->p0;
    result->p1 = t->p1;
    result->status = t->status;
    result->control = t->control;

    evt_index++;
    if (evt_index == TRB_CHAIN) {
        evt_index = 0;
        evt_cycle ^= 1;
    }
    /* Kontroler musi wiedziec, dokad juz doczytalismy - inaczej po
       pelnym obrocie nadpisalby zdarzenia, ktorych nikt nie widzial */
    wr64(rt + RT_ERDP, fiz(&evt_trb[evt_index]) | 0x8);
    return 1;
}

/* Czeka na zdarzenie danego rodzaju. Inne po drodze przepadaja i tak
   ma byc: przez ten pierscien ida jeszcze zgloszenia zmian na portach,
   ktorych nikt nie zamawial i o ktore nikt nie pyta. */
static int wait_for(int kind, struct trb *result, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        struct trb t;
        if (!event(&t)) {
            io_wait();
            continue;
        }
        if ((int)((t.control >> 10) & 0x3F) == kind) {
            *result = t;
            return 1;
        }
    }
    return 0;
}

/* Wklada polecenie i czeka na jego wynik. Zwraca kod ukonczenia
   (KOD_SUKCES gdy dobrze) albo -1, gdy nie doczekalismy sie. */
static int command(uint64_t param, uint32_t control, struct trb *reply)
{
    insert(&cmd_pier, param, 0, control);
    doorbells[0] = 0;                      /* dzwonek pierscienia polecen */

    struct trb t;
    if (!wait_for(TRB_EV_COMMAND, &t, 1000))
        return -1;
    if (reply)
        *reply = t;
    return (int)(t.status >> 24);
}

/* --- opisy urzadzenia i punktow koncowych --------------------------- */

/* Opis numer 'i' w zestawie: zerowy jest opis sterujacy (tylko
   w zestawie wejsciowym), potem urzadzenia, potem punkty koncowe */
static uint32_t *info(uint8_t *set, int i)
{
    return (uint32_t *)(set + (uintptr_t)i * (uintptr_t)ctx_size);
}

/* Numer punktu koncowego w opisie: zerowy ma 1, kolejne IN maja 2n+1 */
static int dci(int ep)
{
    return ep ? ep * 2 + 1 : 1;
}

/* --- uruchomienie ---------------------------------------------------- */

int xhci_ports(void)
{
    return nports;
}

int xhci_init(void)
{
    struct pci_dev dev;
    /* Klasa 0x0C (magistrala szeregowa), podklasa 0x03 (USB),
       interfejs 0x30 (xHCI) */
    if (!pci_find_class(0x0C, 0x03, 0x30, &dev))
        return 0;

    /* xHCI siedzi w pamieci - BAR0, ewentualnie 64-bitowy (bity 2:1
       rowne 2 znacza, ze gorna polowa adresu jest w BAR1) */
    uint64_t addr = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x01)
        return 0;                        /* to nie jest okno w pamieci */
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        addr |= (uint64_t)dev.bar[1] << 32;
    if (!addr || addr > 0xFFFFFFFFull)
        return 0;                        /* wyzej i tak nie siegniemy */

    if (MAP_PHYS((uint32_t)addr, 0x10000) < 0)
        return 0;
    pci_enable_bus_master(&dev);

    base = (uintptr_t)addr;
    op = base + (rd(base + CAP_CAPLENGTH) & 0xFF);
    rt = base + (rd(base + CAP_RTSOFF) & ~0x1Fu);
    doorbells = (volatile uint32_t *)(base + (rd(base + CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = rd(base + CAP_HCSPARAMS1);
    nports = (int)((hcs1 >> 24) & 0xFF);
    if (nports > 32)
        nports = 32;
    ctx_size = (rd(base + CAP_HCCPARAMS1) & 0x4) ? 64 : 32;

    /* Zatrzymaj i wyzeruj kontroler - po BIOS-ie moze byc w dowolnym
       stanie, a reset jest jedynym stanem, ktory znamy */
    wr(op + OP_USBCMD, rd(op + OP_USBCMD) & ~(uint32_t)CMD_RS);
    for (int i = 0; i < 200 && !(rd(op + OP_USBSTS) & STS_HCH); i++)
        wait_ms(1);
    wr(op + OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 200 && (rd(op + OP_USBCMD) & CMD_HCRST); i++)
        wait_ms(1);
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_CNR); i++)
        wait_ms(1);
    if (rd(op + OP_USBCMD) & CMD_HCRST)
        return 0;

    memset(dcbaa, 0, sizeof(dcbaa));
    memset(dev_ctx, 0, sizeof(dev_ctx));
    memset(in_ctx, 0, sizeof(in_ctx));
    for (int s = 0; s < SLOTS; s++)
        for (int e = 0; e < 2; e++)
            ring_setup(&ep_pier[s][e], ep_trb[s][e]);

    wr(op + OP_CONFIG, SLOTS);
    wr64(op + OP_DCBAAP, fiz(dcbaa));

    ring_setup(&cmd_pier, cmd_trb);
    /* Bit zerowy CRCR to bit cyklu, od ktorego kontroler zaczyna
       czytac pierscien polecen - musi zgadzac sie z naszym */
    wr64(op + OP_CRCR, fiz(cmd_trb) | 1);

    memset(evt_trb, 0, sizeof(evt_trb));
    evt_index = 0;
    evt_cycle = 1;
    erst[0] = (uint32_t)fiz(evt_trb);
    erst[1] = (uint32_t)(fiz(evt_trb) >> 32);
    erst[2] = TRB_CHAIN;
    erst[3] = 0;
    wr(rt + RT_ERSTSZ, 1);
    wr64(rt + RT_ERDP, fiz(evt_trb) | 0x8);
    wr64(rt + RT_ERSTBA, fiz(erst));

    wr(op + OP_DNCTRL, 0);
    wr(op + OP_USBCMD, CMD_RS);          /* bez CMD_INTE - odpytujemy */
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_HCH); i++)
        wait_ms(1);
    if (rd(op + OP_USBSTS) & STS_HCH)
        return 0;

    /* Zasilanie portow: bez niego na porcie nigdy nic sie nie zglosi */
    for (int i = 0; i < nports; i++) {
        uint32_t v = rd(port_reg(i));
        if (!(v & PORT_PP))
            wr(port_reg(i), portsc_safe(v) | PORT_PP);
    }
    wait_ms(20);
    return 1;
}

/* --- podlaczenie urzadzenia ------------------------------------------ */

/* Domyslny rozmiar pakietu punktu zerowego wynika z predkosci portu.
   Przy pelnej predkosci moze byc inny - poprawimy go, gdy urzadzenie
   samo o tym powie w swoim opisie. */
static int packet_for_speed(int speed)
{
    switch (speed) {
    case 2:  return 8;                   /* niska */
    case 3:  return 64;                  /* wysoka */
    case 4:
    case 5:  return 512;                 /* super */
    default: return 8;                   /* pelna: zaczynamy od osmiu */
    }
}

static int port_reset(int port)
{
    uintptr_t reg = port_reg(port);
    uint32_t v = rd(reg);
    if (!(v & PORT_CCS))
        return 0;
    if (v & PORT_PED)
        return 1;                        /* port zglosil sie sam (USB 3) */

    wr(reg, portsc_safe(v) | PORT_PR);
    for (int i = 0; i < 100; i++) {
        wait_ms(2);
        v = rd(reg);
        if (!(v & PORT_CCS))
            return 0;                    /* urzadzenie zniklo w trakcie */
        if (v & PORT_PED)
            break;
    }
    if (!(v & PORT_PED))
        return 0;

    wr(reg, portsc_safe(v) | PORT_ZMIANY);   /* skasuj zgloszenia */
    wait_ms(10);
    return 1;
}

/* Sklada zestaw wejsciowy pod polecenie: co dodajemy, opis urzadzenia
   i opis punktu zerowego. */
static void make_input(int slot, int port, int speed, int maxpacket)
{
    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);

    uint32_t *control = info(we, 0);
    control[1] = 0x3;                 /* dodajemy opis urzadzenia i punkt 0 */

    uint32_t *device = info(we, 1);
    device[0] = ((uint32_t)speed << 20) | (1u << 27);  /* jeden punkt */
    device[1] = (uint32_t)(port + 1) << 16;

    uint32_t *punkt0 = info(we, 2);
    punkt0[1] = (3u << 1) |              /* trzy proby przy bledzie */
                (4u << 3) |              /* rodzaj: sterujacy */
                ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][0]) | 1;   /* bit cyklu producenta */
    punkt0[2] = (uint32_t)p;
    punkt0[3] = (uint32_t)(p >> 32);
    punkt0[4] = 8;                       /* przecietna dlugosc TRB */
}

int xhci_attach(int port)
{
    if (!base || port < 0 || port >= nports)
        return -1;
    if (!port_reset(port))
        return -1;

    struct trb reply;
    if (command(0, (uint32_t)TRB_ENABLE_SLOT << 10, &reply) != CODE_OK)
        return -1;
    int slot = (int)(reply.control >> 24);
    if (slot < 1 || slot > SLOTS)
        return -1;

    int speed = (int)((rd(port_reg(port)) >> PORT_SPEED_SH) & 0xF);
    int maxpacket = packet_for_speed(speed);

    dcbaa[slot] = fiz(dev_ctx[slot - 1]);
    memset(dev_ctx[slot - 1], 0, CTX_BYTES);
    ring_setup(&ep_pier[slot - 1][0], ep_trb[slot - 1][0]);

    make_input(slot, port, speed, maxpacket);
    if (command(fiz(in_ctx[slot - 1]),
                  ((uint32_t)TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK)
        return -1;

    /* Osiem pierwszych bajtow opisu urzadzenia mowi, jak duze pakiety
       przyjmuje punkt zerowy. Przy pelnej predkosci bywa to wiecej niz
       osiem - i wtedy trzeba kontrolerowi o tym powiedziec, bo inaczej
       dluzsze odpowiedzi rozjada sie na pakietach. */
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0x00 };
    uint8_t info8[8];
    if (xhci_control(slot, setup, info8, 8, 1) < 8)
        return -1;
    if (info8[7] && info8[7] != maxpacket) {
        uint8_t *we = in_ctx[slot - 1];
        uint32_t *control = info(we, 0);
        control[0] = 0;
        control[1] = 0x2;             /* poprawiamy tylko punkt zerowy */
        uint32_t *punkt0 = info(we, 2);
        punkt0[1] = (punkt0[1] & 0xFFFFu) | ((uint32_t)info8[7] << 16);
        command(fiz(we),
                  ((uint32_t)TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0);
    }
    return slot;
}

/* --- przesylki -------------------------------------------------------- */

int xhci_control(int slot, const void *setup, void *data, int len, int in)
{
    if (!base || slot < 1 || slot > SLOTS)
        return -1;
    if (len < 0 || len > (int)sizeof(buf))
        return -1;

    const uint8_t *s = setup;
    uint64_t query = 0;
    for (int i = 0; i < 8; i++)
        query |= (uint64_t)s[i] << (i * 8);

    if (len && !in)
        memcpy(buf, data, (uint32_t)len);

    struct ring *p = &ep_pier[slot - 1][0];
    /* Rodzaj przesylki: 0 bez danych, 2 dane w dol, 3 dane w gore */
    uint32_t kind = len ? (in ? 3u : 2u) : 0u;
    insert(p, query, 8, ((uint32_t)TRB_SETUP << 10) | TRB_IDT |
                          (kind << 16));
    if (len)
        insert(p, fiz(buf), (uint32_t)len,
             ((uint32_t)TRB_DATA << 10) | TRB_IOC | TRB_ISP |
             ((uint32_t)(in ? 1 : 0) << 16));
    insert(p, 0, 0, ((uint32_t)TRB_STATUS << 10) | TRB_IOC |
                  ((uint32_t)(in ? 0 : 1) << 16));

    doorbells[slot] = 1;                   /* punkt zerowy ma numer 1 */

    int passed = 0;
    if (len) {
        struct trb t;
        if (!wait_for(TRB_EV_TRANSFER, &t, 1000))
            return -1;
        int code = (int)(t.status >> 24);
        if (code != CODE_OK && code != CODE_SHORT)
            return -1;
        /* Kontroler podaje, ILE ZOSTALO, a nie ile przeszlo */
        passed = len - (int)(t.status & 0xFFFFFF);
        if (in)
            memcpy(data, buf, (uint32_t)(passed > 0 ? passed : 0));
    }

    struct trb t;
    if (!wait_for(TRB_EV_TRANSFER, &t, 1000))
        return -1;
    int code = (int)(t.status >> 24);
    if (code != CODE_OK && code != CODE_SHORT)
        return -1;
    return passed;
}

int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval)
{
    if (!base || slot < 1 || slot > SLOTS || ep < 1)
        return -1;
    int number = dci(ep);
    if (number > 31)
        return -1;

    ring_setup(&ep_pier[slot - 1][1], ep_trb[slot - 1][1]);
    armed[slot - 1] = 0;

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BYTES);
    uint32_t *control = info(we, 0);
    control[1] = 0x1u | (1u << number);   /* urzadzenie + ten punkt */

    /* Opis urzadzenia trzeba podac na nowo, bo zmienia sie liczba
       punktow koncowych - kontroler nie domysli sie jej sam */
    uint32_t *src = info(dev_ctx[slot - 1], 0);
    uint32_t *device = info(we, 1);
    device[0] = (src[0] & 0x07FFFFFFu) | ((uint32_t)number << 27);
    device[1] = src[1];

    uint32_t *punkt = info(we, number + 1);
    punkt[0] = (uint32_t)(interval > 0 ? interval : 8) << 16;
    punkt[1] = (3u << 1) |               /* trzy proby przy bledzie */
               (7u << 3) |               /* rodzaj: przerwaniowy w gore */
               ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][1]) | 1;
    punkt[2] = (uint32_t)p;
    punkt[3] = (uint32_t)(p >> 32);
    punkt[4] = (uint32_t)maxpacket | ((uint32_t)maxpacket << 16);

    if (command(fiz(we),
                  ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != CODE_OK)
        return -1;
    return 0;
}

/* Zajrzenie do punktu przerwaniowego.
 *
 * Tak samo jak przy UHCI: odczyt uzbrajamy raz i nie porzucamy go.
 * Roznica jest tylko taka, ze tam deskryptor lezal w kolejce, a tu
 * TRB lezy w pierscieniu przesylek - a o jego ukonczeniu dowiadujemy
 * sie ze zdarzenia, a nie z bitu w samym deskryptorze.
 */
int xhci_int_in(int slot, int ep, void *data, int len)
{
    if (!base || slot < 1 || slot > SLOTS || len <= 0 ||
        len > (int)sizeof(buf))
        return -1;

    if (!armed[slot - 1]) {
        insert(&ep_pier[slot - 1][1], fiz(buf), (uint32_t)len,
             ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        doorbells[slot] = (uint32_t)dci(ep);
        armed[slot - 1] = 1;
        return 0;                        /* odpowiedz przyjdzie za chwile */
    }

    struct trb t;
    if (!event(&t))
        return 0;                        /* urzadzenie milczy - czekamy dalej */
    armed[slot - 1] = 0;
    if ((int)((t.control >> 10) & 0x3F) != TRB_EV_TRANSFER)
        return 0;                        /* zgloszenie z portu, nie nasze dane */

    int code = (int)(t.status >> 24);
    if (code != CODE_OK && code != CODE_SHORT)
        return -1;
    int passed = len - (int)(t.status & 0xFFFFFF);
    if (passed > 0)
        memcpy(data, buf, (uint32_t)passed);
    return passed;
}
