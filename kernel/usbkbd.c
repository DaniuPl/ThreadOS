/* USB keyboard (HID, boot protocol)
 *
 * Recognising the device goes the way it goes in every system: reset
 * the port, ask for the device descriptor at address zero, assign an
 * address, ask for the configuration and find in it an interface of
 * the HID class in its boot version. Then we switch the device into
 * the boot protocol - the one the BIOS uses - and from then on we get
 * a fixed eight-byte report: the modifiers and up to six pressed keys.
 *
 * The report says what IS pressed, not what has just changed - so we
 * compare it with the previous one and report only the new keys.
 *
 * We poll from a thread, a few dozen times a second. The keyboard has
 * nothing more urgent to say anyway, and having no interrupts means
 * one road fewer along which something can go wrong.
 */

#include <stdint.h>
#include "include/usbkbd.h"
#include "include/usbhc.h"
#include "include/keyboard.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"

/* The buffers are static, because the controller reaches for them
   itself - they have to lie in contiguous memory and at an address it
   understands directly */
static uint8_t desc_buf[256] __attribute__((aligned(16)));
static uint8_t report[8] __attribute__((aligned(16)));

static int dev;
static int found;                  /* the device handle from the controller layer */
static int ep_num, ep_maxpacket, ep_interval = 8;

/* Looks in the configuration for a HID interface in its boot version
   and its interrupt endpoint. Returns the interface number or -1. */
static int find_boot_keyboard(const uint8_t *cfg, int total)
{
    int iface = -1;
    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {          /* an interface */
            /* class 3 = HID, subclass 1 = boot, protocol 1 = keyboard */
            if (cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1)
                iface = cfg[i + 2];
            else
                iface = -1;
        } else if (dtype == 0x05 && iface >= 0 && i + 6 < total) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {  /* IN, interrupt */
                ep_num = addr & 0x0F;
                ep_maxpacket = cfg[i + 4] | (cfg[i + 5] << 8);
                if (ep_maxpacket > 8)
                    ep_maxpacket = 8;                  /* the report is 8 bytes */
                ep_interval = cfg[i + 6];
                return iface;
            }
        }
        i += dlen;
    }
    return -1;
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
    default:   return 0;
    }
}

static void handle_key(uint8_t usage, uint8_t mods)
{
    int shift = (mods & 0x22) != 0;      /* the left or the right Shift */
    int ctrl  = (mods & 0x11) != 0;
    int alt   = (mods & 0x44) != 0;

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

static void handle_report(void)
{
    uint8_t mods = report[0];
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
           while one is out with nothing come back yet. */
        int n = usbhc_int_in(dev, ep_num, report, ep_maxpacket);
        if (n >= 8) {
            handle_report();
            continue;
        }
        task_sleep(10);
    }
}

int usbkbd_init(void)
{
    if (!usbhc_init())
        return 0;

    for (int port = 0; port < usbhc_ports(); port++) {
        /* The port reset, the device address and the endpoint zero packet
           size are handled by the controller layer - each of them
           looks different there, and from here the difference is
           invisible */
        dev = usbhc_attach(port);
        if (dev < 0)
            continue;

        if (usbhc_descriptor(dev, 0x02, 0, desc_buf, 9) < 9)
            continue;
        int total = desc_buf[2] | (desc_buf[3] << 8);
        if (total > (int)sizeof(desc_buf))
            total = (int)sizeof(desc_buf);
        if (usbhc_descriptor(dev, 0x02, 0, desc_buf, total) < total)
            continue;

        int iface = find_boot_keyboard(desc_buf, total);
        if (iface < 0)
            continue;                    /* something other than a keyboard */

        uint8_t cfg_value = desc_buf[5];
        if (usbhc_request(dev, 0x00, 0x09, cfg_value, 0) < 0)
            continue;                    /* SET_CONFIGURATION */

        /* SET_IDLE 0 - report changes only, do not repeat endlessly */
        usbhc_request(dev, 0x21, 0x0A, 0, (uint16_t)iface);
        /* SET_PROTOCOL 0 - the boot protocol, a fixed 8-byte report */
        usbhc_request(dev, 0x21, 0x0B, 0, (uint16_t)iface);

        if (usbhc_int_endpoint(dev, ep_num, ep_maxpacket, ep_interval) < 0)
            continue;

        usbhc_claim(port);
        found = 1;
        task_create("usbkbd", poll_thread, 0);
        printk("[ OK ] A USB keyboard (HID, %s) on port %d, "
               "endpoint %d\n", usbhc_port_hc(port), port + 1, ep_num);
        return 1;
    }
    return 0;
}

int usbkbd_present(void)
{
    return found;
}
