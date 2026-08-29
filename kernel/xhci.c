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
#define TRB_ZD_PRZESYLKI 32         /* zdarzenie: przesylka gotowa */
#define TRB_ZD_POLECENIA 33         /* zdarzenie: polecenie wykonane */

#define TRB_IOC         (1u << 5)   /* daj znac, gdy skonczone */
#define TRB_IDT         (1u << 6)   /* dane siedza w samym TRB */
#define TRB_ISP         (1u << 2)   /* krotszy pakiet tez konczy przesylke */
#define TRB_TC          (1u << 1)   /* skok przewraca bit cyklu */

#define KOD_SUKCES      1
#define KOD_KROTKI      13          /* przyszlo mniej, niz prosilismy - to nie blad */

#define SLOTOW          4           /* tyle urzadzen naraz nam wystarczy */
#define TRB_W_PIER      16          /* z czego ostatni to skok na poczatek */
#define CTX_BAJTOW      2112        /* najwiekszy mozliwy zestaw opisow */

struct trb {
    volatile uint32_t p0, p1, status, control;
};

/* Pierscien od strony piszacego: gdzie wlozyc nastepny TRB i z jakim
   bitem cyklu */
struct pierscien {
    struct trb *trb;
    uint32_t indeks;
    uint32_t cykl;
};

static uintptr_t baza;              /* czesc opisowa */
static uintptr_t op;                /* czesc robocza */
static uintptr_t rt;                /* czesc czasu rzeczywistego */
static volatile uint32_t *dzwonki;
static int portow;
static int ctx_rozmiar = 32;        /* 32 albo 64 B na opis - mowi o tym HCCPARAMS1 */

static uint64_t dcbaa[SLOTOW + 1] __attribute__((aligned(64)));
static struct trb cmd_trb[TRB_W_PIER] __attribute__((aligned(64)));
static struct trb evt_trb[TRB_W_PIER] __attribute__((aligned(64)));
static uint32_t erst[4] __attribute__((aligned(64)));
static uint8_t dev_ctx[SLOTOW][CTX_BAJTOW] __attribute__((aligned(64)));
static uint8_t in_ctx[SLOTOW][CTX_BAJTOW] __attribute__((aligned(64)));
/* Pierscienie przesylek: [0] punkt zerowy, [1] przerwaniowy */
static struct trb ep_trb[SLOTOW][2][TRB_W_PIER] __attribute__((aligned(64)));

static struct pierscien cmd_pier;
static struct pierscien ep_pier[SLOTOW][2];
static uint32_t evt_indeks, evt_cykl;
static int uzbrojony[SLOTOW];       /* czy w punkcie przerwaniowym czeka odczyt */

/* Bufor na dane przesylek sterujacych: kontroler siega po niego sam */
static uint8_t bufor[512] __attribute__((aligned(64)));

/* --- dostep do rejestrow -------------------------------------------- */

static uint32_t rd(uintptr_t adres)
{
    return *(volatile uint32_t *)adres;
}

static void wr(uintptr_t adres, uint32_t v)
{
    *(volatile uint32_t *)adres = v;
}

/* Rejestry 64-bitowe wolno zapisywac dwoma polowkami i tak wlasnie
   robimy - inaczej ten sam kod nie zadzialalby w trybie 32-bitowym */
static void wr64(uintptr_t adres, uint64_t v)
{
    wr(adres, (uint32_t)v);
    wr(adres + 4, (uint32_t)(v >> 32));
}

static uint64_t fiz(const void *p)
{
    return (uint64_t)(uintptr_t)p;
}

static void czekaj_ms(uint32_t ms)
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
static uint32_t portsc_bezpiecznie(uint32_t v)
{
    return v & ~(PORT_ZMIANY | PORT_PED);
}

/* --- pierscienie ---------------------------------------------------- */

static void pierscien_zaloz(struct pierscien *p, struct trb *trb)
{
    memset(trb, 0, sizeof(struct trb) * TRB_W_PIER);
    p->trb = trb;
    p->indeks = 0;
    p->cykl = 1;

    /* Ostatni TRB jest skokiem na poczatek. Bit "przewroc cykl" sprawia,
       ze przy kazdym obrocie znaczenie bitu cyklu zmienia sie na
       przeciwne - i dlatego pierscienia nigdy nie trzeba czyscic. */
    struct trb *skok = &trb[TRB_W_PIER - 1];
    skok->p0 = (uint32_t)fiz(trb);
    skok->p1 = (uint32_t)(fiz(trb) >> 32);
    skok->status = 0;
    skok->control = ((uint32_t)TRB_LINK << 10) | TRB_TC;
}

