/* Bluetooth over USB: commands, events and ACL data
 *
 * The adapter in a laptop hangs off the internal USB bus and belongs to
 * a class of its own: 0xE0/0x01/0x01. Its endpoints are always the
 * same three - an interrupt one for events, two bulk ones for data -
 * and commands do not travel through any of them: they go as CONTROL
 * transfers, with a request type of 0x20 and everything else zero.
 * That is the whole transport, and it has been the same since 2001.
 *
 * What this driver does: reset the adapter, ask for its address, its
 * name and its version, look for the devices around it, and carry
 * framed HCI ACL packets over the two bulk endpoints. L2CAP, security,
 * pairing and HID profiles still have to be built on top of that
 * transport.
 *
 * Looking is two searches at once. A phone whose Bluetooth settings
 * are open answers an INQUIRY - classic Bluetooth, the way phones,
 * headsets and computers are found - with its name in the answer
 * (extended inquiry results) or, from older devices, without one, and
 * then the name is asked for (Remote Name Request). Watches, tags and
 * mice ADVERTISE over Bluetooth Low Energy instead, many with their
 * name only in the answer to an active scan. The first version of this
 * did only a passive LE scan, under an event mask that left LE events
 * switched off, so it found nothing - not even the phone in the hand.
 *
 * QEMU emulates no Bluetooth device, so scripts/btadapter.py makes one
 * up - an HCI controller on USB, plugged in through usbredir, with a
 * phone and a watch around it - and t_bt holds the whole conversation
 * against it. Adapters from Intel and Broadcom may additionally want
 * firmware patches before everything works; the Intel Wireless 3160's
 * Bluetooth half answers all of the above without one.
 */

#include <stdint.h>
#include "include/bt.h"
#include "include/bt_info.h"
#include "include/usbhc.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/fs.h"
#include "include/random.h"
#include "include/user.h"
#include "../kernel64/task64.h"
/* AES alone, for LE pairing's c1 and s1 - not the programs' calls */
#define THREADOS_UAPI_H
#include "../user/uaes.h"
#undef THREADOS_UAPI_H

#define DEVICES_MAX 12          /* how many neighbours we remember */
#define NAME_MAX    32
#define ACL_DATA_MAX 1024

/* Opcodes: the group in the upper six bits, the command in the lower
   ten. Written out whole, because that is how they appear in a trace
   and in the specification's tables. */
#define OP_RESET            0x0C03
#define OP_SET_EVENT_MASK   0x0C01
#define OP_READ_LOCAL_NAME  0x0C14
#define OP_READ_VERSION     0x1001
#define OP_READ_BDADDR      0x1009
#define OP_LE_SCAN_PARAMS   0x200B
#define OP_LE_SCAN_ENABLE   0x200C
#define OP_INQUIRY          0x0401
#define OP_INQUIRY_CANCEL   0x0402
#define OP_REMOTE_NAME      0x0419
#define OP_WRITE_INQ_MODE   0x0C45
#define OP_WRITE_LE_HOST    0x0C6D

#define EV_INQUIRY_COMPLETE 0x01
#define EV_INQUIRY_RESULT   0x02
#define EV_REMOTE_NAME      0x07
#define EV_CMD_COMPLETE     0x0E
#define EV_CMD_STATUS       0x0F
#define EV_INQUIRY_RSSI     0x22
#define EV_INQUIRY_EXT      0x2F
#define EV_LE_META          0x3E

struct neighbour {
    uint8_t addr[6];
    char name[NAME_MAX];
    int rssi;
    int classic;                /* 1 = heard through an inquiry, 0 = LE */
    /* What a name request needs to reach a classic device quickly:
       how it listens for pages, and its clock against ours */
    uint8_t scan_mode;
    uint16_t clock;
    uint8_t addr_type;          /* LE: 0 public, 1 random */
    uint32_t cod;               /* the class of device: phone, headset... */
};

static int dev = -1;            /* the handle from the USB layer */
static int ep_events;
static int ep_maxpacket = 16;
static int ep_interval = 1;
static int ep_acl_in, ep_acl_out, ep_acl_maxpacket;
static int acl_ready;
static int present;
static const char *gave_up;

static uint8_t local_addr[6];
static char local_name[NAME_MAX];
static char version[40];
static int have_addr, have_name, have_version;

static struct neighbour seen[DEVICES_MAX];
static int nseen;
static volatile int inquiry_done;
static int name_asked;          /* the name request waiting for its answer */
static uint8_t name_addr[6];

/* The answer to the last command, for whoever is waiting for it */
static uint16_t last_opcode;
static int last_status = -1;

static uint8_t report[260] __attribute__((aligned(16)));
static uint8_t desc_buf[256] __attribute__((aligned(16)));
static uint8_t cmdbuf[260] __attribute__((aligned(16)));
static uint8_t acl_tx[ACL_DATA_MAX + 4] __attribute__((aligned(16)));
static uint8_t acl_rx[ACL_DATA_MAX + 4] __attribute__((aligned(16)));

/* --- text ----------------------------------------------------------- */

static char hexdigit(int v)
{
    return (char)(v < 10 ? '0' + v : 'a' + v - 10);
}

/* An address is printed the other way round than it travels: on the
   wire the lowest byte comes first, on a screen the highest. Getting
   this backwards gives an address that is right and unrecognisable. */
static void addr_text(const uint8_t *a, char *out)
{
    int k = 0;
    for (int i = 5; i >= 0; i--) {
        out[k++] = hexdigit(a[i] >> 4);
        out[k++] = hexdigit(a[i] & 15);
        if (i)
            out[k++] = ':';
    }
    out[k] = 0;
}

static char addr_str[18];
static char found_str[18];

/* --- the parser ------------------------------------------------------
 *
 * Everything below reads bytes and updates what we know. It touches no
 * hardware on purpose: that is what lets it be tested with recorded
 * packets on a machine with no Bluetooth in it.
 */

static struct neighbour *remember(const uint8_t *addr, int classic)
{
    for (int i = 0; i < nseen; i++)
        if (!memcmp(seen[i].addr, addr, 6))
            return &seen[i];
    if (nseen >= DEVICES_MAX)
        return (struct neighbour *)0;

    struct neighbour *n = &seen[nseen++];
    memcpy(n->addr, addr, 6);
    n->name[0] = 0;
    n->rssi = 0;
    n->classic = classic;
    return n;
}

/* What a device says about itself while advertising: a chain of
   pieces, each one a length, a type and that many bytes less one. We
   take the name and ignore the rest - the rest is what a full stack
   would use to know a watch from a headset. */
static void ad_data(struct neighbour *n, const uint8_t *p, int len)
{
    for (int i = 0; i < len;) {
        int flen = p[i];
        if (flen < 1 || i + flen >= len + 1)
            break;
        uint8_t type = p[i + 1];

        if ((type == 0x09 || type == 0x08) && !n->name[0]) {
            int k = 0;
            for (int j = 2; j <= flen && k < NAME_MAX - 1; j++) {
                char c = (char)p[i + j];
                n->name[k++] = (c >= 32 && c < 127) ? c : '?';
            }
            n->name[k] = 0;
        }
        i += flen + 1;
    }
}

static void le_conn_complete(const uint8_t *p, int len);

static void le_meta(const uint8_t *p, int len)
{
    if (len >= 1 && p[0] == 0x01) {      /* LE connection complete */
        le_conn_complete(p + 1, len - 1);
        return;
    }
    if (len < 2 || p[0] != 0x02)         /* else only the advertising report */
        return;

    int reports = p[1];
    int i = 2;
    for (int r = 0; r < reports && i + 9 <= len; r++) {
        /* event type, address type, address, data length, data, RSSI */
        const uint8_t *addr = p + i + 2;
        int dlen = p[i + 8];
        if (i + 9 + dlen > len)
            break;

        struct neighbour *n = remember(addr, 0);
        if (n) {
            n->addr_type = p[i + 1] & 1;
            ad_data(n, p + i + 9, dlen);
            n->rssi = (int)(int8_t)p[i + 9 + dlen];
        }
        i += 10 + dlen;
    }
}

/* The answers to an inquiry come in three forms, by the inquiry mode
   and the adapter's age: the plain one (no name, no signal), one with
   the signal strength, and the extended one with the name inside - the
   same pieces as LE advertising data. In the first two the devices of
   one event are laid out field by field: every address first, then
   every scan mode, and so on. */
static void inquiry_result(const uint8_t *p, int len, int kind)
{
    if (len < 1)
        return;
    int n = p[0];
    if (kind == EV_INQUIRY_EXT)
        n = 1;
    if (n < 1 || 1 + 14 * n > len)
        return;
    const uint8_t *addr = p + 1, *mode = addr + 6 * n;
    const uint8_t *cod = mode + (kind == EV_INQUIRY_RESULT ? 3 : 2) * n;
    const uint8_t *clk = cod + 3 * n, *rssi = clk + 2 * n;

    for (int r = 0; r < n; r++) {
        struct neighbour *d = remember(addr + 6 * r, 1);
        if (!d)
            break;
        d->scan_mode = mode[r];
        d->cod = (uint32_t)(cod[3 * r] | (cod[3 * r + 1] << 8) |
                            (cod[3 * r + 2] << 16));
        d->clock = (uint16_t)(clk[2 * r] | (clk[2 * r + 1] << 8));
        if (kind != EV_INQUIRY_RESULT)
            d->rssi = (int)(int8_t)rssi[r];
    }
    if (kind == EV_INQUIRY_EXT && len > 15) {
        struct neighbour *d = remember(addr, 1);
        if (d)
            ad_data(d, p + 15, len - 15);
    }
}

