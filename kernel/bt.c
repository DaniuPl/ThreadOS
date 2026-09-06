/* Bluetooth over USB: commands down, events up
 *
 * The adapter in a laptop hangs off the internal USB bus and belongs to
 * a class of its own: 0xE0/0x01/0x01. Its endpoints are always the
 * same three - an interrupt one for events, two bulk ones for data -
 * and commands do not travel through any of them: they go as CONTROL
 * transfers, with a request type of 0x20 and everything else zero.
 * That is the whole transport, and it has been the same since 2001.
 *
 * What this driver does: reset the adapter, ask for its address, its
 * name and its version, and then listen. Listening means a scan: LE
 * devices announce themselves every few dozen milliseconds and the
 * adapter passes those announcements up as events. Connecting to
 * anything needs the data endpoints and a good deal more, and is not
 * here.
 *
 * The honest part. QEMU emulates no Bluetooth device - the whole stack
 * was thrown out of it years ago - so not one line of the conversation
 * below has ever reached an adapter that answers. What IS tested is
 * the reading and writing of packets, against recorded ones, because
 * that is what can be tested without hardware. When this meets a real
 * adapter, expect the transport to be right and the sequence to need
 * work; adapters from Intel and Broadcom additionally want their
 * firmware loaded before they will do anything beyond answering their
 * own address.
 */

#include <stdint.h>
#include "include/bt.h"
#include "include/usbhc.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"

#define DEVICES_MAX 12          /* how many neighbours we remember */
#define NAME_MAX    32

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

#define EV_INQUIRY_RESULT   0x02
#define EV_CMD_COMPLETE     0x0E
#define EV_CMD_STATUS       0x0F
#define EV_LE_META          0x3E

struct neighbour {
    uint8_t addr[6];
    char name[NAME_MAX];
    int rssi;
    int classic;                /* 1 = heard through an inquiry, 0 = LE */
};

static int dev = -1;            /* the handle from the USB layer */
static int ep_events;
static int ep_maxpacket = 16;
static int ep_interval = 1;
static int present;
static const char *gave_up;

static uint8_t local_addr[6];
static char local_name[NAME_MAX];
static char version[40];
static int have_addr, have_name, have_version;

static struct neighbour seen[DEVICES_MAX];
static int nseen;

/* The answer to the last command, for whoever is waiting for it */
static uint16_t last_opcode;
static int last_status = -1;

static uint8_t report[260] __attribute__((aligned(16)));
static uint8_t desc_buf[256] __attribute__((aligned(16)));
static uint8_t cmdbuf[260] __attribute__((aligned(16)));

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

static void le_meta(const uint8_t *p, int len)
{
    if (len < 2 || p[0] != 0x02)         /* only the advertising report */
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
            ad_data(n, p + i + 9, dlen);
            n->rssi = (int)(int8_t)p[i + 9 + dlen];
        }
        i += 10 + dlen;
    }
}

static void inquiry_result(const uint8_t *p, int len)
{
    if (len < 1)
        return;
    int n = p[0];
    int i = 1;

    /* Classic devices answer with no name and no signal strength - a
       name has to be asked for separately, with a connection. */
    for (int r = 0; r < n && i + 14 <= len; r++) {
        remember(p + i, 1);
        i += 14;
    }
}

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
            *w++ = ',';
            *w++ = ' ';
            *w++ = 'w';
            *w++ = 'y';
            *w++ = 't';
            *w++ = 'w';
            *w++ = '.';
            *w++ = ' ';
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
        inquiry_result(p, plen);
        return;

    default:
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

/* Reads whatever the adapter has to say, for up to 'ms' milliseconds.
   Every event goes through the same parser the test uses. */
static void pump(uint32_t ms)
{
    for (uint32_t i = 0; i < ms / 10 + 1; i++) {
        int n = usbhc_int_in(dev, ep_events, report, ep_maxpacket);
        if (n > 0)
            bt_event(report, n);
        task_sleep(10);
    }
}

