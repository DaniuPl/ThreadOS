// web - a browser in a window, with litehtml laying the page out
//
// litehtml parses the HTML and CSS and works out where every box and
// word goes; this file is its document_container - the part that
// draws. Text goes through the desktop's own TrueType lettering
// (udraw.h), so the page looks like the rest of the system, and boxes,
// borders and list bullets are the same soft-edged shapes the desktop
// draws with. Pages come from disk, or over HTTP and HTTPS through
// httpget, which already resolves names, follows redirects and checks
// certificates - a second copy of that code in here would be a second
// place for it to go wrong.
//
//   web                     the start page
//   web page.html           a file on the disk
//   web https://host/path   a page from the network
//
// Not yet: images, and style sheets in files of their own (<link>);
// a page's own <style> works.

#include <litehtml.h>
#include <litehtml/url.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "udraw.h"
}

using namespace litehtml;

#define W0 560              // fits a 640x480 screen between the bar and the dock
#define H0 360
#define BAR_H 40              // the toolbar: back, and the address
#define BACK_W 28
#define ADDR_X 46
#define STATUS_H 20           // the line along the bottom
#define LINE_STEP 40          // what an arrow key scrolls
#define FONT_PX 15            // a page's text when its CSS does not say

static const char start_page[] =
    "<html><head><title>Start</title><style>"
    "body{font-family:sans-serif;margin:24px;color:#222}"
    "h1{font-size:28px;margin:0 0 8px} .lead{color:#555}"
    ".card{background:#eef3ff;border:1px solid #9ab;padding:10px 14px;"
    "margin:14px 0} code{font-family:monospace}"
    "</style></head><body><h1>ThreadOS Web</h1>"
    "<p class=lead>Pages laid out by <b>litehtml</b> and drawn in the "
    "system's own lettering.</p><div class=card>Type an address above and "
    "press Enter - <code>https://example.com/</code>, or the name of a file "
    "on the disk.</div><ul><li>Scroll with the wheel, the arrows or "
    "PgUp/PgDn</li><li>Click a link to follow it</li><li>The arrow on the "
    "left goes back</li></ul></body></html>";

// --- the container: litehtml asks, the window draws ------------------

struct font {
    int face, px;
    int ascent, descent;
    bool underline, strike;
};

