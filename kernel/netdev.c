/* The network card layer: one name, several drivers
 *
 * Exactly the same idea as blk.c for disks. Until now the network code
 * asked for the RTL8139 directly, and that showed in every line that
 * needed a MAC address. There is more than one kind of card, though,
 * and they differ in everything: the RTL8139 is configured through I/O
 * ports and has one ring of bytes, the e1000 has registers in memory
 * and two rings of descriptors. To ARP and IP that makes no difference
 * whatsoever.
 *
 * The order of attempts goes from newer to older: when a machine has
 * both cards, it is better to take the one that can do more.
 */

#include "include/netdev.h"
#include "include/rtl8139.h"
#include "include/e1000.h"
#include "include/usbnet.h"

enum { NIC_NONE, NIC_E1000, NIC_RTL8139, NIC_USB };

static int nic;

int netdev_init(void)
{
    if (nic)
        return 1;
    if (e1000_init())
        nic = NIC_E1000;
    else if (rtl8139_init())
        nic = NIC_RTL8139;
    /* And last the one in the socket: a machine with a card of its own
       keeps using it, and a machine with nothing else can still reach
       the network through a docking station. */
    else if (usbnet_init())
        nic = NIC_USB;
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
    case NIC_USB:     return "USB (RNDIS)";
    default:            return "brak";
    }
}

const uint8_t *netdev_mac(void)
{
    static const uint8_t zeros[6];
    switch (nic) {
    case NIC_E1000:   return e1000_mac();
    case NIC_RTL8139: return rtl8139_mac();
    case NIC_USB:     return usbnet_mac();
    default:            return zeros;
    }
}

int netdev_send(const void *frame, uint16_t len)
{
    switch (nic) {
    case NIC_E1000:   return e1000_send(frame, len);
    case NIC_RTL8139: return rtl8139_send(frame, len);
    case NIC_USB:     return usbnet_send(frame, len);
    default:            return -1;
    }
}

uint32_t netdev_rx_packets(void)
{
    switch (nic) {
    case NIC_E1000:   return e1000_rx_packets();
    case NIC_RTL8139: return rtl8139_rx_packets();
    case NIC_USB:     return usbnet_rx_packets();
    default:            return 0;
    }
}

uint32_t netdev_tx_packets(void)
{
    switch (nic) {
    case NIC_E1000:   return e1000_tx_packets();
    case NIC_RTL8139: return rtl8139_tx_packets();
    case NIC_USB:     return usbnet_tx_packets();
    default:            return 0;
    }
}