/* The answer to a name request: the name, up to 248 bytes, ended by a
   zero when shorter */
static void remote_name(const uint8_t *p, int len)
{
    if (len < 7)
        return;
    if (name_asked && !memcmp(p + 1, name_addr, 6))
        name_asked = 0;
    if (p[0] != 0)
        return;                          /* it would not say */
    for (int i = 0; i < nseen; i++) {
        if (memcmp(seen[i].addr, p + 1, 6))
            continue;
        int k = 0;
        for (int j = 7; j < len && p[j] && k < NAME_MAX - 1; j++) {
            char c = (char)p[j];
            seen[i].name[k++] = (c >= 32 && c < 127) ? c : '?';
        }
        seen[i].name[k] = 0;
    }
}

static void pair_event(uint8_t code, const uint8_t *p, int plen);

void bt_event(const uint8_t *pkt, int len)
{
    if (len < 2)
        return;
    uint8_t code = pkt[0];
    int plen = pkt[1];
    const uint8_t *p = pkt + 2;

    if (plen > len - 2)
        plen = len - 2;

    switch (code) {
    case EV_CMD_COMPLETE:
        if (plen < 3)
            return;
        last_opcode = (uint16_t)(p[1] | (p[2] << 8));
        last_status = plen > 3 ? p[3] : 0;
        if (last_status != 0)
            return;

        if (last_opcode == OP_READ_BDADDR && plen >= 10) {
            memcpy(local_addr, p + 4, 6);
            have_addr = 1;
        } else if (last_opcode == OP_READ_LOCAL_NAME && plen >= 5) {
            int k = 0;
            for (int i = 4; i < plen && k < NAME_MAX - 1; i++) {
                if (!p[i])
                    break;
                local_name[k++] = (char)p[i];
            }
            local_name[k] = 0;
            have_name = 1;
        } else if (last_opcode == OP_READ_VERSION && plen >= 12) {
            /* HCI version, HCI revision, LMP version, manufacturer,
               LMP subversion - we print the two that mean something to
               a person. */
            /* HCI version, revision (2), LMP version, MANUFACTURER (2),
               LMP subversion - the maker is at 8, not at 7; one byte
               out and every adapter in the world becomes somebody
               else's. */
            uint16_t maker = (uint16_t)(p[8] | (p[9] << 8));
            char *w = version;
            const char *core = "Bluetooth ";
            while (*core)
                *w++ = *core++;
            /* The HCI version number maps onto the marketing one:
               6 = 4.0, 7 = 4.1, 8 = 4.2, 9 = 5.0, 10 = 5.1, 11 = 5.2 */
            static const char *names[] = { "1.0b", "1.1", "1.2", "2.0",
                                           "2.1", "3.0", "4.0", "4.1",
                                           "4.2", "5.0", "5.1", "5.2",
                                           "5.3", "5.4" };
            int v = p[4];
            const char *nm = v < (int)(sizeof(names) / sizeof(names[0]))
                             ? names[v] : "?";
            while (*nm)
                *w++ = *nm++;
            const char *by = ", maker ";
            while (*by)
                *w++ = *by++;
            *w++ = hexdigit((maker >> 12) & 15);
            *w++ = hexdigit((maker >> 8) & 15);
            *w++ = hexdigit((maker >> 4) & 15);
            *w++ = hexdigit(maker & 15);
            *w = 0;
            have_version = 1;
        }
        return;

    case EV_CMD_STATUS:
        if (plen >= 4) {
            last_status = p[0];
            last_opcode = (uint16_t)(p[2] | (p[3] << 8));
        }
        return;

    case EV_LE_META:
        le_meta(p, plen);
        return;

    case EV_INQUIRY_RESULT:
    case EV_INQUIRY_RSSI:
    case EV_INQUIRY_EXT:
        inquiry_result(p, plen, code);
        return;

    case EV_INQUIRY_COMPLETE:
        inquiry_done = 1;
        return;

    case EV_REMOTE_NAME:
        remote_name(p, plen);
        return;

    default:
        pair_event(code, p, plen);
        return;
    }
}

/* --- the transport --------------------------------------------------- */

static int give_up(const char *why)
{
    gave_up = why;
    return 0;
}

/* A command does not travel through an endpoint: it is a control
   transfer with a request type of 0x20 and everything else zero. */
static int send_command(uint16_t opcode, const uint8_t *params, int plen)
{
    uint8_t setup[8] = { 0x20, 0x00, 0, 0, 0, 0, 0, 0 };

    if (plen > 255 || dev < 0)
        return -1;
    cmdbuf[0] = (uint8_t)opcode;
    cmdbuf[1] = (uint8_t)(opcode >> 8);
    cmdbuf[2] = (uint8_t)plen;
    for (int i = 0; i < plen; i++)
        cmdbuf[3 + i] = params[i];

    setup[6] = (uint8_t)(plen + 3);
    setup[7] = 0;
    last_status = -1;
    last_opcode = 0;
    return usbhc_control(dev, setup, cmdbuf, plen + 3, 0);
}

static int acl_build(uint8_t *packet, int capacity, uint16_t handle,
                     uint8_t packet_boundary, uint8_t broadcast,
                     const uint8_t *payload, uint16_t len)
{
    if (!packet || handle > 0x0FFF || packet_boundary > 3 ||
        broadcast > 3 || len > ACL_DATA_MAX || capacity < (int)len + 4 ||
        (len && !payload))
        return -1;

    uint16_t flags = handle | ((uint16_t)packet_boundary << 12) |
                     ((uint16_t)broadcast << 14);
    packet[0] = (uint8_t)flags;
    packet[1] = (uint8_t)(flags >> 8);
    packet[2] = (uint8_t)len;
    packet[3] = (uint8_t)(len >> 8);
    if (len)
        memcpy(packet + 4, payload, len);
    return (int)len + 4;
}

static int acl_parse(const uint8_t *packet, int packet_len, uint16_t *handle,
                     uint8_t *packet_boundary, uint8_t *broadcast,
                     const uint8_t **payload, uint16_t *payload_len)
{
    if (!packet || packet_len < 4 || !handle || !packet_boundary ||
        !broadcast || !payload || !payload_len)
        return -1;

    uint16_t flags = (uint16_t)(packet[0] | (packet[1] << 8));
    uint16_t len = (uint16_t)(packet[2] | (packet[3] << 8));
    if (len > ACL_DATA_MAX || packet_len != (int)len + 4)
        return -1;

    *handle = flags & 0x0FFF;
    *packet_boundary = (uint8_t)((flags >> 12) & 3);
    *broadcast = (uint8_t)((flags >> 14) & 3);
    *payload = packet + 4;
    *payload_len = len;
    return 0;
}

int bt_acl_available(void)
{
    return present && acl_ready;
}

int bt_acl_send(uint16_t handle, uint8_t packet_boundary, uint8_t broadcast,
                const void *payload, uint16_t len)
{
    if (!bt_acl_available())
        return -1;
    int packet_len = acl_build(acl_tx, sizeof(acl_tx), handle,
                               packet_boundary, broadcast, payload, len);
    if (packet_len < 0)
        return -1;
    return usbhc_bulk_out(dev, ep_acl_out, acl_tx, packet_len) == packet_len
               ? 0 : -1;
}

int bt_acl_receive(uint16_t *handle, uint8_t *packet_boundary,
                   uint8_t *broadcast, void *payload, uint16_t capacity)
{
    if (!bt_acl_available() || !handle || !packet_boundary || !broadcast ||
        (!payload && capacity))
        return -1;

    int packet_len = usbhc_bulk_in(dev, ep_acl_in, acl_rx, sizeof(acl_rx));
    if (packet_len < 0)
        return -1;

    const uint8_t *data;
    uint16_t len;
    if (acl_parse(acl_rx, packet_len, handle, packet_boundary, broadcast,
                  &data, &len) < 0 || len > capacity)
        return -1;
    if (len)
        memcpy(payload, data, len);
    return len;
}

/* --- events arrive in pieces ------------------------------------------
 *
 * The interrupt endpoint carries one packet at a time, and a packet is
 * at most its endpoint's size: 64 bytes on Intel's adapters, 16 on
 * many others. An event can be 257 bytes - the answer to Read Local
 * Name alone is 254 - so it comes in several. Its second byte says how
 * long the whole is, and pieces are gathered until it is all there.
 * Taking each packet for a whole event, as this driver used to, reads
 * a name cut short and then parses the rest of it as events of its
 * own.
 *
 * Every event starts a transfer of its own, so a piece never carries
 * the end of one event and the start of the next; and a piece shorter
 * than a full packet ends a transfer, so an event still short of its
 * length after one was damaged on the way and is dropped rather than
 * glued to whatever comes next.
 */
static uint8_t ev[2 + 255];
static int ev_have;

/* One piece. 1 = it completed an event, which has been handled. */
static int ev_piece(const uint8_t *p, int n, int maxpacket)
{
    if (n <= 0)
        return 0;
    if (ev_have + n > (int)sizeof(ev)) {
        ev_have = 0;                     /* longer than any event can be */
        return 0;
    }
    memcpy(ev + ev_have, p, (uint32_t)n);
    ev_have += n;
    if (ev_have >= 2 && ev_have >= 2 + ev[1]) {
        bt_event(ev, 2 + ev[1]);
        ev_have = 0;
        return 1;
    }
    if (n < maxpacket)
        ev_have = 0;
    return 0;
}

