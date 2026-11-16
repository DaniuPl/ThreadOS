/* Every device in the machine and what became of it
 *
 * The Device Manager in Settings and 'devices' in the shell both ask
 * the same question a person asks of a machine where something does
 * not work: is it there, is there a driver for it, and did the driver
 * get it going. The bus is walked each time, so a device nothing
 * drives is on the list as much as one that works - that one is what
 * the list is for.
 */

#include "include/devices.h"
#include "include/usbhc.h"
#include "include/video.h"
#include "include/blk.h"
#include "include/printk.h"
#include "include/string.h"

/* --- what the drivers said ------------------------------------------- */

#define CLAIMS_MAX 32

static struct claim {
    int used;
    int bus, slot, func;
    const char *driver;
    int state;                       /* DEV_* */
    char why[64];
    int (*live)(char *why, int n);
} claims[CLAIMS_MAX];

static void copy(char *dst, const char *src, int n)
{
    int i = 0;
    for (; src && src[i] && i < n - 1; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static struct claim *claim_of(const struct pci_dev *d, int make)
{
    struct claim *free = 0;
    for (int i = 0; i < CLAIMS_MAX; i++) {
        struct claim *c = &claims[i];
        if (c->used && c->bus == d->bus && c->slot == d->slot &&
            c->func == d->func)
            return c;
        if (!c->used && !free)
            free = c;
    }
    if (!make || !free)
        return 0;
    free->used = 1;
    free->bus = d->bus;
    free->slot = d->slot;
    free->func = d->func;
    return free;
}

void device_claim(const struct pci_dev *d, const char *driver)
{
    struct claim *c = claim_of(d, 1);
    if (!c)
        return;
    c->driver = driver;
    c->state = DEV_FAILED;
    copy(c->why, "its driver could not start it", sizeof(c->why));
    c->live = 0;
}

void device_up(const struct pci_dev *d)
{
    struct claim *c = claim_of(d, 0);
    if (c) {
        c->state = DEV_WORKING;
        c->why[0] = 0;
    }
}

void device_trouble(const struct pci_dev *d, int state, const char *why)
{
    struct claim *c = claim_of(d, 0);
    if (c) {
        c->state = state;
        copy(c->why, why, sizeof(c->why));
    }
}

void device_live(const struct pci_dev *d, int (*state)(char *why, int n))
{
    struct claim *c = claim_of(d, 0);
    if (c)
        c->live = state;
}

/* --- what ThreadOS has drivers for ------------------------------------
 *
 * Only for what nothing claimed: a second network card, or a USB
 * controller of the older kind beside an xHCI one. The drivers stop at
 * the first device that works and never look at the rest, so without
 * this a card ThreadOS can drive, sitting next to the one it does,
 * would be listed as having no driver at all. Each line is the number
 * its driver looks for (kernel/<driver>.c); vendor 0 matches the class.
 */
static const struct known {
    uint16_t vendor, device;
    uint8_t cls, sub, prog;
    const char *driver;
} known[] = {
    { 0x8086, 0x100E, 0, 0, 0, "e1000" },
    { 0x1AF4, 0x1000, 0, 0, 0, "virtio_net" },
    { 0x1AF4, 0x1041, 0, 0, 0, "virtio_net" },
    { 0x10EC, 0x8139, 0, 0, 0, "rtl8139" },
    { 0x10EC, 0x8168, 0, 0, 0, "rtl8168" },
    { 0x10EC, 0x8161, 0, 0, 0, "rtl8168" },
    { 0x8086, 0x08B3, 0, 0, 0, "iwlwifi" },
    { 0x8086, 0x08B4, 0, 0, 0, "iwlwifi" },
    { 0x1234, 0x1111, 0, 0, 0, "video" },
    { 0, 0, 0x01, 0x06, 0x01, "ahci" },
    { 0, 0, 0x01, 0x08, 0x02, "nvme" },
    { 0, 0, 0x0C, 0x03, 0x30, "xhci" },
    { 0, 0, 0x0C, 0x03, 0x20, "ehci" },
    { 0, 0, 0x0C, 0x03, 0x00, "uhci" },
    { 0, 0, 0x04, 0x03, 0x00, "hda" },
};

static const char *known_driver(const struct pci_dev *d, uint8_t cls,
                                uint8_t sub, uint8_t prog)
{
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        const struct known *k = &known[i];
        if (k->vendor ? (k->vendor == d->vendor && k->device == d->device)
                      : (k->cls == cls && k->sub == sub && k->prog == prog))
            return k->driver;
    }
    return 0;
}

/* --- what a device is called ------------------------------------------ */

static const struct named {
    uint16_t vendor, device;
    const char *name;
} named[] = {
    { 0x8086, 0x100E, "Intel PRO/1000 network card" },
    { 0x8086, 0x08B3, "Intel Wireless 3160" },
    { 0x8086, 0x08B4, "Intel Wireless 3160" },
    { 0x8086, 0x1237, "Intel 440FX host bridge" },
    { 0x8086, 0x7000, "Intel PIIX3 ISA bridge" },
    { 0x8086, 0x7010, "Intel PIIX3 IDE controller" },
    { 0x8086, 0x7113, "Intel PIIX4 power management" },
    { 0x8086, 0x29C0, "Intel Q35 host bridge" },
    { 0x8086, 0x2918, "Intel ICH9 LPC bridge" },
    { 0x8086, 0x2922, "Intel ICH9 SATA controller" },
    { 0x8086, 0x2930, "Intel ICH9 SMBus controller" },
    { 0x8086, 0x2668, "Intel HD Audio controller" },
    { 0x8086, 0x293E, "Intel HD Audio controller" },
    { 0x10EC, 0x8139, "Realtek RTL8139 network card" },
    { 0x10EC, 0x8168, "Realtek RTL8168 Ethernet" },
    { 0x10EC, 0x8161, "Realtek RTL8168 Ethernet" },
    { 0x1AF4, 0x1000, "Virtio network card" },
    { 0x1AF4, 0x1041, "Virtio network card" },
    { 0x1234, 0x1111, "QEMU standard display" },
    { 0x1234, 0x11E8, "QEMU teaching device" },
    { 0x1B36, 0x000D, "QEMU USB 3 controller" },
};

static const char *vendor_name(uint16_t v)
{
    switch (v) {
    case 0x8086: case 0x8087: return "Intel";
    case 0x10EC: case 0x0BDA: return "Realtek";
    case 0x1022: return "AMD";
    case 0x1002: return "AMD Radeon";
    case 0x10DE: return "NVIDIA";
    case 0x14E4: case 0x0A5C: return "Broadcom";
    case 0x168C: case 0x17CB: return "Qualcomm";
    case 0x14C3: case 0x0E8D: return "MediaTek";
    case 0x1217: return "O2 Micro";
    case 0x1180: return "Ricoh";
    case 0x144D: case 0x04E8: return "Samsung";
    case 0x15B7: return "SanDisk";
    case 0x1C5C: return "SK hynix";
    case 0x1987: return "Phison";
    case 0x1AF4: case 0x1B36: return "Red Hat";
    case 0x1234: return "QEMU";
    case 0x046D: return "Logitech";
    case 0x045E: return "Microsoft";
    case 0x05AC: return "Apple";
    case 0x0781: return "SanDisk";
    case 0x0951: return "Kingston";
    case 0x0627: return "QEMU";
    case 0x13D3: return "IMC Networks";
    case 0x04F2: return "Chicony";
    case 0x5986: return "Acer";
    case 0x06CB: return "Synaptics";
    case 0x27C6: return "Goodix";
    default:     return 0;
    }
}

static const char *pci_kind(uint8_t cls, uint8_t sub, uint8_t prog)
{
    switch (cls) {
    case 0x01:
        switch (sub) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe drive";
        default:   return "storage controller";
        }
    case 0x02: return sub == 0x80 ? "wireless network card" : "network card";
    case 0x03: return "display";
    case 0x04: return sub == 0x03 ? "audio controller" : "multimedia device";
    case 0x05: return "memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "bridge";
        }
    case 0x07: return sub == 0x80 ? "management engine" : "communication controller";
    case 0x08: return sub == 0x05 ? "SD card reader" : "system peripheral";
    case 0x09: return "input controller";
    case 0x0C:
        switch (sub) {
        case 0x03:
            return prog == 0x30 ? "USB 3 controller" :
                   prog == 0x20 ? "USB 2 controller" :
                   prog == 0x00 || prog == 0x10 ? "USB 1 controller" :
                                                  "USB controller";
        case 0x05: return "SMBus controller";
        case 0x0A: return "Thunderbolt controller";
        case 0x80: return "serial bus controller";
        default:   return "serial bus controller";
        }
    case 0x0D: return "wireless controller";
    case 0x10: return "encryption controller";
    case 0x11: return "signal processor";
    case 0x12: return "processing accelerator";
    default:   return "device";
    }
}

