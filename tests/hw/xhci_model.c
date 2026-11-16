/* An Intel laptop's USB on the host: the Bluetooth driver, through the
 * USB layer and the xHCI driver, against a model of the machine
 *
 * No emulator has what a 2015 laptop has on its internal USB bus: an
 * Intel xHCI that wants scratchpad memory, that the firmware still
 * owns when the system starts, whose USB 2.0 sockets the firmware left
 * with EHCI - and in one of them the Bluetooth half of an Intel
 * Wireless 3160, behind a webcam, a card reader, a fingerprint reader,
 * a mouse and a touchscreen, with a USB 3.0 stick further on. QEMU's
 * xHCI does none of the first three things and has no Bluetooth at
 * all. So the machine is written here - from the xHCI specification,
 * Intel's PCH datasheets, the descriptors those devices really give
 * and the Bluetooth HCI USB transport - and the drivers
 * (kernel/xhci.c, kernel/usbhc.c and kernel/bt.c, compiled as they
 * are) run against it.
 *
 * The machine moves only while the driver waits: every io_wait(),
 * pit_ticks() and task_sleep() is a step of the model. Nothing runs
 * behind the driver's back, so every run is the same.
 *
 * What the model holds against the driver, besides the end result:
 * the firmware owns the controller until asked; the controller stops
 * with an internal error at the first Enable Slot if it was not given
 * its 34 scratchpad pages (a count split across two fields); USB 2.0
 * sockets stay with EHCI until moved; a port change event follows
 * every attach and every reset; contexts may not cross a page, rings
 * may not cross 64 KiB; endpoints must be described as the device
 * describes them; the event ring must be read.
 *
 * Usage: xhci_model boot | late
 *   boot - everything is plugged in from the start
 *   late - the Bluetooth adapter appears only after the walk at
 *          start-up, the way a radio does when the firmware's wireless
 *          switch turns it on late
 * Prints "ok: ..." or "FAIL: ..." per check.
 */

#define _GNU_SOURCE
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "kernel/include/pci.h"
#include "kernel/include/bt.h"
#include "kernel/include/usbhc.h"

/* --- what went against the specification ------------------------------ */

static char problems[32][200];
static int nproblems;

static void problem(const char *what)
{
    for (int i = 0; i < nproblems; i++)
        if (!strcmp(problems[i], what))
            return;
    if (nproblems < 32)
        snprintf(problems[nproblems++], sizeof(problems[0]), "%s", what);
}

static int had_problem(const char *start)
{
    for (int i = 0; i < nproblems; i++)
        if (!strncmp(problems[i], start, strlen(start)))
            return 1;
    return 0;
}

static int failures;

static void check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

/* --- what the kernel prints, kept to be searched ----------------------- */

static char logbuf[1 << 16];
static size_t loglen;

void printk(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    size_t n = strlen(line);
    if (loglen + n + 1 < sizeof(logbuf)) {
        memcpy(logbuf + loglen, line, n);
        loglen += n;
        logbuf[loglen] = 0;
    }
}

/* --- the machine's registers ------------------------------------------- */

#define CAPLEN      0x80
#define OP(x)       (CAPLEN + (x))
#define USBCMD      OP(0x00)
#define USBSTS      OP(0x04)
#define PAGESIZE    OP(0x08)
#define CRCR        OP(0x18)
#define DCBAAP      OP(0x30)
#define CONFIG      OP(0x38)
#define PORTSC(i)   OP(0x400 + 0x10 * (i))
#define RT          0x2000
#define ERSTSZ      (RT + 0x28)
#define ERSTBA      (RT + 0x30)
#define ERDP        (RT + 0x38)
#define DB          0x3000
#define XECP        0x8000

#define CMD_RS      1u
#define CMD_HCRST   2u
#define STS_HCH     1u
#define STS_CNR     (1u << 11)
#define STS_HCE     (1u << 12)

#define NPORTS      12          /* 8 USB 2.0 sockets, then 4 USB 3.0 */
#define USB2_PORTS  8
#define MAXSLOTS    32
#define SCRATCH_WANT 34         /* high field 1, low field 2 */
#define NOT_RUNG    0xFFFFFFFFu

#define USB2PRM     0x000000FFu /* every USB 2.0 socket may be moved */
#define USB3PRM     0x0000000Fu

static uint8_t *mmio;

static uint32_t R(uint32_t off)
{
    return *(volatile uint32_t *)(mmio + off);
}

static void W(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(mmio + off) = v;
}

static uint64_t R64(uint32_t off)
{
    return R(off) | (uint64_t)R(off + 4) << 32;
}

struct trb {
    volatile uint32_t p0, p1, status, control;
};

static struct trb *at(uint64_t a)
{
    return (struct trb *)(uintptr_t)a;
}

/* --- PCI: the xHCI and the EHCI that shares its sockets ---------------- */

struct pcifn {
    int bus, slot, func;
    uint16_t vendor, device;
    uint32_t class;
    uint32_t cfg[64];
};

static struct pcifn pcis[2];
static int bus_master;

static struct pcifn *pci_at(int bus, int slot, int func)
{
    for (int i = 0; i < 2; i++)
        if (pcis[i].bus == bus && pcis[i].slot == slot &&
            pcis[i].func == func)
            return &pcis[i];
    return (struct pcifn *)0;
}

uint32_t pci_config_read(int bus, int slot, int func, int off)
{
    struct pcifn *p = pci_at(bus, slot, func);
    return p ? p->cfg[(off & 0xFF) / 4] : 0xFFFFFFFFu;
}

void pci_config_write(int bus, int slot, int func, int off, uint32_t v)
{
    struct pcifn *p = pci_at(bus, slot, func);
    if (!p)
        return;
    if (p == &pcis[0]) {
        /* Only what the mask registers allow can move; the masks
           themselves are the firmware's and read-only */
        if (off == 0xD0)
            v &= p->cfg[0xD4 / 4];
        else if (off == 0xD8)
            v &= p->cfg[0xDC / 4];
        else if (off == 0xD4 || off == 0xDC)
            return;
    }
    p->cfg[(off & 0xFF) / 4] = v;
}

static void fill(const struct pcifn *p, struct pci_dev *out)
{
    memset(out, 0, sizeof(*out));
    out->bus = p->bus;
    out->slot = p->slot;
    out->func = p->func;
    out->vendor = p->vendor;
    out->device = p->device;
    for (int i = 0; i < 6; i++)
        out->bar[i] = p->cfg[4 + i];
    out->bar0 = out->bar[0];
}

int pci_find_class(uint8_t base, uint8_t sub, uint8_t progif,
                   struct pci_dev *out)
{
    for (int i = 0; i < 2; i++)
        if ((pcis[i].class >> 8) ==
            ((uint32_t)base << 16 | (uint32_t)sub << 8 | progif)) {
            fill(&pcis[i], out);
            return 1;
        }
    return 0;
}

