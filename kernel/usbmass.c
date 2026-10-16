/* USB disk: Bulk-Only Transport with SCSI inside
 *
 * The class is described in two pages and works like this: a command
 * goes out as a thirty-one byte packet (CBW) on the bulk-out endpoint,
 * the data go the way the command said, and a thirteen byte packet
 * (CSW) comes back on the bulk-in one saying whether it worked. Inside
 * the CBW sits an ordinary SCSI command - INQUIRY, READ CAPACITY,
 * READ(10), WRITE(10) - the same ones a SATA disk answers.
 *
 * Two things about it are worth knowing before reading the code.
 *
 * The first: a stick that has just been plugged in usually refuses the
 * first command with "not ready" and expects to be ASKED WHY (REQUEST
 * SENSE) before it will talk. A driver that does not ask sees a disk
 * that never wakes up. So the first READ CAPACITY is retried, with the
 * sense read in between.
 *
 * The second: the sector size is not always 512. Card readers and some
 * external drives report 2048 or 4096. Everything above this layer -
 * TFS, FAT, the partition table - counts in 512-byte sectors, so a
 * disk with larger ones would have to be translated, and translating a
 * write means reading the block first. We do not do that: such a disk
 * is listed with a capacity of zero, that is, not listed at all, and
 * the reason is printed. Better than silently writing to the wrong
 * place.
 */

#include <stdint.h>
#include "include/usbmass.h"
#include "include/usbhc.h"
#include "include/blk.h"
#include "include/printk.h"
#include "include/string.h"

#define UNITS 2                  /* this many USB disks at once */
#define SECTOR 512

struct unit {
    int used;
    int gone;                    /* pulled out of the socket */
    int port;                    /* which socket it was in */
    int dev;                     /* the handle from the controller layer */
    int ep_in, ep_out;
    uint32_t tag;                /* the number that ties a CSW to its CBW */
    uint32_t sectors;
    char name[40];
};

static struct unit units[UNITS];
static int count;

/* The controller reaches for these itself, so they are static and
   aligned rather than living on somebody's stack */
static uint8_t cbw[31] __attribute__((aligned(64)));
static uint8_t csw[13] __attribute__((aligned(64)));
static uint8_t small[64] __attribute__((aligned(64)));
static uint8_t desc_buf[256] __attribute__((aligned(64)));

/* --- the wrapper ---------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* One command: the wrapper out, the data either way, the verdict back.
   Returns 0 when the device says it worked. */
static int command(struct unit *u, const uint8_t *scsi, int scsilen,
                   void *data, uint32_t len, int in)
{
    memset(cbw, 0, sizeof(cbw));
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    put32(cbw + 4, ++u->tag);
    put32(cbw + 8, len);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;                          /* logical unit zero */
    cbw[14] = (uint8_t)scsilen;
    for (int i = 0; i < scsilen && i < 16; i++)
        cbw[15 + i] = scsi[i];

    if (usbhc_bulk_out(u->dev, u->ep_out, cbw, (int)sizeof(cbw)) !=
        (int)sizeof(cbw))
        return -1;

    if (len) {
        int moved = in
            ? usbhc_bulk_in(u->dev, u->ep_in, data, (int)len)
            : usbhc_bulk_out(u->dev, u->ep_out, data, (int)len);
        if (moved < 0)
            return -1;
    }

    if (usbhc_bulk_in(u->dev, u->ep_in, csw, (int)sizeof(csw)) !=
        (int)sizeof(csw))
        return -1;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S')
        return -1;
    /* The tag has to match: an answer to the previous command would
       otherwise be taken as the answer to this one, and from then on
       every reply would be one command late. */
    if ((uint32_t)(csw[4] | (csw[5] << 8) | (csw[6] << 16) |
                   ((uint32_t)csw[7] << 24)) != u->tag)
        return -1;
    return csw[12] == 0 ? 0 : -1;         /* 0 = the command succeeded */
}

/* "Why did you refuse?" - asked for its side effect: a device that has
   just woken up answers the question and only then starts working. */
static void ask_why(struct unit *u)
{
    uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    command(u, sense, 6, small, 18, 1);
}

