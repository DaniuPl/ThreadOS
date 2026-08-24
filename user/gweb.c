/* gweb - przegladarka w oknie pulpitu (ring 3)
 *
 * Wpisz adres w postaci ip[:port][/sciezka] i nacisnij Enter: program
 * laczy sie po TCP, wysyla zadanie HTTP/1.0, obdziera odpowiedz z
 * naglowkow i znacznikow HTML, po czym rysuje tekst w oknie i zapisuje
 * cala tresc na dysk (plik 'strona.htm').
 *
 * Spina cztery warstwy naraz: klawiature i kompozytor pulpitu, TCP,
 * system plikow i procesy ring 3.
 */

#include "usys.h"

#define W 320
#define H 200
#define C_BG     8
#define C_WHITE  4
#define C_GREEN  2
#define C_RED    1
#define C_TITLE  3

#define ADDR_MAX  60
#define PAGE_MAX  3072
#define COLS      38          /* znakow w wierszu przy czcionce 8x8 */
#define ROWS      16

static char addr[ADDR_MAX] = "10.0.2.2:8000/";
static int addr_len = 14;

static char page[PAGE_MAX];   /* surowa odpowiedz (bez naglowkow) */
static int page_len;

static char text[PAGE_MAX];   /* tresc po odarciu ze znacznikow */
static int text_len;

static char status[64] = "Wpisz adres i nacisnij Enter";

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof(status) - 1) {
        status[i] = s[i];
        i++;
    }
    status[i] = '\0';
}

/* Rozbija "ip[:port][/sciezka]" na czesci. 0 = zly adres. */
static int parse_addr(uint32_t *ip, uint32_t *port, char *path, int path_max)
{
    char host[32];
    int h = 0, i = 0;

    while (addr[i] && addr[i] != ':' && addr[i] != '/' &&
           h < (int)sizeof(host) - 1)
        host[h++] = addr[i++];
    host[h] = '\0';

    *port = 80;
    if (addr[i] == ':') {
        i++;
        uint32_t p = 0;
        while (addr[i] >= '0' && addr[i] <= '9')
            p = p * 10 + (uint32_t)(addr[i++] - '0');
        *port = p;
    }

    int t = 0;
    if (addr[i] == '/')
        while (addr[i] && t < path_max - 1)
            path[t++] = addr[i++];
    if (!t)
        path[t++] = '/';
    path[t] = '\0';

    *ip = uparse_ip(host);
    return *ip && *port ? 1 : 0;
}

/* Czy w miejscu i zaczyna sie znacznik o podanej nazwie? */
static int tag_is(int i, const char *name)
{
    int k = 0;
    while (name[k]) {
        char c = page[i + k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != name[k])
            return 0;
        k++;
    }
    char after = page[i + k];
    return after == '>' || after == ' ' || after == '\n' || after == '\r';
}

/* Wycina znaczniki HTML i sklada biale znaki - prosty czytnik tekstu.
   Zawartosc <title>, <script> i <style> pomijamy: to nie jest tresc
   strony, tylko jej metryczka i kod. */
static void strip_html(void)
{
    int out = 0, in_tag = 0, space = 1, skip = 0;

    for (int i = 0; i < page_len && out < PAGE_MAX - 1; i++) {
        char c = page[i];
        if (c == '<') {
            in_tag = 1;
            if (tag_is(i + 1, "title") || tag_is(i + 1, "script") ||
                tag_is(i + 1, "style"))
                skip = 1;
            else if (tag_is(i + 2, "title") || tag_is(i + 2, "script") ||
                     tag_is(i + 2, "style"))
                skip = 0;                 /* znacznik zamykajacy </...> */
            continue;
        }
        if (c == '>') {
            in_tag = 0;
            continue;
        }
        if (in_tag || skip)
            continue;
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!space) {
                text[out++] = ' ';
                space = 1;
            }
            continue;
        }
        text[out++] = c;
        space = 0;
    }
    text[out] = '\0';
    text_len = out;
}

