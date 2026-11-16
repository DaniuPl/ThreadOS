/* USB keyboards (HID): the keys, and the media keys
 *
 * Recognising the device goes the way it goes in every system: reset
 * the port, ask for the device descriptor at address zero, assign an
 * address, ask for the configuration and find in it the interfaces of
 * the HID class.
 *
 * The keyboard itself - the interface in the HID boot version - is
 * switched into the boot protocol, the one the BIOS uses, and from then
 * on we get a fixed eight-byte report: the modifiers and up to six
 * pressed keys.
 *
 * The keys a keyboard has beyond those - louder, quieter, mute - come
 * almost always on a SECOND interface of the same device, in a report
 * whose layout the device describes itself (kernel/hid.c reads it).
 * So does a volume knob, which is nothing but that interface, and a
 * keyboard that reports more than six keys at once. Each such
 * interface gets its own interrupt endpoint, read by the same thread.
 *
 * A report says what IS pressed, not what has just changed - so we
 * compare it with the previous one and act only on the new keys.
 *
 * We poll from a thread, a few dozen times a second. The keyboard has
 * nothing more urgent to say anyway, and having no interrupts means
 * one road fewer along which something can go wrong.
 */

#include <stdint.h>
#include "include/usbkbd.h"
#include "include/usbhc.h"
#include "include/keyboard.h"
#include "include/kbdmap.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/hid.h"

/* The buffers are static, because the controller reaches for them
   itself - they have to lie in contiguous memory and at an address it
   understands directly */
static uint8_t desc_buf[512] __attribute__((aligned(16)));
static uint8_t rdesc[512] __attribute__((aligned(16)));

/* One interrupt endpoint we read: a boot keyboard's, or an interface
   whose reports the device described */
#define HID_EPS 4
#define DOWN_MAX 16
static struct hep {
    int dev, ep, maxpacket;
    int boot;                       /* the fixed eight bytes */
    struct hid_map map;             /* otherwise: where what is */
    uint32_t down[DOWN_MAX];        /* what was down, by the last reports */
    uint8_t down_id[DOWN_MAX];      /* and which report said so */
    int ndown;
} eps[HID_EPS];
static uint8_t reports[HID_EPS][64] __attribute__((aligned(16)));
static int neps;
static int found;

/* An interface of the HID class as the configuration describes it */
struct iface {
    int num, subclass, protocol;
    int ep, maxpacket, interval;
    int rdesc_len;                  /* its report descriptor's length */
};
#define IFACES 8

/* The HID interfaces in a configuration (alternate setting 0), with
   their interrupt IN endpoints. Returns how many. */
static int find_hid(const uint8_t *cfg, int total, struct iface *out)
{
    int n = 0, cur = -1;
    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2 || i + dlen > total)
            break;
        if (dtype == 0x04 && dlen >= 9) {               /* an interface */
            cur = -1;
            /* class 3 = HID; alternate settings other than 0 are not
               what a device starts in */
            if (cfg[i + 5] == 3 && cfg[i + 3] == 0 && n < IFACES) {
                cur = n++;
                out[cur].num = cfg[i + 2];
                out[cur].subclass = cfg[i + 6];
                out[cur].protocol = cfg[i + 7];
                out[cur].ep = 0;
                out[cur].rdesc_len = 0;
            }
        } else if (dtype == 0x21 && cur >= 0 && dlen >= 9) {   /* HID */
            if (cfg[i + 6] == 0x22)                         /* report */
                out[cur].rdesc_len = cfg[i + 7] | (cfg[i + 8] << 8);
        } else if (dtype == 0x05 && cur >= 0 && dlen >= 7 && !out[cur].ep) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {  /* IN, interrupt */
                out[cur].ep = addr & 0x0F;
                out[cur].maxpacket = cfg[i + 4] | (cfg[i + 5] << 8);
                if (out[cur].maxpacket > 64)
                    out[cur].maxpacket = 64;
                out[cur].interval = cfg[i + 6];
            }
        }
        i += dlen;
    }
    return n;
}

/* A HID usage code -> our character. The table covers what matters on
   a boot keyboard; the rest is ignored. */
static const char hid_map[0x40] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'',
    '`', ',', '.', '/', 0,
};

static const char hid_map_shift[0x40] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"',
    '~', '<', '>', '?', 0,
};

/* Keys with no ASCII equivalent - the same codes as with PS/2 */
static char special_key(uint8_t usage)
{
    switch (usage) {
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PGUP;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PGDN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x7F: return KEY_MUTE;      /* where a keyboard has them on its */
    case 0x80: return KEY_VOLUP;     /* own page rather than the media one */
    case 0x81: return KEY_VOLDOWN;
    default:   return 0;
    }
}