static void wloz(struct pierscien *p, uint64_t param, uint32_t status,
                 uint32_t control)
{
    struct trb *t = &p->trb[p->indeks];
    t->p0 = (uint32_t)param;
    t->p1 = (uint32_t)(param >> 32);
    t->status = status;
    /* Bit cyklu na samym koncu: dopiero on oddaje TRB kontrolerowi,
       wiec reszta pol musi juz w nim byc */
    t->control = control | p->cykl;

    p->indeks++;
    if (p->indeks == TRB_W_PIER - 1) {
        struct trb *skok = &p->trb[TRB_W_PIER - 1];
        skok->control = (skok->control & ~1u) | p->cykl;
        p->indeks = 0;
        p->cykl ^= 1;
    }
}

/* Zdejmuje jedno zdarzenie z pierscienia zdarzen. 1 = bylo. */
static int zdarzenie(struct trb *wynik)
{
    struct trb *t = &evt_trb[evt_indeks];
    if ((t->control & 1u) != evt_cykl)
        return 0;                        /* to jeszcze nie nasze */

    wynik->p0 = t->p0;
    wynik->p1 = t->p1;
    wynik->status = t->status;
    wynik->control = t->control;

    evt_indeks++;
    if (evt_indeks == TRB_W_PIER) {
        evt_indeks = 0;
        evt_cykl ^= 1;
    }
    /* Kontroler musi wiedziec, dokad juz doczytalismy - inaczej po
       pelnym obrocie nadpisalby zdarzenia, ktorych nikt nie widzial */
    wr64(rt + RT_ERDP, fiz(&evt_trb[evt_indeks]) | 0x8);
    return 1;
}

/* Czeka na zdarzenie danego rodzaju. Inne po drodze przepadaja i tak
   ma byc: przez ten pierscien ida jeszcze zgloszenia zmian na portach,
   ktorych nikt nie zamawial i o ktore nikt nie pyta. */
static int czekaj_na(int rodzaj, struct trb *wynik, uint32_t ms)
{
    uint32_t az = pit_ticks() + (ms + 9) / 10 + 1;
    while (pit_ticks() < az) {
        struct trb t;
        if (!zdarzenie(&t)) {
            io_wait();
            continue;
        }
        if ((int)((t.control >> 10) & 0x3F) == rodzaj) {
            *wynik = t;
            return 1;
        }
    }
    return 0;
}

/* Wklada polecenie i czeka na jego wynik. Zwraca kod ukonczenia
   (KOD_SUKCES gdy dobrze) albo -1, gdy nie doczekalismy sie. */
static int polecenie(uint64_t param, uint32_t control, struct trb *odp)
{
    wloz(&cmd_pier, param, 0, control);
    dzwonki[0] = 0;                      /* dzwonek pierscienia polecen */

    struct trb t;
    if (!czekaj_na(TRB_ZD_POLECENIA, &t, 1000))
        return -1;
    if (odp)
        *odp = t;
    return (int)(t.status >> 24);
}

/* --- opisy urzadzenia i punktow koncowych --------------------------- */

/* Opis numer 'i' w zestawie: zerowy jest opis sterujacy (tylko
   w zestawie wejsciowym), potem urzadzenia, potem punkty koncowe */
static uint32_t *opis(uint8_t *zestaw, int i)
{
    return (uint32_t *)(zestaw + (uintptr_t)i * (uintptr_t)ctx_rozmiar);
}

/* Numer punktu koncowego w opisie: zerowy ma 1, kolejne IN maja 2n+1 */
static int dci(int ep)
{
    return ep ? ep * 2 + 1 : 1;
}

/* --- uruchomienie ---------------------------------------------------- */