int pci_find_device(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    for (int i = 0; i < 2; i++)
        if (pcis[i].vendor == vendor && pcis[i].device == device) {
            fill(&pcis[i], out);
            return 1;
        }
    return 0;
}

int pci_enumerate(int index, struct pci_dev *out, uint32_t *class_out)
{
    if (index < 0 || index >= 2)
        return 0;
    fill(&pcis[index], out);
    if (class_out)
        *class_out = pcis[index].class;
    return 1;
}

void pci_enable_bus_master(const struct pci_dev *d)
{
    if (d->vendor == pcis[0].vendor && d->device == pcis[0].device)
        bus_master = 1;
}

int paging64_map_phys(uint64_t phys, uint64_t len)
{
    (void)phys;
    (void)len;
    return 0;
}

/* Frames below 4 GiB, as the kernel's allocator hands out */
static int frames_given;

uint32_t pmm_alloc_frame(void)
{
    static uint8_t *pool;
    if (!pool) {
        pool = mmap((void *)0, 128 * 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (pool == MAP_FAILED) {
            printf("FAIL: no memory below 4 GiB on this host\n");
            exit(1);
        }
    }
    if (frames_given >= 128)
        return 0;
    return (uint32_t)(uintptr_t)(pool + 4096 * frames_given++);
}

/* --- the devices --------------------------------------------------------
 *
 * Their descriptors as those devices give them (lsusb -v of the real
 * ones), cut down to what a driver reads: the classes, the endpoints
 * and the lengths. The webcam's configuration is longer than the 256
 * bytes the Bluetooth driver reads of anybody's, as a UVC camera's is.
 */

#define DEVDESC(usb, cls, sub, proto, mps0, vid, pid) \
    { 18, 1, 0x00, usb, cls, sub, proto, mps0, (vid) & 0xFF, (vid) >> 8, \
      (pid) & 0xFF, (pid) >> 8, 0x01, 0x00, 0, 0, 0, 1 }

static const uint8_t bt_desc[18] = DEVDESC(2, 0xE0, 0x01, 0x01, 64, 0x8087, 0x07DC);

#define VOICE_ALT(n, mps) 9, 4, 1, n, 2, 0xE0, 0x01, 0x01, 0, \
                          7, 5, 0x03, 0x01, mps, 0, 1, \
                          7, 5, 0x83, 0x01, mps, 0, 1
static const uint8_t bt_cfg[] = {
    9, 2, 177, 0, 2, 1, 0, 0xE0, 50,
    9, 4, 0, 0, 3, 0xE0, 0x01, 0x01, 0,      /* HCI: events and ACL */
    7, 5, 0x81, 0x03, 64, 0, 1,
    7, 5, 0x02, 0x02, 64, 0, 1,
    7, 5, 0x82, 0x02, 64, 0, 1,
    VOICE_ALT(0, 0), VOICE_ALT(1, 9), VOICE_ALT(2, 17),   /* voice */
    VOICE_ALT(3, 25), VOICE_ALT(4, 33), VOICE_ALT(5, 49) };
_Static_assert(sizeof(bt_cfg) == 177, "the 3160's configuration is 177 bytes");

static const uint8_t cam_desc[18] = DEVDESC(2, 0xEF, 0x02, 0x01, 64, 0x5986, 0x055C);
static uint8_t cam_cfg[300];

static const uint8_t reader_desc[18] = DEVDESC(2, 0xFF, 0xFF, 0xFF, 64, 0x0BDA, 0x0129);
static const uint8_t reader_cfg[] = {
    9, 2, 39, 0, 1, 1, 0, 0x80, 250,
    9, 4, 0, 0, 3, 0xFF, 0x06, 0x50, 0,
    7, 5, 0x81, 0x02, 0x00, 0x02, 0,
    7, 5, 0x02, 0x02, 0x00, 0x02, 0,
    7, 5, 0x83, 0x03, 3, 0, 4 };

static const uint8_t finger_desc[18] = DEVDESC(2, 0xFF, 0x10, 0xFF, 8, 0x138A, 0x0017);
static const uint8_t finger_cfg[] = {
    9, 2, 39, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 3, 0xFF, 0x00, 0x00, 0,
    7, 5, 0x01, 0x02, 64, 0, 0,
    7, 5, 0x81, 0x02, 64, 0, 0,
    7, 5, 0x83, 0x03, 8, 0, 4 };

static const uint8_t mouse_desc[18] = DEVDESC(2, 0x00, 0x00, 0x00, 8, 0x046D, 0xC077);
static const uint8_t mouse_cfg[] = {
    9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 0x03, 0x01, 0x02, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 52, 0,
    7, 5, 0x81, 0x03, 4, 0, 10 };

static const uint8_t touch_desc[18] = DEVDESC(2, 0x00, 0x00, 0x00, 64, 0x04F3, 0x2073);
static const uint8_t touch_cfg[] = {
    9, 2, 34, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 0x03, 0x00, 0x00, 0,
    9, 0x21, 0x10, 0x01, 0, 1, 0x22, 0x2C, 0x03,
    7, 5, 0x81, 0x03, 64, 0, 1 };

static const uint8_t stick_desc[18] = DEVDESC(3, 0x00, 0x00, 0x00, 9, 0x0781, 0x5581);
static const uint8_t stick_cfg[] = {
    9, 2, 44, 0, 1, 1, 0, 0x80, 112,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x81, 0x02, 0x00, 0x04, 0,
    6, 0x30, 15, 0, 0, 0,
    7, 5, 0x02, 0x02, 0x00, 0x04, 0,
    6, 0x30, 15, 0, 0, 0 };

struct dev {
    const char *name;
    int speed;                  /* 1 full, 2 low, 3 high, 4 super */
    const uint8_t *desc;
    const uint8_t *cfg;
    int cfg_len;
    int configured;
    int slot;                   /* the slot it was addressed through */
    int bt;
};

static struct dev cam = { "webcam", 3, cam_desc, cam_cfg, sizeof(cam_cfg), 0, 0, 0 };
static struct dev reader = { "card reader", 3, reader_desc, reader_cfg, sizeof(reader_cfg), 0, 0, 0 };
static struct dev finger = { "fingerprint reader", 1, finger_desc, finger_cfg, sizeof(finger_cfg), 0, 0, 0 };
static struct dev mouse = { "mouse", 2, mouse_desc, mouse_cfg, sizeof(mouse_cfg), 0, 0, 0 };
static struct dev touch = { "touchscreen", 1, touch_desc, touch_cfg, sizeof(touch_cfg), 0, 0, 0 };
static struct dev btdev = { "Bluetooth", 1, bt_desc, bt_cfg, sizeof(bt_cfg), 0, 0, 1 };
static struct dev stick = { "USB 3.0 stick", 4, stick_desc, stick_cfg, sizeof(stick_cfg), 0, 0, 0 };

/* Socket 1 is empty; the adapter is in socket 8, the last USB 2.0 one,
   after five other devices; the stick in socket 10, a USB 3.0 one */
static struct dev *plugged[NPORTS] = {
    0, &cam, &reader, &finger, &mouse, 0, &touch, &btdev,
    0, &stick, 0, 0 };

static void build_cam_cfg(void)
{
    uint8_t *c = cam_cfg;
    int n = 0;
    static const uint8_t head[] = {
        9, 2, sizeof(cam_cfg) & 0xFF, sizeof(cam_cfg) >> 8, 2, 1, 0, 0x80, 250,
        8, 0x0B, 0, 2, 0x0E, 0x03, 0, 0,          /* the two belong together */
        9, 4, 0, 0, 1, 0x0E, 0x01, 0x00, 0 };     /* video control */
    memcpy(c, head, sizeof(head));
    n = sizeof(head);
    /* UVC's class-specific descriptors: units, terminals, formats */
    while (n + 32 + 7 + 9 <= (int)sizeof(cam_cfg) - 32) {
        c[n] = 32;
        c[n + 1] = 0x24;
        n += 32;
    }
    static const uint8_t tail[] = {
        7, 5, 0x83, 0x03, 16, 0, 6,
        9, 4, 1, 0, 0, 0x0E, 0x02, 0x00, 0 };    /* video streaming */
    memcpy(c + n, tail, sizeof(tail));
    n += sizeof(tail);
    while (n < (int)sizeof(cam_cfg)) {
        int len = (int)sizeof(cam_cfg) - n;
        if (len > 32)
            len = 32;
        c[n] = (uint8_t)len;
        c[n + 1] = 0x24;
        n += len;
    }
}

/* An endpoint as the device's own configuration has it (alternate
   setting 0 of each interface - what is live before anybody chooses
   another). 1 = found. */
static int dev_endpoint(const struct dev *d, int addr, int *attr, int *mps,
                        int *interval)
{
    int alt = 0;
    for (int i = 0; i + 1 < d->cfg_len && d->cfg[i] >= 2; i += d->cfg[i]) {
        if (d->cfg[i + 1] == 4)
            alt = d->cfg[i + 3];
        if (d->cfg[i + 1] == 5 && alt == 0 && d->cfg[i + 2] == addr) {
            *attr = d->cfg[i + 3] & 3;
            *mps = (d->cfg[i + 4] | d->cfg[i + 5] << 8) & 0x7FF;
            *interval = d->cfg[i + 6];
            return 1;
        }
    }
    return 0;
}

/* --- the Bluetooth adapter ---------------------------------------------
 *
 * HCI over USB: commands arrive as class requests on endpoint zero,
 * events leave through the interrupt endpoint in 64-byte packets, the
 * last one short. An event whose length is a multiple of 64 ends on a
 * full packet and nothing more - the transport promises no empty packet
 * after it - so a driver asking for more than one packet at a time
 * would get the next event glued on.
 */
#define BTQ 64
static uint8_t btq[BTQ][260];
static int btq_len[BTQ];
static int btq_head, btq_tail, btq_off;

static const uint8_t bt_addr_wire[6] = { 0x56, 0x34, 0x12, 0xD9, 0x34, 0xA4 };
static const char bt_local_name[] = "IdeaPad 3160";
static int bt_first_op = -1;
static int bt_commands;

static void bt_queue(const uint8_t *e, int n)
{
    int next = (btq_tail + 1) % BTQ;
    if (next == btq_head) {
        problem("the Bluetooth adapter's events were never read");
        return;
    }
    memcpy(btq[btq_tail], e, (size_t)n);
    btq_len[btq_tail] = n;
    btq_tail = next;
}

static void command_complete(uint16_t op, const uint8_t *ret, int n)
{
    uint8_t e[260];
    e[0] = 0x0E;
    e[1] = (uint8_t)(3 + n);
    e[2] = 1;
    e[3] = (uint8_t)op;
    e[4] = (uint8_t)(op >> 8);
    memcpy(e + 5, ret, (size_t)n);
    bt_queue(e, 5 + n);
}

static void advert(const uint8_t *addr, const char *name, int rssi)
{
    uint8_t e[64];
    int n = 0, dlen = 0;
    uint8_t data[31];

    data[dlen++] = 2;                    /* flags */
    data[dlen++] = 0x01;
    data[dlen++] = 0x06;
    if (name) {
        int l = (int)strlen(name);
        data[dlen++] = (uint8_t)(l + 1);
        data[dlen++] = 0x09;             /* complete local name */
        memcpy(data + dlen, name, (size_t)l);
        dlen += l;
    }
    e[n++] = 0x3E;
    e[n++] = 0;                          /* length, below */
    e[n++] = 0x02;                       /* advertising report */
    e[n++] = 1;
    e[n++] = 0x00;                       /* connectable, undirected */
    e[n++] = 0x01;                       /* a random address */
    memcpy(e + n, addr, 6);
    n += 6;
    e[n++] = (uint8_t)dlen;
    memcpy(e + n, data, (size_t)dlen);
    n += dlen;
    e[n++] = (uint8_t)(int8_t)rssi;
    e[1] = (uint8_t)(n - 2);
    bt_queue(e, n);
}

static void bt_command(const uint8_t *p, int len)
{
    if (len < 3 || len != 3 + p[2]) {
        problem("an HCI command whose length byte disagrees with the "
                "transfer");
        return;
    }
    uint16_t op = (uint16_t)(p[0] | p[1] << 8);
    uint8_t r[256];
    memset(r, 0, sizeof(r));
    if (bt_first_op < 0)
        bt_first_op = op;
    bt_commands++;

    switch (op) {
    case 0x0C03:                         /* Reset */
    case 0x0C01:                         /* Set Event Mask */
    case 0x200B:                         /* LE Set Scan Parameters */
        command_complete(op, r, 1);
        break;
    case 0x1009:                         /* Read BD_ADDR */
        memcpy(r + 1, bt_addr_wire, 6);
        command_complete(op, r, 7);
        break;
    case 0x0C14:                         /* Read Local Name: 248 bytes */
        memcpy(r + 1, bt_local_name, sizeof(bt_local_name) - 1);
        command_complete(op, r, 249);
        break;
    case 0x1001:                         /* Read Local Version */
        r[1] = 6;                        /* HCI 4.0 */
        r[2] = 0x00; r[3] = 0x10;
        r[4] = 6;                        /* LMP 4.0 */
        r[5] = 0x02; r[6] = 0x00;        /* Intel */
        r[7] = 0x0E; r[8] = 0x22;
        command_complete(op, r, 9);
        break;
    case 0x200C:                         /* LE Set Scan Enable */
        command_complete(op, r, 1);
        if (len >= 4 && p[3]) {
            static const uint8_t a1[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
            static const uint8_t a2[6] = { 0x66, 0x77, 0x88, 0x99, 0xAA, 0xDB };
            static const uint8_t a3[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0xF6 };
            advert(a1, "Mi Band 3", -61);
            advert(a2, "JBL Flip 5", -70);
            advert(a3, (const char *)0, -82);
            advert(a1, "Mi Band 3", -60);   /* heard again */
        }
        break;
    default:
        r[0] = 0x01;                     /* Unknown HCI Command */
        command_complete(op, r, 1);
        break;
    }
}

/* --- the requests every device answers --------------------------------- */

static int copy_out(void *buf, int len, const uint8_t *src, int n)
{
    int k = len < n ? len : n;
    memcpy(buf, src, (size_t)k);
    return k;
}

/* What a device does with a control transfer: bytes moved, -1 = stall */
static int device_request(struct dev *d, const uint8_t *s, uint8_t *buf,
                          int len)
{
    uint16_t value = (uint16_t)(s[2] | s[3] << 8);
    uint16_t wlen = (uint16_t)(s[6] | s[7] << 8);

    if (len != wlen)
        problem("a control transfer whose data stage is not wLength long");
    if (s[0] == 0x80 && s[1] == 6) {
        if ((value >> 8) == 1)
            return copy_out(buf, len, d->desc, 18);
        if ((value >> 8) == 2)
            return copy_out(buf, len, d->cfg, d->cfg_len);
        return -1;
    }
    if (s[0] == 0x00 && s[1] == 9) {
        d->configured = value;
        return 0;
    }
    if (d->bt && s[0] == 0x20 && s[1] == 0) {
        if (!d->configured)
            problem("an HCI command to an adapter not yet configured");
        bt_command(buf, len);
        return len;
    }
    if (s[0] == 0x21 && (s[1] == 0x0A || s[1] == 0x0B))
        return 0;                        /* HID: set idle, set protocol */
    return -1;
}

/* --- the controller ----------------------------------------------------- */

static unsigned long steps;
static uint32_t ticks;
static unsigned long tick_calls;

static int late, late_armed, late_go;

static int running, hc_error;
static int fw_owns = 1;
static unsigned long fw_letgo_at;
static int touched_while_owned;
static uint32_t last_cmd;
static int scratch_ok;

static uint64_t cr_deq;
static int cr_cycle;
static uint64_t ev_seg;
static int ev_size, ev_enq, ev_cycle;
static int events_posted, port_events;

struct pstate {
    int connected, enabled;
    unsigned long reset_until, visible_since, settle_at;
    uint32_t change;
    uint32_t shown;
};
static struct pstate ps[NPORTS];

struct slot {
    int used;
    struct dev *dev;
    int port;
    uint8_t *out;
    int entries;
    uint64_t ring[32];
    int cycle[32];
    int type[32];
    int mps[32];
    int enabled[32];
    int active[32];
    int filled;                          /* an interrupt TRB half full */
};
static struct slot slots[MAXSLOTS + 1];

static void post(uint64_t p, uint32_t status, uint32_t control)
{
    if (!running || hc_error)
        return;
    struct trb *seg = at(ev_seg);
    uint64_t deq = R64(ERDP) & ~0xFull;
    if (deq < ev_seg || deq >= ev_seg + (uint64_t)ev_size * 16) {
        problem("the event ring's dequeue pointer points outside it");
        return;
    }
    int deq_i = (int)((deq - ev_seg) / 16);
    if ((ev_enq + 1) % ev_size == deq_i) {
        problem("the event ring ran full: events were not being read");
        return;
    }
    seg[ev_enq].p0 = (uint32_t)p;
    seg[ev_enq].p1 = (uint32_t)(p >> 32);
    seg[ev_enq].status = status;
    seg[ev_enq].control = control | (uint32_t)ev_cycle;
    if (++ev_enq == ev_size) {
        ev_enq = 0;
        ev_cycle ^= 1;
    }
    events_posted++;
}

static void port_event(int i)
{
    if (!running || hc_error)
        return;
    port_events++;
    post((uint64_t)(i + 1) << 24, 1u << 24, 34u << 10);
}

static void hc_died(void)
{
    hc_error = 1;
    running = 0;
    W(USBSTS, R(USBSTS) | STS_HCE | STS_HCH);
}

static void check_place(const void *p, int len, const char *what)
{
    char m[160];
    if ((uintptr_t)p & 63) {
        snprintf(m, sizeof(m), "%s not aligned to 64 bytes", what);
        problem(m);
    }
    if (((uintptr_t)p & 0xFFF) + (uintptr_t)len > 4096) {
        snprintf(m, sizeof(m), "%s crosses a page boundary", what);
        problem(m);
    }
}

static void check_scratchpad(void)
{
    uint64_t *dcbaa = (uint64_t *)(uintptr_t)R64(DCBAAP);
    uint64_t *arr = dcbaa ? (uint64_t *)(uintptr_t)dcbaa[0] : 0;
    int good = arr != 0;

    if (good)
        check_place(arr, SCRATCH_WANT * 8, "the scratchpad array");
    for (int i = 0; good && i < SCRATCH_WANT; i++) {
        if (!arr[i] || (arr[i] & 0xFFF))
            good = 0;
        for (int j = 0; good && j < i; j++)
            if (arr[j] == arr[i])
                good = 0;
    }
    scratch_ok = good;
}

static void hc_reset(void)
{
    if (running)
        problem("the controller was reset while running");
    running = 0;
    hc_error = 0;
    W(USBCMD, 0);
    W(USBSTS, STS_HCH | STS_CNR);
    W(CRCR, 0); W(CRCR + 4, 0);
    W(DCBAAP, 0); W(DCBAAP + 4, 0);
    W(CONFIG, 0);
    for (int i = 0; i < NPORTS; i++) {
        struct pstate z = { 0, 0, 0, 0, steps + 20, 0, 0 };
        ps[i] = z;
    }
    for (int s = 0; s <= MAXSLOTS; s++) {
        if (slots[s].dev)
            slots[s].dev->slot = 0;
        memset(&slots[s], 0, sizeof(slots[s]));
    }
    last_cmd = 0;
}

static void hc_start(void)
{
    if (!bus_master)
        problem("the controller was started before it was allowed to "
                "reach memory");
    running = 1;
    W(USBSTS, R(USBSTS) & ~STS_HCH);
    cr_deq = R64(CRCR) & ~0x3Full;
    cr_cycle = (int)(R(CRCR) & 1);
    if ((R(ERSTSZ) & 0xFFFF) != 1)
        problem("the event ring segment table does not have one entry");
    uint32_t *erst = (uint32_t *)(uintptr_t)R64(ERSTBA);
    if (!erst) {
        problem("started with no event ring");
        hc_died();
        return;
    }
    ev_seg = erst[0] | (uint64_t)erst[1] << 32;
    ev_size = (int)(erst[2] & 0xFFFF);
    ev_enq = 0;
    ev_cycle = 1;
    if (ev_size < 16 || (ev_seg & 63) ||
        (ev_seg >> 16) != ((ev_seg + (uint64_t)ev_size * 16 - 1) >> 16))
        problem("the event ring segment is too small, misaligned or "
                "crosses 64 KiB");
    check_scratchpad();
    if (!R64(DCBAAP))
        problem("started with no context array");
    /* Changes that happened while stopped are reported on starting */
    for (int i = 0; i < NPORTS; i++)
        if (ps[i].change)
            port_event(i);
}

static void firmware_step(void)
{
    uint32_t leg = R(XECP);
    if (fw_owns && !(leg & (1u << 16)))
        fw_owns = 0;                     /* taken from it by force */
    if (fw_owns && (leg & (1u << 24))) {
        if (!fw_letgo_at)
            fw_letgo_at = steps + 3;
        else if (steps >= fw_letgo_at) {
            W(XECP, leg & ~(1u << 16));
            fw_owns = 0;
        }
    }
}

static void command_reg_step(void)
{
    uint32_t cmd = R(USBCMD);
    if (cmd == last_cmd)
        return;
    if (fw_owns)
        touched_while_owned = 1;
    if (cmd & CMD_HCRST) {
        hc_reset();
        return;
    }
    if ((cmd & CMD_RS) && !(last_cmd & CMD_RS) && !hc_error)
        hc_start();
    else if (!(cmd & CMD_RS) && (last_cmd & CMD_RS)) {
        running = 0;
        W(USBSTS, R(USBSTS) | STS_HCH);
    }
    last_cmd = cmd;
}

static int visible(int i)
{
    struct dev *d = plugged[i];
    if (!d)
        return 0;
    if (d == &btdev && late && !late_go)
        return 0;
    if (i < USB2_PORTS)
        return (pcis[0].cfg[0xD0 / 4] >> i) & 1;      /* not EHCI's */
    return (pcis[0].cfg[0xD8 / 4] >> (i - USB2_PORTS)) & 1;
}

static void port_written(int i, uint32_t v)
{
    struct pstate *p = &ps[i];
    if ((v & (1u << 4)) && p->connected) {   /* reset */
        p->enabled = 0;
        p->reset_until = steps + 5;
    }
    if (v & (1u << 1))
        p->enabled = 0;                  /* a 1 to PED turns the port off */
    p->change &= ~(v & 0x00FE0000u);
}

static void ports_step(void)
{
    if (R(USBSTS) & STS_CNR) {
        /* The controller is still coming out of its reset */
        static unsigned long cnr_until;
        if (!cnr_until)
            cnr_until = steps + 3;
        if (steps >= cnr_until) {
            W(USBSTS, R(USBSTS) & ~STS_CNR);
            cnr_until = 0;
        }
        return;
    }
    for (int i = 0; i < NPORTS; i++) {
        struct pstate *p = &ps[i];
        uint32_t reg = R(PORTSC(i));
        if (reg != p->shown)
            port_written(i, reg);

        int vis = visible(i) && steps >= p->settle_at;
        if (vis && !p->connected) {
            if (!p->visible_since)
                p->visible_since = steps;
            if (steps - p->visible_since >= 6) {
                p->connected = 1;
                p->change |= 1u << 17;   /* CSC */
                if (i >= USB2_PORTS)
                    p->enabled = 1;      /* SuperSpeed trains by itself */
                port_event(i);
            }
        } else if (!vis) {
            p->visible_since = 0;
            if (p->connected) {
                p->connected = 0;
                p->enabled = 0;
                p->change |= 1u << 17;
                port_event(i);
            }
        }
        if (p->reset_until && steps >= p->reset_until) {
            p->reset_until = 0;
            if (p->connected) {
                p->enabled = 1;
                p->change |= 1u << 21;   /* PRC */
                port_event(i);
            }
        }
        uint32_t v = (p->connected ? 1u : 0) | (p->enabled ? 2u : 0) |
                     (p->reset_until ? 0x10u : 0) | (1u << 9) | p->change;
        if (p->enabled)
            v |= (uint32_t)plugged[i]->speed << 10;
        else if (!p->connected)
            v |= 5u << 5;                /* link state: RxDetect */
        W(PORTSC(i), v);
        p->shown = v;
    }
}

/* --- commands ----------------------------------------------------------- */

static uint8_t *ctx(uint8_t *base, int i)
{
    return base + 64 * i;
}

static int address_device(struct trb *t, int s)
{
    uint8_t *in = (uint8_t *)(uintptr_t)(t->p0 | (uint64_t)t->p1 << 32);
    if (s < 1 || s > MAXSLOTS || !slots[s].used)
        return 11;                       /* slot not enabled */
    if (!in)
        return 5;
    check_place(in, 33 * 64, "an input context");
    uint32_t *ctl = (uint32_t *)ctx(in, 0);
    uint32_t *sc = (uint32_t *)ctx(in, 1);
    uint32_t *e0 = (uint32_t *)ctx(in, 2);
    if ((ctl[1] & 3) != 3) {
        problem("Address Device without the slot and endpoint zero");
        return 17;
    }
    int port = (int)((sc[1] >> 16) & 0xFF);
    int speed = (int)((sc[0] >> 20) & 0xF);
    int mps = (int)(e0[1] >> 16);
    if (port < 1 || port > NPORTS || !ps[port - 1].connected ||
        !ps[port - 1].enabled)
        return 4;                        /* nobody answered SET_ADDRESS */
    struct dev *d = plugged[port - 1];
    if (speed != d->speed) {
        problem("a device addressed at a speed it does not run at");
        return 4;
    }
    if (((e0[1] >> 3) & 7) != 4)
        problem("endpoint zero not described as a control endpoint");
    int want = d->speed == 2 ? 8 : d->speed == 3 ? 64 : d->speed >= 4 ? 512 : -1;
    if (want > 0 ? mps != want : (mps != 8 && mps != 16 && mps != 32 && mps != 64))
        problem("endpoint zero given a packet size its speed does not allow");

    uint64_t *dcbaa = (uint64_t *)(uintptr_t)R64(DCBAAP);
    uint8_t *out = (uint8_t *)(uintptr_t)dcbaa[s];
    if (!out) {
        problem("Address Device for a slot with no device context");
        return 5;
    }
    check_place(out, 32 * 64, "a device context");
    memcpy(ctx(out, 0), sc, 64);
    memcpy(ctx(out, 1), e0, 64);
    ((uint32_t *)ctx(out, 0))[3] = (2u << 27) | (uint32_t)s;

    uint64_t deq = e0[2] | (uint64_t)e0[3] << 32;
    struct slot *sl = &slots[s];
    sl->dev = d;
    sl->port = port - 1;
    sl->out = out;
    sl->ring[1] = deq & ~0xFull;
    sl->cycle[1] = (int)(deq & 1);
    sl->type[1] = 4;
    sl->mps[1] = mps;
    sl->enabled[1] = 1;
    d->slot = s;
    d->configured = 0;
    return 1;
}

static int evaluate_context(struct trb *t, int s)
{
    uint8_t *in = (uint8_t *)(uintptr_t)(t->p0 | (uint64_t)t->p1 << 32);
    if (s < 1 || s > MAXSLOTS || !slots[s].dev || !in)
        return 5;
    check_place(in, 33 * 64, "an input context");
    uint32_t *ctl = (uint32_t *)ctx(in, 0);
    if (ctl[1] & 2) {
        int mps = (int)(((uint32_t *)ctx(in, 2))[1] >> 16);
        struct dev *d = slots[s].dev;
        int given = d->speed >= 4 ? 1 << d->desc[7] : d->desc[7];
        if (mps != given)
            problem("endpoint zero told a packet size the device never "
                    "gave");
        slots[s].mps[1] = mps;
    }
    return 1;
}

static int configure_endpoint(struct trb *t, int s)
{
    uint8_t *in = (uint8_t *)(uintptr_t)(t->p0 | (uint64_t)t->p1 << 32);
    if (s < 1 || s > MAXSLOTS || !slots[s].dev || !in)
        return 5;
    check_place(in, 33 * 64, "an input context");
    struct slot *sl = &slots[s];
    uint32_t *ctl = (uint32_t *)ctx(in, 0);
    if (ctl[1] & 1)
        sl->entries = (int)((((uint32_t *)ctx(in, 1))[0] >> 27) & 0x1F);

    for (int dci = 2; dci < 32; dci++) {
        if (!(ctl[1] & (1u << dci)))
            continue;
        uint32_t *ep = (uint32_t *)ctx(in, dci + 1);
        int type = (int)((ep[1] >> 3) & 7);
        int mps = (int)(ep[1] >> 16);
        int interval = (int)((ep[0] >> 16) & 0xFF);
        int addr = (dci / 2) | ((dci & 1) ? 0x80 : 0);
        int attr, dmps, dint;
        if (!dev_endpoint(sl->dev, addr, &attr, &dmps, &dint)) {
            problem("an endpoint the device does not have");
            return 17;
        }
        int want = attr + ((addr & 0x80) ? 4 : 0);
        if (type != want)
            problem("an endpoint described with the wrong type");
        if (mps != dmps)
            problem("an endpoint described with the wrong packet size");
        if (attr == 3 && sl->dev->speed <= 2) {
            /* milliseconds become 2^n microframes, 3 to 10 */
            int n = 3;
            while ((1 << (n - 3)) < dint && n < 10)
                n++;
            if (interval != n)
                problem("an interrupt endpoint polled at another interval "
                        "than the device asked for");
        }
        uint64_t deq = ep[2] | (uint64_t)ep[3] << 32;
        sl->ring[dci] = deq & ~0xFull;
        sl->cycle[dci] = (int)(deq & 1);
        sl->type[dci] = type;
        sl->mps[dci] = mps;
        sl->enabled[dci] = 1;
        sl->active[dci] = 0;
    }
    for (int dci = 2; dci < 32; dci++)
        if (sl->enabled[dci] && dci > sl->entries)
            problem("an endpoint beyond the slot's Context Entries");
    return 1;
}

static int run_command(struct trb *t, int type, int *slot_out)
{
    int s = (int)(t->control >> 24);
    *slot_out = s;
    switch (type) {
    case 9: {                            /* Enable Slot */
        if (!scratch_ok) {
            /* Where the controller would keep the device's state is
               memory it was never given: it stops, and says so only
               in its status register */
            problem("the controller asked for 34 scratchpad pages and was "
                    "not given them");
            hc_died();
            return -1;
        }
        int maxen = (int)(R(CONFIG) & 0xFF);
        for (int id = 1; id <= maxen && id <= MAXSLOTS; id++) {
            if (!slots[id].used) {
                memset(&slots[id], 0, sizeof(slots[id]));
                slots[id].used = 1;
                *slot_out = id;
                return 1;
            }
        }
        *slot_out = 0;
        return 9;                        /* no slots available */
    }
    case 10:                             /* Disable Slot */
        if (s < 1 || s > MAXSLOTS || !slots[s].used)
            return 11;
        if (slots[s].dev)
            slots[s].dev->slot = 0;
        memset(&slots[s], 0, sizeof(slots[s]));
        return 1;
    case 11:
        return address_device(t, s);
    case 12:
        return configure_endpoint(t, s);
    case 13:
        return evaluate_context(t, s);
    case 23:                             /* No Op */
        return 1;
    default:
        problem("a command this model does not know");
        return 5;
    }
}

static void commands(void)
{
    for (int guard = 0; guard < 64 && running && !hc_error; guard++) {
        struct trb *t = at(cr_deq);
        if (!t) {
            problem("the command ring is nowhere");
            return;
        }
        if ((t->control & 1) != (uint32_t)cr_cycle)
            return;
        int type = (int)((t->control >> 10) & 0x3F);
        if (type == 6) {
            uint64_t to = (t->p0 | (uint64_t)t->p1 << 32) & ~0xFull;
            if ((to >> 16) != ((cr_deq + 15) >> 16))
                problem("the command ring crosses 64 KiB");
            if (t->control & 2)
                cr_cycle ^= 1;
            cr_deq = to;
            continue;
        }
        uint64_t here = cr_deq;
        cr_deq += 16;
        int slot = 0;
        int code = run_command(t, type, &slot);
        if (code < 0)
            return;
        post(here, (uint32_t)code << 24, (33u << 10) | ((uint32_t)slot << 24));
    }
}

/* --- transfers ---------------------------------------------------------- */

/* The next TRB the controller owns on an endpoint's ring, past any link */
static struct trb *peek(struct slot *sl, int dci)
{
    for (int hops = 0; hops < 4; hops++) {
        struct trb *t = at(sl->ring[dci]);
        if (!t || (t->control & 1) != (uint32_t)sl->cycle[dci])
            return (struct trb *)0;
        if (((t->control >> 10) & 0x3F) != 6)
            return t;
        uint64_t to = (t->p0 | (uint64_t)t->p1 << 32) & ~0xFull;
        if ((to >> 16) != ((sl->ring[dci] + 15) >> 16))
            problem("a transfer ring crosses 64 KiB");
        if (t->control & 2)
            sl->cycle[dci] ^= 1;
        sl->ring[dci] = to;
    }
    return (struct trb *)0;
}

static void transfer_event(int s, int dci, uint64_t trb, int code, int left)
{
    post(trb, ((uint32_t)code << 24) | (uint32_t)left,
         (32u << 10) | ((uint32_t)dci << 16) | ((uint32_t)s << 24));
}

static void control_ring(int s)
{
    struct slot *sl = &slots[s];
    for (int guard = 0; guard < 16; guard++) {
        struct trb *t = peek(sl, 1);
        if (!t)
            return;
        uint64_t setup_at = sl->ring[1];
        if (((t->control >> 10) & 0x3F) != 2) {
            problem("a control transfer that does not start with Setup");
            sl->ring[1] += 16;
            continue;
        }
        if (!(t->control & (1u << 6)) || (t->status & 0x1FFFF) != 8)
            problem("a Setup TRB without its eight bytes inside it");
        uint8_t setup[8];
        uint32_t w[2] = { t->p0, t->p1 };
        memcpy(setup, w, 8);
        sl->ring[1] += 16;
        (void)setup_at;

        struct trb *data = peek(sl, 1);
        uint64_t data_at = 0;
        if (data && ((data->control >> 10) & 0x3F) == 3) {
            data_at = sl->ring[1];
            sl->ring[1] += 16;
        } else {
            data = (struct trb *)0;
        }
        struct trb *status = peek(sl, 1);
        if (!status || ((status->control >> 10) & 0x3F) != 4) {
            problem("a control transfer without its status stage");
            return;
        }
        uint64_t status_at = sl->ring[1];
        sl->ring[1] += 16;

        uint8_t *buf = data ? (uint8_t *)(uintptr_t)(data->p0 |
                                   (uint64_t)data->p1 << 32) : (uint8_t *)0;
        int len = data ? (int)(data->status & 0x1FFFF) : 0;
        if (data && ((data->control >> 16) & 1) != (uint32_t)(setup[0] >> 7))
            problem("a data stage going the other way than its request");
        int got = device_request(sl->dev, setup, buf, len);
        if (got < 0) {
            transfer_event(s, 1, data ? data_at : status_at, 6, 0);
            continue;                    /* a stall */
        }
        if (data && (data->control & ((1u << 5) | (1u << 2))))
            transfer_event(s, 1, data_at, got < len ? 13 : 1, len - got);
        if (status->control & (1u << 5))
            transfer_event(s, 1, status_at, 1, 0);
    }
}

/* The adapter's events leave through its interrupt endpoint, a packet
   at a time, while the driver has a TRB there for them */
static void interrupt_endpoints(void)
{
    for (int s = 1; s <= MAXSLOTS; s++) {
        struct slot *sl = &slots[s];
        int dci = 3;                     /* endpoint 1 IN */
        if (!sl->used || !sl->dev || !sl->dev->bt || !sl->enabled[dci] ||
            !sl->active[dci])
            continue;
        while (btq_head != btq_tail) {
            struct trb *t = peek(sl, dci);
            if (!t) {
                sl->active[dci] = 0;
                break;
            }
            uint8_t *buf = (uint8_t *)(uintptr_t)(t->p0 |
                                                  (uint64_t)t->p1 << 32);
            int len = (int)(t->status & 0x1FFFF);
            int mps = sl->mps[dci];
            int short_pkt = 0;
            while (sl->filled < len && btq_head != btq_tail) {
                int left = btq_len[btq_head] - btq_off;
                int pkt = left < mps ? left : mps;
                if (pkt > len - sl->filled) {
                    problem("an interrupt TRB smaller than the endpoint's "
                            "packet (babble)");
                    pkt = len - sl->filled;
                }
                memcpy(buf + sl->filled, &btq[btq_head][btq_off],
                       (size_t)pkt);
                sl->filled += pkt;
                btq_off += pkt;
                if (btq_off >= btq_len[btq_head]) {
                    btq_head = (btq_head + 1) % BTQ;
                    btq_off = 0;
                }
                if (pkt < mps) {
                    short_pkt = 1;
                    break;
                }
            }
            if (!short_pkt && sl->filled < len)
                break;                   /* still open: more may come */
            uint64_t trb = sl->ring[dci];
            sl->ring[dci] += 16;
            int left = len - sl->filled;
            sl->filled = 0;
            transfer_event(s, dci, trb, left ? 13 : 1, left);
        }
    }
}

static void doorbells(void)
{
    if (R(DB) != NOT_RUNG) {
        W(DB, NOT_RUNG);
        commands();
    }
    for (int s = 1; s <= MAXSLOTS; s++) {
        uint32_t v = R(DB + 4 * (uint32_t)s);
        if (v == NOT_RUNG)
            continue;
        W(DB + 4 * (uint32_t)s, NOT_RUNG);
        int dci = (int)(v & 0xFF);
        if (!slots[s].used || !slots[s].dev) {
            problem("a doorbell for a slot with no device");
            continue;
        }
        if (dci < 1 || dci > 31 || !slots[s].enabled[dci]) {
            problem("a doorbell for an endpoint never set up");
            continue;
        }
        if (dci == 1)
            control_ring(s);
        else
            slots[s].active[dci] = 1;
    }
}

void model_step(void)
{
    steps++;
    if (!mmio)
        return;
    firmware_step();
    command_reg_step();
    ports_step();
    if (running && !hc_error) {
        doorbells();
        interrupt_endpoints();
    }
}

/* --- time --------------------------------------------------------------- */

uint32_t pit_ticks(void)
{
    model_step();
    if (++tick_calls % 4 == 0)
        ticks++;
    return ticks;
}

/* The socket watcher: started as a thread on the machine, run here by
   hand in the "late" scenario, and left through task_sleep once it has
   done what it is there for */
static void (*watch_fn)(void *);
static void *watch_arg;
static int in_watch, watch_naps;
static jmp_buf watch_exit;

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    (void)name;
    watch_fn = entry;
    watch_arg = arg;
    return (struct task *)(uintptr_t)1;
}

int task_scheduler_ready(void)
{
    return 1;
}

void task_sleep(uint32_t ms)
{
    uint32_t n = ms / 10 + 1;
    for (uint32_t i = 0; i < n; i++) {
        model_step();
        model_step();
    }
    ticks += n;
    if (in_watch) {
        if (late_armed)
            late_go = 1;                 /* the radio comes on */
        if (bt_present() || ++watch_naps > 200)
            longjmp(watch_exit, 1);
    }
}

/* --- the machine as the firmware leaves it ------------------------------ */

static void machine(void)
{
    build_cam_cfg();
    mmio = mmap((void *)0, 0x10000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mmio == MAP_FAILED) {
        printf("FAIL: no memory for the registers\n");
        exit(1);
    }
    W(0x00, 0x01000000u | CAPLEN);
    W(0x04, ((uint32_t)NPORTS << 24) | (1u << 8) | MAXSLOTS);
    W(0x08, (2u << 27) | (1u << 26) | (1u << 21) | (1u << 4) | 1u);
    W(0x10, ((uint32_t)(XECP / 4) << 16) | 0x4u | 0x1u);
    W(0x14, DB);
    W(0x18, RT);
    W(PAGESIZE, 1);
    for (int i = 0; i <= MAXSLOTS; i++)
        W(DB + 4 * (uint32_t)i, NOT_RUNG);
    /* USB Legacy Support: the firmware owns it, SMIs on */
    W(XECP, (1u << 16) | (4u << 8) | 1u);
    W(XECP + 4, 0x0000E011u);
    /* Supported protocols: USB 2.0 on 1-8, USB 3.0 on 9-12 */
    W(XECP + 0x10, (0x02u << 24) | (4u << 8) | 2u);
    W(XECP + 0x14, 0x20425355u);
    W(XECP + 0x18, (8u << 8) | 1u);
    W(XECP + 0x20, (0x03u << 24) | 2u);
    W(XECP + 0x24, 0x20425355u);
    W(XECP + 0x28, (4u << 8) | 9u);

    /* The firmware left it running, as it does after its own menus */
    W(USBCMD, CMD_RS);
    W(USBSTS, 0);
    last_cmd = CMD_RS;
    running = 0;

    uint64_t a = (uint64_t)(uintptr_t)mmio;
    struct pcifn x = { 0, 20, 0, 0x8086, 0x9CB1, 0x0C033003u, { 0 } };
    x.cfg[0] = 0x9CB18086u;
    x.cfg[4] = (uint32_t)a | 0x4u;       /* 64-bit memory BAR */
    x.cfg[5] = (uint32_t)(a >> 32);
    x.cfg[0xD4 / 4] = USB2PRM;
    x.cfg[0xDC / 4] = USB3PRM;
    pcis[0] = x;
    struct pcifn e = { 0, 29, 0, 0x8086, 0x9CA6, 0x0C032003u, { 0 } };
    e.cfg[0] = 0x9CA68086u;
    pcis[1] = e;
    for (int i = 0; i < NPORTS; i++)
        ps[i].shown = R(PORTSC(i));
}

static int addressed(void)
{
    int n = 0;
    struct dev *all[] = { &cam, &reader, &finger, &mouse, &touch, &btdev,
                          &stick };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]->slot)
            n++;
    return n;
}