static void handle_key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & 0x22) != 0;      /* the left or the right Shift */
    int ctrl  = (mods & 0x11) != 0;
    int alt   = (mods & 0x44) != 0;

    /* The right Alt alone is AltGr: a Polish letter, as on PS/2 */
    if ((mods & 0x40) && !(mods & 0x04) && !ctrl && usage < 0x40) {
        char pl[2];
        if (kbdmap_polish(hid_map[usage], shift, pl)) {
            keyboard_inject(pl[0]);
            keyboard_inject(pl[1]);
            return;
        }
    }

    /* Alt+F1..F4 switches consoles - just as with PS/2 */
    if (alt && usage >= 0x3A && usage < 0x3A + 4) {
        keyboard_inject_switch(usage - 0x3A);
        return;
    }

    char c = usage < 0x40 ? (shift ? hid_map_shift[usage] : hid_map[usage])
                          : special_key(usage);
    if (!c)
        return;
    /* The same Ctrl+letter keys the PS/2 path knows (kernel/kbdmap.c):
       a program cannot tell which keyboard was pressed, and should not
       have to */
    if (ctrl && c == 'c')
        c = KEY_INTR;
    else if (ctrl && c == 'd')
        c = KEY_EOF;
    else if (ctrl && c == 's')
        c = KEY_SAVE;
    else if (ctrl && c == 'o')
        c = KEY_OPEN;
    else if (ctrl && c == 'f')
        c = KEY_FIND;
    else if (ctrl && c == 'z')
        c = KEY_UNDO;
    else if (ctrl && c == ' ')
        c = KEY_SEARCH;
    else if (ctrl && alt && c == 'l')
        c = KEY_LOCK;
    keyboard_inject(c);
}

static uint8_t prev[6];
static uint8_t prev_mods;

static void boot_report(const uint8_t *report)
{
    uint8_t mods = report[0];
    /* The Windows key is a modifier in a USB report (GUI, left 0x08,
       right 0x80) - it opens the menu the moment it goes down */
    if ((mods & 0x88) && !(prev_mods & 0x88))
        keyboard_inject(KEY_MENU);
    prev_mods = mods;
    kbdmap_usb_mods(mods);
    /* The report says what is pressed NOW - a new key is one that was
       not in the previous report */
    for (int i = 0; i < 6; i++) {
        uint8_t k = report[2 + i];
        if (!k)
            continue;
        int seen = 0;
        for (int j = 0; j < 6; j++)
            if (prev[j] == k)
                seen = 1;
        if (!seen)
            handle_key(k, mods);
    }
    for (int i = 0; i < 6; i++)
        prev[i] = report[2 + i];
}

/* A report laid out by the device's own descriptor */
static void described_report(struct hep *h, const uint8_t *report, int len)
{
    uint32_t now[DOWN_MAX];
    int n = hid_pressed(&h->map, report, len, now, DOWN_MAX);
    /* Each report id is a report of its own - the media keys' says
       nothing about the system keys' - so what is down is kept per id */
    uint8_t id = h->map.ids ? report[0] : 0;
    uint8_t mods = 0;
    for (int i = 0; i < n; i++)
        if (now[i] >> 16 == HID_PAGE_KEYBOARD &&
            (now[i] & 0xFFFF) >= 0xE0 && (now[i] & 0xFFFF) <= 0xE7)
            mods |= (uint8_t)(1 << ((now[i] & 0xFFFF) - 0xE0));
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < h->ndown; j++)
            if (h->down[j] == now[i] && h->down_id[j] == id)
                seen = 1;
        if (seen)
            continue;
        uint16_t page = (uint16_t)(now[i] >> 16), usage = (uint16_t)now[i];
        if (page == HID_PAGE_CONSUMER) {
            /* HID Usage Tables, 15: the consumer page */
            if (usage == 0xE9)
                keyboard_inject(KEY_VOLUP);
            else if (usage == 0xEA)
                keyboard_inject(KEY_VOLDOWN);
            else if (usage == 0xE2)
                keyboard_inject(KEY_MUTE);
        } else if (page == HID_PAGE_KEYBOARD) {
            if (usage == 0xE3 || usage == 0xE7)
                keyboard_inject(KEY_MENU);
            else if (usage < 0xE0)
                handle_key((uint8_t)usage, mods);
        }
    }
    /* This id's keys are now exactly these; the other ids' stay */
    int k = 0;
    for (int j = 0; j < h->ndown; j++)
        if (h->down_id[j] != id) {
            h->down[k] = h->down[j];
            h->down_id[k++] = h->down_id[j];
        }
    for (int i = 0; i < n && k < DOWN_MAX; i++) {
        h->down[k] = now[i];
        h->down_id[k++] = id;
    }
    h->ndown = k;
}

static void poll_thread(void *arg)
{
    (void)arg;

    for (;;) {
        /* A call either hands the controller a read to wait in, or takes
           what that read brought - never both. Taking one report a round
           meant a read was out only every other round: a report every
           forty milliseconds, two to a key (pressed, let go) - a dozen
           keys a second, and a fast typist's next ones queued in the
           keyboard until it threw them away. So a report taken is
           followed at once by the next read, and the thread sleeps only
           while every endpoint has one out with nothing come back yet. */
        int any = 0;
        for (int i = 0; i < neps; i++) {
            struct hep *h = &eps[i];
            int n = usbhc_int_in(h->dev, h->ep, reports[i], h->maxpacket);
            if (n <= 0)
                continue;
            any = 1;
            if (h->boot && n >= 8)
                boot_report(reports[i]);
            else if (!h->boot)
                described_report(h, reports[i], n);
        }
        if (!any)
            task_sleep(10);
    }
}

