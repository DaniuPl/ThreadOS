/* gweb - a browser in a desktop window (ring 3)
 *
 * Type an address in the form host[:port][/path] (or https://...) and
 * press Enter: the program resolves the name through DNS, connects
 * over TCP (for https through TLS 1.3 with certificate verification),
 * strips the response of headers and HTML tags, then draws the text in
 * the window and writes the whole body to disk (the file
 * 'strona.htm').
 *
 * The page can be scrolled (arrows, PgUp/PgDn, Home), and links are
 * clickable - a click follows the address from <a href>.
 *
 * It ties all the layers together at once: the keyboard and mouse from
 * the desktop compositor, DNS, TCP, TLS, the file system and ring 3
 * processes.
 */

#include "udraw.h"
#include "ulocale.h"
#include "uhttp.h"
#include "ucert.h"
#include "uhtml.h"

/* Drawn the way the desktop draws, in its theme: a toolbar with the
   address in a field of its own and a lock when the page came over
   TLS, a line saying what happened, and the page - in DejaVu Sans Mono
   on a grid of cells, because the page is laid out in columns
   (uhtml.h), with links in the accent and underlined, which is what a
   link has looked like since there were links. */
#define W 440
#define H 320
#define BAR_H2    40          /* the toolbar */
#define CELL      8
#define PAGE_PX   12
#define PAGE_BASE 12          /* a line's baseline, from its top */

#define ADDR_MAX  80
#define PAGE_MAX  8192
#define COLS      53          /* characters on a line */
#define ROWS      15
#define ROW_H     17
#define PAGE_X    8
#define PAGE_Y    (BAR_H2 + 22)   /* the first line of the page */
/* --- state --- */

static char addr[ADDR_MAX] = "threados.test/";
static int addr_len = 14;

static char cur_host[64] = "threados.test";
static uint32_t cur_port = 80;
static char cur_path[96] = "/";
static int cur_tls;

static char page[PAGE_MAX];   /* the raw response (without headers) */
static int page_len;

static char text[PAGE_MAX];   /* the body once stripped of tags */
static struct html_doc doc;   /* the text, the links and the split into lines */
static int scroll;

static char status[64] = "Type an address and press Enter";
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

static void set_status_localized(const char *english, const char *polish)
{
    set_status(ulocale_language_polish() ? polish : english);
}

/* --- fetching --- */

static void page_sink(const char *data, uint32_t len, void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < len && page_len < PAGE_MAX - 1; i++)
        page[page_len++] = data[i];
}

