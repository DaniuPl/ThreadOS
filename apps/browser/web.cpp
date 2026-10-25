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
// Pictures are PNG (8 bits a channel, not interlaced) and baseline
// JPEG, the decoders the picture viewer uses; style sheets in files of
// their own (<link>) are fetched like the page.

#include <litehtml.h>
#include <litehtml/url.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "udraw.h"
#include "uinflate.h"
#include "ujpeg.h"
}

using namespace litehtml;

#define W0 560              // fits a 640x480 screen between the bar and the dock
#define H0 360
#define BAR_H 40              // the toolbar: back, and the address
#define BACK_W 28
#define ADDR_X 46
#define STATUS_H 20           // the line along the bottom
#define LINE_STEP 40          // what an arrow key scrolls
#define SCROLL_W 6             // the scroll thumb along the right edge
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

// --- what a page refers to: pictures and style sheets -----------------

static bool get_url(const std::string& where, std::string& out,
                    const char* file);

struct picture {
    int w = 0, h = 0;
    std::vector<uint32_t> px;         // 0xRRGGBB, already on white
};

static uint32_t on_white(int r, int g, int b, int a)
{
    r = (r * a + 255 * (255 - a)) / 255;
    g = (g * a + 255 * (255 - a)) / 255;
    b = (b * a + 255 * (255 - a)) / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// A PNG: the chunks, the zlib stream they carry, a filter byte a row
static bool decode_png(const std::string& f, picture& pic)
{
    const unsigned char* d = (const unsigned char*)f.data();
    size_t n = f.size();
    if (n < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0)
        return false;
    int w = 0, h = 0, depth = 0, type = 0, interlace = 0;
    std::string idat;
    unsigned char pal[256][4];
    int npal = 0;
    for (size_t at = 8; at + 12 <= n;) {
        uint32_t len = be32(d + at);
        const unsigned char* body = d + at + 8;
        if (at + 12 + len > n)
            return false;
        if (!memcmp(d + at + 4, "IHDR", 4) && len >= 13) {
            w = (int)be32(body);
            h = (int)be32(body + 4);
            depth = body[8];
            type = body[9];
            interlace = body[12];
        } else if (!memcmp(d + at + 4, "PLTE", 4)) {
            npal = (int)len / 3;
            for (int i = 0; i < npal && i < 256; i++) {
                pal[i][0] = body[3 * i];
                pal[i][1] = body[3 * i + 1];
                pal[i][2] = body[3 * i + 2];
                pal[i][3] = 255;
            }
        } else if (!memcmp(d + at + 4, "tRNS", 4) && type == 3) {
            for (uint32_t i = 0; i < len && i < 256; i++)
                pal[i][3] = body[i];
        } else if (!memcmp(d + at + 4, "IDAT", 4)) {
            idat.append((const char*)body, len);
        } else if (!memcmp(d + at + 4, "IEND", 4)) {
            break;
        }
        at += 12 + len;
    }
    static const int channels[7] = {1, 0, 3, 1, 2, 0, 4};
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || depth != 8 ||
        interlace || type > 6 || !channels[type] || (type == 3 && !npal))
        return false;
    int bpp = channels[type], stride = w * bpp;
    std::vector<unsigned char> raw((size_t)(stride + 1) * h);
    if (inflate_zlib((const unsigned char*)idat.data(), (unsigned)idat.size(),
                     raw.data(), (unsigned)raw.size()) != (int)raw.size())
        return false;
    std::vector<unsigned char> prev(stride, 0), cur(stride);
    pic.w = w;
    pic.h = h;
    pic.px.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        const unsigned char* in = raw.data() + (size_t)y * (stride + 1);
        int filter = in[0];
        for (int i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i];
            int c = i >= bpp ? prev[i - bpp] : 0, x = in[1 + i];
            switch (filter) {
            case 1: x += a; break;
            case 2: x += b; break;
            case 3: x += (a + b) / 2; break;
            case 4: x += paeth(a, b, c); break;
            default: break;
            }
            cur[i] = (unsigned char)x;
        }
        for (int x = 0; x < w; x++) {
            const unsigned char* p = cur.data() + x * bpp;
            uint32_t v;
            switch (type) {
            case 0: v = on_white(p[0], p[0], p[0], 255); break;
            case 2: v = on_white(p[0], p[1], p[2], 255); break;
            case 3: {
                const unsigned char* e = pal[p[0] < npal ? p[0] : 0];
                v = on_white(e[0], e[1], e[2], e[3]);
                break;
            }
            case 4: v = on_white(p[0], p[0], p[0], p[1]); break;
            default: v = on_white(p[0], p[1], p[2], p[3]); break;
            }
            pic.px[(size_t)y * w + x] = v;
        }
        prev.swap(cur);
    }
    return true;
}

