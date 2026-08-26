/* gweb - przegladarka w oknie pulpitu (ring 3)
 *
 * Wpisz adres w postaci host[:port][/sciezka] (albo https://...) i
 * nacisnij Enter: program rozwiazuje nazwe przez DNS, laczy sie po TCP
 * (dla https przez TLS 1.3 ze sprawdzeniem certyfikatu), obdziera
 * odpowiedz z naglowkow i znacznikow HTML, po czym rysuje tekst w oknie
 * i zapisuje cala tresc na dysk (plik 'strona.htm').
 *
 * Strona da sie przewijac (strzalki, PgUp/PgDn, Home), a odnosniki sa
 * klikalne - klikniecie idzie za adresem z <a href>.
 *
 * Spina wszystkie warstwy naraz: klawiature i mysz z kompozytora
 * pulpitu, DNS, TCP, TLS, system plikow i procesy ring 3.
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
#define C_LINK   5            /* odnosniki - inny kolor niz reszta tekstu */

#define ADDR_MAX  80
#define PAGE_MAX  8192
#define COLS      38          /* znakow w wierszu przy czcionce 8x8 */
#define ROWS      15
#define ROW_H     10
#define PAGE_Y    30          /* pierwszy wiersz tresci w oknie */
#define LINES_MAX 400
#define LINKS_MAX 48

/* --- stan --- */

static char addr[ADDR_MAX] = "protonos.test/";
static int addr_len = 14;

static char cur_host[64] = "protonos.test";
static uint32_t cur_port = 80;
static char cur_path[96] = "/";
static int cur_tls;

static char page[PAGE_MAX];   /* surowa odpowiedz (bez naglowkow) */
static int page_len;

static char text[PAGE_MAX];   /* tresc po odarciu ze znacznikow */
static int text_len;

struct link {
    int start, end;           /* zakres w text[] */
    char href[96];
};
static struct link links[LINKS_MAX];
static int link_count;

struct line {
    int start, len;           /* zakres w text[] */
};
static struct line lines[LINES_MAX];
static int line_count;
static int scroll;

static char status[64] = "Wpisz adres i nacisnij Enter";
static int last_secure;

static void set_status(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof(status) - 1) {
        status[i] = s[i];
        i++;
    }
    status[i] = '\0';
}

/* --- rozbior HTML --- */

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

/* Czy w miejscu i stoi nazwa atrybutu (po niej moze byc '=' albo spacja)? */
static int attr_is(int i, const char *name)
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
    return after == '=' || after == ' ';
}

/* Wyciaga wartosc href="..." z wnetrza znacznika <a ...> */
static void grab_href(int i, char *out, int max)
{
    out[0] = '\0';
    while (i < page_len && page[i] != '>') {
        if ((page[i] == 'h' || page[i] == 'H') && attr_is(i, "href")) {
            int j = i + 4;
            while (j < page_len && (page[j] == ' ' || page[j] == '='))
                j++;
            char quote = 0;
            if (j < page_len && (page[j] == '"' || page[j] == '\'')) {
                quote = page[j];
                j++;
            }
            int n = 0;
            while (j < page_len && n < max - 1) {
                char c = page[j];
                if (quote ? c == quote : (c == ' ' || c == '>'))
                    break;
                out[n++] = c;
                j++;
            }
            out[n] = '\0';
            return;
        }
        i++;
    }
}

/* Znaczniki blokowe rozdzielaja tekst - bez tego "<h1>Tytul</h1><p>Tresc"
   skleilby sie w "TytulTresc" */
static int block_tag(int i)
{
    static const char *tags[] = {
        "p", "br", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul",
        "ol", "tr", "td", "th", "table", "section", "article", "header",
        "footer", "nav", "blockquote", "pre", "hr", 0
    };
    if (page[i] == '/')
        i++;
    for (int k = 0; tags[k]; k++)
        if (tag_is(i, tags[k]))
            return 1;
    return 0;
}

/* Wycina znaczniki, sklada biale znaki, rozwija encje i zapamietuje
   zakresy odnosnikow. Zawartosc <title>, <script> i <style> pomijamy. */