/* Refreshes the address bar from the current target */
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
        /* The built-in public CA roots plus any of our own from disk. A
           certificate that does not hold up ends the connection - just
           as in a real browser. */
        uint32_t n = cert_load_roots("roots.der", roots, sizeof(roots));
        http_set_roots(roots, n, 1);
        roots_loaded = 1;
    }

    page_len = 0;
    doc.text_len = 0;
    doc.line_count = 0;
    text[0] = '\0';

    struct http_result res;
    res.status = 0;
    res.body_len = 0;
    res.verified = 0;
    res.verify_err = 0;

    for (int hop = 0; hop < 3; hop++) {
        set_status_localized("Looking the address up...",
                             "Wyszukiwanie adresu...");
        uint32_t ip = http_resolve(cur_host);
        if (!ip) {
            set_status_localized("I do not know that name",
                                 "Nie znam tej nazwy");
            return;
        }

        set_status_localized("Connecting...", "Łączenie...");
        if (http_get(ip, cur_port, cur_host, cur_path, cur_tls, page_sink, 0,
                     &res) < 0) {
            if (cur_tls && res.verify_err) {
                char msg[64];
                int k = 0;
                const char *pre = "Certificate refused: ";
                while (*pre && k < (int)sizeof(msg) - 1)
                    msg[k++] = *pre++;
                const char *why = cert_error_text(res.verify_err);
                while (*why && k < (int)sizeof(msg) - 1)
                    msg[k++] = *why++;
                msg[k] = '\0';
                set_status(msg);
            } else {
                set_status_localized("Could not connect", "Nie udało się połączyć");
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
        set_status_localized("Redirecting...", "Przekierowywanie...");
    }

    last_secure = cur_tls && res.verified;
    page[page_len] = '\0';

    if (!page_len) {
        set_status_localized("An empty answer", "Pusta odpowiedź");
        return;
    }

    int fd = sys_open("page.htm", O_CREATE | O_WRITE);
    if (fd >= 0) {
        sys_write(fd, page, (uint32_t)page_len);
        sys_close(fd);
    }

    doc.src = page;
    doc.src_len = page_len;
    doc.text = text;
    doc.text_max = PAGE_MAX;
    html_extract(&doc);
    html_layout(&doc, COLS);
    scroll = 0;

    /* The status bar: "TLS+" = the certificate was verified, "TLS?" =
   encryption only */
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
    const char *tail = " B, links ";
    while (*tail)
        st[i++] = *tail++;
    v = (uint32_t)doc.link_count;
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

/* Sets a new target from the address bar and fetches the page */
static void navigate_from_addr(void)
{
    addr[addr_len] = '\0';
    cur_tls = 0;
    if (!http_parse_url(addr, cur_host, sizeof(cur_host), &cur_port,
                        cur_path, sizeof(cur_path), &cur_tls)) {
        set_status_localized("A bad address", "Nieprawidłowy adres");
        return;
    }
    fetch();
}

/* A clicked link: a relative or a full address */
static void follow_link(int k)
{
    if (!http_apply_location(doc.links[k].href, cur_host, sizeof(cur_host),
                             &cur_port, cur_path, sizeof(cur_path),
                             &cur_tls)) {
        set_status_localized("A bad link", "Nieprawidłowy odnośnik");
        return;
    }
    rebuild_addr();
    fetch();
}

/* --- drawing --- */

static void draw_page(int w, const struct ud_look *l)
{
    char buf[COLS + 1];

    for (int row = 0; row < ROWS; row++) {
        int li = scroll + row;
        if (li >= doc.line_count)
            break;

        int y = PAGE_Y + row * ROW_H;
        int i = doc.lines[li].start;
        int end = i + doc.lines[li].len;

        while (i < end) {                /* draw in pieces of a single colour */
            int k = html_link_at(&doc, i);
            int run = 0;
            while (i + run < end && html_link_at(&doc, i + run) == k &&
                   run < COLS)
                run++;

            for (int j = 0; j < run; j++)
                buf[j] = text[i + j];
            buf[run] = '\0';

            int col = i - doc.lines[li].start;
            int x = PAGE_X + col * CELL;
            ud_cells(w, x, y + PAGE_BASE, CELL, buf,
                     k >= 0 ? l->accent : l->text, UD_MONO, PAGE_PX);
            if (k >= 0)
                ud_fill(w, x, y + PAGE_BASE + 2, run * CELL, 1, l->accent);
            i += run;
        }
    }
}

/* A small lock: a body and a shackle */
static void lock(int w, int x, int y, uint32_t rgb)
{
    ud_round(w, x, y + 6, 10, 8, 2, rgb);
    ud_ring(w, x + 2, y, 6, 10, 3, rgb);
}

/* A click in the content: we convert it to a character index and look
   for a link */
static void handle_click(int x, int y)
{
    if (y < PAGE_Y)
        return;
    int row = (y - PAGE_Y) / ROW_H;
    int col = (x - PAGE_X) / CELL;
    int li = scroll + row;
    if (li < 0 || li >= doc.line_count || col < 0 ||
        col >= doc.lines[li].len)
        return;

    int k = html_link_at(&doc, doc.lines[li].start + col);
    if (k >= 0)
        follow_link(k);
}

static void scroll_by(int delta)
{
    /* The last screen stays fully visible - scrolling to the very end
       of the text would leave one line on an empty window */
    int max_scroll = doc.line_count - ROWS;
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
    int w = win_create(W, H, ulocale_language_polish()
                                ? "Przeglądarka" : "Browser");
    if (w < 0)
        sys_exit(1);
    set_status_localized("Type an address and press Enter",
                         "Wpisz adres i naciśnij Enter");

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1) {
            /* Asked to end by the desktop - so we end */
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_CLICK) {
                handle_click(ev.x, ev.y);
                continue;
            }
            /* Three rows a notch: the number every other machine uses,
               and the one that makes a flick of two fingers move a
               paragraph rather than a line. Away from the person is up
               the page. */
            if (ev.type == WEV_SCROLL) {
                scroll_by(-ev.val * 3);
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
                scroll_by(doc.line_count);   /* it will clamp to the last screen anyway */
            } else if (c >= 0x20 && c < 0x7F && addr_len < ADDR_MAX - 1) {
                addr[addr_len++] = c;
            }
            addr[addr_len] = '\0';
        }

        struct ud_look l;
        ud_look(&l);
        ud_fill(w, 0, 0, W, H, l.field);

        /* The toolbar: the address in its field, with what kind of
           connection it is at its left */
        ud_fill(w, 0, 0, W, BAR_H2, l.bg);
        ud_fill(w, 0, BAR_H2 - 1, W, 1, l.line);
        ud_round(w, 10, 8, W - 20, 24, 12, l.field);
        ud_ring(w, 10, 8, W - 20, 24, 12, l.line);
        if (cur_tls)
            lock(w, 22, 13, last_secure ? l.good : l.danger);
        else
            ud_text(w, 20, ud_baseline(8, 24, 10), "http", l.dim, UD_SANS,
                    10);
        addr[addr_len] = '\0';
        int end = ud_text(w, 50, ud_baseline(8, 24, UD_PX), addr, l.text,
                          UD_SANS, UD_PX);
        ud_fill(w, end + 1, 13, 1, 14, l.accent);
        /* What happened: quiet when all is well, in the theme's red
           when nothing came at all */
        ud_text(w, PAGE_X, ud_baseline(BAR_H2, 22, 11), status,
                doc.text_len ? l.dim : l.danger, UD_SANS, 11);
        draw_page(w, &l);

        sys_sleep(60);
    }
}