int xhci_ports(void)
{
    return portow;
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
    uint64_t adres = dev.bar[0] & ~0xFULL;
    if (dev.bar[0] & 0x01)
        return 0;                        /* to nie jest okno w pamieci */
    if (((dev.bar[0] >> 1) & 0x3) == 0x2)
        adres |= (uint64_t)dev.bar[1] << 32;
    if (!adres || adres > 0xFFFFFFFFull)
        return 0;                        /* wyzej i tak nie siegniemy */

    if (MAP_PHYS((uint32_t)adres, 0x10000) < 0)
        return 0;
    pci_enable_bus_master(&dev);

    baza = (uintptr_t)adres;
    op = baza + (rd(baza + CAP_CAPLENGTH) & 0xFF);
    rt = baza + (rd(baza + CAP_RTSOFF) & ~0x1Fu);
    dzwonki = (volatile uint32_t *)(baza + (rd(baza + CAP_DBOFF) & ~0x3u));

    uint32_t hcs1 = rd(baza + CAP_HCSPARAMS1);
    portow = (int)((hcs1 >> 24) & 0xFF);
    if (portow > 32)
        portow = 32;
    ctx_rozmiar = (rd(baza + CAP_HCCPARAMS1) & 0x4) ? 64 : 32;

    /* Zatrzymaj i wyzeruj kontroler - po BIOS-ie moze byc w dowolnym
       stanie, a reset jest jedynym stanem, ktory znamy */
    wr(op + OP_USBCMD, rd(op + OP_USBCMD) & ~(uint32_t)CMD_RS);
    for (int i = 0; i < 200 && !(rd(op + OP_USBSTS) & STS_HCH); i++)
        czekaj_ms(1);
    wr(op + OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 200 && (rd(op + OP_USBCMD) & CMD_HCRST); i++)
        czekaj_ms(1);
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_CNR); i++)
        czekaj_ms(1);
    if (rd(op + OP_USBCMD) & CMD_HCRST)
        return 0;

    memset(dcbaa, 0, sizeof(dcbaa));
    memset(dev_ctx, 0, sizeof(dev_ctx));
    memset(in_ctx, 0, sizeof(in_ctx));
    for (int s = 0; s < SLOTOW; s++)
        for (int e = 0; e < 2; e++)
            pierscien_zaloz(&ep_pier[s][e], ep_trb[s][e]);

    wr(op + OP_CONFIG, SLOTOW);
    wr64(op + OP_DCBAAP, fiz(dcbaa));

    pierscien_zaloz(&cmd_pier, cmd_trb);
    /* Bit zerowy CRCR to bit cyklu, od ktorego kontroler zaczyna
       czytac pierscien polecen - musi zgadzac sie z naszym */
    wr64(op + OP_CRCR, fiz(cmd_trb) | 1);

    memset(evt_trb, 0, sizeof(evt_trb));
    evt_indeks = 0;
    evt_cykl = 1;
    erst[0] = (uint32_t)fiz(evt_trb);
    erst[1] = (uint32_t)(fiz(evt_trb) >> 32);
    erst[2] = TRB_W_PIER;
    erst[3] = 0;
    wr(rt + RT_ERSTSZ, 1);
    wr64(rt + RT_ERDP, fiz(evt_trb) | 0x8);
    wr64(rt + RT_ERSTBA, fiz(erst));

    wr(op + OP_DNCTRL, 0);
    wr(op + OP_USBCMD, CMD_RS);          /* bez CMD_INTE - odpytujemy */
    for (int i = 0; i < 200 && (rd(op + OP_USBSTS) & STS_HCH); i++)
        czekaj_ms(1);
    if (rd(op + OP_USBSTS) & STS_HCH)
        return 0;

    /* Zasilanie portow: bez niego na porcie nigdy nic sie nie zglosi */
    for (int i = 0; i < portow; i++) {
        uint32_t v = rd(port_reg(i));
        if (!(v & PORT_PP))
            wr(port_reg(i), portsc_bezpiecznie(v) | PORT_PP);
    }
    czekaj_ms(20);
    return 1;
}

/* --- podlaczenie urzadzenia ------------------------------------------ */

/* Domyslny rozmiar pakietu punktu zerowego wynika z predkosci portu.
   Przy pelnej predkosci moze byc inny - poprawimy go, gdy urzadzenie
   samo o tym powie w swoim opisie. */
static int pakiet_dla_predkosci(int predkosc)
{
    switch (predkosc) {
    case 2:  return 8;                   /* niska */
    case 3:  return 64;                  /* wysoka */
    case 4:
    case 5:  return 512;                 /* super */
    default: return 8;                   /* pelna: zaczynamy od osmiu */
    }
}

static int reset_portu(int port)
{
    uintptr_t reg = port_reg(port);
    uint32_t v = rd(reg);
    if (!(v & PORT_CCS))
        return 0;
    if (v & PORT_PED)
        return 1;                        /* port zglosil sie sam (USB 3) */

    wr(reg, portsc_bezpiecznie(v) | PORT_PR);
    for (int i = 0; i < 100; i++) {
        czekaj_ms(2);
        v = rd(reg);
        if (!(v & PORT_CCS))
            return 0;                    /* urzadzenie zniklo w trakcie */
        if (v & PORT_PED)
            break;
    }
    if (!(v & PORT_PED))
        return 0;

    wr(reg, portsc_bezpiecznie(v) | PORT_ZMIANY);   /* skasuj zgloszenia */
    czekaj_ms(10);
    return 1;
}

