#ifndef STUB_RTL8139_H
#define STUB_RTL8139_H
#include <stdint.h>
/* Karta udaje, ze dziala: pakiety wychodzace znikaja w prozni */
static inline int rtl8139_present(void) { return 1; }
static inline const uint8_t *rtl8139_mac(void)
{
    static const uint8_t mac[6] = {0x52,0x54,0x00,0x12,0x34,0x56};
    return mac;
}
static inline int rtl8139_send(const void *f, uint16_t len)
{ (void)f; (void)len; return 0; }
static inline uint32_t rtl8139_rx_packets(void) { return 0; }
static inline uint32_t rtl8139_tx_packets(void) { return 0; }
static inline int rtl8139_init(void) { return 1; }
#endif