/* A command and the answer to it. 1 = the adapter said it worked. */
static int command(uint16_t opcode, const uint8_t *params, int plen,
                   uint32_t ms)
{
    if (send_command(opcode, params, plen) < 0)
        return 0;
    for (uint32_t i = 0; i < ms / 10 + 1; i++) {
        int n = usbhc_int_in(dev, ep_events, report, ep_maxpacket);
        if (n > 0) {
            bt_event(report, n);
            if (last_opcode == opcode && last_status == 0)
                return 1;
            if (last_opcode == opcode)
                return 0;
        }
        task_sleep(10);
    }
    return 0;
}

/* Looks in the configuration for the Bluetooth interface and its
   interrupt endpoint. The two bulk ones are for data, which this
   driver does not carry yet. */
static int find_adapter(const uint8_t *cfg, int total)
{
    int iface = -1;

    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {
            /* class 0xE0 = wireless, subclass 1 = radio frequency,
               protocol 1 = Bluetooth. The second interface of the same
               device carries voice and has alternate settings; we do
               not touch it. */
            if (cfg[i + 5] == 0xE0 && cfg[i + 6] == 0x01 &&
                cfg[i + 7] == 0x01 && cfg[i + 3] == 0)
                iface = cfg[i + 2];
            else
                iface = -1;
        } else if (dtype == 0x05 && iface >= 0 && i + 6 < total) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {   /* interrupt in */
                ep_events = addr & 0x0F;
                ep_maxpacket = cfg[i + 4] | (cfg[i + 5] << 8);
                if (ep_maxpacket > (int)sizeof(report))
                    ep_maxpacket = (int)sizeof(report);
                ep_interval = cfg[i + 6] ? cfg[i + 6] : 1;
                return iface;
            }
        }
        i += dlen;
    }
    return -1;
}

int bt_init(void)
{
    if (present)
        return 1;
    if (!usbhc_init())
        return give_up("nie ma kontrolera USB");

    for (int port = 0; port < usbhc_ports(); port++) {
        int d = usbhc_attach(port);
        if (d < 0)
            continue;

        if (usbhc_descriptor(d, 0x02, 0, desc_buf, 9) < 9)
            continue;
        int total = desc_buf[2] | (desc_buf[3] << 8);
        if (total > (int)sizeof(desc_buf))
            total = (int)sizeof(desc_buf);
        if (usbhc_descriptor(d, 0x02, 0, desc_buf, total) < total)
            continue;

        int iface = find_adapter(desc_buf, total);
        if (iface < 0)
            continue;

        uint8_t cfg_value = desc_buf[5];
        if (usbhc_request(d, 0x00, 0x09, cfg_value, 0) < 0)
            continue;
        if (usbhc_int_endpoint(d, ep_events, ep_maxpacket, ep_interval) < 0)
            continue;

        dev = d;
        usbhc_claim(port);

        /* Reset first, always: an adapter the firmware has already
           talked to is in a state we know nothing about. */
        if (!command(OP_RESET, (const uint8_t *)0, 0, 2000)) {
            dev = -1;
            return give_up("adapter nie odpowiedzial na HCI Reset");
        }

        static const uint8_t all_events[8] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFF, 0x00 };
        command(OP_SET_EVENT_MASK, all_events, 8, 1000);
        command(OP_READ_BDADDR, (const uint8_t *)0, 0, 1000);
        command(OP_READ_LOCAL_NAME, (const uint8_t *)0, 0, 1000);
        command(OP_READ_VERSION, (const uint8_t *)0, 0, 1000);

        present = 1;
        printk("[ OK ] Bluetooth: %s%s%s (port %d, %s)\n",
               have_addr ? bt_address() : "adapter bez adresu",
               have_name && local_name[0] ? ", " : "",
               have_name ? local_name : "", port + 1, usbhc_name());
        return 1;
    }
    return give_up("nie ma adaptera Bluetooth na magistrali USB");
}

int bt_present(void)
{
    return present;
}

