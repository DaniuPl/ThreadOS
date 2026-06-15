/* Sterownik myszy PS/2 (IRQ12)
 *
 * Mysz wisi na drugim porcie kontrolera 8042 (urzadzenie pomocnicze).
 * Sterujemy nia przez te same porty co klawiatura: 0x60 (dane) i 0x64
 * (status/komendy), z prefiksem 0xD4 dla bajtow kierowanych do myszy.
 * Po wlaczeniu raportowania mysz przysyla 3-bajtowe pakiety na IRQ12:
 *   bajt0 - przyciski + bity znaku/przepelnienia,
 *   bajt1 - przesuniecie X (dx), bajt2 - przesuniecie Y (dy).
 *
 * Pozycje akumulujemy w zakresie 320x200 (gotowe pod tryb graficzny);
 * w trybie tekstowym pokazujemy ja w lewym gornym rogu ekranu.
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

/* Czeka, az bufor wejsciowy kontrolera bedzie pusty (mozna pisac) */
static void wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if ((inb(0x64) & 0x02) == 0)
            return;
}

/* Czeka, az pojawia sie dane do odczytu */
static void wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 0x01)
            return;
}

/* Wysyla bajt do samej myszy (prefiks 0xD4) */
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
    return inb(0x60);          /* mysz odpowiada 0xFA (ACK) */
}

/* Rysuje pozycje i przyciski myszy w lewym gornym rogu (tryb tekstowy) */
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
    /* Czytaj tylko, gdy dane pochodza od myszy (bit 5 statusu) */
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || !(status & 0x20))
        return;

    uint8_t data = inb(0x60);
    switch (cycle) {
    case 0:
        if (!(data & 0x08))    /* bit3 zawsze 1 - synchronizacja pakietu */
            return;            /* desync: pomijamy do nastepnego bajtu */
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

        if (packet[0] & 0xC0)  /* przepelnienie X/Y - odrzuc pakiet */
            break;

        int dx = packet[1];
        if (packet[0] & 0x10)
            dx -= 256;         /* znak X */
        int dy = packet[2];
        if (packet[0] & 0x20)
            dy -= 256;         /* znak Y */

        mouse_x += dx;
        mouse_y -= dy;         /* os Y ekranu rosnie w dol */
        int mw = video_w(), mh = video_h();   /* przycinaj do trybu */
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
    outb(0x64, 0xA8);          /* wlacz urzadzenie pomocnicze */

    /* Wlacz przerwanie IRQ12 w bajcie konfiguracyjnym kontrolera */
    wait_write();
    outb(0x64, 0x20);          /* odczytaj bajt konfiguracyjny */
    wait_read();
    uint8_t cfg = inb(0x60);
    cfg |= 0x02;               /* bit1: IRQ12 wlaczone */
    cfg &= ~0x20;              /* bit5: zegar myszy wlaczony */
    wait_write();
    outb(0x64, 0x60);          /* zapisz bajt konfiguracyjny */
    wait_write();
    outb(0x60, cfg);

    mouse_command(0xF6);       /* ustawienia domyslne */
    mouse_ack();
    mouse_command(0xF4);       /* wlacz raportowanie ruchu */
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
