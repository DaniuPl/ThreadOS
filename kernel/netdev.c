/* Warstwa kart sieciowych: jedna nazwa, kilka sterownikow
 *
 * Dokladnie ten sam pomysl, co blk.c dla dyskow. Kod sieciowy pytal
 * dotad wprost o RTL8139 i to bylo widac w kazdej linii, ktora
 * potrzebowala adresu MAC. Kart bywa jednak wiecej niz jedna i roznia
 * sie wszystkim: RTL8139 konfiguruje sie portami I/O i ma jeden
 * pierscien bajtow, e1000 ma rejestry w pamieci i dwa pierscienie
 * deskryptorow. Dla ARP-a i IP nie ma to zadnego znaczenia.
 *
 * Kolejnosc prob jest od nowszej do starszej: gdy w maszynie sa obie
 * karty, lepiej wziac te, ktora umie wiecej.
 */

#include "include/netdev.h"
#include "include/rtl8139.h"
#include "include/e1000.h"

enum { NIC_NONE, NIC_E1000, NIC_RTL8139 };

static int nic;

int netdev_init(void)
{
    if (nic)
        return 1;
    if (e1000_init())
        nic = NIC_E1000;
    else if (rtl8139_init())
        nic = NIC_RTL8139;
    return nic != NIC_NONE;
}

int netdev_present(void)
{
    return nic != NIC_NONE;
}

const char *netdev_name(void)
{
    switch (nic) {
    case NIC_E1000:   return "e1000";
    case NIC_RTL8139: return "RTL8139";
    default:            return "brak";
    }
}

const uint8_t *netdev_mac(void)
{
    static const uint8_t zeros[6];
    switch (nic) {
    case NIC_E1000:   return e1000_mac();
    case NIC_RTL8139: return rtl8139_mac();
    default:            return zeros;
    }
}

int netdev_send(const void *frame, uint16_t len)
{
    switch (nic) {
    case NIC_E1000:   return e1000_send(frame, len);
    case NIC_RTL8139: return rtl8139_send(frame, len);
    default:            return -1;
    }
}

uint32_t netdev_rx_packets(void)
{
    switch (nic) {
    case NIC_E1000:   return e1000_rx_packets();
    case NIC_RTL8139: return rtl8139_rx_packets();
    default:            return 0;
    }
}

uint32_t netdev_tx_packets(void)
{
    switch (nic) {
    case NIC_E1000:   return e1000_tx_packets();
    case NIC_RTL8139: return rtl8139_tx_packets();
    default:            return 0;
    }
}