/* One look at the event endpoint: -1 error, 0 nothing yet, 1 a piece
   arrived, 2 a piece arrived and completed an event. */
static int poll_event(void)
{
    int n = usbhc_int_in(dev, ep_events, report, ep_maxpacket);
    if (n <= 0)
        return n < 0 ? -1 : 0;
    return ev_piece(report, n, ep_maxpacket) ? 2 : 1;
}

/* Reads whatever the adapter has to say, for up to 'ms' milliseconds.
   Every event goes through the same parser the test uses. */
static void pump(uint32_t ms)
{
    for (uint32_t i = 0; i < ms / 10 + 1; i++)
        if (poll_event() <= 0)
            task_sleep(10);
}

/* A command and the answer to it. 1 = the adapter said it worked. */
static int command(uint16_t opcode, const uint8_t *params, int plen,
                   uint32_t ms)
{
    if (send_command(opcode, params, plen) < 0)
        return 0;
    for (uint32_t i = 0; i < ms / 10 + 1; i++) {
        int got = poll_event();
        if (got == 2 && last_opcode == opcode)
            return last_status == 0;
        /* A piece that arrived means more may be right behind it */
        if (got <= 0)
            task_sleep(10);
    }
    return 0;
}

/* Looks in the configuration for the Bluetooth interface, its event
   endpoint and the two bulk endpoints used for HCI ACL data. */
static int find_adapter(const uint8_t *cfg, int total)
{
    int iface = -1;
    int found = -1;
    int have_events = 0;
    int in_mps = 0, out_mps = 0;

    ep_events = ep_acl_in = ep_acl_out = 0;
    ep_acl_maxpacket = 0;

    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {
            /* class 0xE0 = wireless, subclass 1 = radio frequency,
               protocol 1 = Bluetooth. The second interface of the same
               device carries voice, and it has the SAME class - the
               transport specification gives both interfaces 0xE0/1/1.
               So only the first one counts. Taking the last instead, as
               this did, threw away the event endpoint just found and
               ended on an interface with nothing but isochronous ones:
               every real adapter came out as "no adapter". */
            if (cfg[i + 5] == 0xE0 && cfg[i + 6] == 0x01 &&
                cfg[i + 7] == 0x01 && cfg[i + 3] == 0 && found < 0) {
                iface = cfg[i + 2];
                found = iface;
                have_events = 0;
                in_mps = out_mps = 0;
                ep_events = ep_acl_in = ep_acl_out = 0;
            } else {
                iface = -1;
            }
        } else if (dtype == 0x05 && iface >= 0 && i + 6 < total) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            int mps = (cfg[i + 4] | (cfg[i + 5] << 8)) & 0x07FF;
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {
                ep_events = addr & 0x0F;   /* interrupt in */
                ep_maxpacket = mps;
                if (ep_maxpacket > (int)sizeof(report))
                    ep_maxpacket = (int)sizeof(report);
                ep_interval = cfg[i + 6] ? cfg[i + 6] : 1;
                have_events = 1;
            } else if ((attr & 0x03) == 0x02) {
                if (addr & 0x80) {
                    ep_acl_in = addr & 0x0F;
                    in_mps = mps;
                } else {
                    ep_acl_out = addr & 0x0F;
                    out_mps = mps;
                }
            }
        }
        i += dlen;
    }
    if (found < 0 || !have_events)
        return -1;
    if (ep_acl_in && ep_acl_out && in_mps > 0 && out_mps > 0)
        ep_acl_maxpacket = in_mps < out_mps ? in_mps : out_mps;
    return found;
}

/* Is the device in this socket an adapter, and if so, wake it. 1 = it
   is ours now. 0 = not an adapter, or one that would not wake - and
   then 'gave_up' says which step stopped it. */
static int bt_port;

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

    if (find_adapter(desc_buf, total) < 0)
        return 0;

    /* From here on it IS an adapter, and a step that fails is worth
       naming: "no adapter" would send a person looking for one. */
    uint8_t cfg_value = desc_buf[5];
    if (usbhc_request(d, 0x00, 0x09, cfg_value, 0) < 0)
        return give_up("an adapter is there, but would not take its "
                       "configuration");
    if (usbhc_int_endpoint(d, ep_events, ep_maxpacket, ep_interval) < 0)
        return give_up("an adapter is there, but its event endpoint "
                       "could not be set up");
    acl_ready = ep_acl_maxpacket > 0 &&
                usbhc_bulk_endpoint(d, ep_acl_in, ep_acl_out,
                                    ep_acl_maxpacket) == 0;

    dev = d;
    ev_have = 0;
    usbhc_claim(port, "bt");

    /* Reset first, always: an adapter the firmware has already talked
       to is in a state we know nothing about. */
    if (!command(OP_RESET, (const uint8_t *)0, 0, 2000)) {
        dev = -1;
        return give_up("the adapter did not answer HCI Reset");
    }

    /* The events this driver reads, as Linux asks for them. Bit 61,
       the LE meta event, is off after a reset: without it an adapter
       scans for LE devices and keeps every one it hears to itself.
       Bit 46 is the extended inquiry result - the one with the name. */
    static const uint8_t events[8] = {
        0xFF, 0xFF, 0xFB, 0xFF, 0x07, 0xF8, 0xBF, 0x3D };
    command(OP_SET_EVENT_MASK, events, 8, 1000);
    /* Answers to an inquiry with their names in, or at least with the
       signal; an adapter older than either keeps the plain ones */
    static const uint8_t eir_mode = 2, rssi_mode = 1;
    if (!command(OP_WRITE_INQ_MODE, &eir_mode, 1, 1000))
        command(OP_WRITE_INQ_MODE, &rssi_mode, 1, 1000);
    /* A dual-mode adapter is told the host speaks LE too */
    static const uint8_t le_host[2] = { 0x01, 0x00 };
    command(OP_WRITE_LE_HOST, le_host, 2, 1000);
    /* Secure Simple Pairing, the only kind a phone still does */
    static const uint8_t ssp_on = 1;
    command(0x0C56, &ssp_on, 1, 1000);
    command(OP_READ_BDADDR, (const uint8_t *)0, 0, 1000);
    command(OP_READ_LOCAL_NAME, (const uint8_t *)0, 0, 1000);
    command(OP_READ_VERSION, (const uint8_t *)0, 0, 1000);

    bt_port = port;
    present = 1;
    printk("[ OK ] Bluetooth: %s%s%s (port %d, %s)\n",
           have_addr ? bt_address() : "an adapter with no address",
           have_name && local_name[0] ? ", " : "",
           have_name ? local_name : "", port + 1, usbhc_name());
    return 1;
}

/* An adapter can appear after the walk at start-up: a laptop powers
   its radio on when the firmware's wireless switch says so, and that
   can come seconds late. The sockets are watched anyway, for sticks;
   an adapter is one more thing to recognise in them. */
static int on_plug(int port)
{
    if (present)
        return 0;
    return try_port(port);
}

static void on_unplug(int port)
{
    if (!present || port != bt_port)
        return;
    present = 0;
    acl_ready = 0;
    dev = -1;
    have_addr = have_name = have_version = 0;
    nseen = 0;
    give_up("the adapter went away (switched off, or unplugged)");
}

int bt_init(void)
{
    static int watching;

    if (present)
        return 1;
    if (!usbhc_init())
        return give_up("there is no USB controller");
    if (!watching) {
        usbhc_on_plug(on_plug);
        usbhc_on_unplug(on_unplug);
        usbhc_watch();
        watching = 1;
    }

    gave_up = (const char *)0;
    for (int port = 0; port < usbhc_ports(); port++)
        if (try_port(port))
            return 1;
    /* An adapter that stopped half way has already said where */
    if (gave_up)
        return 0;
    return give_up("there is no Bluetooth adapter on the USB bus");
}

int bt_present(void)
{
    return present;
}

const char *bt_why(void)
{
    return present ? (const char *)0
                   : (gave_up ? gave_up : "Bluetooth was never looked for");
}

const char *bt_address(void)
{
    if (!have_addr)
        return "none";
    addr_text(local_addr, addr_str);
    return addr_str;
}

const char *bt_name(void)
{
    return have_name && local_name[0] ? local_name : "no name";
}

const char *bt_version(void)
{
    return have_version ? version : "unknown";
}

/* One search at a time: two would send their commands down the same
   control pipe and read each other's answers. The bar's card and the
   Settings window can both ask. */
static volatile int scanning, scan_pending;

static int scan(uint32_t seconds);

int bt_scan(uint32_t seconds)
{
    if (!present || scanning)
        return -1;
    scanning = 1;
    int n = scan(seconds);
    scanning = 0;
    return n;
}

static void scan_thread(void *arg)
{
    (void)arg;
    bt_scan(4);
    scan_pending = 0;
}

int bt_scan_start(void)
{
    if (!present || scanning || scan_pending)
        return -1;
    scan_pending = 1;
    if (!task_create("btscan", scan_thread, (void *)0)) {
        scan_pending = 0;
        return -1;
    }
    return 0;
}