/* Sklada zestaw wejsciowy pod polecenie: co dodajemy, opis urzadzenia
   i opis punktu zerowego. */
static void zloz_wejscie(int slot, int port, int predkosc, int maxpakiet)
{
    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BAJTOW);

    uint32_t *sterowanie = opis(we, 0);
    sterowanie[1] = 0x3;                 /* dodajemy opis urzadzenia i punkt 0 */

    uint32_t *urzadzenie = opis(we, 1);
    urzadzenie[0] = ((uint32_t)predkosc << 20) | (1u << 27);  /* jeden punkt */
    urzadzenie[1] = (uint32_t)(port + 1) << 16;

    uint32_t *punkt0 = opis(we, 2);
    punkt0[1] = (3u << 1) |              /* trzy proby przy bledzie */
                (4u << 3) |              /* rodzaj: sterujacy */
                ((uint32_t)maxpakiet << 16);
    uint64_t p = fiz(ep_trb[slot - 1][0]) | 1;   /* bit cyklu producenta */
    punkt0[2] = (uint32_t)p;
    punkt0[3] = (uint32_t)(p >> 32);
    punkt0[4] = 8;                       /* przecietna dlugosc TRB */
}

int xhci_attach(int port)
{
    if (!baza || port < 0 || port >= portow)
        return -1;
    if (!reset_portu(port))
        return -1;

    struct trb odp;
    if (polecenie(0, (uint32_t)TRB_ENABLE_SLOT << 10, &odp) != KOD_SUKCES)
        return -1;
    int slot = (int)(odp.control >> 24);
    if (slot < 1 || slot > SLOTOW)
        return -1;

    int predkosc = (int)((rd(port_reg(port)) >> PORT_SPEED_SH) & 0xF);
    int maxpakiet = pakiet_dla_predkosci(predkosc);

    dcbaa[slot] = fiz(dev_ctx[slot - 1]);
    memset(dev_ctx[slot - 1], 0, CTX_BAJTOW);
    pierscien_zaloz(&ep_pier[slot - 1][0], ep_trb[slot - 1][0]);

    zloz_wejscie(slot, port, predkosc, maxpakiet);
    if (polecenie(fiz(in_ctx[slot - 1]),
                  ((uint32_t)TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != KOD_SUKCES)
        return -1;

    /* Osiem pierwszych bajtow opisu urzadzenia mowi, jak duze pakiety
       przyjmuje punkt zerowy. Przy pelnej predkosci bywa to wiecej niz
       osiem - i wtedy trzeba kontrolerowi o tym powiedziec, bo inaczej
       dluzsze odpowiedzi rozjada sie na pakietach. */
    uint8_t setup[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0x00 };
    uint8_t opis8[8];
    if (xhci_control(slot, setup, opis8, 8, 1) < 8)
        return -1;
    if (opis8[7] && opis8[7] != maxpakiet) {
        uint8_t *we = in_ctx[slot - 1];
        uint32_t *sterowanie = opis(we, 0);
        sterowanie[0] = 0;
        sterowanie[1] = 0x2;             /* poprawiamy tylko punkt zerowy */
        uint32_t *punkt0 = opis(we, 2);
        punkt0[1] = (punkt0[1] & 0xFFFFu) | ((uint32_t)opis8[7] << 16);
        polecenie(fiz(we),
                  ((uint32_t)TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0);
    }
    return slot;
}

/* --- przesylki -------------------------------------------------------- */

int xhci_control(int slot, const void *setup, void *data, int len, int in)
{
    if (!baza || slot < 1 || slot > SLOTOW)
        return -1;
    if (len < 0 || len > (int)sizeof(bufor))
        return -1;

    const uint8_t *s = setup;
    uint64_t zapytanie = 0;
    for (int i = 0; i < 8; i++)
        zapytanie |= (uint64_t)s[i] << (i * 8);

    if (len && !in)
        memcpy(bufor, data, (uint32_t)len);

    struct pierscien *p = &ep_pier[slot - 1][0];
    /* Rodzaj przesylki: 0 bez danych, 2 dane w dol, 3 dane w gore */
    uint32_t rodzaj = len ? (in ? 3u : 2u) : 0u;
    wloz(p, zapytanie, 8, ((uint32_t)TRB_SETUP << 10) | TRB_IDT |
                          (rodzaj << 16));
    if (len)
        wloz(p, fiz(bufor), (uint32_t)len,
             ((uint32_t)TRB_DATA << 10) | TRB_IOC | TRB_ISP |
             ((uint32_t)(in ? 1 : 0) << 16));
    wloz(p, 0, 0, ((uint32_t)TRB_STATUS << 10) | TRB_IOC |
                  ((uint32_t)(in ? 0 : 1) << 16));

    dzwonki[slot] = 1;                   /* punkt zerowy ma numer 1 */

    int przeszlo = 0;
    if (len) {
        struct trb t;
        if (!czekaj_na(TRB_ZD_PRZESYLKI, &t, 1000))
            return -1;
        int kod = (int)(t.status >> 24);
        if (kod != KOD_SUKCES && kod != KOD_KROTKI)
            return -1;
        /* Kontroler podaje, ILE ZOSTALO, a nie ile przeszlo */
        przeszlo = len - (int)(t.status & 0xFFFFFF);
        if (in)
            memcpy(data, bufor, (uint32_t)(przeszlo > 0 ? przeszlo : 0));
    }

    struct trb t;
    if (!czekaj_na(TRB_ZD_PRZESYLKI, &t, 1000))
        return -1;
    int kod = (int)(t.status >> 24);
    if (kod != KOD_SUKCES && kod != KOD_KROTKI)
        return -1;
    return przeszlo;
}

int xhci_int_endpoint(int slot, int ep, int maxpacket, int interval)
{
    if (!baza || slot < 1 || slot > SLOTOW || ep < 1)
        return -1;
    int numer = dci(ep);
    if (numer > 31)
        return -1;

    pierscien_zaloz(&ep_pier[slot - 1][1], ep_trb[slot - 1][1]);
    uzbrojony[slot - 1] = 0;

    uint8_t *we = in_ctx[slot - 1];
    memset(we, 0, CTX_BAJTOW);
    uint32_t *sterowanie = opis(we, 0);
    sterowanie[1] = 0x1u | (1u << numer);   /* urzadzenie + ten punkt */

    /* Opis urzadzenia trzeba podac na nowo, bo zmienia sie liczba
       punktow koncowych - kontroler nie domysli sie jej sam */
    uint32_t *zrodlo = opis(dev_ctx[slot - 1], 0);
    uint32_t *urzadzenie = opis(we, 1);
    urzadzenie[0] = (zrodlo[0] & 0x07FFFFFFu) | ((uint32_t)numer << 27);
    urzadzenie[1] = zrodlo[1];

    uint32_t *punkt = opis(we, numer + 1);
    punkt[0] = (uint32_t)(interval > 0 ? interval : 8) << 16;
    punkt[1] = (3u << 1) |               /* trzy proby przy bledzie */
               (7u << 3) |               /* rodzaj: przerwaniowy w gore */
               ((uint32_t)maxpacket << 16);
    uint64_t p = fiz(ep_trb[slot - 1][1]) | 1;
    punkt[2] = (uint32_t)p;
    punkt[3] = (uint32_t)(p >> 32);
    punkt[4] = (uint32_t)maxpacket | ((uint32_t)maxpacket << 16);

    if (polecenie(fiz(we),
                  ((uint32_t)TRB_CONFIG_EP << 10) | ((uint32_t)slot << 24),
                  (struct trb *)0) != KOD_SUKCES)
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
    if (!baza || slot < 1 || slot > SLOTOW || len <= 0 ||
        len > (int)sizeof(bufor))
        return -1;

    if (!uzbrojony[slot - 1]) {
        wloz(&ep_pier[slot - 1][1], fiz(bufor), (uint32_t)len,
             ((uint32_t)TRB_NORMAL << 10) | TRB_IOC | TRB_ISP);
        dzwonki[slot] = (uint32_t)dci(ep);
        uzbrojony[slot - 1] = 1;
        return 0;                        /* odpowiedz przyjdzie za chwile */
    }

    struct trb t;
    if (!zdarzenie(&t))
        return 0;                        /* urzadzenie milczy - czekamy dalej */
    uzbrojony[slot - 1] = 0;
    if ((int)((t.control >> 10) & 0x3F) != TRB_ZD_PRZESYLKI)
        return 0;                        /* zgloszenie z portu, nie nasze dane */

    int kod = (int)(t.status >> 24);
    if (kod != KOD_SUKCES && kod != KOD_KROTKI)
        return -1;
    int przeszlo = len - (int)(t.status & 0xFFFFFF);
    if (przeszlo > 0)
        memcpy(data, bufor, (uint32_t)przeszlo);
    return przeszlo;
}