static void fetch(void)
{
    uint32_t ip, port;
    char path[64];

    page_len = 0;
    text_len = 0;
    text[0] = '\0';

    if (!parse_addr(&ip, &port, path, sizeof(path))) {
        set_status("Zly adres (uzyj ip[:port][/sciezka])");
        return;
    }

    set_status("Lacze...");
    int32_t conn = tcp_connect(ip, port, 5000);
    if (conn < 0) {
        set_status("Nie udalo sie polaczyc");
        return;
    }

    static char req[192];
    int r = 0;
    const char *p1 = "GET ";
    while (*p1)
        req[r++] = *p1++;
    for (int i = 0; path[i]; i++)
        req[r++] = path[i];
    const char *p2 = " HTTP/1.0\r\nHost: protonos\r\nConnection: close\r\n\r\n";
    while (*p2)
        req[r++] = *p2++;

    if (tcp_send(conn, req, (uint32_t)r, 5000) < 0) {
        tcp_close(conn);
        set_status("Blad wysylania zadania");
        return;
    }

    static char chunk[512];
    int header_done = 0;
    int status_code = 0;

    for (;;) {
        int32_t n = tcp_recv(conn, chunk, sizeof(chunk), 8000);
        if (n <= 0)
            break;                        /* 0 = koniec, -1 = timeout */

        int start = 0;
        if (!header_done) {
            if (!status_code && n > 12) {  /* "HTTP/1.x 200 ..." */
                status_code = (chunk[9] - '0') * 100 + (chunk[10] - '0') * 10 +
                              (chunk[11] - '0');
            }
            for (int i = 0; i + 3 < n; i++) {
                if (chunk[i] == '\r' && chunk[i + 1] == '\n' &&
                    chunk[i + 2] == '\r' && chunk[i + 3] == '\n') {
                    start = i + 4;
                    header_done = 1;
                    break;
                }
            }
            if (!header_done)
                continue;
        }
        for (int i = start; i < n && page_len < PAGE_MAX - 1; i++)
            page[page_len++] = chunk[i];
    }
    tcp_close(conn);
    page[page_len] = '\0';

    if (!page_len) {
        set_status("Pusta odpowiedz");
        return;
    }

    /* Tresc trafia takze na dysk - siec i system plikow w jednym kroku */
    int fd = sys_open("strona.htm", O_CREATE | O_WRITE);
    if (fd >= 0) {
        sys_write(fd, page, (uint32_t)page_len);
        sys_close(fd);
    }

    strip_html();

    char st[64];
    int s = 0;
    const char *lead = "HTTP ";
    while (*lead)
        st[s++] = *lead++;
    st[s++] = (char)('0' + (status_code / 100) % 10);
    st[s++] = (char)('0' + (status_code / 10) % 10);
    st[s++] = (char)('0' + status_code % 10);
    const char *tail = ", ";
    while (*tail)
        st[s++] = *tail++;
    uint32_t v = (uint32_t)page_len;
    char digits[10];
    int d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (d)
        st[s++] = digits[--d];
    const char *end = " B -> strona.htm";
    while (*end)
        st[s++] = *end++;
    st[s] = '\0';
    set_status(st);
}

/* Rysuje tresc zawijana na granicach slow (lamanie w srodku wyrazu
   robi sie nieczytelne przy czcionce 8x8) */
static void draw_page(int w)
{
    char linebuf[COLS + 1];
    int col = 0, row = 0, y = 30, i = 0;

    while (i < text_len && row < ROWS) {
        /* dlugosc najblizszego slowa */
        int wlen = 0;
        while (i + wlen < text_len && text[i + wlen] != ' ')
            wlen++;

        if (wlen > COLS) {               /* slowo dluzsze niz wiersz */
            wlen = COLS - col;
        } else if (col && col + 1 + wlen > COLS) {
            linebuf[col] = '\0';         /* nie miesci sie - nowy wiersz */
            win_text(w, 4, y, linebuf, C_WHITE);
            y += 10;
            row++;
            col = 0;
            continue;
        }

        if (col)
            linebuf[col++] = ' ';
        for (int k = 0; k < wlen && col < COLS; k++)
            linebuf[col++] = text[i + k];
        i += wlen;
        while (i < text_len && text[i] == ' ')
            i++;
    }

    if (col && row < ROWS) {
        linebuf[col] = '\0';
        win_text(w, 4, y, linebuf, C_WHITE);
    }
}

void _start(void)
{
    int w = win_create(W, H, "Przegladarka");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type != WEV_KEY)
                continue;
            char c = (char)ev.val;
            if (c == '\n') {
                fetch();
            } else if (c == '\b') {
                if (addr_len > 0)
                    addr_len--;
            } else if (c >= 0x20 && c < 0x7F && addr_len < ADDR_MAX - 1) {
                addr[addr_len++] = c;
            }
            addr[addr_len] = '\0';
        }

        win_rect(w, 0, 0, W, H, C_BG);
        win_rect(w, 0, 0, W, 14, C_TITLE);
        addr[addr_len] = '\0';
        win_text(w, 4, 3, addr, C_WHITE);
        win_text(w, 4, 17, status, text_len ? C_GREEN : C_RED);
        draw_page(w);

        sys_sleep(60);
    }
}
