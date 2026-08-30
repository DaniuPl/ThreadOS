/* PS/2 mouse driver (IRQ12)
 *
 * The mouse hangs off the second port of the 8042 controller (the
 * auxiliary device). We drive it through the same ports as the
 * keyboard: 0x60 (data) and 0x64 (status/commands), with the 0xD4
 * prefix for bytes addressed to the mouse. Once reporting is turned
 * on, the mouse sends 3-byte packets on IRQ12:
 *   byte0 - the buttons plus sign/overflow bits,
 *   byte1 - the X movement (dx), byte2 - the Y movement (dy).
 *
 * We accumulate the position within a 320x200 range (ready for
 * graphics mode); in text mode we show it in the top left corner of
 * the screen.
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
static uint8_t packet[3];

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

/* Draws the mouse position and buttons in the top left corner (text mode) */
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

static void mouse_irq(struct regs *r)
{
    (void)r;
    /* Read only when the data comes from the mouse (status bit 5) */
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20))
        return;

    uint8_t data = inb(0x60);
    switch (cycle) {
    case 0:
        if (!(data & 0x08))    /* bit3 is always 1 - packet synchronisation */
            return;            /* out of sync: skip to the next byte */
        packet[0] = data;
        cycle = 1;
        break;
    case 1:
        packet[1] = data;
        cycle = 2;
        break;
    case 2:
        packet[2] = data;
        cycle = 0;

        if (packet[0] & 0xC0)  /* an X/Y overflow - drop the packet */
            break;

        int dx = packet[1];
        if (packet[0] & 0x10)
            dx -= 256;         /* the X sign */
        int dy = packet[2];
        if (packet[0] & 0x20)
            dy -= 256;         /* the Y sign */

        mouse_x += dx;
        mouse_y -= dy;         /* the screen's Y axis grows downwards */
        int mw = video_w(), mh = video_h();   /* clamp to the mode */
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= mw) mouse_x = mw - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= mh) mouse_y = mh - 1;
        mouse_buttons = packet[0] & 0x07;

        draw_status();
        break;
    }
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