int bt_scanning(void)
{
    return scanning || scan_pending;
}

static void put_text(char *dst, int room, const char *src)
{
    int i = 0;
    while (src && src[i] && i < room - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void bt_info_fill(struct bt_info *o)
{
    memset(o, 0, sizeof(*o));
    o->present = present;
    o->scanning = bt_scanning();
    if (!present) {
        put_text(o->why, sizeof(o->why), bt_why());
        return;
    }
    put_text(o->address, sizeof(o->address), bt_address());
    put_text(o->name, sizeof(o->name), have_name ? local_name : "");
    put_text(o->version, sizeof(o->version), bt_version());
    int n = nseen < BT_INFO_DEVICES ? nseen : BT_INFO_DEVICES;
    o->found = n;
    for (int i = 0; i < n; i++) {
        put_text(o->dev_addr[i], sizeof(o->dev_addr[i]), bt_found_addr(i));
        put_text(o->dev_name[i], sizeof(o->dev_name[i]), seen[i].name);
        o->dev_rssi[i] = seen[i].rssi;
        o->dev_paired[i] = bt_paired(o->dev_addr[i]);
    }
    o->pair_state = bt_pair_state();
    o->pair_number = bt_pair_number();
    put_text(o->pair_name, sizeof(o->pair_name), bt_pair_name());
    put_text(o->pair_why, sizeof(o->pair_why), bt_pair_why());
}

/* A classic device found without its name is asked for it: a short
   connection of its own, a second or two, so a few at most */
static void ask_names(void)
{
    int asked = 0;
    for (int i = 0; i < nseen && asked < 4; i++) {
        struct neighbour *d = &seen[i];
        if (!d->classic || d->name[0])
            continue;
        uint8_t q[10];
        memcpy(q, d->addr, 6);
        q[6] = d->scan_mode;
        q[7] = 0;
        q[8] = (uint8_t)d->clock;
        q[9] = (uint8_t)((d->clock >> 8) | 0x80);   /* the offset is valid */
        memcpy(name_addr, d->addr, 6);
        name_asked = 1;
        asked++;
        if (!command(OP_REMOTE_NAME, q, 10, 1000)) {
            name_asked = 0;
            continue;
        }
        for (int t = 0; t < 600 && name_asked; t++)
            if (poll_event() <= 0)
                task_sleep(10);
        name_asked = 0;
    }
}

static int scan(uint32_t seconds)
{
    nseen = 0;

    /* LE: active scanning - the scanner asks every advertiser for its
       scan response, which is where most keep their name. An interval
       of 0x0010 and a window the same size mean "listen all the time",
       which for a few seconds of scanning is exactly what is wanted. */
    static const uint8_t params[7] = {
        0x01,                            /* active */
        0x10, 0x00,                      /* interval */
        0x10, 0x00,                      /* window */
        0x00,                            /* our address: public */
        0x00 };                          /* accept everything */
    command(OP_LE_SCAN_PARAMS, params, 7, 1000);
    static const uint8_t on[2] = { 0x01, 0x01 };   /* enable, drop repeats */
    int le = command(OP_LE_SCAN_ENABLE, on, 2, 1000);

    /* Classic: an inquiry of the general access code, which is what a
       device whose Bluetooth settings are open listens for. Its length
       is counted in 1.28 s. */
    uint32_t units = seconds * 100 / 128;
    uint8_t inq[5] = { 0x33, 0x8B, 0x9E,
                       (uint8_t)(units < 1 ? 1 : units > 0x30 ? 0x30 : units),
                       0 };
    inquiry_done = 0;
    int classic = command(OP_INQUIRY, inq, 5, 1000);
    if (!le && !classic)
        return -1;

    pump(seconds * 1000);

    if (le) {
        static const uint8_t off[2] = { 0x00, 0x00 };
        command(OP_LE_SCAN_ENABLE, off, 2, 1000);
    }
    if (classic && !inquiry_done)
        command(OP_INQUIRY_CANCEL, (const uint8_t *)0, 0, 1000);
    if (classic)
        ask_names();
    return nseen;
}

int bt_found(void)
{
    return nseen;
}

const char *bt_found_addr(int i)
{
    if (i < 0 || i >= nseen)
        return "none";
    addr_text(seen[i].addr, found_str);
    return found_str;
}

const char *bt_found_name(int i)
{
    if (i < 0 || i >= nseen || !seen[i].name[0])
        return "";
    return seen[i].name;
}

int bt_found_rssi(int i)
{
    return (i < 0 || i >= nseen) ? 0 : seen[i].rssi;
}

/* --- what can be checked without an adapter -------------------------
 *
 * Recorded packets, of the shape the specification gives, pushed
 * through the same parser the transport uses. This proves the reading
 * of packets and nothing else - and saying which is which is the whole
 * point of having it.
 */
int bt_smp_selftest(void);

void bt_selftest(void)
{
    /* Command Complete for Read BD_ADDR. On the wire the address goes
       lowest byte first, so this is 00:1a:7d:da:71:13 read aloud. */
    static const uint8_t ev_addr[] = {
        0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00,
        0x13, 0x71, 0xDA, 0x7D, 0x1A, 0x00 };

    /* Command Complete for Read Local Name - the name, then zeros to
       the end of its two hundred and forty-eight bytes. */
    static const uint8_t ev_name[] = {
        0x0E, 0x0B, 0x01, 0x14, 0x0C, 0x00,
        'T', 'h', 'r', 'e', 'a', 'd', 0x00 };

    /* Read Local Version: HCI 5.0 (version 9), maker 0x000F */
    static const uint8_t ev_ver[] = {
        0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00,
        0x09, 0x00, 0x00, 0x09, 0x0F, 0x00, 0x00, 0x00 };

    /* An LE advertising report: one device, a random address, a
       Complete Local Name in the data, and a signal of -59 dBm. */
    static const uint8_t ev_adv[] = {
        0x3E, 0x18, 0x02, 0x01, 0x00, 0x01,
        0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB,
        0x0C,                                  /* twelve bytes of data */
        0x0B, 0x09, 'H', 'e', 'a', 'd', 'p', 'h', 'o', 'n', 'e', 's',
        0xC5 };                                /* -59 dBm */

    /* An inquiry result: one classic device, no name, no signal */
    static const uint8_t ev_inq[] = {
        0x02, 0x0F, 0x01,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x01, 0x00, 0x00, 0x0C, 0x02, 0x5A, 0x00, 0x00 };
    static const uint8_t l2cap[] = { 0x01, 0x00, 0x04, 0x00 };
    uint8_t acl[16];
    uint16_t handle = 0, payload_len = 0;
    uint8_t pb = 0, bc = 0;
    const uint8_t *payload = (const uint8_t *)0;
    int acl_len = acl_build(acl, sizeof(acl), 0x123, 2, 1, l2cap,
                            sizeof(l2cap));
    int acl_ok = acl_len == 8 && acl[0] == 0x23 && acl[1] == 0x61 &&
                 acl[2] == 4 && acl[3] == 0 &&
                 acl_parse(acl, acl_len, &handle, &pb, &bc,
                           &payload, &payload_len) == 0 &&
                 handle == 0x123 && pb == 2 && bc == 1 && payload_len == 4 &&
                 memcmp(payload, l2cap, sizeof(l2cap)) == 0;

    have_addr = have_name = have_version = 0;
    nseen = 0;

    bt_event(ev_addr, (int)sizeof(ev_addr));
    bt_event(ev_name, (int)sizeof(ev_name));
    bt_event(ev_ver, (int)sizeof(ev_ver));
    bt_event(ev_adv, (int)sizeof(ev_adv));
    bt_event(ev_inq, (int)sizeof(ev_inq));

    printk("bt: checking how packets are read (with no adapter - "
           "QEMU has none)\n");
    printk("  adapter address: %s\n", bt_address());
    printk("  adapter name: %s\n", bt_name());
    printk("  version: %s\n", bt_version());
    printk("  devices heard: %u\n", (uint32_t)nseen);
    printk("  ACL framing: %s\n", acl_ok ? "OK" : "failed");
    for (int i = 0; i < nseen; i++)
        printk("    %s  %s%s%d dBm\n", bt_found_addr(i), bt_found_name(i),
               bt_found_name(i)[0] ? "  " : "", bt_found_rssi(i));

    /* Read Local Name's answer the way an adapter with 16-byte packets
       sends it: 254 bytes in sixteen pieces, the name crossing the
       first. Taken a packet at a time, it reads as its first ten
       letters and fifteen events of nonsense. */
    static const char long_name[] = "ThreadOS laptop adapter";
    static uint8_t whole[2 + 252];
    memset(whole, 0, sizeof(whole));
    whole[0] = 0x0E;
    whole[1] = 252;
    whole[2] = 0x01;
    whole[3] = 0x14;
    whole[4] = 0x0C;
    memcpy(whole + 6, long_name, sizeof(long_name) - 1);
    have_name = 0;
    ev_have = 0;
    int events = 0;
    for (int at = 0; at < (int)sizeof(whole); at += 16) {
        int n = (int)sizeof(whole) - at < 16 ? (int)sizeof(whole) - at : 16;
        events += ev_piece(whole + at, n, 16);
    }
    printk("  name from 16-byte pieces: %s (%u event)\n", bt_name(),
           (uint32_t)events);

    /* A transfer that ends short of its event's length was damaged on
       the way: what came is dropped, and the next event is read clean
       instead of being glued onto the remains. */
    have_addr = 0;
    ev_piece(whole, 16, 16);
    ev_piece(whole + 16, 5, 16);
    ev_piece(ev_addr, (int)sizeof(ev_addr), 16);
    printk("  after a cut-off event, the next one read: %s\n", bt_address());
    ev_have = 0;

    /* How the Bluetooth half of an Intel Wireless 3160 describes itself
       (8087:07dc): the HCI interface, then the voice interface with the
       same class and six alternate settings. Every adapter has that
       second interface; one that reads as "no adapter" here is how
       every real adapter used to read. */
#define VOICE_ALT(n, mps) 9, 4, 1, n, 2, 0xE0, 0x01, 0x01, 0, \
                          7, 5, 0x03, 0x01, mps, 0, 1, \
                          7, 5, 0x83, 0x01, mps, 0, 1
    static const uint8_t intel_cfg[] = {
        9, 2, 177, 0, 2, 1, 0, 0xE0, 50,
        9, 4, 0, 0, 3, 0xE0, 0x01, 0x01, 0,
        7, 5, 0x81, 0x03, 64, 0, 1,
        7, 5, 0x02, 0x02, 64, 0, 1,
        7, 5, 0x82, 0x02, 64, 0, 1,
        VOICE_ALT(0, 0), VOICE_ALT(1, 9), VOICE_ALT(2, 17),
        VOICE_ALT(3, 25), VOICE_ALT(4, 33), VOICE_ALT(5, 49) };
#undef VOICE_ALT
    /* The parser fills the live adapter's endpoints; they are put back */
    int keep[6] = { ep_events, ep_maxpacket, ep_interval, ep_acl_in,
                    ep_acl_out, ep_acl_maxpacket };
    int iface = find_adapter(intel_cfg, (int)sizeof(intel_cfg));
    printk("  Intel 3160 configuration: interface %d, events on endpoint "
           "%d (%d bytes), data in %d / out %d\n", iface, ep_events,
           ep_maxpacket, ep_acl_in, ep_acl_out);
    ep_events = keep[0];
    ep_maxpacket = keep[1];
    ep_interval = keep[2];
    ep_acl_in = keep[3];
    ep_acl_out = keep[4];
    ep_acl_maxpacket = keep[5];

    /* And what it does NOT prove, said out loud rather than left to be
       assumed from a row of green ticks. */
    bt_smp_selftest();
    printk("bt: this checks how packets are built and read, not a conversation "
           "with an adapter\n");

    have_addr = have_name = have_version = 0;
    nseen = 0;
}

/* --- pairing ---------------------------------------------------------
 *
 * Secure Simple Pairing, the way every phone, headset and computer
 * since Bluetooth 2.1 pairs (Core 5.4, vol 3 part C 5.2.2; the HCI side
 * in vol 4 part E 7.1 and 7.7). The two controllers do the whole of the
 * cryptography between them - the elliptic curve, the commitments, the
 * link key - and ask their hosts only what a person has to answer:
 *
 *   Create Connection, then Authentication Requested; the controller
 *   asks for a link key (Link Key Request) - a device paired before
 *   gets the one kept, any other a negative reply - then for our input
 *   and output (IO Capability Request: a display and yes/no, MITM
 *   protection, dedicated bonding), says the other side's, and asks
 *   the person to confirm the six digits both screens show (User
 *   Confirmation Request). Confirmed, the controllers finish, and the
 *   new link key comes up (Link Key Notification) to be kept: next
 *   time it answers the Link Key Request and nothing is asked. The link
 *   is then encrypted, and closed again - pairing for its own sake
 *   ("dedicated bonding").
 *
 * Link keys are secrets: they are kept in the root file .btkeys, written
 * with the system's rights as the account database is.
 */

#define OP_CREATE_CONN      0x0405
#define OP_DISCONNECT       0x0406
#define OP_LINK_KEY_REPLY   0x040B
#define OP_LINK_KEY_NEG     0x040C
#define OP_AUTH_REQUESTED   0x0411
#define OP_SET_ENCRYPTION   0x0413
#define OP_IO_CAP_REPLY     0x042B
#define OP_USER_CONFIRM     0x042C
#define OP_USER_CONFIRM_NEG 0x042D
#define OP_WRITE_SSP_MODE   0x0C56

#define EV_CONN_COMPLETE    0x03
#define EV_DISCONN_COMPLETE 0x05
#define EV_AUTH_COMPLETE    0x06
#define EV_ENCRYPT_CHANGE   0x08
#define EV_LINK_KEY_REQ     0x17
#define EV_LINK_KEY_NOTIFY  0x18
#define EV_IO_CAP_REQ       0x31
#define EV_IO_CAP_RESP      0x32
#define EV_USER_CONFIRM_REQ 0x33
#define EV_SSP_COMPLETE     0x36

#define IO_DISPLAY_YES_NO   0x01
#define IO_NO_INPUT_OUTPUT  0x03
#define AUTH_MITM_DEDICATED 0x03

#define KEYS_FILE ".btkeys"
#define KEYS_MAX  8
#define CONFIRM_MS 60000

struct bt_key {
    uint8_t addr[6];
    uint8_t type;
    uint8_t used;
    uint8_t key[16];                    /* the link key, or LE's LTK */
    char name[NAME_MAX];
    uint8_t le, addr_type;              /* an LE device's, and its address's type */
    uint8_t ediv[2], rand[8];           /* what names the LTK to the device */
};

static struct {
    char magic[8];                      /* "BTKEYS2" */
    struct bt_key k[KEYS_MAX];
} keys;
static int keys_loaded;

static struct {
    volatile int state;                 /* BT_PAIR_* */
    uint8_t addr[6];
    char name[NAME_MAX];
    uint16_t handle;
    volatile int connected;             /* 1 up, -1 refused */
    volatile uint32_t number;
    volatile int answer;                /* 1 confirmed, -1 refused */
    int remote_io;
    volatile int auth_done, auth_status, enc_done, gone, got_key;
    int known;
    int typing;                         /* LE: the digits are to be typed
                                           on the device, not compared */
    char why[96];
} pr;

static void keys_load(void)
{
    if (keys_loaded || !fs_ready())
        return;
    keys_loaded = 1;
    /* The file is the system's: read with its rights, whoever asked
       first (a shell's user, a program through Settings) */
    int old_level = task64_level(), old_uid = task64_uid();
    task64_set_uid(0);
    task64_set_level(USER_ADMIN);
    int n = fs_read(KEYS_FILE, &keys, sizeof(keys));
    task64_set_level(old_level);
    task64_set_uid(old_uid);
    if (n != (int)sizeof(keys) || memcmp(keys.magic, "BTKEYS2", 8))
        memset(&keys, 0, sizeof(keys));
}

static int keys_save(void)
{
    if (!fs_ready())
        return -1;
    memcpy(keys.magic, "BTKEYS2", 8);
    int old_level = task64_level(), old_uid = task64_uid();
    task64_set_uid(0);
    task64_set_level(USER_ADMIN);
    int r = fs_write(KEYS_FILE, &keys, sizeof(keys));
    task64_set_level(old_level);
    task64_set_uid(old_uid);
    return r;
}

static struct bt_key *key_find(const uint8_t *addr)
{
    keys_load();
    for (int i = 0; i < KEYS_MAX; i++)
        if (keys.k[i].used && !memcmp(keys.k[i].addr, addr, 6))
            return &keys.k[i];
    return (struct bt_key *)0;
}

static void key_keep(const uint8_t *addr, const uint8_t *key, uint8_t type)
{
    struct bt_key *k = key_find(addr);
    for (int i = 0; i < KEYS_MAX && !k; i++)
        if (!keys.k[i].used)
            k = &keys.k[i];
    if (!k)
        k = &keys.k[0];                 /* full: the oldest goes */
    memset(k, 0, sizeof(*k));
    k->used = 1;
    memcpy(k->addr, addr, 6);
    memcpy(k->key, key, 16);
    k->type = type;
    put_text(k->name, sizeof(k->name), pr.name);
    if (keys_save() < 0)
        printk("[bt] the link key could not be kept on the disk\n");
}

static void pair_fail(const char *why)
{
    put_text(pr.why, sizeof(pr.why), why);
    pr.state = BT_PAIR_FAILED;
    printk("[bt] not paired with %s: %s\n", pr.name, why);
}

/* The events of a pairing, from bt_event: answers that need nobody are
   sent from here; the person's answer is the pairing thread's */
static void pair_event(uint8_t code, const uint8_t *p, int plen)
{
    if (pr.state == BT_PAIR_IDLE)
        return;
    uint8_t r[24];
    switch (code) {
    case EV_CONN_COMPLETE:
        if (plen >= 11 && !memcmp(p + 3, pr.addr, 6)) {
            pr.handle = (uint16_t)((p[1] | (p[2] << 8)) & 0x0FFF);
            pr.connected = p[0] == 0 ? 1 : -1;
        }
        return;
    case EV_LINK_KEY_REQ:
        if (plen < 6 || memcmp(p, pr.addr, 6))
            return;
        {
            const struct bt_key *k = key_find(p);
            memcpy(r, p, 6);
            if (k) {
                memcpy(r + 6, k->key, 16);
                pr.known = 1;
                send_command(OP_LINK_KEY_REPLY, r, 22);
            } else {
                send_command(OP_LINK_KEY_NEG, r, 6);
            }
        }
        return;
    case EV_IO_CAP_REQ:
        if (plen < 6 || memcmp(p, pr.addr, 6))
            return;
        memcpy(r, p, 6);
        r[6] = IO_DISPLAY_YES_NO;
        r[7] = 0;                       /* no out-of-band data */
        r[8] = AUTH_MITM_DEDICATED;
        send_command(OP_IO_CAP_REPLY, r, 9);
        return;
    case EV_IO_CAP_RESP:
        if (plen >= 9 && !memcmp(p, pr.addr, 6))
            pr.remote_io = p[6];
        return;
    case EV_USER_CONFIRM_REQ:
        if (plen < 10 || memcmp(p, pr.addr, 6))
            return;
        pr.number = (uint32_t)(p[6] | (p[7] << 8) | (p[8] << 16) |
                               ((uint32_t)p[9] << 24)) % 1000000;
        /* A device with no screen and no buttons cannot show a number:
           "just works", nothing to compare */
        if (pr.remote_io == IO_NO_INPUT_OUTPUT) {
            pr.answer = 1;
        } else {
            char six[7];                /* printk has no zero padding */
            for (int i = 5, v = (int)pr.number; i >= 0; i--, v /= 10)
                six[i] = (char)('0' + v % 10);
            six[6] = 0;
            printk("[bt] pairing with %s: does it show %s? "
                   "('bt yes' or 'bt no')\n", pr.name, six);
        }
        pr.state = BT_PAIR_CONFIRM;
        return;
    case EV_SSP_COMPLETE:
        if (plen >= 7 && !memcmp(p + 1, pr.addr, 6) && p[0] != 0)
            pr.auth_status = p[0];
        return;
    case EV_LINK_KEY_NOTIFY:
        if (plen >= 23 && !memcmp(p, pr.addr, 6)) {
            key_keep(p, p + 6, p[22]);
            pr.got_key = 1;
        }
        return;
    case EV_AUTH_COMPLETE:
        if (plen >= 3 && ((p[1] | (p[2] << 8)) & 0x0FFF) == pr.handle) {
            if (p[0] != 0)
                pr.auth_status = p[0];
            pr.auth_done = 1;
        }
        return;
    case EV_ENCRYPT_CHANGE:
        if (plen >= 4 && ((p[1] | (p[2] << 8)) & 0x0FFF) == pr.handle)
            pr.enc_done = p[0] == 0 && p[3] ? 1 : -1;
        return;
    case EV_DISCONN_COMPLETE:
        if (plen >= 4 && ((p[1] | (p[2] << 8)) & 0x0FFF) == pr.handle)
            pr.gone = 1;
        return;
    }
}

/* Reads events until a flag is set, for up to ms */
static int pair_wait(volatile int *flag, uint32_t ms)
{
    for (uint32_t t = 0; t < ms && !*flag; t += 10)
        if (poll_event() <= 0)
            task_sleep(10);
    return *flag;
}

static void pair_thread(void *arg)
{
    (void)arg;
    const struct neighbour *n = (const struct neighbour *)0;
    for (int i = 0; i < nseen; i++)
        if (!memcmp(seen[i].addr, pr.addr, 6))
            n = &seen[i];
    uint8_t c[13];
    memcpy(c, pr.addr, 6);
    c[6] = 0x18;                        /* DM1 DH1 DM3 DH3 DM5 DH5 */
    c[7] = 0xCC;
    c[8] = n ? n->scan_mode : 0x01;     /* page scan repetition mode */
    c[9] = 0;
    uint16_t clock = n && n->clock ? (uint16_t)(n->clock | 0x8000) : 0;
    c[10] = (uint8_t)clock;
    c[11] = (uint8_t)(clock >> 8);
    c[12] = 1;                          /* the other side may lead */
    printk("[bt] pairing with %s\n", pr.name);
    if (send_command(OP_CREATE_CONN, c, 13) < 0 ||
        !pair_wait(&pr.connected, 10000) || pr.connected < 0) {
        pair_fail(pr.connected < 0 ? "it would not connect"
                                   : "it did not answer");
        scanning = 0;
        return;
    }
    uint8_t h[3] = { (uint8_t)pr.handle, (uint8_t)(pr.handle >> 8), 1 };
    send_command(OP_AUTH_REQUESTED, h, 2);
    for (uint32_t t = 0; t < CONFIRM_MS && !pr.auth_done; t += 10) {
        if (pr.state == BT_PAIR_CONFIRM && pr.answer) {
            send_command(pr.answer > 0 ? OP_USER_CONFIRM : OP_USER_CONFIRM_NEG,
                         pr.addr, 6);
            pr.state = BT_PAIR_BONDING;
        }
        if (poll_event() <= 0)
            task_sleep(10);
    }
    if (!pr.auth_done || pr.auth_status) {
        pair_fail(pr.answer < 0 ? "refused here"
                  : !pr.auth_done ? "no answer in time"
                  : pr.auth_status == 0x05 ? "the numbers did not match, or "
                                             "it was refused there"
                  : pr.known ? "the key kept from before was refused - forget "
                               "it and pair again"
                  : "it did not complete");
    } else {
        send_command(OP_SET_ENCRYPTION, h, 3);
        pair_wait(&pr.enc_done, 3000);
        pr.state = BT_PAIR_PAIRED;
        printk("[bt] paired with %s%s%s\n", pr.name,
               pr.known ? " (the key kept from before)" : "",
               pr.enc_done > 0 ? ", encrypted" : "");
    }
    uint8_t d[3] = { (uint8_t)pr.handle, (uint8_t)(pr.handle >> 8), 0x13 };
    send_command(OP_DISCONNECT, d, 3);
    pair_wait(&pr.gone, 3000);
    scanning = 0;
}

static int le_pair_start(const struct neighbour *n);

int bt_pair_start(const char *addr_or_index)
{
    if (!present || scanning || scan_pending) {
        printk("[bt] cannot pair now: %s\n", !present ? "no adapter"
               : "the adapter is busy (a search)");
        return -1;
    }
    uint8_t a[6];
    const struct neighbour *n = (const struct neighbour *)0;
    if (addr_or_index[0] >= '1' && addr_or_index[0] <= '9' &&
        !addr_or_index[1]) {
        int i = addr_or_index[0] - '1';
        if (i < nseen)
            n = &seen[i];
    } else {
        /* aa:bb:cc:dd:ee:ff, written highest byte first */
        for (int i = 0, k = 0; i < 6; i++) {
            int v = 0;
            for (int j = 0; j < 2; j++, k++) {
                char ch = addr_or_index[k];
                int d = ch >= '0' && ch <= '9' ? ch - '0'
                        : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                        : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (d < 0) {
                    printk("[bt] cannot pair: '%s' is not an address\n",
                           addr_or_index);
                    return -1;
                }
                v = v * 16 + d;
            }
            a[5 - i] = (uint8_t)v;
            if (i < 5 && addr_or_index[k++] != ':') {
                printk("[bt] cannot pair: '%s' is not an address\n",
                       addr_or_index);
                return -1;
            }
        }
        for (int i = 0; i < nseen; i++)
            if (!memcmp(seen[i].addr, a, 6))
                n = &seen[i];
    }
    if (n && !n->classic)
        return le_pair_start(n);
    memset(&pr, 0, sizeof(pr));
    if (n) {
        memcpy(pr.addr, n->addr, 6);
        put_text(pr.name, sizeof(pr.name), n->name[0] ? n->name : "the device");
    } else {
        memcpy(pr.addr, a, 6);
        const struct bt_key *k = key_find(a);
        put_text(pr.name, sizeof(pr.name), k && k->name[0] ? k->name
                                                            : "the device");
    }
    pr.state = BT_PAIR_CONNECTING;
    scanning = 1;                       /* one user of the adapter */
    if (!task_create("btpair", pair_thread, (void *)0)) {
        scanning = 0;
        pr.state = BT_PAIR_IDLE;
        return -1;
    }
    return 0;
}

void bt_pair_answer(int yes)
{
    if (pr.state == BT_PAIR_CONFIRM && !pr.answer)
        pr.answer = yes ? 1 : -1;
}

int bt_pair_state(void)
{
    return pr.state;
}

uint32_t bt_pair_number(void)
{
    return pr.number;
}

const char *bt_pair_name(void)
{
    return pr.name;
}

const char *bt_pair_why(void)
{
    return pr.why;
}

int bt_paired(const char *addr_text_in)
{
    keys_load();
    char t[18];
    for (int i = 0; i < KEYS_MAX; i++) {
        if (!keys.k[i].used)
            continue;
        addr_text(keys.k[i].addr, t);
        if (!strcmp(t, addr_text_in))
            return 1;
    }
    return 0;
}

int bt_forget(const char *addr_text_in)
{
    keys_load();
    char t[18];
    for (int i = 0; i < KEYS_MAX; i++) {
        if (!keys.k[i].used)
            continue;
        addr_text(keys.k[i].addr, t);
        if (!strcmp(t, addr_text_in)) {
            memset(&keys.k[i], 0, sizeof(keys.k[i]));
            return keys_save();
        }
    }
    return -1;
}

/* The devices paired: their names and addresses, for 'bt' and Settings */
int bt_paired_list(char names[][NAME_MAX], char addrs[][18], int max)
{
    keys_load();
    int n = 0;
    for (int i = 0; i < KEYS_MAX && n < max; i++)
        if (keys.k[i].used) {
            put_text(names[n], NAME_MAX, keys.k[i].name);
            addr_text(keys.k[i].addr, addrs[n]);
            n++;
        }
    return n;
}

/* --- LE pairing --------------------------------------------------------
 *
 * Watches, mice, keyboards and tags pair over Bluetooth Low Energy, by
 * the Security Manager Protocol on L2CAP channel 6 (Core 5.4, vol 3
 * part H). The Intel 3160's controller is Bluetooth 4.0, which leaves
 * the host LE legacy pairing - the elliptic curve of Secure Connections
 * needs 4.2 - and that is what every LE device still accepts:
 *
 *   we connect (LE Create Connection) and send a Pairing Request: a
 *   display and yes/no (so a keyboard is shown six digits to type, and
 *   anything with no keyboard pairs without a question - "Just Works"),
 *   bonding with MITM protection, and the device to hand us its long
 *   term key and identity; it answers with its own Pairing Response;
 *   the temporary key TK is 0 (Just Works) or the six digits; each side
 *   sends a confirm value c1(TK, random, ...) and then the random, and
 *   checks the other's; both then hold the short term key s1(TK, ...),
 *   which encrypts the link; over it the device sends its long term key
 *   (LTK, EDIV, Rand) and identity, which are kept - next time the link
 *   is encrypted with the LTK at once, and nothing is asked.
 *
 * c1 and s1 are AES (user/uaes.h); their values are little-endian here,
 * AES's big-endian, hence the turning round. The Core specification's
 * own example values are checked in 'bt test'.
 */

#define OP_LE_CREATE_CONN   0x200D
#define OP_LE_START_ENC     0x2019
#define L2CAP_CID_SMP       0x0006
#define SMP_PAIRING_REQ     0x01
#define SMP_PAIRING_RSP     0x02
#define SMP_CONFIRM         0x03
#define SMP_RANDOM          0x04
#define SMP_FAILED          0x05
#define SMP_ENC_INFO        0x06
#define SMP_MASTER_ID       0x07
#define SMP_ID_INFO         0x08
#define SMP_ID_ADDR         0x09
#define SMP_SECURITY_REQ    0x0B
#define SMP_IO_KEYBOARD     0x02
#define SMP_IO_KEYB_DISPLAY 0x04
#define SMP_AUTH_BOND_MITM  0x05
#define SMP_KD_ENC_ID       0x03

static struct {
    volatile int conn;                  /* 1 up, -1 refused */
    uint16_t handle;
    uint8_t preq[7], pres[7];
    uint8_t tk[16], mrand[16], sconfirm[16], srand[16];
    volatile int got_rsp, got_confirm, got_random, failed;
    volatile int got_ltk, got_mid, got_irk, got_idaddr;
    uint8_t ltk[16], ediv[2], rand[8];
    uint8_t smp[64];                    /* the message waiting, in order */
    volatile int enc;                   /* 1 on, -1 refused */
} le;

static void smp_e(const uint8_t k[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t kb[16], ib[16];
    for (int i = 0; i < 16; i++) {
        kb[i] = k[15 - i];
        ib[i] = in[15 - i];
    }
    struct aes a;
    aes_key(&a, kb, 128);
    aes_encrypt_block(&a, ib, ib);
    for (int i = 0; i < 16; i++)
        out[i] = ib[15 - i];
}

/* c1(k, r, preq, pres, iat, rat, ia, ra) (vol 3 part H 2.2.3) */
static void smp_c1(const uint8_t k[16], const uint8_t r[16],
                   const uint8_t preq[7], const uint8_t pres[7], uint8_t iat,
                   uint8_t rat, const uint8_t ia[6], const uint8_t ra[6],
                   uint8_t out[16])
{
    uint8_t p1[16], p2[16], t[16];
    p1[0] = iat;
    p1[1] = rat;
    memcpy(p1 + 2, preq, 7);
    memcpy(p1 + 9, pres, 7);
    memcpy(p2, ra, 6);
    memcpy(p2 + 6, ia, 6);
    memset(p2 + 12, 0, 4);
    for (int i = 0; i < 16; i++)
        t[i] = (uint8_t)(r[i] ^ p1[i]);
    smp_e(k, t, t);
    for (int i = 0; i < 16; i++)
        t[i] ^= p2[i];
    smp_e(k, t, out);
}

/* s1(k, r1, r2): the low halves, r1's above r2's (2.2.4) */
static void smp_s1(const uint8_t k[16], const uint8_t r1[16],
                   const uint8_t r2[16], uint8_t out[16])
{
    uint8_t r[16];
    memcpy(r, r2, 8);
    memcpy(r + 8, r1, 8);
    smp_e(k, r, out);
}

static void le_conn_complete(const uint8_t *p, int len)
{
    /* status, handle, role, peer address type, peer address, ... */
    if (pr.state == BT_PAIR_IDLE || len < 11 || memcmp(p + 5, pr.addr, 6))
        return;
    le.handle = (uint16_t)((p[1] | (p[2] << 8)) & 0x0FFF);
    le.conn = p[0] == 0 ? 1 : -1;
}

static int smp_send(const uint8_t *msg, int n)
{
    uint8_t pkt[4 + 64];
    pkt[0] = (uint8_t)n;
    pkt[1] = 0;
    pkt[2] = L2CAP_CID_SMP;
    pkt[3] = 0;
    memcpy(pkt + 4, msg, (uint32_t)n);
    return bt_acl_send(le.handle, 0x00, 0, pkt, (uint16_t)(4 + n));
}

/* One SMP message from the device */
static void smp_rx(const uint8_t *m, int n)
{
    if (n < 1)
        return;
    switch (m[0]) {
    case SMP_PAIRING_RSP:
        if (n >= 7) {
            memcpy(le.pres, m, 7);
            le.got_rsp = 1;
        }
        return;
    case SMP_CONFIRM:
        if (n >= 17) {
            memcpy(le.sconfirm, m + 1, 16);
            le.got_confirm = 1;
        }
        return;
    case SMP_RANDOM:
        if (n >= 17) {
            memcpy(le.srand, m + 1, 16);
            le.got_random = 1;
        }
        return;
    case SMP_FAILED:
        le.failed = n >= 2 ? m[1] : 0xFF;
        return;
    case SMP_ENC_INFO:
        if (n >= 17) {
            memcpy(le.ltk, m + 1, 16);
            le.got_ltk = 1;
        }
        return;
    case SMP_MASTER_ID:
        if (n >= 11) {
            memcpy(le.ediv, m + 1, 2);
            memcpy(le.rand, m + 3, 8);
            le.got_mid = 1;
        }
        return;
    case SMP_ID_INFO:
        le.got_irk = 1;
        return;
    case SMP_ID_ADDR:
        le.got_idaddr = 1;
        return;
    }
}

/* ACL data in: L2CAP frames, put together from their pieces */
static uint8_t l2_buf[512];
static int l2_have, l2_want;

static void acl_in(const uint8_t *pkt, int n)
{
    uint16_t handle;
    uint8_t pb, bc;
    const uint8_t *d;
    uint16_t dl;
    if (acl_parse(pkt, n, &handle, &pb, &bc, &d, &dl) < 0)
        return;
    if (pb != 0x01) {                   /* a start: the L2CAP header */
        if (dl < 4)
            return;
        l2_want = 4 + (d[0] | (d[1] << 8));
        l2_have = 0;
        if (l2_want > (int)sizeof(l2_buf)) {
            l2_want = 0;
            return;
        }
    }
    if (!l2_want || l2_have + dl > l2_want)
        return;
    memcpy(l2_buf + l2_have, d, dl);
    l2_have += dl;
    if (l2_have < l2_want)
        return;
    uint16_t cid = (uint16_t)(l2_buf[2] | (l2_buf[3] << 8));
    if (handle == le.handle && cid == L2CAP_CID_SMP)
        smp_rx(l2_buf + 4, l2_want - 4);
    l2_want = 0;
}

/* Events and ACL data, one look at each */
static int le_pump(void)
{
    int got = poll_event() > 0;
    if (acl_ready) {
        int n = usbhc_bulk_in_poll(dev, ep_acl_in, acl_rx, sizeof(acl_rx));
        if (n > 0) {
            acl_in(acl_rx, n);
            got = 1;
        }
    }
    return got;
}

static int le_wait(volatile int *flag, uint32_t ms)
{
    for (uint32_t t = 0; t < ms && !*flag && !le.failed; t += 10)
        if (!le_pump())
            task_sleep(10);
    return *flag;
}

static int le_encrypt(const uint8_t ltk[16], const uint8_t ediv[2],
                      const uint8_t rand[8])
{
    uint8_t c[28];
    c[0] = (uint8_t)le.handle;
    c[1] = (uint8_t)(le.handle >> 8);
    memcpy(c + 2, rand, 8);
    memcpy(c + 10, ediv, 2);
    memcpy(c + 12, ltk, 16);
    pr.enc_done = 0;
    pr.handle = le.handle;              /* Encryption Change comes to pr */
    if (send_command(OP_LE_START_ENC, c, 28) < 0)
        return 0;
    pair_wait(&pr.enc_done, 3000);
    return pr.enc_done > 0;
}

static void le_pair_thread(void *arg)
{
    const struct neighbour *n = (const struct neighbour *)arg;
    uint8_t c[25];
    memset(c, 0, sizeof(c));
    c[0] = 0x60;                        /* scan interval, 60 ms */
    c[2] = 0x30;                        /* window, 30 ms */
    c[5] = n->addr_type;
    memcpy(c + 6, n->addr, 6);
    c[13] = 0x18;                       /* connection interval 30-50 ms */
    c[15] = 0x28;
    c[19] = 0xF4;                       /* supervision timeout 5 s */
    c[20] = 0x01;
    printk("[bt] pairing with %s (LE)\n", pr.name);
    if (send_command(OP_LE_CREATE_CONN, c, 25) < 0 ||
        !le_wait(&le.conn, 10000) || le.conn < 0) {
        pair_fail(le.conn < 0 ? "it would not connect" : "it did not answer");
        scanning = 0;
        return;
    }
    const struct bt_key *k = key_find(pr.addr);
    if (k && k->le) {
        /* Bonded before: the long term key, and nothing asked */
        pr.known = 1;
        if (le_encrypt(k->key, k->ediv, k->rand)) {
            pr.state = BT_PAIR_PAIRED;
            printk("[bt] paired with %s (the key kept from before), "
                   "encrypted\n", pr.name);
        } else {
            pair_fail("the key kept from before was refused - forget it "
                      "and pair again");
        }
    } else {
        static const uint8_t req[7] = { SMP_PAIRING_REQ, 0x01, 0x00,
                                        SMP_AUTH_BOND_MITM, 16, 0x00,
                                        SMP_KD_ENC_ID };
        memcpy(le.preq, req, 7);
        smp_send(req, 7);
        if (!le_wait(&le.got_rsp, 10000)) {
            pair_fail(le.failed ? "it refused to pair" : "it did not answer");
            goto out;
        }
        /* A keyboard is given six digits to type; anything else, Just
           Works (legacy pairing's table, vol 3 part H 2.3.5.1) */
        memset(le.tk, 0, 16);
        if (le.pres[1] == SMP_IO_KEYBOARD || le.pres[1] == SMP_IO_KEYB_DISPLAY) {
            uint32_t v;
            random_bytes(&v, sizeof(v));
            v %= 1000000;
            le.tk[0] = (uint8_t)v;
            le.tk[1] = (uint8_t)(v >> 8);
            le.tk[2] = (uint8_t)(v >> 16);
            pr.number = v;
            pr.typing = 1;
            pr.state = BT_PAIR_CONFIRM;
            char six[7];
            for (int i = 5, x = (int)v; i >= 0; i--, x /= 10)
                six[i] = (char)('0' + x % 10);
            six[6] = 0;
            printk("[bt] pairing with %s: type %s on it, then Enter\n",
                   pr.name, six);
        }
        random_bytes(le.mrand, 16);
        uint8_t mconfirm[17];
        mconfirm[0] = SMP_CONFIRM;
        smp_c1(le.tk, le.mrand, le.preq, le.pres, 0, n->addr_type,
               local_addr, n->addr, mconfirm + 1);
        smp_send(mconfirm, 17);
        if (!le_wait(&le.got_confirm, pr.typing ? CONFIRM_MS : 10000)) {
            pair_fail(le.failed ? "the digits were not typed right, or it "
                                  "refused" : "no answer in time");
            goto out;
        }
        uint8_t rnd[17];
        rnd[0] = SMP_RANDOM;
        memcpy(rnd + 1, le.mrand, 16);
        smp_send(rnd, 17);
        if (!le_wait(&le.got_random, 10000)) {
            pair_fail(le.failed ? "it refused our confirm value (the "
                                  "digits typed wrong?)" : "no answer in time");
            goto out;
        }
        uint8_t check[16];
        smp_c1(le.tk, le.srand, le.preq, le.pres, 0, n->addr_type,
               local_addr, n->addr, check);
        if (memcmp(check, le.sconfirm, 16)) {
            static const uint8_t fail[2] = { SMP_FAILED, 0x04 };
            smp_send(fail, 2);
            pair_fail("its confirm value does not hold up");
            goto out;
        }
        pr.state = BT_PAIR_BONDING;
        uint8_t stk[16];
        static const uint8_t zero[8];
        smp_s1(le.tk, le.srand, le.mrand, stk);
        if (!le_encrypt(stk, zero, zero)) {
            pair_fail("the link would not encrypt");
            goto out;
        }
        /* The keys it hands over, now that nobody else can read them */
        le_wait(&le.got_mid, 5000);
        if (le.pres[6] & 0x02)          /* its identity is to come too */
            le_wait(&le.got_idaddr, 2000);
        if (!le.got_ltk || !le.got_mid) {
            pair_fail("it gave no long term key");
            goto out;
        }
        struct bt_key *slot;
        key_keep(pr.addr, le.ltk, 0);
        slot = key_find(pr.addr);
        if (slot) {
            slot->le = 1;
            slot->addr_type = n->addr_type;
            memcpy(slot->ediv, le.ediv, 2);
            memcpy(slot->rand, le.rand, 8);
            keys_save();
        }
        pr.state = BT_PAIR_PAIRED;
        printk("[bt] paired with %s, encrypted%s\n", pr.name,
               le.got_irk ? ", its identity kept" : "");
    }
out:;
    uint8_t d[3] = { (uint8_t)le.handle, (uint8_t)(le.handle >> 8), 0x13 };
    pr.handle = le.handle;
    pr.gone = 0;
    send_command(OP_DISCONNECT, d, 3);
    pair_wait(&pr.gone, 3000);
    scanning = 0;
}

static int le_pair_start(const struct neighbour *n)
{
    if (!acl_ready) {
        printk("[bt] cannot pair with %s: no data path to the adapter (LE "
               "pairing needs its bulk endpoints on an xHCI)\n", n->name);
        return -1;
    }
    memset(&le, 0, sizeof(le));
    memset(&pr, 0, sizeof(pr));
    memcpy(pr.addr, n->addr, 6);
    put_text(pr.name, sizeof(pr.name), n->name[0] ? n->name : "the device");
    pr.state = BT_PAIR_CONNECTING;
    scanning = 1;
    if (!task_create("btlepair", le_pair_thread, (void *)n)) {
        scanning = 0;
        pr.state = BT_PAIR_IDLE;
        return -1;
    }
    return 0;
}

/* The Core specification's own values for c1 and s1 (vol 3 part H
   2.2.3, 2.2.4), for 'bt test' */
int bt_smp_selftest(void)
{
    static const uint8_t k[16];
    uint8_t r[16], preq[7], pres[7], ia[6], ra[6], out[16];
    static const uint8_t r_be[16] = { 0x57, 0x83, 0xD5, 0x21, 0x56, 0xAD,
                                      0x6F, 0x0E, 0x63, 0x88, 0x27, 0x4E,
                                      0xC6, 0x70, 0x2E, 0xE0 };
    static const uint8_t preq_be[7] = { 0x07, 0x07, 0x10, 0x00, 0x00, 0x01,
                                        0x01 };
    static const uint8_t pres_be[7] = { 0x05, 0x00, 0x08, 0x00, 0x00, 0x03,
                                        0x02 };
    static const uint8_t ia_be[6] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 };
    static const uint8_t ra_be[6] = { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 };
    static const uint8_t c1_be[16] = { 0x1E, 0x1E, 0x3F, 0xEF, 0x87, 0x89,
                                       0x88, 0xEA, 0xD2, 0xA7, 0x4D, 0xC5,
                                       0xBE, 0xF1, 0x3B, 0x86 };
    static const uint8_t r1_be[16] = { 0x00, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
                                       0x0A, 0x09, 0x11, 0x22, 0x33, 0x44,
                                       0x55, 0x66, 0x77, 0x88 };
    static const uint8_t r2_be[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                       0x07, 0x08, 0x99, 0xAA, 0xBB, 0xCC,
                                       0xDD, 0xEE, 0xFF, 0x00 };
    static const uint8_t s1_be[16] = { 0x9A, 0x1F, 0xE1, 0xF0, 0xE8, 0xB0,
                                       0xF4, 0x9B, 0x5B, 0x42, 0x16, 0xAE,
                                       0x79, 0x6D, 0xA0, 0x62 };
    /* the specification writes its values most significant octet first */
    for (int i = 0; i < 16; i++)
        r[i] = r_be[15 - i];
    for (int i = 0; i < 7; i++) {
        preq[i] = preq_be[6 - i];
        pres[i] = pres_be[6 - i];
    }
    for (int i = 0; i < 6; i++) {
        ia[i] = ia_be[5 - i];
        ra[i] = ra_be[5 - i];
    }
    smp_c1(k, r, preq, pres, 1, 0, ia, ra, out);
    int ok = 1;
    for (int i = 0; i < 16; i++)
        ok &= out[i] == c1_be[15 - i];
    uint8_t r1[16], r2[16];
    for (int i = 0; i < 16; i++) {
        r1[i] = r1_be[15 - i];
        r2[i] = r2_be[15 - i];
    }
    smp_s1(k, r1, r2, out);
    int ok2 = 1;
    for (int i = 0; i < 16; i++)
        ok2 &= out[i] == s1_be[15 - i];
    printk("SMP c1 (the Core specification's example): %s\n",
           ok ? "OK" : "WRONG");
    printk("SMP s1 (the Core specification's example): %s\n",
           ok2 ? "OK" : "WRONG");
    return ok && ok2;
}
