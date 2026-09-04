/* PS/2 mouse and laptop touchpad driver (IRQ12)
 *
 * The mouse hangs off the second port of the 8042 controller (the
 * auxiliary device). We drive it through the same ports as the
 * keyboard: 0x60 (data) and 0x64 (status/commands), with the 0xD4
 * prefix for bytes addressed to the mouse. Once reporting is turned
 * on, the mouse sends 3-byte packets on IRQ12:
 *   byte0 - the buttons plus sign/overflow bits,
 *   byte1 - the X movement (dx), byte2 - the Y movement (dy).
 *
 * A laptop's touchpad is this same device: in the mode it powers up
 * in, it reports relative movement and buttons exactly as a mouse
 * does, and the firmware turns a tap into a click. What it has and a
 * two-button mouse of 1987 did not is a wheel - which is how it
 * reports two fingers sliding. That is the IntelliMouse extension: a
 * fourth byte, asked for with a knock nobody would send by accident -
 * three sample rates, 200, 100 and 80 - after which the device says
 * who it is. Answer 3 means the fourth byte is there.
 *
 * We accumulate the position and clamp it to the mode; the wheel is
 * accumulated separately and taken away by whoever reads it, because
 * scrolling is a thing that HAPPENED, not a thing that IS.
 */

#include <stdint.h>
#include "include/mouse.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/vga.h"
#include "include/video.h"

#define SCREEN_W 320
#define SCREEN_H 200

static int mouse_x = SCREEN_W / 2;
static int mouse_y = SCREEN_H / 2;
static int mouse_buttons;

static int cycle;
static uint8_t packet[4];
static int wheel;              /* the device sends a fourth byte */
static int scroll_acc;         /* notches since somebody last asked */

/* Waits until the controller's input buffer is empty (writing is possible) */
static void wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & 0x02) == 0)
            return;
}

/* Waits until there is data to read */
static void wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01)
            return;
}

/* Sends a byte to the mouse itself (the 0xD4 prefix) */
static void mouse_command(uint8_t cmd)
{
    wait_write();
    outb(0x64, 0xD4);
    wait_write();
    outb(0x60, cmd);
}

static uint8_t mouse_ack(void)
{
    wait_read();
    return inb(0x60);          /* the mouse answers 0xFA (ACK) */
}

/* Draws the mouse position and buttons in the top left corner.
 *
 * A boot-time diagnostic, and only that: vga_write_at does nothing
 * once the compositor has taken the screen, which is what keeps this
 * from drawing on the desktop from inside an interrupt. */
static void draw_status(void)
{
    char buf[20];
    int i = 0;
    const char *lab = "M ";
    while (*lab)
        buf[i++] = *lab++;

    int vals[2] = { mouse_x, mouse_y };
    for (int v = 0; v < 2; v++) {
        int n = vals[v];
        char tmp[6];
        int t = 0;
        do {
            tmp[t++] = (char)('0' + n % 10);
            n /= 10;
        } while (n);
        while (t)
            buf[i++] = tmp[--t];
        buf[i++] = v == 0 ? ',' : ' ';
    }
    buf[i++] = 'b';
    buf[i++] = (char)('0' + mouse_buttons);
    buf[i] = '\0';
    vga_write_at(0, 0, buf);
}

/* A whole packet has arrived: turn it into a position, buttons and
   whatever the wheel did. */
