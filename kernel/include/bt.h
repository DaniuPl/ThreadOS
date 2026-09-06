#ifndef PROTONOS_BT_H
#define PROTONOS_BT_H

#include <stdint.h>

/* Bluetooth: the HCI conversation with an adapter
 *
 * Every Bluetooth adapter, whatever is inside it, answers the same
 * language: HCI. Commands go down, events come back, and the packets
 * have the same shape whether the adapter sits on USB, on a serial
 * port or on SDIO. In a laptop it sits on USB - internally, on the
 * same bus as the webcam - so this driver speaks to it through the USB
 * layer we already have: commands as control transfers, events on an
 * interrupt endpoint.
 *
 * What is honest to say about this file: the PACKETS are tested, the
 * WIRE is not. QEMU has no Bluetooth device to emulate - the whole
 * stack was removed from it years ago - so nothing here has ever
 * spoken to an adapter that answers. The building and reading of
 * packets is checked against recorded ones ('bt test'), and the rest
 * waits for hardware. That is written down so that nobody mistakes
 * "the tests pass" for "Bluetooth works".
 */

/* Looks for an adapter on the USB bus and wakes it: reset, address,
   name, version. 1 = there is one. */
int bt_init(void);

int bt_present(void);

/* When there is none: why - a sentence for a person. */
const char *bt_why(void);

/* The adapter's own address and what it says about itself */
const char *bt_address(void);
const char *bt_name(void);
const char *bt_version(void);

/* A scan for devices in range (LE advertising). Returns how many were
   heard, or -1 when there is no adapter. */
int bt_scan(uint32_t seconds);

int bt_found(void);
const char *bt_found_addr(int i);
const char *bt_found_name(int i);
int bt_found_rssi(int i);

/* Feeds one event packet through the parser - the transport's job in
   normal life, and a test's job when there is no adapter. */
void bt_event(const uint8_t *pkt, int len);

/* Runs the recorded packets through the parser and prints what it
   understood. This is the only part of Bluetooth that can be tested
   without an adapter, and it says so. */
void bt_selftest(void);

#endif