static bool decode_jpeg(const std::string& f, picture& pic)
{
    // The decoder's planes, as large as the picture viewer allows -
    // and only once a page has a JPEG on it
    static std::vector<unsigned char> mem;
    if (mem.empty())
        mem.resize(3 * 1024 * 1024);
    int w = 0, h = 0;
    const char* why = "";
    if (f.size() < 4 || (unsigned char)f[0] != 0xFF ||
        (unsigned char)f[1] != 0xD8 ||
        jpeg_decode((const unsigned char*)f.data(), (unsigned)f.size(),
                    mem.data(), (unsigned)mem.size(), &w, &h, &why) < 0)
        return false;
    pic.w = w;
    pic.h = h;
    pic.px.resize((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r, g, b;
            jpeg_pixel(x, y, &r, &g, &b);
            pic.px[(size_t)y * w + x] = on_white(r, g, b, 255);
        }
    return true;
}

// Every picture a page names, by its full address; one that could not
// be had is remembered too, as nothing, so it is not asked for again
static std::map<std::string, picture> pictures;

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

    std::string full(const char* src, const char* baseurl) const
    {
        std::string b = baseurl && *baseurl ? baseurl : base;
        return resolve(url(b), url(src ? src : "")).str();
    }

    // Fetched and unpacked while the page is read: the layout needs
    // each picture's size before it can place anything after it
    void load_image(const char* src, const char* baseurl, bool) override
    {
        std::string where = full(src, baseurl);
        if (pictures.count(where))
            return;
        picture& pic = pictures[where];
        std::string data;
        if (get_url(where, data, ".web-image") &&
            !decode_png(data, pic) && !decode_jpeg(data, pic))
            pic = picture();
    }
    void get_image_size(const char* src, const char* baseurl,
                        size& sz) override
    {
        auto it = pictures.find(full(src, baseurl));
        sz.width = it == pictures.end() ? 0 : it->second.w;
        sz.height = it == pictures.end() ? 0 : it->second.h;
    }
    // Scaled to the box the layout gave it, a pixel at a time from the
    // nearest one, and cut to what of it is on the page
    void draw_image(uint_ptr, const background_layer& l,
                    const std::string& u, const std::string& b) override
    {
        auto it = pictures.find(full(u.c_str(), b.c_str()));
        if (it == pictures.end() || !it->second.w)
            return;
        const picture& pic = it->second;
        const position& o = l.origin_box;
        int ow = (int)o.width, oh = (int)o.height;
        if (ow <= 0 || oh <= 0)
            return;
        int x0 = sx(o.x), y0 = sy(o.y);
        static std::vector<uint32_t> row;
        for (int y = 0; y < oh; y++) {
            int wy = y0 + y;
            if (wy < page_top() || wy >= page_top() + page_h())
                continue;
            int from = x0 < 0 ? -x0 : 0, to = ow;
            if (x0 + to > width)
                to = width - x0;
            if (to <= from)
                return;
            row.resize(to - from);
            int py = y * pic.h / oh;
            for (int x = from; x < to; x++)
                row[x - from] = pic.px[(size_t)py * pic.w + x * pic.w / ow];
            for (int k = 0; k < to - from; k += 256)
                win_blit32(win, x0 + from + k, wy,
                           to - from - k < 256 ? to - from - k : 256, 1,
                           row.data() + k);
        }
    }

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
    // A <link rel=stylesheet>: fetched like the page, and what it names
    // in turn is relative to where it came from
    void import_css(std::string& text, const std::string& u,
                    std::string& baseurl) override
    {
        std::string where = full(u.c_str(), baseurl.c_str());
        if (get_url(where, text, ".web-style.css"))
            baseurl = where;
    }
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
static bool fetch(const std::string& address, std::string& out,
                  const char* file = ".web-page.htm")
{
    url u(address);
    std::string path = u.path().empty() ? "/" : u.path();
    if (!u.query().empty())
        path += "?" + u.query();
    // (files whose names start with a dot: not the person's)
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

// Whatever an address names: a page, a picture, a style sheet - over
// the network into 'file', or straight off the disk
static bool get_url(const std::string& where, std::string& out,
                    const char* file)
{
    if (where.compare(0, 7, "http://") == 0 ||
        where.compare(0, 8, "https://") == 0)
        return fetch(where, out, file);
    return read_file(where.c_str(), out);
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
    // Where in the page the window is, when the page is longer than it:
    // a thumb as long as the share that shows, at the right edge
    int total = doc ? (int)doc->height() : 0;
    if (total > cont.page_h()) {
        int ph = cont.page_h(), th = ph * ph / total;
        if (th < 20)
            th = 20;
        int ty = BAR_H + (ph - th) * cont.scroll / (total - ph);
        ud_round(w, cont.width - SCROLL_W - 2, ty + 2, SCROLL_W, th - 4,
                 SCROLL_W / 2, 0x9A9AA0);
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
