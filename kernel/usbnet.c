/* USB Ethernet: RNDIS
 *
 * The card in a docking station. It differs from the two cards this
 * system already knows in a way that goes deeper than registers: there
 * are no registers. Everything is a conversation - "start yourself",
 * "what is your address", "pass me broadcasts" - carried as
 * ENCAPSULATED COMMANDS through endpoint zero, and every frame gets a
 * forty-four byte header in front of it on the way to the bulk
 * endpoints.
 *
 * The protocol is RNDIS, which is NDIS - the driver interface of
 * Windows - stretched down a wire. That is why its messages ask about
 * "OIDs" instead of about hardware: an OID is a number naming a
 * property, and 0x01010101 happens to mean "your permanent Ethernet
 * address".
 *
 * The other difference is that nothing here can be done from an
 * interrupt. A PCI card raises one and the handler reads the ring; a
 * USB card has to be ASKED, and asking means a transfer, and a
 * transfer cannot be done inside an interrupt handler. So frames are
 * collected by a thread that spends its life waiting on a bulk read -
 * which is exactly what that thread is for.
 */

#include <stdint.h>
#include "include/usbnet.h"
#include "include/usbhc.h"
#include "include/net.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"

#define MSG_PACKET      0x00000001u
#define MSG_INIT        0x00000002u
#define MSG_QUERY       0x00000004u
#define MSG_SET         0x00000005u
#define CMPLT_INIT      0x80000002u
#define CMPLT_QUERY     0x80000004u
#define CMPLT_SET       0x80000005u

#define OID_MAC_ADDRESS 0x01010101u      /* the permanent one */
#define OID_FILTER      0x0001010Eu      /* what to pass upwards */
#define FILTER_NORMAL   0x0000000Fu      /* to us, multicast, broadcast */

#define RNDIS_HEADER    44               /* in front of every frame */
#define FRAME_MAX       1600

static int dev = -1;
static int ep_in, ep_out;
static int control_iface;
static int present;
static uint8_t mac[6];
static uint32_t rx_packets, tx_packets;
static uint32_t request_id = 1;

/* The controller reaches for these itself */
static uint8_t msg[256] __attribute__((aligned(64)));
static uint8_t reply[256] __attribute__((aligned(64)));
static uint8_t txbuf[RNDIS_HEADER + FRAME_MAX] __attribute__((aligned(64)));
static uint8_t rxbuf[2048] __attribute__((aligned(64)));
static uint8_t desc_buf[256] __attribute__((aligned(16)));

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A command down endpoint zero, and the answer from the same place.
 *
 * The card is allowed to think about it, and says it has an answer on
 * an endpoint we do not read - so we simply ask again a few times.
 * That costs a few transfers on a machine's startup and saves a whole
 * endpoint's worth of code. */
static int talk(uint32_t len, uint32_t want_type)
{
    uint8_t setup[8];

    /* SEND_ENCAPSULATED_COMMAND */
    setup[0] = 0x21; setup[1] = 0x00;
    setup[2] = 0; setup[3] = 0;
    setup[4] = (uint8_t)control_iface; setup[5] = 0;
    setup[6] = (uint8_t)len; setup[7] = (uint8_t)(len >> 8);
    if (usbhc_control(dev, setup, msg, (int)len, 0) < 0)
        return -1;

    for (int attempt = 0; attempt < 20; attempt++) {
        task_sleep(5);
        /* GET_ENCAPSULATED_RESPONSE */
        setup[0] = 0xA1; setup[1] = 0x01;
        setup[2] = 0; setup[3] = 0;
        setup[4] = (uint8_t)control_iface; setup[5] = 0;
        setup[6] = (uint8_t)sizeof(reply);
        setup[7] = (uint8_t)(sizeof(reply) >> 8);

        int n = usbhc_control(dev, setup, reply, (int)sizeof(reply), 1);
        if (n >= 12 && get32(reply) == want_type)
            return n;
    }
    return -1;
}

static int rndis_init(void)
{
    memset(msg, 0, 24);
    put32(msg + 0, MSG_INIT);
    put32(msg + 4, 24);
    put32(msg + 8, request_id++);
    put32(msg + 12, 1);                  /* the version we speak */
    put32(msg + 16, 0);
    put32(msg + 20, 0x4000);             /* the largest transfer we take */

    if (talk(24, CMPLT_INIT) < 0)
        return 0;
    return get32(reply + 12) == 0;       /* status */
}

static int rndis_query(uint32_t oid, uint8_t *out, uint32_t outlen)
{
    memset(msg, 0, 28);
    put32(msg + 0, MSG_QUERY);
    put32(msg + 4, 28);
    put32(msg + 8, request_id++);
    put32(msg + 12, oid);
    put32(msg + 16, 0);                  /* no data going with the question */
    /* And the offset of that data is ZERO, not the twenty a message
       WITH data would carry. The card checks that the offset points
       inside the message, and a question of twenty-eight bytes whose
       data begins at the twenty-eighth is a question it refuses to
       answer - which looks exactly like a card that does not work. */
    put32(msg + 20, 0);
    put32(msg + 24, 0);

    int n = talk(28, CMPLT_QUERY);
    if (n < 24 || get32(reply + 12) != 0)
        return 0;

    uint32_t len = get32(reply + 16);
    uint32_t off = get32(reply + 20) + 8;   /* counted from the field itself */
    if (len < outlen || off + outlen > (uint32_t)n)
        return 0;
    for (uint32_t i = 0; i < outlen; i++)
        out[i] = reply[off + i];
    return 1;
}