/* --- recognising a device ------------------------------------------- */

/* Looks in the configuration for an interface of the mass storage
   class in its SCSI, bulk-only version, and for its two bulk
   endpoints. Returns the interface number or -1. */
static int find_disk(const uint8_t *cfg, int total, int *in_ep, int *out_ep,
                     int *maxpacket)
{
    int iface = -1;
    *in_ep = *out_ep = 0;

    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {          /* an interface */
            /* class 8 = mass storage, subclass 6 = SCSI, protocol
               0x50 = bulk-only. The other combinations exist (floppies
               speak a different one) and we do not pretend to know
               them. */
            if (cfg[i + 5] == 0x08 && cfg[i + 6] == 0x06 &&
                cfg[i + 7] == 0x50) {
                iface = cfg[i + 2];
                *in_ep = *out_ep = 0;
            } else {
                iface = -1;
            }
        } else if (dtype == 0x05 && iface >= 0 && i + 6 < total) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            int packet = cfg[i + 4] | (cfg[i + 5] << 8);

            if ((attr & 0x03) == 0x02) {               /* bulk */
                if (addr & 0x80)
                    *in_ep = addr & 0x0F;
                else
                    *out_ep = addr & 0x0F;
                if (packet > 0)
                    *maxpacket = packet;
            }
            if (*in_ep && *out_ep)
                return iface;
        }
        i += dlen;
    }
    return -1;
}

static void read_name(struct unit *u)
{
    uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
    int k = 0;

    memset(u->name, 0, sizeof(u->name));
    if (command(u, inquiry, 6, small, 36, 1) < 0) {
        memcpy(u->name, "USB disk", 9);
        return;
    }
    /* Bytes 8..15 are the maker, 16..31 the model - padded with
       spaces, which we squeeze out so that two words do not become
       one long stretch of nothing. */
    for (int i = 8; i < 32 && k < (int)sizeof(u->name) - 1; i++) {
        char c = (char)small[i];
        if (c == ' ' && (k == 0 || u->name[k - 1] == ' '))
            continue;
        if (c < 32 || c > 126)
            continue;
        u->name[k++] = c;
    }
    while (k > 0 && u->name[k - 1] == ' ')
        k--;
    u->name[k] = 0;
    if (!k)
        memcpy(u->name, "USB disk", 9);
}

/* READ CAPACITY (10): the last sector's number and how large a sector
   is. Retried, because a stick that has just been plugged in refuses
   the first question on principle. */
static int read_capacity(struct unit *u)
{
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    for (int attempt = 0; attempt < 5; attempt++) {
        if (command(u, cap, 10, small, 8, 1) == 0) {
            uint32_t last = get32be(small);
            uint32_t size = get32be(small + 4);

            if (size != SECTOR) {
                printk("[usb] the disk has %u B sectors - not supported\n",
                       size);
                return -1;
            }
            if (last == 0xFFFFFFFFu)     /* over two terabytes: READ(16) */
                return -1;
            u->sectors = last + 1;
            return 0;
        }
        ask_why(u);
    }
    return -1;
}

/* One port, one question: is there a disk in it? Returns 1 when it took
   the device. The same function does the work at start-up and when
   somebody pushes a stick in three hours later - a disk plugged in
   later is not a different kind of disk. */
