/* Klawiatura USB (HID, protokol rozruchowy)
 *
 * Rozpoznanie urzadzenia przebiega tak, jak w kazdym systemie: reset
 * portu, pytanie o deskryptor urzadzenia pod adresem zerowym, nadanie
 * adresu, pytanie o konfiguracje i odszukanie w niej interfejsu klasy
 * HID w wersji rozruchowej. Potem przelaczamy urzadzenie w protokol
 * rozruchowy - ten, ktorego uzywa BIOS - i od tej pory dostajemy staly
 * raport osmiu bajtow: modyfikatory i do szesciu wcisnietych klawiszy.
 *
 * Raport mowi, co JEST wcisniete, a nie co sie wlasnie zmienilo -
 * porownujemy go wiec z poprzednim i zglaszamy tylko nowe klawisze.
 *
 * Odpytujemy z watku, kilkadziesiat razy na sekunde. Klawiatura i tak
 * nie ma nic pilniejszego do powiedzenia, a brak przerwan oznacza jedna
 * droge mniej, ktora moze cos pojsc nie tak.
 */

#include <stdint.h>
#include "include/usbkbd.h"
#include "include/uhci.h"
#include "include/keyboard.h"
#include "include/task.h"
#include "include/printk.h"
#include "include/string.h"

/* Bufory sa statyczne, bo kontroler siega po nie sam - musza lezec
   w pamieci ciaglej i pod adresem, ktory rozumie wprost */
static uint8_t desc_buf[256] __attribute__((aligned(16)));
static uint8_t report[8] __attribute__((aligned(16)));

static int dev_addr;
static int dev_lowspeed;
static int dev_maxpacket = 8;
static int ep_num, ep_maxpacket;
static int toggle;

static void setup_packet(uint8_t *p, uint8_t type, uint8_t req,
                         uint16_t value, uint16_t index, uint16_t len)
{
    p[0] = type;
    p[1] = req;
    p[2] = (uint8_t)value;
    p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index;
    p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)len;
    p[7] = (uint8_t)(len >> 8);
}

static int get_descriptor(int addr, uint8_t type, uint8_t index,
                          void *buf, int len)
{
    uint8_t setup[8];
    setup_packet(setup, 0x80, 0x06, (uint16_t)(type << 8 | index), 0,
                 (uint16_t)len);
    return uhci_control(addr, dev_lowspeed, dev_maxpacket, setup, buf, len, 1);
}

static int no_data_request(int addr, uint8_t type, uint8_t req,
                           uint16_t value, uint16_t index)
{
    uint8_t setup[8];
    setup_packet(setup, type, req, value, index, 0);
    return uhci_control(addr, dev_lowspeed, dev_maxpacket, setup,
                        (void *)0, 0, 0);
}

/* Szuka w konfiguracji interfejsu HID w wersji rozruchowej i jego
   punktu koncowego przerwaniowego. Zwraca numer interfejsu albo -1. */
static int find_boot_keyboard(const uint8_t *cfg, int total)
{
    int iface = -1;
    for (int i = 0; i + 1 < total;) {
        int dlen = cfg[i];
        uint8_t dtype = cfg[i + 1];
        if (dlen < 2)
            break;

        if (dtype == 0x04 && i + 8 < total) {          /* interfejs */
            /* klasa 3 = HID, podklasa 1 = rozruchowa, protokol 1 = klawiatura */
            if (cfg[i + 5] == 3 && cfg[i + 6] == 1 && cfg[i + 7] == 1)
                iface = cfg[i + 2];
            else
                iface = -1;
        } else if (dtype == 0x05 && iface >= 0 && i + 6 < total) {
            uint8_t addr = cfg[i + 2];
            uint8_t attr = cfg[i + 3];
            if ((addr & 0x80) && (attr & 0x03) == 0x03) {  /* IN, przerwaniowy */
                ep_num = addr & 0x0F;
                ep_maxpacket = cfg[i + 4] | (cfg[i + 5] << 8);
                if (ep_maxpacket > 8)
                    ep_maxpacket = 8;                  /* raport ma 8 bajtow */
                return iface;
            }
        }
        i += dlen;
    }
    return -1;
}

/* Kod uzycia HID -> nasz znak. Tablica obejmuje to, co ma znaczenie
   na klawiaturze rozruchowej; reszta jest ignorowana. */
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

