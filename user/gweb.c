/* gweb - przegladarka w oknie pulpitu (ring 3)
 *
 * Wpisz adres w postaci host[:port][/sciezka] i nacisnij Enter: program
 * rozwiazuje nazwe przez DNS (albo bierze podany adres IP), laczy sie po
 * TCP, wysyla zadanie HTTP/1.0, obdziera odpowiedz z naglowkow i
 * znacznikow HTML, po czym rysuje tekst w oknie i zapisuje cala tresc
 * na dysk (plik 'strona.htm').
 *
 * Spina cztery warstwy naraz: klawiature i kompozytor pulpitu, TCP,
 * system plikow i procesy ring 3.
 */

#include "usys.h"
#include "uhttp.h"
#include "ucert.h"

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

static char status[64] = "Wpisz adres (nazwa albo IP) i nacisnij Enter";
static int last_secure;              /* czy ostatnia strona byla sprawdzona */

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof(status) - 1) {
        status[i] = s[i];
        i++;
    }
    status[i] = '\0';
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
        if (c == '&') {                  /* encja: &amp; &nbsp; &#39; ... */
            int j = i + 1, len = 0;
            char ent[10];
            while (j < page_len && page[j] != ';' && len < 9 &&
                   page[j] != ' ' && page[j] != '<')
                ent[len++] = page[j++];
            ent[len] = '\0';
            if (j < page_len && page[j] == ';') {
                i = j;                   /* zjedz cala encje */
                char rep = 0;
                if (ent[0] == 'a' && ent[1] == 'm')       rep = '&';
                else if (ent[0] == 'l' && ent[1] == 't')  rep = '<';
                else if (ent[0] == 'g' && ent[1] == 't')  rep = '>';
                else if (ent[0] == 'q')                   rep = '"';
                else if (ent[0] == '#')                   rep = '?';
                else if (ent[0] == 'n' && ent[1] == 'b')  rep = ' ';
                else                                      rep = '?';
                if (rep == ' ') {
                    if (!space) {
                        text[out++] = ' ';
                        space = 1;
                    }
                } else {
                    text[out++] = rep;
                    space = 0;
                }
                continue;
            }
        }
        if ((unsigned char)c >= 0x80) {   /* UTF-8: font ma tylko ASCII */
            /* bajty kontynuacji pomijamy, pierwszy zamieniamy na kropke */
            if (((unsigned char)c & 0xC0) != 0x80) {
                text[out++] = '.';
                space = 0;
            }
            continue;
        }
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

/* Odbiera tresc strumieniem do bufora strony */
static void page_sink(const char *data, uint32_t len, void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < len && page_len < PAGE_MAX - 1; i++)
        page[page_len++] = data[i];
}

static void fetch(void)
{
    char host[64];
    char path[96];
    uint32_t port = 80;
    int use_tls = 0;

    page_len = 0;
    text_len = 0;
    text[0] = '\0';

    if (!http_parse_url(addr, host, sizeof(host), &port, path, sizeof(path),
                        &use_tls)) {
        set_status("Zly adres");
        return;
    }

    static uint8_t roots[16384];
    static int roots_loaded;
    if (use_tls && !roots_loaded) {
        uint32_t n = cert_load_roots("roots.der", roots, sizeof(roots));
        http_set_roots(roots, n, 0);     /* pokaz stan, nie zrywaj polaczenia */
        roots_loaded = 1;
    }

    struct http_result res;
    res.status = 0;
    res.body_len = 0;

    for (int hop = 0; hop < 3; hop++) {
        set_status("Szukam adresu...");
        uint32_t ip = http_resolve(host);
        if (!ip) {
            set_status("Nie znam takiej nazwy");
            return;
        }

        set_status(use_tls ? "Uzgadniam TLS..." : "Lacze...");
        if (http_get(ip, port, host, path, use_tls, page_sink, 0, &res) < 0) {
            set_status("Nie udalo sie polaczyc");
            return;
        }

        int redirect = (res.status == 301 || res.status == 302 ||
                        res.status == 307 || res.status == 308);
        if (!redirect || !res.location[0])
            break;

        page_len = 0;                    /* strona przekierowania nas nie interesuje */
        if (!http_apply_location(res.location, host, sizeof(host), &port,
                                 path, sizeof(path), &use_tls))
            break;
        set_status("Przekierowanie...");
    }

    last_secure = use_tls && res.verified;

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
    int i = 0;
    /* Stan polaczenia widoczny w oknie: "TLS+" = certyfikat sprawdzony,
       "TLS?" = szyfrowane, ale bez potwierdzonej tozsamosci */
    const char *lead = use_tls ? (res.verified ? "TLS+ HTTP " : "TLS? HTTP ")
                               : "HTTP ";
    while (*lead)
        st[i++] = *lead++;
    st[i++] = (char)('0' + (res.status / 100) % 10);
    st[i++] = (char)('0' + (res.status / 10) % 10);
    st[i++] = (char)('0' + res.status % 10);
    const char *sep = res.chunked ? " chunked, " : ", ";
    while (*sep)
        st[i++] = *sep++;
    uint32_t v = (uint32_t)page_len;
    char digits[10];
    int d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (d)
        st[i++] = digits[--d];
    const char *end = " B -> strona.htm";
    while (*end)
        st[i++] = *end++;
    st[i] = '\0';
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
        win_text(w, 4, 17, status,
             text_len ? (last_secure ? C_GREEN : C_WHITE) : C_RED);
        draw_page(w);

        sys_sleep(60);
    }
}