/* Asks for an interface's report descriptor and reads it. 1 = it has
   keys we act on (the keyboard page or the consumer page). */
static int describe(int dev, const struct iface *f, struct hid_map *m)
{
    int len = f->rdesc_len;
    if (len <= 0 || len > (int)sizeof(rdesc))
        return 0;
    /* GET_DESCRIPTOR addressed to the interface: type 0x22, a report */
    uint8_t setup[8] = { 0x81, 0x06, 0x00, 0x22, (uint8_t)f->num, 0,
                         (uint8_t)len, (uint8_t)(len >> 8) };
    if (usbhc_control(dev, setup, rdesc, len, 1) < len)
        return 0;
    if (hid_parse(rdesc, len, m) < 0)
        return 0;
    return hid_has_page(m, HID_PAGE_CONSUMER) ||
           hid_has_page(m, HID_PAGE_KEYBOARD);
}

static int add_endpoint(int dev, const struct iface *f, int boot)
{
    if (neps == HID_EPS || !f->ep)
        return -1;
    if (usbhc_int_endpoint(dev, f->ep, f->maxpacket, f->interval) < 0)
        return -1;
    struct hep *h = &eps[neps++];
    h->dev = dev;
    h->ep = f->ep;
    h->maxpacket = f->maxpacket;
    h->boot = boot;
    h->ndown = 0;
    return 0;
}

int usbkbd_init(void)
{
    if (!usbhc_init())
        return 0;

    for (int port = 0; port < usbhc_ports() && neps < HID_EPS; port++) {
        /* The port reset, the device address and the endpoint zero packet
           size are handled by the controller layer - each of them
           looks different there, and from here the difference is
           invisible */
        int dev = usbhc_attach(port);
        if (dev < 0)
            continue;

        if (usbhc_descriptor(dev, 0x02, 0, desc_buf, 9) < 9)
            continue;
        int total = desc_buf[2] | (desc_buf[3] << 8);
        if (total > (int)sizeof(desc_buf))
            total = (int)sizeof(desc_buf);
        if (usbhc_descriptor(dev, 0x02, 0, desc_buf, total) < total)
            continue;

        struct iface ifs[IFACES];
        int nifs = find_hid(desc_buf, total, ifs);
        /* A mouse is the mouse driver's, media buttons and all: a
           device with a boot mouse and no boot keyboard is left alone */
        int kbd = 0, mouse = 0;
        for (int i = 0; i < nifs; i++) {
            if (ifs[i].subclass == 1 && ifs[i].protocol == 1)
                kbd = 1;
            if (ifs[i].subclass == 1 && ifs[i].protocol == 2)
                mouse = 1;
        }
        if (!nifs || (mouse && !kbd))
            continue;                    /* something other than a keyboard */

        uint8_t cfg_value = desc_buf[5];
        if (usbhc_request(dev, 0x00, 0x09, cfg_value, 0) < 0)
            continue;                    /* SET_CONFIGURATION */

        int bound = 0;
        for (int i = 0; i < nifs && neps < HID_EPS; i++) {
            struct iface *f = &ifs[i];
            if (f->subclass == 1 && f->protocol == 2)
                continue;
            int boot = f->subclass == 1 && f->protocol == 1;
            if (!boot && !describe(dev, f, &eps[neps].map))
                continue;                /* no keys of ours in it */
            /* SET_IDLE 0 - report changes only, do not repeat endlessly */
            usbhc_request(dev, 0x21, 0x0A, 0, (uint16_t)f->num);
            /* SET_PROTOCOL 0 - the boot protocol, a fixed 8-byte report;
               the others stay in the report protocol they start in */
            if (boot)
                usbhc_request(dev, 0x21, 0x0B, 0, (uint16_t)f->num);
            if (add_endpoint(dev, f, boot) < 0)
                continue;
            bound++;
            if (boot)
                printk("[ OK ] A USB keyboard (HID, %s) on port %d, "
                       "endpoint %d\n", usbhc_port_hc(port), port + 1, f->ep);
            else
                printk("[ OK ] USB %s (HID report of %d field%s) on port "
                       "%d, interface %d, endpoint %d\n",
                       hid_has_page(&eps[neps - 1].map, HID_PAGE_CONSUMER)
                           ? "media keys" : "keys",
                       eps[neps - 1].map.nfields,
                       eps[neps - 1].map.nfields == 1 ? "" : "s",
                       port + 1, f->num, f->ep);
        }
        if (bound)
            usbhc_claim(port);
    }
    if (!neps)
        return 0;
    found = 1;
    task_create("usbkbd", poll_thread, 0);
    return 1;
}

int usbkbd_present(void)
{
    return found;
}