/* Klawisze bez odpowiednika w ASCII - te same kody, co przy PS/2 */
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
    int shift = (mods & 0x22) != 0;      /* lewy albo prawy Shift */
    int ctrl  = (mods & 0x11) != 0;
    int alt   = (mods & 0x44) != 0;

    /* Alt+F1..F4 przelacza konsole - tak samo jak z PS/2 */
    if (alt && usage >= 0x3A && usage < 0x3A + 4) {
        keyboard_inject_switch(usage - 0x3A);
        return;
    }

    char c = usage < 0x40 ? (shift ? hid_map_shift[usage] : hid_map[usage])
                          : special_key(usage);
    if (!c)
        return;
    if (ctrl && c == 'c')
        c = KEY_INTR;
    keyboard_inject(c);
}

static uint8_t prev[6];

static void handle_report(void)
{
    uint8_t mods = report[0];
    /* Raport mowi, co jest wcisniete TERAZ - nowy klawisz to taki,
       ktorego nie bylo w poprzednim */
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
        /* Klawiatura kolejkuje raporty i oddaje po jednym na zapytanie.
           Jedno zapytanie na obieg gubiloby klawisze przy szybkim
           pisaniu - a przy przepelnieniu kolejki urzadzenie po prostu
           wyrzuca to, czego nie zdazylismy odebrac. Opozniamy sie wiec
           dopiero wtedy, gdy naprawde nie ma juz nic do wziecia. */
        /* Klawiatura kolejkuje raporty i oddaje po jednym na zapytanie.
           Przy szybkim pisaniu jedno zapytanie na obieg to za malo -
           czytamy, dopoki cos jest, i dopiero potem idziemy spac.

           To wlasnie drenaz kolejki radzi sobie z szybkim pisaniem,
           a nie czestsze zagladanie: odpytywanie co 10 ms zamiast co 20
           przestaje dzialac zupelnie - watek nie schodzi wtedy z drogi
           kontrolerowi na tyle dlugo, by ten zdazyl przerobic ramke. */
        for (int i = 0; i < 8; i++) {
            int n = uhci_interrupt_in(dev_addr, dev_lowspeed, ep_num,
                                      ep_maxpacket, report, ep_maxpacket,
                                      &toggle);
            if (n < 8)
                break;
            handle_report();
        }
        task_sleep(20);
    }
}

int usbkbd_init(void)
{
    if (!uhci_init())
        return 0;

    for (int port = 0; port < uhci_ports(); port++) {
        if (!uhci_port_reset(port))
            continue;

        dev_lowspeed = uhci_port_lowspeed(port);
        dev_maxpacket = 8;
        dev_addr = 0;

        /* Pierwsze osiem bajtow deskryptora wystarczy, zeby poznac
           rozmiar pakietu punktu zerowego - reszty jeszcze nie umiemy
           poprawnie odebrac */
        if (get_descriptor(0, 0x01, 0, desc_buf, 8) < 8)
            continue;
        if (desc_buf[7])
            dev_maxpacket = desc_buf[7];

        /* Kazdy port dostaje inny adres: gdyby na pierwszym wisialo
           cos, co klawiatura nie jest, drugi nie moze po nim odziedziczyc
           tego samego numeru */
        int addr = port + 1;
        if (no_data_request(0, 0x00, 0x05, (uint16_t)addr, 0) < 0)
            continue;
        dev_addr = addr;
        task_sleep(20);

        if (get_descriptor(dev_addr, 0x02, 0, desc_buf, 9) < 9)
            continue;
        int total = desc_buf[2] | (desc_buf[3] << 8);
        if (total > (int)sizeof(desc_buf))
            total = (int)sizeof(desc_buf);
        if (get_descriptor(dev_addr, 0x02, 0, desc_buf, total) < total)
            continue;

        int iface = find_boot_keyboard(desc_buf, total);
        if (iface < 0)
            continue;                    /* cos innego niz klawiatura */

        uint8_t cfg_value = desc_buf[5];
        if (no_data_request(dev_addr, 0x00, 0x09, cfg_value, 0) < 0)
            continue;                    /* SET_CONFIGURATION */

        /* SET_IDLE 0 - zglaszaj tylko zmiany, nie powtarzaj w kolko */
        no_data_request(dev_addr, 0x21, 0x0A, 0, (uint16_t)iface);
        /* SET_PROTOCOL 0 - protokol rozruchowy, staly raport 8 bajtow */
        no_data_request(dev_addr, 0x21, 0x0B, 0, (uint16_t)iface);

        toggle = 0;
        task_create("usbkbd", poll_thread, 0);
        printk("[ OK ] Klawiatura USB (HID) na porcie %d, punkt koncowy %d\n",
               port + 1, ep_num);
        return 1;
    }
    return 0;
}
