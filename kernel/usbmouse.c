/* USB mouse (HID, boot protocol)
 *
 * The same road as the keyboard, and deliberately so: reset the port,
 * ask for the configuration, find an interface of the HID class in its
 * boot version - and there the two part company on one byte. The
 * interface protocol is 1 for a keyboard and 2 for a mouse; everything
 * else, up to and including the poll thread, is the same shape.
 *
 * Why this exists at all: a laptop made in the last few years may have
 * no 8042 controller, and then it has no PS/2 anything - keyboard and
 * touchpad both arrive as HID devices. The PS/2 driver next door will
 * find nothing on such a machine, and this one will find the pointer.
 * On a machine that has both, both work, and there is still one
 * pointer on the screen because both roads end in mouse_inject().
 *
 * The boot report is three bytes and sometimes four: the buttons, the
 * movement in X, the movement in Y, and - on a device that has a wheel
 * and a report descriptor to say so - a notch count. The movement is
 * signed and RELATIVE, which is the whole reason a mouse needs a
 * driver rather than a reading.
 */

#include <stdint.h>
#include "include/usbmouse.h"
#include "include/usbhc.h"
#include "include/mouse.h"
#include "include/task.h"
#include "include/printk.h"

/* The controller reaches for these itself, so they lie still and
   aligned - as with the keyboard */
static uint8_t desc_buf[256] __attribute__((aligned(16)));
static uint8_t report[8] __attribute__((aligned(16)));

static int dev;
static int found;
static int ep_num, ep_maxpacket, ep_interval = 8;

/* Looks in the configuration for a boot-protocol mouse and its
   interrupt endpoint. Returns the interface number or -1. */
static int find_boot_mouse(const uint8_t *cfg, int total)
{
    int iface = -1;
    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {          /* an interface */
            /* class 3 = HID, subclass 1 = boot, protocol 2 = mouse */
            if (cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 2)
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
                    ep_maxpacket = 8;      /* the boot report is at most 8 */
                ep_interval = cfg[i + 6];
                return iface;
            }
        }
        i += dlen;
    }
    return -1;
}

static void handle_report(int n)
{
    /* Signed bytes, and they have to be read as signed: a movement of
       minus one arrives as 0xFF, and taken as 255 it would throw the
       pointer across the screen. */
    int dx = (int8_t)report[1];
    int dy = (int8_t)report[2];
    int notches = n >= 4 ? (int8_t)report[3] : 0;
    mouse_inject(dx, dy, report[0] & 0x07, notches);
}

static void poll_thread(void *arg)
{
    (void)arg;

    for (;;) {
        int n = usbhc_int_in(dev, ep_num, report, ep_maxpacket);
        if (n >= 3)
            handle_report(n);
        /* Faster than the keyboard's round, because a pointer that
           lags is a pointer nobody can aim: at fifty rounds a second
           the movement is already made of small steps, and any slower
           and it starts to jump. */
        task_sleep(10);
    }
}

int usbmouse_init(void)
{
    if (!usbhc_init())
        return 0;

    for (int port = 0; port < usbhc_ports(); port++) {
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

        int iface = find_boot_mouse(desc_buf, total);
        if (iface < 0)
            continue;                    /* something other than a mouse */

        uint8_t cfg_value = desc_buf[5];
        if (usbhc_request(dev, 0x00, 0x09, cfg_value, 0) < 0)
            continue;                    /* SET_CONFIGURATION */

        /* SET_IDLE 0 - say something only when something changed */
        usbhc_request(dev, 0x21, 0x0A, 0, (uint16_t)iface);
        /* SET_PROTOCOL 0 - the boot protocol, a fixed short report */
        usbhc_request(dev, 0x21, 0x0B, 0, (uint16_t)iface);

        if (usbhc_int_endpoint(dev, ep_num, ep_maxpacket, ep_interval) < 0)
            continue;

        usbhc_claim(port);
        found = 1;
        task_create("usbmouse", poll_thread, 0);
        printk("[ OK ] A USB mouse (HID, %s) on port %d, "
               "endpoint %d\n", usbhc_port_hc(port), port + 1, ep_num);
        return 1;
    }
    return 0;
}

int usbmouse_present(void)
{
    return found;
}
