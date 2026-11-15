#ifndef THREADOS_TCP_H
#define THREADOS_TCP_H

#include <stdint.h>

/* Minimal TCP: active open (client), passive open (server), data
   transfer with acknowledgements and retransmission. 'owner' is a
   process pid (-1 = a kernel connection); connections left by dead
   processes are closed, just like UDP sockets and desktop windows. */

void tcp_init(void);

/* Opens a connection; blocks up to timeout_ms. Returns an id or -1. */
int tcp_connect(uint32_t ip, uint16_t port, int owner, uint32_t timeout_ms);

/* The same without waiting: the SYN is on its way, the id comes back
   at once and tcp_poll says when it is connected (or failed) */
int tcp_connect_start(uint32_t ip, uint16_t port, int owner);
/* What a connection could do now, for poll(): TCP_POLL_* bits, and in
   the top 16 the bytes waiting to be read. -1 = no such connection. */
#define TCP_POLL_IN        0x01  /* data waiting */
#define TCP_POLL_OUT       0x02  /* a send would not wait */
#define TCP_POLL_EOF       0x04  /* the other side has finished */
#define TCP_POLL_ERROR     0x08  /* reset, or the retries ran out */
#define TCP_POLL_CONNECTED 0x10  /* the handshake is done */
#define TCP_POLL_ACCEPT    0x20  /* a listener has a connection waiting */
int tcp_poll(int conn, int owner);
/* Who is at the other end, and our own port */
int tcp_addresses(int conn, int owner, uint32_t *remote_ip,
                  uint16_t *remote_port, uint16_t *local_port);
/* Listening on a port: returns the listening socket id or -1 */
int tcp_listen(uint16_t port, int owner);

/* Accepts an established connection from a listening socket's queue */
int tcp_accept(int listener, int owner, uint32_t timeout_ms);

/* Sends data and waits for an acknowledgement. Returns bytes sent or -1. */
int tcp_send(int conn, int owner, const void *data, uint16_t len,
             uint32_t timeout_ms);

/* Receives data: >0 = bytes, 0 = the other side closed (FIN),
   -1 = error or timeout. */
int tcp_recv(int conn, int owner, void *buf, uint16_t max,
             uint32_t timeout_ms);

void tcp_close(int conn, int owner);

/* Entry from the IP layer (card interrupt context) */
void tcp_input(uint32_t src_ip, uint32_t dst_ip, const uint8_t *src_mac,
               const uint8_t *seg, uint16_t len);

void tcp_dump(void);            /* the 'netstat' command */

#endif
