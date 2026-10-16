#ifndef STUB_NETDEV_H
#define STUB_NETDEV_H
#include <stdint.h>
/* The card pretends to work: outgoing packets vanish into thin air.
   We check the parsers on what comes IN, so the other end of the cable
   is of no use here at all. */
static inline int netdev_present(void) { return 1; }
static inline const char *netdev_name(void) { return "stub"; }
static inline const uint8_t *netdev_mac(void)
{
    static const uint8_t mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
    return mac;
}
static inline int netdev_send(const void *f, uint16_t len)
{ (void)f; (void)len; return 0; }
static inline uint32_t netdev_rx_packets(void) { return 0; }
static inline uint32_t netdev_tx_packets(void) { return 0; }
static inline int netdev_init(void) { return 1; }
#endif