static int try_port(int port)
{
    int dev = usbhc_attach(port);

    if (dev < 0 || count >= UNITS)
        return 0;

    if (usbhc_descriptor(dev, 0x02, 0, desc_buf, 9) < 9)
        return 0;
    int total = desc_buf[2] | (desc_buf[3] << 8);
    if (total > (int)sizeof(desc_buf))
        total = (int)sizeof(desc_buf);
    if (usbhc_descriptor(dev, 0x02, 0, desc_buf, total) < total)
        return 0;

    int in_ep = 0, out_ep = 0, maxpacket = 64;
    int iface = find_disk(desc_buf, total, &in_ep, &out_ep, &maxpacket);
    if (iface < 0)
        return 0;                        /* something other than a disk */

    uint8_t cfg_value = desc_buf[5];
    if (usbhc_request(dev, 0x00, 0x09, cfg_value, 0) < 0)
        return 0;                        /* SET_CONFIGURATION */
    if (usbhc_bulk_endpoint(dev, in_ep, out_ep, maxpacket) < 0)
        return 0;

    struct unit *u = &units[count];
    u->used = 1;
    u->gone = 0;
    u->port = port;
    u->dev = dev;
    u->ep_in = in_ep;
    u->ep_out = out_ep;
    u->tag = 0;
    u->sectors = 0;

    read_name(u);
    if (read_capacity(u) < 0) {
        u->used = 0;
        return 0;
    }

    usbhc_claim(port);
    printk("[ OK ] USB disk: %s, %u sectors (port %d, %s)\n",
           u->name, u->sectors, port + 1, usbhc_name());
    count++;
    return 1;
}

/* Pushed in while the machine is running. Everything above this layer
   learns about it the same way it learns about a disk found at
   start-up: through the block layer's list. */
static int on_plug(int port)
{
    int before = count;

    if (!try_port(port))
        return 0;
    blk_usb_added(before);
    printk("[ OK ] A USB disk in socket %d - it is disk %u\n",
           port + 1, (uint32_t)(blk_count()));
    return 1;
}

/* And pulled out. We do not pretend it is still there: the block layer
   marks it gone, and every read of it fails from now on. A disk that
   answers from memory after it has left the building is how a
   filesystem gets written to nothing. */
static void on_unplug(int port)
{
    for (int i = 0; i < count; i++) {
        if (!units[i].used || units[i].gone || units[i].port != port)
            continue;
        units[i].gone = 1;
        blk_usb_gone(i);
        printk("[ !! ] The USB disk was pulled out of socket %d\n", port + 1);
    }
}

int usbmass_init(void)
{
    if (!usbhc_init())
        return 0;

    for (int port = 0; port < usbhc_ports() && count < UNITS; port++)
        try_port(port);

    /* From now on the sockets are watched. This costs one thread and a
       read of a register twice a second, and it is the difference
       between a machine one uses and a machine one reboots. */
    usbhc_on_plug(on_plug);
    usbhc_on_unplug(on_unplug);
    usbhc_watch();

    return count > 0;
}

int usbmass_count(void)
{
    return count;
}

uint32_t usbmass_sectors(int unit)
{
    if (unit < 0 || unit >= count)
        return 0;
    return units[unit].sectors;
}

const char *usbmass_name(int unit)
{
    if (unit < 0 || unit >= count)
        return "none";
    return units[unit].name;
}

int usbmass_present(int unit)
{
    return unit >= 0 && unit < count && units[unit].used && !units[unit].gone;
}

/* Reading and writing.
 *
 * One command per call, however many sectors - a stick moves data far
 * faster than it answers commands, so asking for eight sectors at once
 * is not an optimisation but the difference between a copy that ends
 * and one that does not.
 */
int usbmass_read(int unit, uint32_t lba, uint32_t count_sec, void *buf)
{
    if (unit < 0 || unit >= count || !count_sec || units[unit].gone)
        return -1;
    struct unit *u = &units[unit];
    uint8_t read10[10] = { 0x28, 0 };

    put32be(read10 + 2, lba);
    read10[7] = (uint8_t)(count_sec >> 8);
    read10[8] = (uint8_t)count_sec;

    if (command(u, read10, 10, buf, count_sec * SECTOR, 1) < 0)
        return -1;
    return 0;
}

int usbmass_write(int unit, uint32_t lba, uint32_t count_sec,
                  const void *buf)
{
    if (unit < 0 || unit >= count || !count_sec || units[unit].gone)
        return -1;
    struct unit *u = &units[unit];
    uint8_t write10[10] = { 0x2A, 0 };

    put32be(write10 + 2, lba);
    write10[7] = (uint8_t)(count_sec >> 8);
    write10[8] = (uint8_t)count_sec;

    if (command(u, write10, 10, (void *)(uintptr_t)buf,
                count_sec * SECTOR, 0) < 0)
        return -1;
    return 0;
}