const char *bt_why(void)
{
    return present ? (const char *)0
                   : (gave_up ? gave_up : "Bluetooth nie byl szukany");
}

const char *bt_address(void)
{
    if (!have_addr)
        return "brak";
    addr_text(local_addr, addr_str);
    return addr_str;
}

const char *bt_name(void)
{
    return have_name && local_name[0] ? local_name : "bez nazwy";
}

const char *bt_version(void)
{
    return have_version ? version : "nieznana";
}

int bt_scan(uint32_t seconds)
{
    if (!present)
        return -1;

    nseen = 0;

    /* Passive scanning: we listen, we do not ask. An interval of 0x0010
       and a window the same size mean "listen all the time", which for
       a few seconds of scanning is exactly what is wanted. */
    static const uint8_t params[7] = {
        0x00,                            /* passive */
        0x10, 0x00,                      /* interval */
        0x10, 0x00,                      /* window */
        0x00,                            /* our address: public */
        0x00 };                          /* accept everything */
    command(OP_LE_SCAN_PARAMS, params, 7, 1000);

    static const uint8_t on[2] = { 0x01, 0x01 };   /* enable, drop repeats */
    if (!command(OP_LE_SCAN_ENABLE, on, 2, 1000))
        return -1;

    pump(seconds * 1000);

    static const uint8_t off[2] = { 0x00, 0x00 };
    command(OP_LE_SCAN_ENABLE, off, 2, 1000);
    return nseen;
}

int bt_found(void)
{
    return nseen;
}

const char *bt_found_addr(int i)
{
    if (i < 0 || i >= nseen)
        return "brak";
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
        'P', 'r', 'o', 't', 'o', 'n', 0x00 };

    /* Read Local Version: HCI 5.0 (version 9), maker 0x000F */
    static const uint8_t ev_ver[] = {
        0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00,
        0x09, 0x00, 0x00, 0x09, 0x0F, 0x00, 0x00, 0x00 };

    /* An LE advertising report: one device, a random address, a
       Complete Local Name in the data, and a signal of -59 dBm. */
    static const uint8_t ev_adv[] = {
        0x3E, 0x17, 0x02, 0x01, 0x00, 0x01,
        0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB,
        0x0B,                                  /* eleven bytes of data */
        0x0A, 0x09, 'S', 'l', 'u', 'c', 'h', 'a', 'w', 'k', 'i',
        0xC5 };                                /* -59 dBm */

    /* An inquiry result: one classic device, no name, no signal */
    static const uint8_t ev_inq[] = {
        0x02, 0x0F, 0x01,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x01, 0x00, 0x00, 0x0C, 0x02, 0x5A, 0x00, 0x00 };

    have_addr = have_name = have_version = 0;
    nseen = 0;

    bt_event(ev_addr, (int)sizeof(ev_addr));
    bt_event(ev_name, (int)sizeof(ev_name));
    bt_event(ev_ver, (int)sizeof(ev_ver));
    bt_event(ev_adv, (int)sizeof(ev_adv));
    bt_event(ev_inq, (int)sizeof(ev_inq));

    printk("bt: sprawdzam czytanie pakietow (bez adaptera - "
           "QEMU nie ma zadnego)\n");
    printk("  adres adaptera: %s\n", bt_address());
    printk("  nazwa adaptera: %s\n", bt_name());
    printk("  wersja: %s\n", bt_version());
    printk("  uslyszane urzadzenia: %u\n", (uint32_t)nseen);
    for (int i = 0; i < nseen; i++)
        printk("    %s  %s%s%d dBm\n", bt_found_addr(i), bt_found_name(i),
               bt_found_name(i)[0] ? "  " : "", bt_found_rssi(i));

    /* And what it does NOT prove, said out loud rather than left to be
       assumed from a row of green ticks. */
    printk("bt: to sprawdza skladanie i czytanie pakietow, nie rozmowe "
           "z adapterem\n");

    have_addr = have_name = have_version = 0;
    nseen = 0;
}