static void packet_done(void)
{
    if (packet[0] & 0xC0)      /* an X/Y overflow - drop the packet */
        return;

    int dx = packet[1];
    if (packet[0] & 0x10)
        dx -= 256;             /* the X sign */
    int dy = packet[2];
    if (packet[0] & 0x20)
        dy -= 256;             /* the Y sign */

    mouse_x += dx;
    mouse_y -= dy;             /* the screen's Y axis grows downwards */
    int mw = video_w(), mh = video_h();   /* clamp to the mode */
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= mw) mouse_x = mw - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= mh) mouse_y = mh - 1;
    mouse_buttons = packet[0] & 0x07;

    /* The wheel: four bits of signed movement in the low half of the
       fourth byte, the rest being the two extra buttons on mice that
       have them. Two fingers sliding down a touchpad arrive here as a
       run of these.
       The sign is the protocol's, and it is the other way round from
       the one everything above wants: the device counts a notch
       TOWARDS the person as positive, and the rest of this machine
       calls away-from-the-person positive, because that is the
       direction a document moves. Turning it here means nobody above
       has to remember it. */
    if (wheel) {
        int z = packet[3] & 0x0F;
        if (z & 0x08)
            z -= 16;
        scroll_acc -= z;
    }

    draw_status();
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    /* Read only when the data comes from the mouse (status bit 5) */
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20))
        return;

    uint8_t data = inb(0x60);
    if (cycle == 0 && !(data & 0x08))
        return;                /* bit3 is always 1: out of sync, skip it */

    packet[cycle++] = data;
    int len = wheel ? 4 : 3;
    if (cycle < len)
        return;
    cycle = 0;
    packet_done();
}

void mouse_init(void)
{
    wait_write();
    outb(0x64, 0xA8);          /* enable the auxiliary device */

    /* Enable IRQ12 in the controller's configuration byte */
    wait_write();
    outb(0x64, 0x20);          /* read the configuration byte */
    wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x02;               /* bit1: IRQ12 enabled */
    cfg &= ~0x20;              /* bit5: the mouse clock enabled */
    wait_write();
    outb(0x64, 0x60);          /* write the configuration byte */
    wait_write();
    outb(0x60, cfg);

    mouse_command(0xF6);       /* default settings */
    mouse_ack();

    /* The knock that asks for a wheel: three sample rates in an order
       no ordinary setup would use. A device that understands it starts
       calling itself 3 and sending four bytes a packet; one that does
       not goes on calling itself 0 and we leave it at three. */
    static const uint8_t knock[3] = { 200, 100, 80 };
    for (int i = 0; i < 3; i++) {
        mouse_command(0xF3);   /* set sample rate */
        mouse_ack();
        mouse_command(knock[i]);
        mouse_ack();
    }
    mouse_command(0xF2);       /* who are you */
    mouse_ack();
    wait_read();
    wheel = inb(0x60) == 3;

    mouse_command(0xF4);       /* enable movement reporting */
    mouse_ack();

    irq_install_handler(12, mouse_irq);
    pic_unmask(12);
}

void mouse_get(int *x, int *y, int *buttons)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (buttons) *buttons = mouse_buttons;
}

/* How far the wheel has turned since this was last asked, and then
   nothing until it turns again. Positive is away from the person -
   which on a screen means up. Reading takes the value away: two
   readers would each get half of the scrolling, and one reader who
   forgot to ask would find a year of it waiting. */
int mouse_take_scroll(void)
{
    int n = scroll_acc;
    scroll_acc = 0;
    return n;
}

int mouse_has_wheel(void)
{
    return wheel;
}

/* Movement from somewhere other than the PS/2 port.
 *
 * A laptop without an 8042 controller - and there are more of those
 * every year - has no PS/2 anything: its pointer arrives over USB, and
 * so does the pointer of anyone who plugs a mouse into a dock. There
 * is still only ONE pointer on the screen, so the second road ends
 * here rather than in a second position that the compositor would have
 * to choose between.
 *
 * The wheel's sign is the one this file uses everywhere: away from the
 * person is positive. USB HID counts it the same way, so nothing is
 * turned round here - unlike the PS/2 packet, which does not. */
void mouse_inject(int dx, int dy, int buttons, int wheel_notches)
{
    mouse_x += dx;
    mouse_y += dy;             /* HID's Y already grows downwards */
    int mw = video_w(), mh = video_h();
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= mw) mouse_x = mw - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= mh) mouse_y = mh - 1;
    mouse_buttons = buttons & 0x07;
    scroll_acc += wheel_notches;
    draw_status();
}
