#ifndef PROTONOS_DHCP_H
#define PROTONOS_DHCP_H

/* DHCP client: DISCOVER -> OFFER -> REQUEST -> ACK.
   Runs in a thread of its own, because it waits for server replies. */

/* Creates the thread that configures the network (call after
   scheduler_init) */
void dhcp_start(void);

/* The full exchange, synchronously - from a thread context only.
   1 = we got an address, 0 = no server / timeout. */
int dhcp_configure(void);

#endif
