/* Wybor kontrolera USB
 *
 * Tak samo jak blk.c wybiera miedzy ATA, AHCI i NVMe: probujemy
 * najpierw xHCI, bo nowsze plyty nie maja juz nic innego, a potem
 * UHCI. Wyzej nikogo nie obchodzi, ktory z nich odpowiedzial.
 *
 * Ten plik jest tez miejscem, w ktorym mieszka rozmowa POWITALNA
 * z urzadzeniem - a wiec ta jedyna czesc rozpoznawania, ktora
 * w obu kontrolerach wyglada zupelnie inaczej. W UHCI adres nadaje
 * sterownik zwyklym zapytaniem i sam musi go sobie wymyslic; w xHCI
 * robi to kontroler przy okazji przydzielania slotu, a sterownik
 * dostaje tylko numer. Powyzej tej granicy jest juz tylko rozmowa
 * z urzadzeniem, ta sama niezaleznie od kontrolera - i dlatego
 * usbkbd.c nie wie, przez co mowi.
 */

#include <stdint.h>
#include "include/usbhc.h"
#include "include/uhci.h"
#include "include/xhci.h"
#include "include/task.h"

enum { BRAK, XHCI, UHCI };

static int kontroler;

/* Urzadzenia po stronie UHCI trzeba opisywac samemu: kontroler nie
   pamieta o nich nic. Przy xHCI cala ta wiedza siedzi w kontrolerze,
   a nam wystarczy numer slotu. */
#define URZADZEN 4

struct urzadzenie {
    int uzyte;
    int adres;
    int lowspeed;
    int maxpakiet;
    int ep;
    int ep_maxpakiet;
    int toggle;
};

static struct urzadzenie lista[URZADZEN];

const char *usbhc_name(void)
{
    if (kontroler == XHCI)
        return "xHCI";
    if (kontroler == UHCI)
        return "UHCI";
    return "brak";
}

int usbhc_init(void)
{
    if (xhci_init()) {
        kontroler = XHCI;
        return 1;
    }
    if (uhci_init()) {
        kontroler = UHCI;
        return 1;
    }
    kontroler = BRAK;
    return 0;
}

int usbhc_ports(void)
{
    if (kontroler == XHCI)
        return xhci_ports();
    if (kontroler == UHCI)
        return uhci_ports();
    return 0;
}

static void zapytanie(uint8_t *p, uint8_t typ, uint8_t req,
                      uint16_t value, uint16_t index, uint16_t len)
{
    p[0] = typ;
    p[1] = req;
    p[2] = (uint8_t)value;
    p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index;
    p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)len;
    p[7] = (uint8_t)(len >> 8);
}

/* Powitanie urzadzenia na UHCI: osiem pierwszych bajtow opisu mowi,
   jak duze pakiety przyjmuje punkt zerowy, a potem nadajemy adres.
   Kazdy port dostaje inny numer - gdyby na pierwszym wisialo cos,
   co klawiatura nie jest, drugi nie moze po nim odziedziczyc adresu. */
static int uhci_powitaj(int port, struct urzadzenie *u)
{
    if (!uhci_port_reset(port))
        return -1;

    u->lowspeed = uhci_port_lowspeed(port);
    u->maxpakiet = 8;

    uint8_t setup[8];
    uint8_t opis[8];
    zapytanie(setup, 0x80, 0x06, 0x0100, 0, 8);
    if (uhci_control(0, u->lowspeed, u->maxpakiet, setup, opis, 8, 1) < 8)
        return -1;
    if (opis[7])
        u->maxpakiet = opis[7];

    int adres = port + 1;
    zapytanie(setup, 0x00, 0x05, (uint16_t)adres, 0, 0);
    if (uhci_control(0, u->lowspeed, u->maxpakiet, setup, (void *)0, 0, 0) < 0)
        return -1;
    u->adres = adres;
    task_sleep(20);
    return 0;
}

int usbhc_attach(int port)
{
    if (kontroler == XHCI) {
        int slot = xhci_attach(port);
        if (slot < 0)
            return -1;
        /* Uchwyt to numer slotu; zeby nie mieszal sie z uchwytami
           UHCI, przechowujemy go tak samo - kazdy backend czyta swoje */
        return slot;
    }
    if (kontroler != UHCI)
        return -1;

    for (int i = 0; i < URZADZEN; i++) {
        if (lista[i].uzyte)
            continue;
        struct urzadzenie u = { 0, 0, 0, 8, 0, 8, 0 };
        if (uhci_powitaj(port, &u) < 0)
            return -1;
        u.uzyte = 1;
        lista[i] = u;
        return i + 1;
    }
    return -1;
}

int usbhc_control(int dev, const void *setup, void *data, int len, int in)
{
    if (dev < 1)
        return -1;
    if (kontroler == XHCI)
        return xhci_control(dev, setup, data, len, in);
    if (kontroler != UHCI || dev > URZADZEN)
        return -1;
    struct urzadzenie *u = &lista[dev - 1];
    return uhci_control(u->adres, u->lowspeed, u->maxpakiet, setup, data,
                        len, in);
}

int usbhc_int_endpoint(int dev, int ep, int maxpacket, int interval)
{
    if (dev < 1)
        return -1;
    if (kontroler == XHCI)
        return xhci_int_endpoint(dev, ep, maxpacket, interval);
    if (kontroler != UHCI || dev > URZADZEN)
        return -1;
    /* UHCI nie ma czego zakladac: o punkcie koncowym mowi sie mu przy
       kazdej przesylce z osobna */
    struct urzadzenie *u = &lista[dev - 1];
    u->ep = ep;
    u->ep_maxpakiet = maxpacket;
    u->toggle = 0;
    return 0;
}

int usbhc_int_in(int dev, int ep, void *data, int len)
{
    if (dev < 1)
        return -1;
    if (kontroler == XHCI)
        return xhci_int_in(dev, ep, data, len);
    if (kontroler != UHCI || dev > URZADZEN)
        return -1;
    struct urzadzenie *u = &lista[dev - 1];
    return uhci_interrupt_in(u->adres, u->lowspeed, ep, u->ep_maxpakiet,
                             data, len, &u->toggle);
}