static int rndis_set(uint32_t oid, uint32_t value)
{
    memset(msg, 0, 32);
    put32(msg + 0, MSG_SET);
    put32(msg + 4, 32);
    put32(msg + 8, request_id++);
    put32(msg + 12, oid);
    put32(msg + 16, 4);                  /* four bytes of value */
    put32(msg + 20, 20);
    put32(msg + 24, 0);
    put32(msg + 28, value);

    if (talk(32, CMPLT_SET) < 0)
        return 0;
    return get32(reply + 12) == 0;
}

/* --- frames ---------------------------------------------------------- */

int usbnet_send(const void *frame, uint16_t len)
{
    if (!present || !len || len > FRAME_MAX)
        return -1;

    memset(txbuf, 0, RNDIS_HEADER);
    put32(txbuf + 0, MSG_PACKET);
    put32(txbuf + 4, RNDIS_HEADER + len);
    /* The offset is counted from this field, not from the start of the
       message - which is the kind of detail that makes a card answer
       nothing at all and say nothing about why. */
    put32(txbuf + 8, RNDIS_HEADER - 8);
    put32(txbuf + 12, len);
    memcpy(txbuf + RNDIS_HEADER, frame, len);

    if (usbhc_bulk_out(dev, ep_out, txbuf, RNDIS_HEADER + len) < 0)
        return -1;
    tx_packets++;
    return 0;
}

/* Everything that came in one transfer: a card is allowed to put
   several frames into one, one after another. */
static void take_frames(int n)
{
    int at = 0;

    while (at + 16 <= n) {
        uint32_t type = get32(rxbuf + at);
        uint32_t msglen = get32(rxbuf + at + 4);
        uint32_t off = get32(rxbuf + at + 8);
        uint32_t len = get32(rxbuf + at + 12);

        if (type != MSG_PACKET || msglen < 16 || at + (int)msglen > n)
            return;
        uint32_t start = (uint32_t)at + off + 8;
        if (len && len <= FRAME_MAX && start + len <= (uint32_t)n) {
            rx_packets++;
            net_rx(rxbuf + start, (uint16_t)len);
        }
        at += (int)msglen;
    }
}

static void rx_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int n = usbhc_bulk_in(dev, ep_in, rxbuf, (int)sizeof(rxbuf));
        if (n > 0)
            take_frames(n);
        else
            task_sleep(5);               /* nothing came - do not spin */
    }
}

/* --- finding one ----------------------------------------------------- */

/* The control interface of an RNDIS card says it is a communications
   device with a vendor-specific protocol, and the data interface next
   to it carries the two bulk endpoints. */
static int find_card(const uint8_t *cfg, int total)
{
    int iface = -1, found_control = -1;

    ep_in = ep_out = 0;
    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        if (dlen < 2)
            break;

        if (cfg[i + 1] == 0x04 && i + 8 < total) {
            iface = cfg[i + 2];
            uint8_t cls = cfg[i + 5], sub = cfg[i + 6], proto = cfg[i + 7];

            if (cls == 0x02 && sub == 0x02 && proto == 0xFF)
                found_control = iface;
        } else if (cfg[i + 1] == 0x05 && i + 6 < total &&
                   found_control >= 0) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            if ((attr & 0x03) == 0x02) {           /* bulk */
                if (addr & 0x80)
                    ep_in = addr & 0x0F;
                else
                    ep_out = addr & 0x0F;
            }
        }
        i += dlen;
    }
    return (ep_in && ep_out) ? found_control : -1;
}

static int try_port(int port)
{
    int d = usbhc_attach(port);

    if (d < 0)
        return 0;
    if (usbhc_descriptor(d, 0x02, 0, desc_buf, 9) < 9)
        return 0;
    int total = desc_buf[2] | (desc_buf[3] << 8);
    if (total > (int)sizeof(desc_buf))
        total = (int)sizeof(desc_buf);
    if (usbhc_descriptor(d, 0x02, 0, desc_buf, total) < total)
        return 0;

    int control = find_card(desc_buf, total);
    if (control < 0)
        return 0;

    if (usbhc_request(d, 0x00, 0x09, desc_buf[5], 0) < 0)
        return 0;                        /* SET_CONFIGURATION */
    if (usbhc_bulk_endpoint(d, ep_in, ep_out, 64) < 0)
        return 0;

    dev = d;
    control_iface = control;

    if (!rndis_init()) {
        dev = -1;
        return 0;
    }
    if (!rndis_query(OID_MAC_ADDRESS, mac, 6)) {
        dev = -1;
        return 0;
    }
    /* Without this the card receives nothing at all: a filter of zero
       means "pass nothing upwards", and that is what it starts with. */
    rndis_set(OID_FILTER, FILTER_NORMAL);

    usbhc_claim(port);
    present = 1;
    task_create("usbnet", rx_thread, 0);
    printk("[ OK ] A USB network card (RNDIS): "
           "%x:%x:%x:%x:%x:%x\n",
           (uint32_t)mac[0], (uint32_t)mac[1], (uint32_t)mac[2],
           (uint32_t)mac[3], (uint32_t)mac[4], (uint32_t)mac[5]);
    return 1;
}

int usbnet_init(void)
{
    if (present)
        return 1;
    if (!usbhc_init()) {
        return 0;
    }

    for (int port = 0; port < usbhc_ports(); port++)
        if (try_port(port))
            return 1;
    return 0;
}

int usbnet_present(void)
{
    return present;
}

const uint8_t *usbnet_mac(void)
{
    return mac;
}

uint32_t usbnet_rx_packets(void)
{
    return rx_packets;
}

uint32_t usbnet_tx_packets(void)
{
    return tx_packets;
}