static void report_problems(void)
{
    for (int i = 0; i < nproblems; i++)
        printf("  against the specification: %s\n", problems[i]);
    check("nothing the specification forbids: contexts, rings, endpoints, "
          "the event ring", nproblems == 0);
}

static void boot(void)
{
    int found = bt_init();
    /* Bluetooth stops looking where it finds its adapter; the disk and
       keyboard drivers greet every socket at start-up, and so here */
    for (int p = 0; p < usbhc_ports(); p++)
        usbhc_attach(p);

    check("Bluetooth: the adapter found on the laptop's bus, in socket 8 "
          "behind five other devices", found && bt_present());
    check("the firmware let go of the controller before the system "
          "touched it", !touched_while_owned && !fw_owns &&
          (R(XECP) & (1u << 24)));
    check("the controller's SMIs to the firmware are all switched off",
          (R(XECP + 4) & 0x0000E011u) == 0);
    check("the USB 2.0 sockets taken over from EHCI",
          pcis[0].cfg[0xD0 / 4] == USB2PRM);
    check("SuperSpeed switched on wherever it may be",
          pcis[0].cfg[0xD8 / 4] == USB3PRM);
    check("the controller got all 34 scratchpad pages it asked for (the "
          "count split across two fields)", scratch_ok);
    check("every device on the bus got a slot and an address (7 of 7)",
          addressed() == 7);
    check("port changes kept coming and commands still got their "
          "answers", port_events >= 10 && addressed() == 7);
    check("the USB 3.0 stick's endpoint zero takes 512-byte packets, not "
          "9", stick.slot && slots[stick.slot].mps[1] == 512);

    check("the adapter was reset before it was asked anything",
          bt_first_op == 0x0C03);
    check("its address, read the right way round",
          !strcmp(bt_address(), "a4:34:d9:12:34:56"));
    check("its name", !strcmp(bt_name(), bt_local_name));
    check("its version, with Intel as the maker",
          strstr(bt_version(), "Bluetooth 4.0") &&
          strstr(bt_version(), "0002"));

    int n = bt_scan(1);
    int names = 0;
    for (int i = 0; i < bt_found(); i++)
        if (!strcmp(bt_found_name(i), "Mi Band 3") ||
            !strcmp(bt_found_name(i), "JBL Flip 5"))
            names++;
    check("a scan hears the three devices advertising nearby, once each",
          n == 3);
    check("with their names", names == 2);

    usbhc_print_devices();
    check("'hardware' lists the adapter in its socket",
          strstr(logbuf, "socket 8: 8087:07dc, Bluetooth, full speed") != 0);
    check("and the devices around it, by what their interfaces are",
          strstr(logbuf, "socket 2: 5986:055c, video (webcam), high speed") &&
          strstr(logbuf, "socket 5: 046d:c077, input (HID), low speed") &&
          strstr(logbuf, "socket 10: 0781:5581, storage, SuperSpeed"));
    report_problems();
}

static void radio_late(void)
{
    int found = bt_init();
    check("radio off: the walk at start-up finds no adapter, and says so",
          !found && !bt_present() &&
          strstr(bt_why(), "no Bluetooth adapter") != 0);
    check("the sockets are being watched", watch_fn != 0);
    if (!watch_fn) {
        report_problems();
        return;
    }
    late_armed = 1;
    in_watch = 1;
    if (!setjmp(watch_exit))
        watch_fn(watch_arg);
    in_watch = 0;
    check("radio on: the watcher finds the adapter when it appears",
          bt_present());
    check("and talks to it: its address",
          !strcmp(bt_address(), "a4:34:d9:12:34:56"));
    report_problems();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, (char *)0, _IONBF, 0);
    late = argc > 1 && !strcmp(argv[1], "late");
    machine();
    if (late)
        radio_late();
    else
        boot();
    (void)had_problem;
    return failures ? 1 : 0;
}