static uint32_t rgb(const web_color& c)
{
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

class window_container : public document_container {
  public:
    int win = -1;
    int width = W0, height = H0;      // the window's content
    int scroll = 0;                   // how far down the page we are
    std::string base;                 // what relative addresses are relative to
    std::string title;
    std::string clicked;              // a link to follow, once the click is done

    int page_top() const { return BAR_H; }
    int page_h() const { return height - BAR_H - STATUS_H; }

    uint_ptr create_font(const font_description& d, const document*,
                         font_metrics* fm) override
    {
        font* f = new font;
        f->px = (int)(d.size + 0.5f);
        if (f->px < 6)
            f->px = 6;
        if (f->px > 72)
            f->px = 72;
        bool mono = d.family.find("mono") != std::string::npos ||
                    d.family.find("courier") != std::string::npos ||
                    d.family.find("Courier") != std::string::npos;
        f->face = mono ? UD_MONO : d.weight >= 600 ? UD_BOLD : UD_SANS;
        // DejaVu's own proportions: 0.93 of the size above the
        // baseline and 0.24 below it
        f->ascent = (f->px * 93 + 50) / 100;
        f->descent = (f->px * 24 + 50) / 100;
        f->underline = d.decoration_line & text_decoration_line_underline;
        f->strike = d.decoration_line & text_decoration_line_line_through;
        if (fm) {
            fm->font_size = (pixel_t)f->px;
            fm->ascent = (pixel_t)f->ascent;
            fm->descent = (pixel_t)f->descent;
            fm->height = (pixel_t)(f->ascent + f->descent);
            fm->x_height = (pixel_t)((f->px * 55 + 50) / 100);
            fm->ch_width = (pixel_t)ud_width("0", f->face, f->px);
            fm->draw_spaces = true;
        }
        return (uint_ptr)f;
    }
    void delete_font(uint_ptr h) override { delete (font*)h; }

    pixel_t text_width(const char* text, uint_ptr h) override
    {
        font* f = (font*)h;
        return (pixel_t)ud_width(text, f->face, f->px);
    }

    // Everything litehtml places is in page coordinates; the window
    // shows a band of the page under the toolbar
    int sx(pixel_t x) const { return (int)x; }
    int sy(pixel_t y) const { return (int)y + page_top() - scroll; }

    void draw_text(uint_ptr, const char* text, uint_ptr h, web_color c,
                   const position& pos) override
    {
        font* f = (font*)h;
        int x = sx(pos.x), y = sy(pos.y);
        if (y + (int)pos.height < page_top() || y > page_top() + page_h())
            return;
        int w = ud_text(win, x, y + f->ascent, text, rgb(c), f->face, f->px);
        if (f->underline)
            ud_fill(win, x, y + f->ascent + 2, w - x, 1, rgb(c));
        if (f->strike)
            ud_fill(win, x, y + f->ascent - f->px / 3, w - x, 1, rgb(c));
    }

    pixel_t pt_to_px(float pt) const override { return pt * 96 / 72; }
    pixel_t get_default_font_size() const override { return FONT_PX; }
    const char* get_default_font_name() const override { return "sans-serif"; }

    void draw_list_marker(uint_ptr, const list_marker& m) override
    {
        int x = sx(m.pos.x), y = sy(m.pos.y), d = (int)m.pos.width;
        if (d < 4)
            d = 4;
        if (m.marker_type == list_style_type_square)
            ud_fill(win, x, y, d, d, rgb(m.color));
        else if (m.marker_type == list_style_type_circle)
            ud_ring(win, x, y, d, d, d / 2, rgb(m.color));
        else
            ud_disc(win, x, y, d, rgb(m.color));
    }

    // No images yet: no size, so the layout leaves no hole for them
    void load_image(const char*, const char*, bool) override {}
    void get_image_size(const char*, const char*, size& sz) override
    {
        sz.width = 0;
        sz.height = 0;
    }
    void draw_image(uint_ptr, const background_layer&, const std::string&,
                    const std::string&) override {}

    void draw_solid_fill(uint_ptr, const background_layer& l,
                         const web_color& c) override
    {
        if (c.alpha == 0)
            return;
        const position& b = l.border_box;
        int r = (int)l.border_radius.top_left_x;
        if (r > 0)
            ud_round(win, sx(b.x), sy(b.y), (int)b.width, (int)b.height, r,
                     rgb(c));
        else
            ud_fill(win, sx(b.x), sy(b.y), (int)b.width, (int)b.height,
                    rgb(c));
    }
    void draw_linear_gradient(uint_ptr, const background_layer&,
                              const background_layer::linear_gradient&) override {}
    void draw_radial_gradient(uint_ptr, const background_layer&,
                              const background_layer::radial_gradient&) override {}
    void draw_conic_gradient(uint_ptr, const background_layer&,
                             const background_layer::conic_gradient&) override {}

    void draw_borders(uint_ptr, const borders& b, const position& p,
                      bool) override
    {
        int x = sx(p.x), y = sy(p.y), w = (int)p.width, h = (int)p.height;
        auto side = [&](const border& s, int bx, int by, int bw, int bh) {
            if ((int)s.width > 0 && s.style != border_style_none &&
                s.style != border_style_hidden && s.color.alpha)
                ud_fill(win, bx, by, bw, bh, rgb(s.color));
        };
        int t = (int)b.top.width, l = (int)b.left.width;
        int r = (int)b.right.width, bo = (int)b.bottom.width;
        side(b.top, x, y, w, t);
        side(b.bottom, x, y + h - bo, w, bo);
        side(b.left, x, y, l, h);
        side(b.right, x + w - r, y, r, h);
    }

    void set_caption(const char* c) override { title = c; }
    void set_base_url(const char* u) override
    {
        if (u && *u)
            base = resolve(url(base), url(u)).str();
    }
    void link(const std::shared_ptr<document>&, const element::ptr&) override {}
    void on_anchor_click(const char* u, const element::ptr&) override
    {
        clicked = u ? u : "";
    }
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char*) override {}
    void transform_text(std::string& text, text_transform tt) override
    {
        bool start = true;
        for (auto& c : text) {
            if (tt == text_transform_uppercase ||
                (tt == text_transform_capitalize && start))
                c = (char)toupper((unsigned char)c);
            else if (tt == text_transform_lowercase)
                c = (char)tolower((unsigned char)c);
            start = c == ' ';
        }
    }
    void import_css(std::string&, const std::string&, std::string&) override {}
    void set_clip(const position&, const border_radiuses&) override {}
    void del_clip() override {}
    void get_viewport(position& vp) const override
    {
        vp = position(0, 0, width, page_h());
    }
    element::ptr create_element(const char*, const string_map&,
                                const std::shared_ptr<document>&) override
    {
        return nullptr;
    }
    void get_media_features(media_features& m) const override
    {
        m.type = media_type_screen;
        m.width = m.device_width = width;
        m.height = m.device_height = page_h();
        m.color = 8;
        m.monochrome = 0;
        m.color_index = 256;
        m.resolution = 96;
    }
    void get_language(std::string& lang, std::string& culture) const override
    {
        lang = "en";
        culture = "";
    }
};