static void strip_html(void)
{
    int out = 0, in_tag = 0, space = 1, skip = 0;
    int open_link = -1;

    link_count = 0;

    for (int i = 0; i < page_len && out < PAGE_MAX - 1; i++) {
        char c = page[i];

        if (c == '<') {
            in_tag = 1;
            if (!skip && block_tag(i + 1) && !space && out < PAGE_MAX - 1) {
                text[out++] = ' ';
                space = 1;
            }
            if (tag_is(i + 1, "title") || tag_is(i + 1, "script") ||
                tag_is(i + 1, "style"))
                skip = 1;
            else if (tag_is(i + 2, "title") || tag_is(i + 2, "script") ||
                     tag_is(i + 2, "style"))
                skip = 0;
            else if (tag_is(i + 1, "a") && link_count < LINKS_MAX) {
                grab_href(i + 2, links[link_count].href,
                          (int)sizeof(links[0].href));
                if (links[link_count].href[0]) {
                    links[link_count].start = out;
                    open_link = link_count;
                }
            } else if (tag_is(i + 2, "a") && open_link >= 0) {
                links[open_link].end = out;
                if (links[open_link].end > links[open_link].start)
                    link_count++;
                open_link = -1;
            }
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
                i = j;
                char rep;
                if (ent[0] == 'a' && ent[1] == 'm')       rep = '&';
                else if (ent[0] == 'l' && ent[1] == 't')  rep = '<';
                else if (ent[0] == 'g' && ent[1] == 't')  rep = '>';
                else if (ent[0] == 'q')                   rep = '"';
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
        if ((unsigned char)c >= 0x80) {  /* UTF-8: czcionka zna tylko ASCII */
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

/* --- lamanie tekstu na wiersze (na granicy slow) --- */

static void layout(void)
{
    line_count = 0;
    scroll = 0;

    int i = 0;
    while (i < text_len && line_count < LINES_MAX) {
        int start = i, col = 0, last_space = -1;

        while (i < text_len && col < COLS) {
            if (text[i] == ' ')
                last_space = i;
            i++;
            col++;
        }
        int end = i;
        if (i < text_len && text[i] != ' ' && last_space > start)
            end = last_space;            /* zawin na granicy slowa */

        lines[line_count].start = start;
        lines[line_count].len = end - start;
        line_count++;

        i = end;
        while (i < text_len && text[i] == ' ')
            i++;                         /* zjedz spacje na zawinieciu */
    }
}

/* Do ktorego odnosnika nalezy znak o indeksie i (-1 = do zadnego)? */
static int link_at(int i)
{
    for (int k = 0; k < link_count; k++)
        if (i >= links[k].start && i < links[k].end)
            return k;
    return -1;
}

/* --- pobieranie --- */

static void page_sink(const char *data, uint32_t len, void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < len && page_len < PAGE_MAX - 1; i++)
        page[page_len++] = data[i];
}

/* Odswieza pasek adresu z biezacego celu */
static void rebuild_addr(void)
{
    int n = 0;
    const char *scheme = cur_tls ? "https://" : "";
    while (*scheme && n < ADDR_MAX - 1)
        addr[n++] = *scheme++;
    for (int i = 0; cur_host[i] && n < ADDR_MAX - 1; i++)
        addr[n++] = cur_host[i];
    if ((cur_tls && cur_port != 443) || (!cur_tls && cur_port != 80)) {
        if (n < ADDR_MAX - 1)
            addr[n++] = ':';
        uint32_t v = cur_port;
        char d[8];
        int k = 0;
        do {
            d[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (k && n < ADDR_MAX - 1)
            addr[n++] = d[--k];
    }
    for (int i = 0; cur_path[i] && n < ADDR_MAX - 1; i++)
        addr[n++] = cur_path[i];
    addr[n] = '\0';
    addr_len = n;
}

static void fetch(void)
{
    static uint8_t roots[16384];
    static int roots_loaded;

    if (cur_tls && !roots_loaded) {
        /* Wbudowane korzenie publicznych CA + ewentualne wlasne z dysku.
           Certyfikat, ktory sie nie obroni, konczy polaczenie - tak jak
           w prawdziwej przegladarce. */
        uint32_t n = cert_load_roots("roots.der", roots, sizeof(roots));
        http_set_roots(roots, n, 1);
        roots_loaded = 1;
    }

    page_len = 0;
    text_len = 0;
    text[0] = '\0';
    line_count = 0;

    struct http_result res;
    res.status = 0;
    res.body_len = 0;
    res.verified = 0;
    res.verify_err = 0;

    for (int hop = 0; hop < 3; hop++) {
        set_status("Szukam adresu...");
        uint32_t ip = http_resolve(cur_host);
        if (!ip) {
            set_status("Nie znam takiej nazwy");
            return;
        }

        set_status("Lacze...");
        if (http_get(ip, cur_port, cur_host, cur_path, cur_tls, page_sink, 0,
                     &res) < 0) {
            if (cur_tls && res.verify_err) {
                char msg[64];
                int k = 0;
                const char *pre = "Certyfikat odrzucony: ";
                while (*pre && k < (int)sizeof(msg) - 1)
                    msg[k++] = *pre++;
                const char *why = cert_error_text(res.verify_err);
                while (*why && k < (int)sizeof(msg) - 1)
                    msg[k++] = *why++;
                msg[k] = '\0';
                set_status(msg);
            } else {
                set_status("Nie udalo sie polaczyc");
            }
            return;
        }

        int redirect = (res.status == 301 || res.status == 302 ||
                        res.status == 307 || res.status == 308);
        if (!redirect || !res.location[0])
            break;

        page_len = 0;
        if (!http_apply_location(res.location, cur_host, sizeof(cur_host),
                                 &cur_port, cur_path, sizeof(cur_path),
                                 &cur_tls))
            break;
        rebuild_addr();
        set_status("Przekierowanie...");
    }

    last_secure = cur_tls && res.verified;
    page[page_len] = '\0';

    if (!page_len) {
        set_status("Pusta odpowiedz");
        return;
    }

    int fd = sys_open("strona.htm", O_CREATE | O_WRITE);
    if (fd >= 0) {
        sys_write(fd, page, (uint32_t)page_len);
        sys_close(fd);
    }

    strip_html();
    layout();

    /* Pasek stanu: "TLS+" = certyfikat sprawdzony, "TLS?" = tylko szyfr */
    char st[64];
    int i = 0;
    const char *lead = cur_tls ? (res.verified ? "TLS+ HTTP " : "TLS? HTTP ")
                               : "HTTP ";
    while (*lead)
        st[i++] = *lead++;
    st[i++] = (char)('0' + (res.status / 100) % 10);
    st[i++] = (char)('0' + (res.status / 10) % 10);
    st[i++] = (char)('0' + res.status % 10);
    const char *sep = ", ";
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
    const char *tail = " B, odnosnikow ";
    while (*tail)
        st[i++] = *tail++;
    v = (uint32_t)link_count;
    d = 0;
    do {
        digits[d++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (d)
        st[i++] = digits[--d];
    st[i] = '\0';
    set_status(st);
}

/* Ustawia nowy cel z paska adresu i pobiera strone */
static void navigate_from_addr(void)
{
    addr[addr_len] = '\0';
    cur_tls = 0;
    if (!http_parse_url(addr, cur_host, sizeof(cur_host), &cur_port,
                        cur_path, sizeof(cur_path), &cur_tls)) {
        set_status("Zly adres");
        return;
    }
    fetch();
}

/* Klikniety odnosnik: adres wzgledny albo pelny */
static void follow_link(int k)
{
    if (!http_apply_location(links[k].href, cur_host, sizeof(cur_host),
                             &cur_port, cur_path, sizeof(cur_path),
                             &cur_tls)) {
        set_status("Zly odnosnik");
        return;
    }
    rebuild_addr();
    fetch();
}

/* --- rysowanie --- */

static void draw_page(int w)
{
    char buf[COLS + 1];

    for (int row = 0; row < ROWS; row++) {
        int li = scroll + row;
        if (li >= line_count)
            break;

        int y = PAGE_Y + row * ROW_H;
        int i = lines[li].start;
        int end = i + lines[li].len;

        while (i < end) {                /* rysuj kawalkami o jednym kolorze */
            int k = link_at(i);
            int run = 0;
            while (i + run < end && link_at(i + run) == k && run < COLS)
                run++;

            for (int j = 0; j < run; j++)
                buf[j] = text[i + j];
            buf[run] = '\0';

            int col = i - lines[li].start;
            win_text(w, 4 + col * 8, y, buf, k >= 0 ? C_LINK : C_WHITE);
            i += run;
        }
    }
}

/* Klikniecie w tresc: przeliczamy na indeks znaku i szukamy odnosnika */
static void handle_click(int x, int y)
{
    if (y < PAGE_Y)
        return;
    int row = (y - PAGE_Y) / ROW_H;
    int col = (x - 4) / 8;
    int li = scroll + row;
    if (li < 0 || li >= line_count || col < 0 || col >= lines[li].len)
        return;

    int k = link_at(lines[li].start + col);
    if (k >= 0)
        follow_link(k);
}

static void scroll_by(int delta)
{
    /* Ostatni ekran zostaje w calosci widoczny - zjezdzanie do samego
       konca tekstu zostawialoby jeden wiersz na pustym oknie */
    int max_scroll = line_count - ROWS;
    if (max_scroll < 0)
        max_scroll = 0;

    scroll += delta;
    if (scroll > max_scroll)
        scroll = max_scroll;
    if (scroll < 0)
        scroll = 0;
}

void _start(void)
{
    int w = win_create(W, H, "Przegladarka");
    if (w < 0)
        sys_exit(1);

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            if (ev.type == WEV_CLICK) {
                handle_click(ev.x, ev.y);
                continue;
            }
            if (ev.type != WEV_KEY)
                continue;

            char c = (char)ev.val;
            if (c == '\n') {
                navigate_from_addr();
            } else if (c == '\b') {
                if (addr_len > 0)
                    addr_len--;
            } else if (c == KEY_DOWN) {
                scroll_by(1);
            } else if (c == KEY_UP) {
                scroll_by(-1);
            } else if (c == KEY_PGDN) {
                scroll_by(ROWS - 1);
            } else if (c == KEY_PGUP) {
                scroll_by(-(ROWS - 1));
            } else if (c == KEY_HOME) {
                scroll = 0;
            } else if (c == KEY_END) {
                scroll_by(line_count);   /* i tak przytnie do ostatniego ekranu */
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
                 text_len ? (last_secure || !cur_tls ? C_GREEN : C_WHITE)
                          : C_RED);
        draw_page(w);

        sys_sleep(60);
    }
}