static const char *usb_kind(uint8_t cls, uint8_t sub, uint8_t proto)
{
    switch (cls) {
    case 0x01: return "USB audio";
    case 0x02: case 0x0A: return "USB network adapter";
    case 0x03:
        if (sub == 0x01 && proto == 0x01)
            return "USB keyboard";
        if (sub == 0x01 && proto == 0x02)
            return "USB mouse";
        return "USB input device";
    case 0x06: return "USB camera";
    case 0x07: return "USB printer";
    case 0x08: return "USB drive";
    case 0x09: return "USB hub";
    case 0x0B: return "USB smart card reader";
    case 0x0E: return "USB camera";
    case 0xE0: return sub == 0x01 && proto == 0x01 ? "Bluetooth adapter"
                                                   : "USB wireless adapter";
    default:   return "USB device";
    }
}

/* "Intel", then the kind: a name made of what is known, when the
   device itself is not in the list above */
static void make_name(char *out, int n, uint16_t vendor, const char *kind)
{
    const char *v = vendor_name(vendor);
    int k = 0;
    if (v) {
        for (; *v && k < n - 2; v++)
            out[k++] = *v;
        out[k++] = ' ';
    }
    out[k] = 0;
    copy(out + k, kind, n - k);
    if (!v && out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 'a' + 'A');
}