// --- fetching ---------------------------------------------------------

static bool read_file(const char* name, std::string& out)
{
    FILE* f = fopen(name, "rb");
    if (!f)
        return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

// A page over the network: httpget writes it to a file and says by its
// exit code whether it worked
static bool fetch(const std::string& address, std::string& out)
{
    url u(address);
    std::string path = u.path().empty() ? "/" : u.path();
    if (!u.query().empty())
        path += "?" + u.query();
    const char* file = ".web-page.htm";   // hidden: not a file of the person's
    pid_t pid = fork();
    if (pid == 0) {
        char* args[] = {(char*)"httpget", (char*)address.c_str(), (char*)"0",
                        (char*)path.c_str(), (char*)file, nullptr};
        execv("httpget", args);
        _exit(127);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return false;
    return read_file(file, out);
}

// --- the browser ------------------------------------------------------

static window_container cont;
static document::ptr doc;
static std::string address;           // what the address field holds
static std::vector<std::string> history;
static std::string status_line;
static bool address_picked;           // clicked: the next key replaces it

static void draw();

static void layout()
{
    if (doc)
        doc->render(cont.width);
}

static void open_page(const std::string& where, bool remember)
{
    std::string html, target = where;
    bool network = where.compare(0, 7, "http://") == 0 ||
                   where.compare(0, 8, "https://") == 0;
    // "example.com/" is what people type: no such file and something
    // shaped like a host name means the web, as in any browser
    if (!network && !where.empty() && where.compare(0, 6, "about:") != 0 &&
        where.find_first_of(".:") != std::string::npos &&
        where.find_first_of(".:") < where.find('/')) {
        FILE* f = std::fopen(where.c_str(), "rb");
        if (f)
            std::fclose(f);
        else {
            target = "http://" + where;
            network = true;
        }
    }
    bool ok;
    if (where.empty() || where == "about:start") {
        target = "about:start";
        html = start_page;
        ok = true;
    } else if (network) {
        // Said before the wait, not after it - the fetch holds the
        // window until the page is in
        status_line = "Loading " + target + " ...";
        draw();
        ok = fetch(target, html);
    } else {
        ok = read_file(where.c_str(), html);
    }
    if (!ok) {
        status_line = "Could not open " + where;
        return;
    }
    if (remember && !cont.base.empty())
        history.push_back(cont.base);
    cont.base = target;
    cont.title.clear();
    cont.scroll = 0;
    address = target;
    address_picked = false;
    doc = document::createFromString(html, &cont);
    layout();
    status_line = network ? "Done" : "";
    std::string t = "Web";
    if (!cont.title.empty())
        t += ": " + cont.title;
    win_title(cont.win, t.c_str());
    std::printf("[web] opened %s (%d x %d)\n", target.c_str(),
                (int)doc->width(), (int)doc->height());
    std::fflush(stdout);
}

static void follow(const std::string& link)
{
    std::string target = resolve(url(cont.base), url(link)).str();
    open_page(target, true);
}

static void scroll_by(int dy)
{
    int max = doc ? (int)doc->height() - cont.page_h() : 0;
    cont.scroll += dy;
    if (cont.scroll > max)
        cont.scroll = max;
    if (cont.scroll < 0)
        cont.scroll = 0;
}

static void draw()
{
    int w = cont.win;
    struct ud_look l;
    ud_look(&l);
    // The page is white, as pages are, whatever the theme around it
    ud_fill(w, 0, BAR_H, cont.width, cont.page_h(), 0xFFFFFF);
    if (doc) {
        position clip(0, cont.scroll, cont.width, cont.page_h());
        doc->draw(0, 0, 0, &clip);
    }
    // The toolbar and the status line go over whatever the page drew
    // past its edges
    ud_fill(w, 0, 0, cont.width, BAR_H, l.bg);
    ud_fill(w, 0, BAR_H - 1, cont.width, 1, l.line);
    ud_round(w, 10, 8, BACK_W, 24, 12, history.empty() ? l.row : l.accent);
    ud_line(w, 28, 13, 21, 20, 20, history.empty() ? l.dim : l.on_accent);
    ud_line(w, 21, 20, 28, 27, 20, history.empty() ? l.dim : l.on_accent);
    ud_round(w, ADDR_X, 8, cont.width - ADDR_X - 10, 24, 12, l.field);
    ud_ring(w, ADDR_X, 8, cont.width - ADDR_X - 10, 24, 12, l.line);
    int tx = ADDR_X + 12, ty = ud_baseline(8, 24, UD_PX);
    if (address_picked) {
        // Picked whole, the way a browser's address bar takes a click
        int aw = ud_width(address.c_str(), UD_SANS, UD_PX);
        ud_fill(w, tx - 1, 12, aw + 2, 16, l.accent);
        ud_text(w, tx, ty, address.c_str(), l.on_accent, UD_SANS, UD_PX);
    } else {
        int end = ud_text(w, tx, ty, address.c_str(), l.text, UD_SANS,
                          UD_PX);
        ud_fill(w, end + 1, 13, 1, 14, l.accent);
    }
    int fy = cont.height - STATUS_H;
    ud_fill(w, 0, fy, cont.width, STATUS_H, l.bg);
    ud_fill(w, 0, fy, cont.width, 1, l.line);
    ud_text(w, 10, ud_baseline(fy, STATUS_H, 11), status_line.c_str(),
            l.dim, UD_SANS, 11);
}

int main(int argc, char** argv)
{
    cont.win = (int)win_create(W0, H0, "Web");
    if (cont.win < 0)
        return 1;
    // Real colours: a page is not drawn in the desktop's palette
    win_truecolour(cont.win);
    open_page(argc > 1 ? argv[1] : "about:start", false);
    draw();

    for (;;) {
        struct win_event ev;
        bool again = false;
        while (win_poll(cont.win, &ev) == 1) {
            again = true;
            if (ev.type == WEV_CLOSE)
                return 0;
            if (ev.type == WEV_SIZE) {
                cont.width = ev.x;
                cont.height = ev.y;
                layout();
                scroll_by(0);
            } else if (ev.type == WEV_SCROLL) {
                scroll_by(-ev.val * LINE_STEP);
            } else if (ev.type == WEV_CLICK) {
                if (ev.y < BAR_H) {
                    if (ev.x < ADDR_X && !history.empty()) {
                        std::string back = history.back();
                        history.pop_back();
                        open_page(back, false);
                    } else if (ev.x >= ADDR_X) {
                        address_picked = !address_picked;
                    }
                } else if (doc && ev.y < cont.height - STATUS_H) {
                    int px = ev.x, py = ev.y - BAR_H + cont.scroll;
                    cont.clicked.clear();
                    doc->on_lbutton_down(px, py, ev.x, ev.y,
                                         [](const position&) {});
                    doc->on_lbutton_up(px, py, ev.x, ev.y,
                                       [](const position&) {});
                    if (!cont.clicked.empty())
                        follow(cont.clicked);
                }
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                bool picked = address_picked;
                address_picked = false;
                if (picked && (c == '\b' || (c >= 0x20 && c < 0x7F)))
                    address.clear();
                if (c == '\n')
                    open_page(address, true);
                else if (c == '\b') {
                    if (!address.empty())
                        address.pop_back();
                } else if (c == (char)KEY_DOWN)
                    scroll_by(LINE_STEP);
                else if (c == (char)KEY_UP)
                    scroll_by(-LINE_STEP);
                else if (c == (char)KEY_PGDN)
                    scroll_by(cont.page_h() - LINE_STEP);
                else if (c == (char)KEY_PGUP)
                    scroll_by(-(cont.page_h() - LINE_STEP));
                else if (c == (char)KEY_HOME)
                    cont.scroll = 0;
                else if (c >= 0x20 && c < 0x7F)
                    address += c;
            }
        }
        if (again)
            draw();
        usleep(30000);
    }
}