/* --- the list ---------------------------------------------------------- */

static int pci_count(void)
{
    int n = 0;
    struct pci_dev d;
    uint32_t cls;
    while (n < 256 && pci_enumerate(n, &d, &cls) > 0)
        n++;
    return n;
}

static void fill_pci(const struct pci_dev *d, uint32_t class_code,
                     struct device_info *o)
{
    uint8_t cls = (uint8_t)(class_code >> 24);
    uint8_t sub = (uint8_t)(class_code >> 16);
    uint8_t prog = (uint8_t)(class_code >> 8);
    o->bus = DEV_BUS_PCI;
    o->cls = cls;
    o->sub = sub;
    o->prog = prog;
    o->where[0] = (uint8_t)d->bus;
    o->where[1] = (uint8_t)d->slot;
    o->where[2] = (uint8_t)d->func;
    o->vendor = d->vendor;
    o->device = d->device;

    o->name[0] = 0;
    for (unsigned i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (named[i].vendor == d->vendor && named[i].device == d->device)
            copy(o->name, named[i].name, sizeof(o->name));
    if (!o->name[0])
        make_name(o->name, sizeof(o->name), d->vendor,
                  pci_kind(cls, sub, prog));

    struct claim *c = claim_of(d, 0);
    if (c) {
        copy(o->driver, c->driver, sizeof(o->driver));
        o->state = (uint8_t)c->state;
        copy(o->why, c->why, sizeof(o->why));
        if (c->live) {
            char why[64];
            why[0] = 0;
            o->state = (uint8_t)c->live(why, sizeof(why));
            copy(o->why, why, sizeof(o->why));
        }
        return;
    }
    if (cls == 0x06 || (cls == 0x05 && sub == 0x00)) {
        /* The machine's own wiring: nothing to drive, nothing missing */
        o->state = DEV_PLUMBING;
        return;
    }
    if (cls == 0x01 && sub == 0x01) {
        /* The old IDE bus is spoken to through its fixed ports, never
           through the device: a disk answering there is the sign */
        copy(o->driver, "ata", sizeof(o->driver));
        if (blk_has_ata()) {
            o->state = DEV_WORKING;
        } else {
            o->state = DEV_SPARE;
            copy(o->why, "no disk on it", sizeof(o->why));
        }
        return;
    }
    if (cls == 0x03 && video_w() > 0) {
        /* The screen works. On QEMU's standard card it is video.c's
           own mode; anywhere else it is the picture the firmware set
           up, drawn into directly - no driver of its own */
        if (d->vendor == 0x1234 && d->device == 0x1111) {
            copy(o->driver, "video", sizeof(o->driver));
        } else {
            copy(o->driver, "framebuffer", sizeof(o->driver));
            copy(o->why, "the screen the firmware set up", sizeof(o->why));
        }
        o->state = DEV_WORKING;
        return;
    }
    const char *k = known_driver(d, cls, sub, prog);
    if (k) {
        copy(o->driver, k, sizeof(o->driver));
        o->state = DEV_SPARE;
        copy(o->why, "ThreadOS uses another one of these",
             sizeof(o->why));
        return;
    }
    o->state = DEV_NO_DRIVER;
}

int devices_get(int index, struct device_info *o)
{
    memset(o, 0, sizeof(*o));
    if (index < 0)
        return 0;
    struct pci_dev d;
    uint32_t cls;
    if (pci_enumerate(index, &d, &cls) > 0) {
        fill_pci(&d, cls, o);
        return 1;
    }
    /* Past the PCI devices: the sockets with something in them */
    int k = index - pci_count();
    for (int s = 0; s < usbhc_ports(); s++) {
        struct usb_seen u;
        if (!usbhc_seen(s, &u))
            continue;
        if (k-- > 0)
            continue;
        o->bus = DEV_BUS_USB;
        o->where[0] = (uint8_t)(s + 1);
        o->vendor = u.vid;
        o->device = u.pid;
        o->cls = u.cls;
        o->sub = u.sub;
        o->prog = u.proto;
        if (u.trouble) {
            copy(o->name, "USB device", sizeof(o->name));
            o->state = DEV_FAILED;
            copy(o->why, u.trouble, sizeof(o->why));
            return 1;
        }
        make_name(o->name, sizeof(o->name), u.vid,
                  usb_kind(u.cls, u.sub, u.proto));
        if (u.driver) {
            copy(o->driver, u.driver, sizeof(o->driver));
            o->state = DEV_WORKING;
        } else if (u.cls == 0x09) {
            copy(o->driver, "usbhc", sizeof(o->driver));
            o->state = DEV_WORKING;      /* the bus's own: it found the rest */
        } else {
            o->state = DEV_NO_DRIVER;
        }
        return 1;
    }
    return 0;
}

static const char *state_name(int s)
{
    switch (s) {
    case DEV_FAILED:    return "failed";
    case DEV_FIRMWARE:  return "needs firmware";
    case DEV_NO_DRIVER: return "no driver";
    case DEV_SPARE:     return "not in use";
    case DEV_STARTING:  return "starting";
    case DEV_WORKING:   return "working";
    default:            return "part of the machine";
    }
}

static void hex(char *o, uint32_t v, int digits)
{
    for (int i = 0; i < digits; i++)
        o[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 0xF];
    o[digits] = 0;
}

void devices_print(void)
{
    struct device_info di;
    for (int i = 0; devices_get(i, &di); i++) {
        char v[5], p[5];
        hex(v, di.vendor, 4);
        hex(p, di.device, 4);
        if (di.bus == DEV_BUS_PCI)
            printk("  pci %u:%u.%u", (uint32_t)di.where[0],
                   (uint32_t)di.where[1], (uint32_t)di.where[2]);
        else
            printk("  usb socket %u", (uint32_t)di.where[0]);
        printk("  %s:%s  %s - %s", v, p, di.name, state_name(di.state));
        if (di.driver[0])
            printk(" (%s)", di.driver);
        if (di.why[0] && di.state != DEV_WORKING)
            printk(": %s", di.why);
        printk("\n");
    }
}
