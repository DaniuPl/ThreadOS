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
// their own (<link>) are fetched like the page. Forms send what is in
// their fields as a query; words typed as an address go to the search
// engine the 'search' setting names.

#include <litehtml.h>
#include <litehtml/el_text.h>
#include <litehtml/render_item.h>
#include <litehtml/url.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <csignal>
#include <cmath>
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
#define BACK_W 28             // back, forward, reload, new tab: round buttons
#define BTN_STEP 34           // from one to the next
#define ADDR_X 146
#define TAB_H 28              // the strip of tabs, under the toolbar, with two
#define TAB_MAX 180           // the widest a tab gets
#define STATUS_H 20           // the line along the bottom
#define LINE_STEP 40          // what an arrow key scrolls
#define SCROLL_W 6             // the scroll thumb along the right edge
#define STAR_W 26             // the bookmark star, at the address field's end
#define ZOOM_W 96             // "-  100%  +" at the status line's right end
#define ZOOM_BTN 26           // each of its buttons
#define FONT_PX 15            // a page's text when its CSS does not say
#define SEL_BG 0xB4D5FE       // behind selected text
#define FOUND_BG 0xFFE97A     // behind what Ctrl+F found
#define FOUND_NOW 0xFF9632    // and behind the match it is at
// The clipboard's keys, as the keyboard driver sends them
// (kernel/include/keyboard.h): Ctrl+Insert and Shift+Insert
#define KEY_COPY 0x1A
#define KEY_PASTE 0x1B

static const char start_page[] =
    "<html><head><title>Start</title><style>"
    "body{font-family:sans-serif;margin:24px;color:#222}"
    "h1{font-size:28px;margin:0 0 8px} .lead{color:#555}"
    ".card{background:#eef3ff;border:1px solid #9ab;padding:10px 14px;"
    "margin:14px 0} code{font-family:monospace}"
    "h2{font-size:17px;margin:18px 0 4px} .marks a{color:#1a55c0}"
    ".seen a{color:#5a6a80} .none{color:#888}"
    "</style></head><body><h1>ThreadOS Web</h1>"
    "<p class=lead>Pages laid out by <b>litehtml</b> and drawn in the "
    "system's own lettering.</p><div class=card>Type an address above and "
    "press Enter - <code>https://example.com/</code>, or the name of a file "
    "on the disk.</div><ul><li>Scroll with the wheel, the arrows or "
    "PgUp/PgDn</li><li>Click a link to follow it</li><li>The star keeps "
    "the page among the bookmarks below; Ctrl+F finds on the page</li>"
    "</ul>";

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
    std::vector<uint32_t> px;         // 0xRRGGBB, already on the backdrop
};

// What a picture's transparent parts are laid on, once and for all: a
// page's white, or for a site's icon the toolbar's colour
static uint32_t backdrop = 0xFFFFFF;

static uint32_t on_backdrop(int r, int g, int b, int a)
{
    int br = backdrop >> 16 & 255, bg = backdrop >> 8 & 255, bb = backdrop & 255;
    r = (r * a + br * (255 - a)) / 255;
    g = (g * a + bg * (255 - a)) / 255;
    b = (b * a + bb * (255 - a)) / 255;
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
            case 0: v = on_backdrop(p[0], p[0], p[0], 255); break;
            case 2: v = on_backdrop(p[0], p[1], p[2], 255); break;
            case 3: {
                const unsigned char* e = pal[p[0] < npal ? p[0] : 0];
                v = on_backdrop(e[0], e[1], e[2], e[3]);
                break;
            }
            case 4: v = on_backdrop(p[0], p[0], p[0], p[1]); break;
            default: v = on_backdrop(p[0], p[1], p[2], p[3]); break;
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
            pic.px[(size_t)y * w + x] = on_backdrop(r, g, b, 255);
        }
    return true;
}

// Every picture a page names, by its full address; one that could not
// be had is remembered too, as nothing, so it is not asked for again
static uint32_t le32(const unsigned char* p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// A site's icon (.ico): a directory of pictures, each a PNG or a bitmap
// without its file header - 32 bits a pixel with alpha, or 24 with a
// mask of one bit beside it. The one nearest 16 pixels, not under, is
// taken: the size it is shown at.
static bool decode_ico(const std::string& f, picture& pic)
{
    const unsigned char* d = (const unsigned char*)f.data();
    size_t n = f.size();
    if (n < 6 || d[0] || d[1] || d[2] != 1 || d[3])
        return false;
    int count = d[4] | d[5] << 8, best = -1, best_w = 0;
    for (int i = 0; i < count && 6 + 16 * (size_t)(i + 1) <= n; i++) {
        int w = d[6 + 16 * i] ? d[6 + 16 * i] : 256;
        bool better = best < 0 || (w >= 16 && (best_w < 16 || w < best_w)) ||
                      (w < 16 && best_w < 16 && w > best_w);
        if (better) {
            best = i;
            best_w = w;
        }
    }
    if (best < 0)
        return false;
    const unsigned char* e = d + 6 + 16 * best;
    uint32_t size = le32(e + 8), at = le32(e + 12);
    if (at >= n || size > n - at || size < 40)
        return false;
    std::string sub = f.substr(at, size);
    if (!sub.compare(0, 8, "\x89PNG\r\n\x1a\n", 8))
        return decode_png(sub, pic);
    const unsigned char* b = d + at;
    uint32_t hdr = le32(b);
    int w = (int)le32(b + 4), h = (int)le32(b + 8) / 2;
    int bpp = b[14] | b[15] << 8;
    if (hdr < 40 || w <= 0 || w > 256 || h <= 0 || h > 256 || le32(b + 16) ||
        (bpp != 32 && bpp != 24))
        return false;
    size_t stride = (size_t)((w * bpp + 31) / 32) * 4;
    size_t mstride = (size_t)((w + 31) / 32) * 4;
    if (hdr + stride * h + (bpp == 24 ? mstride * h : 0) > size)
        return false;
    const unsigned char* px = b + hdr;
    const unsigned char* mask = px + stride * h;
    pic.w = w;
    pic.h = h;
    pic.px.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        const unsigned char* row = px + stride * (h - 1 - y);    // bottom up
        for (int x = 0; x < w; x++) {
            const unsigned char* p = row + x * (bpp / 8);
            int a = bpp == 32 ? p[3]
                  : (mask[mstride * (h - 1 - y) + x / 8] >> (7 - x % 8) & 1)
                        ? 0 : 255;
            pic.px[(size_t)y * w + x] = on_backdrop(p[2], p[1], p[0], a);
        }
    }
    return true;
}

static std::map<std::string, picture> pictures;

// --- selecting text --------------------------------------------------

// litehtml has no selection of its own; what it has is every run of
// text it draws, with where it goes. Those are kept from each drawing,
// and a drag picks the runs between where it started and where it is -
// a word at a time, in the order the page reads.
struct text_run {
    std::string text;
    int x, y, w, h;                  // in the page's coordinates
};
static std::vector<text_run> runs;   // those the last drawing drew

// Finding on the page (Ctrl+F): each match is the text elements its
// letters lie in; at drawing time their places are worked out, and a
// run drawn at one of them is lit
static bool finding;
static std::string find_q;
static std::vector<std::vector<std::weak_ptr<element>>> matches;
static int match_at;                     // the current one
static std::vector<position> lit_all, lit_now;

static bool at_one_of(const std::vector<position>& ps, const position& p)
{
    for (auto& q : ps)
        if (std::abs((int)q.x - (int)p.x) <= 1 &&
            std::abs((int)q.y - (int)p.y) <= 1)
            return true;
    return false;
}
static bool sel_drag, sel_any;
static int sel_x0, sel_y0, sel_x1, sel_y1;

static bool selected(int x, int y, int w, int h)
{
    if (!sel_any)
        return false;
    int ax = sel_x0, ay = sel_y0, bx = sel_x1, by = sel_y1;
    // (a before b: the one higher up, or on one line, the one to the left)
    if (by < ay - 3 || (by <= ay + 3 && bx < ax)) {
        std::swap(ax, bx);
        std::swap(ay, by);
    }
    if (y + h <= ay || y > by)
        return false;                     // above the start, below the end
    if (ay >= y && ay < y + h && x + w <= ax)
        return false;                     // on the start's line, before it
    if (by >= y && by < y + h && x >= bx)
        return false;                     // on the end's line, after it
    return true;
}

class window_container : public document_container {
  public:
    int win = -1;
    int width = W0, height = H0;      // the window's content
    int scroll = 0;                   // how far down the page, in its own px
    int zoom = 100;                   // the page's pixels to the screen's, %
    std::string base;                 // what relative addresses are relative to
    std::string title;
    std::string clicked;              // a link to follow, once the click is done
    bool clicked_download = false;    // ... with a 'download' attribute

    int strip = 0;                    // the tabs' strip, when it shows
    int page_top() const { return BAR_H + strip; }
    int page_h() const { return height - page_top() - STATUS_H; }
    // The window in the page's own pixels: smaller when zoomed in
    int view_w() const { return width * 100 / zoom; }
    int view_h() const { return page_h() * 100 / zoom; }
    int zs(double v) const { return (int)(v * zoom / 100.0 + 0.5); }

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
    // shows a band of the page under the toolbar, at the zoom - the
    // layout is done in the page's pixels, and only the drawing scaled
    int sx(pixel_t x) const { return zs(x); }
    int sy(pixel_t y) const { return zs((double)y - scroll) + page_top(); }

    void draw_text(uint_ptr, const char* text, uint_ptr h, web_color c,
                   const position& pos) override
    {
        font* f = (font*)h;
        int x = sx(pos.x), y = sy(pos.y), hh = zs(pos.height);
        if (y + hh < page_top() || y > page_top() + page_h())
            return;
        int px = zs(f->px), asc = zs(f->ascent);
        int tw = ud_width(text, f->face, f->px);       // in the page's px
        int sw = ud_width(text, f->face, px);          // on the screen
        runs.push_back({text, (int)pos.x, (int)pos.y, tw, (int)pos.height});
        if (at_one_of(lit_now, pos))
            ud_fill(win, x, y, sw, hh, FOUND_NOW);
        else if (at_one_of(lit_all, pos))
            ud_fill(win, x, y, sw, hh, FOUND_BG);
        if (selected((int)pos.x, (int)pos.y, tw, (int)pos.height))
            ud_fill(win, x, y, sw, hh, SEL_BG);
        int w = ud_text(win, x, y + asc, text, rgb(c), f->face, px);
        int line = zs(1) > 1 ? zs(1) : 1;
        if (f->underline)
            ud_fill(win, x, y + asc + zs(2), w - x, line, rgb(c));
        if (f->strike)
            ud_fill(win, x, y + asc - px / 3, w - x, line, rgb(c));
    }

    pixel_t pt_to_px(float pt) const override { return pt * 96 / 72; }
    pixel_t get_default_font_size() const override { return FONT_PX; }
    const char* get_default_font_name() const override { return "sans-serif"; }

    void draw_list_marker(uint_ptr, const list_marker& m) override
    {
        int x = sx(m.pos.x), y = sy(m.pos.y), d = zs(m.pos.width);
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
        int ow = zs(o.width), oh = zs(o.height);
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
            ud_round(win, sx(b.x), sy(b.y), zs(b.width), zs(b.height), zs(r),
                     rgb(c));
        else
            ud_fill(win, sx(b.x), sy(b.y), zs(b.width), zs(b.height),
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
        int x = sx(p.x), y = sy(p.y), w = zs(p.width), h = zs(p.height);
        auto side = [&](const border& s, int bx, int by, int bw, int bh) {
            if ((int)s.width > 0 && s.style != border_style_none &&
                s.style != border_style_hidden && s.color.alpha)
                ud_fill(win, bx, by, bw, bh, rgb(s.color));
        };
        // (a border stays at least a pixel, however far out)
        auto thick = [&](pixel_t v) {
            return (double)v > 0 ? std::max(1, zs((double)v)) : 0;
        };
        int t = thick(b.top.width), l = thick(b.left.width);
        int r = thick(b.right.width), bo = thick(b.bottom.width);
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
    void on_anchor_click(const char* u, const element::ptr& el) override
    {
        clicked = u ? u : "";
        clicked_download = el && el->get_attr("download");
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
        vp = position(0, 0, view_w(), view_h());
    }
    element::ptr create_element(const char* tag, const string_map&,
                                const std::shared_ptr<document>& d) override;
    void get_media_features(media_features& m) const override
    {
        m.type = media_type_screen;
        m.width = m.device_width = view_w();
        m.height = m.device_height = view_h();
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
static bool write_file(const char* name, const std::string& data)
{
    FILE* f = fopen(name, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// Where the browser keeps cookies, between its fetches and its runs
static const char COOKIE_JAR[] = ".web-cookies.txt";
// What the last answer said it was (Content-Type), for its charset
static const char TYPE_FILE[] = ".web-type.txt";

// --- bookmarks and history ---------------------------------------------

// Each a file of lines "address<TAB>title", the newest first, kept in
// the person's home like the cookies
struct visit {
    std::string where, title;
};
static const char BOOKMARKS[] = ".web-bookmarks.txt";
static const char HISTORY[] = ".web-history.txt";
#define HISTORY_MAX 50
static bool page_marked;               // is the page shown among them
static std::string page_icon;          // its site's icon: a key in pictures

static std::vector<visit> load_list(const char* file)
{
    std::vector<visit> out;
    std::string text;
    if (!read_file(file, text))
        return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        size_t tab = line.find('\t');
        if (tab != std::string::npos && tab > 0)
            out.push_back({line.substr(0, tab), line.substr(tab + 1)});
        at = end + 1;
    }
    return out;
}

static void save_list(const char* file, const std::vector<visit>& list)
{
    std::string text;
    for (auto& v : list)
        text += v.where + "\t" + v.title + "\n";
    FILE* f = fopen(file, "wb");
    if (!f)
        return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

static int find_visit(const std::vector<visit>& list, const std::string& w)
{
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].where == w)
            return (int)i;
    return -1;
}

// A page opened goes to the top of the history, once
static void remember_visit(const std::string& where, const std::string& title)
{
    std::vector<visit> h = load_list(HISTORY);
    int at = find_visit(h, where);
    if (at >= 0)
        h.erase(h.begin() + at);
    h.insert(h.begin(), {where, title.empty() ? where : title});
    if (h.size() > HISTORY_MAX)
        h.resize(HISTORY_MAX);
    save_list(HISTORY, h);
}

static std::string html_escape(const std::string& s)
{
    std::string out;
    for (char ch : s)
        out += ch == '<' ? std::string("&lt;") : ch == '>' ? std::string("&gt;")
             : ch == '&' ? std::string("&amp;") : ch == '"' ? std::string("&quot;")
             : ch == '\t' || ch == '\n' ? std::string(" ") : std::string(1, ch);
    return out;
}

static std::string list_html(const std::vector<visit>& list, size_t most,
                             const char* cls, const char* none)
{
    if (list.empty())
        return std::string("<p class=none>") + none + "</p>";
    std::string out = std::string("<ul class=") + cls + ">";
    for (size_t i = 0; i < list.size() && i < most; i++)
        out += "<li><a href=\"" + html_escape(list[i].where) + "\">" +
               html_escape(list[i].title) + "</a></li>";
    return out + "</ul>";
}

// The start page: the words above, then the bookmarks and the pages
// seen last
static std::string start_html()
{
    return std::string(start_page) + "<h2>Bookmarks</h2>" +
           list_html(load_list(BOOKMARKS), 100, "marks",
                     "None yet - the star at the end of the address keeps "
                     "a page here.") +
           "<h2>Recently visited</h2>" +
           list_html(load_list(HISTORY), 10, "seen", "Nothing yet.") +
           "</body></html>";
}

// With 'post', a form's fields go as the body of a POST - through a
// file, since they may be longer than an argument can be
static window_container cont;
static std::string status_line;
static bool loading;                  // httpget is fetching something
static bool stopping;                 // stopped: the rest is not fetched
static long stopped_after;            // bytes in when it was stopped
static void draw_chrome();

static long file_size(const char* name)
{
    long n = 0;
    if (FILE* f = std::fopen(name, "rb")) {
        std::fseek(f, 0, SEEK_END);
        n = std::ftell(f);
        std::fclose(f);
    }
    return n;
}

// What the window says while something loads: a click on the cross or
// Escape is "stop"; a new size is kept for when the page is laid out;
// the rest waits for nobody
static bool stop_asked()
{
    struct win_event ev;
    while (win_poll(cont.win, &ev) == 1) {
        if (ev.type == WEV_CLOSE)
            std::exit(0);
        if (ev.type == WEV_SIZE) {
            cont.width = ev.x;
            cont.height = ev.y;
        }
        if (ev.type == WEV_KEY && ev.val == 0x1B)
            return true;
        if (ev.type == WEV_CLICK && ev.y < BAR_H &&
            ev.x >= 10 + 2 * BTN_STEP && ev.x < 10 + 2 * BTN_STEP + BACK_W)
            return true;
    }
    return false;
}

static bool fetch_to(const std::string& address, const char* file,
                     const std::string* post = nullptr)
{
    if (stopping)
        return false;
    // (the fragment is the browser's, not the server's)
    std::string where = address.substr(0, address.find('#'));
    // (files whose names start with a dot: not the person's)
    const char* body = ".web-form.txt";
    if (post && !write_file(body, *post))
        return false;
    pid_t pid = fork();
    if (pid == 0) {
        // Every fetch - pages, pictures, style sheets - goes with the
        // jar: a signed-in site's pictures may need its cookie too
        char* args[] = {(char*)"httpget", (char*)"-c", (char*)COOKIE_JAR,
                        (char*)"-o", (char*)file, (char*)"-t", (char*)TYPE_FILE,
                        post ? (char*)"-p" : (char*)where.c_str(),
                        post ? (char*)body : nullptr,
                        post ? (char*)where.c_str() : nullptr, nullptr};
        execv("httpget", args);
        _exit(127);
    }
    if (pid < 0)
        return false;
    // The window keeps answering while it waits: the status line counts
    // what has come, and the cross (or Escape) stops it
    int status = 0, tick = 0;
    loading = true;
    std::string was = status_line;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0) {
            loading = false;
            return false;
        }
        if (stop_asked()) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            stopping = true;
            stopped_after = file_size(file);
            loading = false;
            return false;
        }
        if (++tick % 5 == 0) {
            status_line = "Loading " + where + " ... " +
                          std::to_string(file_size(file) / 1024) + " KB";
            draw_chrome();
        }
        usleep(40000);
    }
    loading = false;
    status_line = was;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool fetch(const std::string& address, std::string& out,
                  const char* file = ".web-page.htm",
                  const std::string* post = nullptr)
{
    return fetch_to(address, file, post) && read_file(file, out);
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

static document::ptr doc;
static std::string address;           // what the address field holds
static std::vector<std::string> history;   // back
static std::vector<std::string> ahead;     // forward, after going back
static bool address_picked;           // clicked: the next key replaces it

// --- forms ------------------------------------------------------------

// litehtml lays out <input> and <button> but leaves what they hold and
// do to the container. Each is a box the page's CSS sizes (the defaults
// are in FORM_CSS); a text field draws its value itself, so typing into
// it does not lay the page out again, and a button carries its words as
// text litehtml places, so the box fits them. A <select> keeps its
// options and shows the chosen one; a <textarea> takes its text out of
// the page and holds it as its value.
struct option {
    std::string text, value;
};

// Text broken into lines that fit 'width': at its newlines, and at the
// last space before the edge (mid-word only when a word is wider still)
static std::vector<std::string> wrap(const std::string& s, int width,
                                     int face, int px)
{
    std::vector<std::string> out;
    std::string line;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\n') {
            out.push_back(line);
            line.clear();
            continue;
        }
        line += s[i];
        if (i + 1 < s.size() && (s[i + 1] & 0xC0) == 0x80)
            continue;                  // the rest of a UTF-8 letter first
        if (ud_width(line.c_str(), face, px) <= width || line.size() < 2)
            continue;
        size_t cut = line.rfind(' ');
        std::string rest;
        if (cut != std::string::npos && cut > 0) {
            rest = line.substr(cut + 1);
            line.erase(cut);
        } else {
            cut = line.size() - 1;
            while (cut > 0 && (line[cut] & 0xC0) == 0x80)
                cut--;
            rest = line.substr(cut);
            line.erase(cut);
        }
        out.push_back(line);
        line = rest;
    }
    return out;
}

class el_input : public html_tag {
  public:
    std::string type, name, value, placeholder;
    bool checked = false;
    std::vector<option> options;       // a <select>'s
    int chosen = 0;

    explicit el_input(const document::ptr& d) : html_tag(d) {}

    bool is_button() const
    {
        return type == "submit" || type == "button" || type == "reset" ||
               type == "image";
    }
    bool is_text() const
    {
        return !is_button() && !is_box() && type != "select" &&
               type != "textarea";
    }
    bool is_box() const { return type == "checkbox" || type == "radio"; }

    void parse_attributes() override
    {
        bool button = !strcmp(get_tagName(), "button");
        type = get_attr("type", button ? "submit" : "text");
        if (!strcmp(get_tagName(), "select") ||
            !strcmp(get_tagName(), "textarea"))
            type = get_tagName();
        for (auto& c : type)
            c = (char)tolower((unsigned char)c);
        if (type == "select")
            read_options(shared_from_this());
        if (type == "textarea") {
            std::string text;
            get_text(text);
            value = text;
            clearRecursive();          // drawn by us, not laid out as words
        }
        name = get_attr("name", "");
        if (type != "textarea")
            value = get_attr("value", is_box() ? "on" : "");
        placeholder = get_attr("placeholder", "");
        checked = get_attr("checked") != nullptr;
        if (!button && is_button())
            appendChild(std::make_shared<el_text>(
                (value.empty() ? "Submit" : value).c_str(), get_document()));
        html_tag::parse_attributes();
    }

    // Options straight in the <select> or in an <optgroup> in it
    void read_options(const element::ptr& in)
    {
        for (auto& el : in->children()) {
            if (!strcmp(el->get_tagName(), "optgroup")) {
                read_options(el);
                continue;
            }
            if (strcmp(el->get_tagName(), "option"))
                continue;
            option o;
            el->get_text(o.text);
            size_t a = o.text.find_first_not_of(" \t\r\n");
            size_t b = o.text.find_last_not_of(" \t\r\n");
            o.text = a == std::string::npos ? "" : o.text.substr(a, b - a + 1);
            o.value = el->get_attr("value", o.text.c_str());
            if (el->get_attr("selected"))
                chosen = (int)options.size();
            options.push_back(o);
        }
    }

    // A <select> as wide as its longest option, a <textarea> as its
    // cols and rows say - unless the page's CSS gives a size
    void compute_styles(bool recursive) override
    {
        html_tag::compute_styles(recursive);
        font* f = (font*)css().get_font();
        int px = f ? f->px : FONT_PX, face = f ? f->face : UD_SANS;
        if (type == "select" && css().get_width().is_predefined()) {
            int w = 40;
            for (auto& o : options) {
                int ow = ud_width(o.text.c_str(), face, px);
                if (ow > w)
                    w = ow;
            }
            css_w().set_width(css_length((float)(w + 22)));
        }
        if (type == "textarea") {
            int cols = atoi(get_attr("cols", "20")), rows = atoi(get_attr("rows", "2"));
            if (cols < 2)
                cols = 20;
            if (rows < 1)
                rows = 2;
            if (css().get_width().is_predefined())
                css_w().set_width(css_length(
                    (float)(cols * ud_width("0", face, px))));
            if (css().get_height().is_predefined())
                css_w().set_height(css_length((float)(rows * line_h(px))));
        }
    }

    static int line_h(int px) { return (px * 117 + 50) / 100; }

    void draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
              const std::shared_ptr<render_item>& ri) override;
};

static std::vector<std::weak_ptr<el_input>> inputs;   // the page's, in order
static std::shared_ptr<el_input> typing;              // the field with the keys
static std::shared_ptr<el_input> list_open;           // a <select> showing its list

static const char FORM_CSS[] =
    "input{border:1px solid #8a8a90;background:#fff;padding:3px 5px;"
    "width:12em;height:1.2em;vertical-align:middle}"
    "input[type=submit],input[type=button],input[type=reset],button{"
    "width:auto;height:auto;background:#e6e6ea;border:1px solid #8a8a90;"
    "border-radius:4px;padding:3px 10px;white-space:nowrap}"
    "input[type=checkbox],input[type=radio]{width:11px;height:11px;"
    "padding:0;margin:0 3px}"
    "select,textarea{border:1px solid #8a8a90;background:#fff;"
    "padding:3px 5px;vertical-align:middle}"
    "select{height:1.2em;border-radius:4px}";

element::ptr window_container::create_element(
    const char* tag, const string_map&, const std::shared_ptr<document>& d)
{
    if (strcmp(tag, "input") && strcmp(tag, "button") &&
        strcmp(tag, "select") && strcmp(tag, "textarea"))
        return nullptr;
    auto el = std::make_shared<el_input>(d);
    inputs.push_back(el);
    return el;
}

void el_input::draw(uint_ptr hdc, pixel_t x, pixel_t y, const position* clip,
                    const std::shared_ptr<render_item>& ri)
{
    position p = ri->pos();
    int bx = cont.sx(p.x + x), by = cont.sy(p.y + y);
    int bw = cont.zs(p.width), bh = cont.zs(p.height);
    int w = cont.win;
    if (type == "radio") {
        // Round, which CSS borders here cannot be: drawn whole by us
        if (by + bh < cont.page_top() || by > cont.page_top() + cont.page_h())
            return;
        ud_round(w, bx - 1, by - 1, bw + 2, bh + 2, (bw + 2) / 2, 0x8A8A90);
        ud_round(w, bx, by, bw, bh, bw / 2, 0xFFFFFF);
        if (checked)
            ud_round(w, bx + 3, by + 3, bw - 6, bh - 6, (bw - 6) / 2,
                     0x3366CC);
        return;
    }
    html_tag::draw(hdc, x, y, clip, ri);      // the box and its border
    if (by + bh < cont.page_top() || by > cont.page_top() + cont.page_h())
        return;
    if (is_box()) {
        if (!checked)
            return;
        ud_line(w, bx + 1, by + bh / 2, bx + bw / 2 - 1, by + bh - 2, 24,
                0x202024);
        ud_line(w, bx + bw / 2 - 1, by + bh - 2, bx + bw - 1, by + 1, 24,
                0x202024);
        return;
    }
    font* f = (font*)css().get_font();
    int px = cont.zs(f ? f->px : FONT_PX), face = f ? f->face : UD_SANS;
    uint32_t ink = rgb(css().get_color());
    int base = by + (bh - px) / 2 + (px * 93 + 50) / 100;
    if (type == "select") {
        if (!options.empty())
            ud_text(w, bx, base, options[chosen].text.c_str(), ink, face, px);
        int ax = bx + bw - 8, ay = by + bh / 2;   // the arrow: a list here
        ud_line(w, ax - 4, ay - 2, ax, ay + 2, 20, 0x505056);
        ud_line(w, ax, ay + 2, ax + 4, ay - 2, 20, 0x505056);
        return;
    }
    if (type == "textarea") {
        std::vector<std::string> lines = wrap(value, bw - 2, face, px);
        int lh = line_h(px), fit = bh / lh > 0 ? bh / lh : 1;
        // Being typed into, it shows the end, where the caret is
        size_t first = typing.get() == this && lines.size() > (size_t)fit
                           ? lines.size() - fit : 0;
        int end = bx;
        for (size_t i = first; i < lines.size() && i < first + fit; i++)
            end = ud_text(w, bx, by + (int)(i - first) * lh + (px * 93 + 50) / 100,
                          lines[i].c_str(), ink, face, px);
        if (typing.get() == this) {
            int row = (int)(lines.size() - 1 - first);
            ud_fill(w, lines.back().empty() ? bx : end + 1, by + row * lh, 1,
                    px, 0x202024);
        }
        return;
    }
    if (!is_text())
        return;                               // the words are litehtml's
    std::string shown = type == "password" ? std::string(value.size(), '*')
                                           : value;
    // The end of what is typed, where the caret is, stays in sight
    while (!shown.empty() && ud_width(shown.c_str(), face, px) > bw - 2)
        shown.erase(0, 1);
    if (shown.empty() && !placeholder.empty() && typing.get() != this) {
        std::string ph = placeholder;
        while (!ph.empty() && ud_width(ph.c_str(), face, px) > bw - 2)
            ph.pop_back();
        ud_text(w, bx, base, ph.c_str(), 0x8A8A90, face, px);
        return;
    }
    int end = ud_text(w, bx, base, shown.c_str(), ink, face, px);
    if (typing.get() == this)
        ud_fill(w, shown.empty() ? bx : end + 1, by + (bh - px) / 2, 1, px,
                0x202024);
}

static void draw();
static void draw_chrome();
static void new_tab(const std::string& where);
static void open_in_tab_behind(const std::string& where);
static void click_strip(int x);

// "Web: " and the page's title, on the window
static void show_title()
{
    std::string t = "Web";
    if (!cont.title.empty())
        t += ": " + cont.title;
    win_title(cont.win, t.c_str());
}

// The steps a page can be zoomed by, as browsers have them
static const int zooms[] = {50, 67, 80, 90, 100, 110, 125, 150, 175, 200};

static void layout();
static void scroll_by(int dy);

// A new zoom: laid out again for the width it makes, the same place on
// the page at the top
static void set_zoom(int z)
{
    if (z == cont.zoom)
        return;
    cont.zoom = z;
    layout();
    scroll_by(0);
    std::printf("[web] zoom %d%%\n", z);
    std::fflush(stdout);
}

// A click on the zoom's buttons: out a step, in a step, or back to 100%
static void click_zoom(int x)
{
    int x0 = cont.width - ZOOM_W, n = sizeof(zooms) / sizeof(zooms[0]);
    int at = 0;
    while (at + 1 < n && zooms[at] < cont.zoom)
        at++;
    if (x < x0 + ZOOM_BTN)
        set_zoom(zooms[at > 0 ? at - 1 : 0]);
    else if (x >= cont.width - ZOOM_BTN)
        set_zoom(zooms[at + 1 < n ? at + 1 : n - 1]);
    else
        set_zoom(100);
}

// Where on the page a point of the window is, at the zoom
static int page_x(int x) { return x * 100 / cont.zoom; }
static int page_y(int y)
{
    return (y - cont.page_top()) * 100 / cont.zoom + cont.scroll;
}
static void scroll_by(int dy);
static void open_page(const std::string& where0, bool remember,
                      const std::string* post = nullptr);
static std::string form_encode(const std::string& s);
static std::string in_page_charset(const std::string& utf8);
static void find_all();
static std::string load_icon(const std::string& target);
static void save_session();
static std::string extension(const std::string& target);

static void layout()
{
    if (doc)
        doc->render(cont.view_w());
}

// Words, not an address, go to the search engine the 'search' setting
// names (the address the words are put after)
static std::string search_for(const std::string& words)
{
    std::string base = "https://duckduckgo.com/html/?q=";
    struct cfg_entry c;
    for (uint32_t i = 0; sys_cfg_get(i, &c) == 0; i++)
        if (!strcmp(c.name, "search") && c.value[0])
            base = c.value;
    return base + form_encode(words);
}

// --- forms, sent -------------------------------------------------------

static element::ptr form_of(const element::ptr& el)
{
    for (element::ptr p = el->parent(); p; p = p->parent())
        if (!strcmp(p->get_tagName(), "form"))
            return p;
    return nullptr;
}

// What a form holds goes as name=value pairs - the button pressed among
// them, the other buttons not: after the action's address, or for a
// POST form as the body. ('by' is a copy: it may be the field with the
// keys, which the next page lets go of.)
static void submit(std::shared_ptr<el_input> by)
{
    element::ptr form = form_of(by);
    if (!form)
        return;
    std::string q;
    for (auto& w : inputs) {
        auto in = w.lock();
        if (!in || in->name.empty() || form_of(in) != form)
            continue;
        if ((in->is_button() && in != by) || (in->is_box() && !in->checked))
            continue;
        std::string v = in->value;
        if (in->type == "select") {
            if (in->options.empty())
                continue;
            v = in->options[in->chosen].value;
        } else if (in->type == "textarea") {
            // Lines end in CR LF in what a form sends, as in browsers
            v.clear();
            for (char c : in->value) {
                if (c == '\n')
                    v += '\r';
                v += c;
            }
        }
        if (!q.empty())
            q += '&';
        q += form_encode(in_page_charset(in->name)) + "=" +
             form_encode(in_page_charset(v));
    }
    const char* action = form->get_attr("action", "");
    std::string method = form->get_attr("method", "get");
    for (auto& c : method)
        c = (char)tolower((unsigned char)c);
    std::string target =
        resolve(url(cont.base), url(*action ? action : cont.base)).str();
    target = target.substr(0, target.find('#'));
    if (method == "post") {
        std::printf("[web] form posted to %s: %s\n", target.c_str(),
                    q.c_str());
        std::fflush(stdout);
        open_page(target, true, &q);
        return;
    }
    target = target.substr(0, target.find('?')) + "?" + q;
    std::printf("[web] form sent: %s\n", target.c_str());
    std::fflush(stdout);
    open_page(target, true);
}

// A click on the page that litehtml did not take for a link: a form's
// field gets the keys, a box is ticked, a button sends its form
static bool click_form(int px, int py)
{
    typing.reset();
    for (auto& w : inputs) {
        auto in = w.lock();
        if (!in)
            continue;
        position p = in->get_placement();
        int x = (int)p.x, y = (int)p.y, w0 = (int)p.width, h0 = (int)p.height;
        // (the placement is the content; the padding and the border are
        // the field's too)
        if (px < x - 6 || px >= x + w0 + 6 || py < y - 4 || py >= y + h0 + 4)
            continue;
        if (in->type == "checkbox")
            in->checked = !in->checked;
        else if (in->type == "radio") {
            element::ptr form = form_of(in);
            for (auto& w2 : inputs) {
                auto o = w2.lock();
                if (o && o->type == "radio" && o->name == in->name &&
                    form_of(o) == form)
                    o->checked = false;
            }
            in->checked = true;
        } else if (in->is_button()) {
            if (in->type == "submit" || in->type == "image")
                submit(in);
        } else if (in->type == "select") {
            if (!in->options.empty())
                list_open = in;
        } else
            typing = in;
        return true;
    }
    return false;
}

// The list an open <select> shows: under its box, a row per option, in
// the window's coordinates
struct list_geom {
    int x, y, w, row, px;
};

static list_geom list_at(const std::shared_ptr<el_input>& sel)
{
    position p = sel->get_placement();
    font* f = (font*)sel->css().get_font();
    list_geom g;
    g.px = cont.zs(f ? f->px : FONT_PX);
    g.x = cont.sx(p.x) - 6;
    g.y = cont.sy(p.y + p.height) + 5;
    g.w = (int)p.width + 12;
    g.row = g.px + 8;
    return g;
}

static void draw_list()
{
    if (!list_open)
        return;
    list_geom g = list_at(list_open);
    int n = (int)list_open->options.size(), w = cont.win;
    ud_fill(w, g.x, g.y, g.w, n * g.row + 2, 0x8A8A90);
    for (int i = 0; i < n; i++) {
        int ry = g.y + 1 + i * g.row;
        bool on = i == list_open->chosen;
        ud_fill(w, g.x + 1, ry, g.w - 2, g.row, on ? 0x3366CC : 0xFFFFFF);
        ud_text(w, g.x + 6, ry + (g.row - g.px) / 2 + (g.px * 93 + 50) / 100,
                list_open->options[i].text.c_str(),
                on ? 0xFFFFFF : 0x202024, UD_SANS, g.px);
    }
}

// A click while the list is open: on an option it is chosen; anywhere
// the list closes
static void click_list(int x, int y)
{
    list_geom g = list_at(list_open);
    int i = (y - g.y - 1) / g.row;
    if (x >= g.x && x < g.x + g.w && y > g.y &&
        i < (int)list_open->options.size()) {
        list_open->chosen = i;
        std::printf("[web] chose '%s'\n", list_open->options[i].text.c_str());
        std::fflush(stdout);
    }
    list_open.reset();
}

static std::string form_encode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string q;
    for (unsigned char ch : s) {
        if (ch == ' ')
            q += '+';
        else if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' ||
                 ch == '~')
            q += (char)ch;
        else {
            q += '%';
            q += hex[ch >> 4];
            q += hex[ch & 15];
        }
    }
    return q;
}

// --- characters ---------------------------------------------------------

// What the page in the window was read as: a form on it is sent in it
static encoding page_charset = encoding::utf_8;

static uint32_t next_codepoint(const std::string& s, size_t& i)
{
    unsigned char c = (unsigned char)s[i++];
    int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F)
                                                           : (c & 0x07);
    for (int k = 0; k < n && i < s.size(); k++)
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

// A form's text from UTF-8 into the page's single-byte set, as browsers
// send it: what the set has not, as a numeric reference "&#9731;". The
// table is the set's own decoding, turned round.
static std::string in_page_charset(const std::string& utf8)
{
    encoding e = page_charset;
    if ((int)e < (int)encoding::ibm866 || (int)e > (int)encoding::x_mac_cyrillic)
        return utf8;                   // UTF-8, or one we cannot write
    static encoding built = encoding::null;
    static std::map<uint32_t, char> back;
    if (built != e) {
        back.clear();
        for (int b = 0x80; b < 0x100; b++) {
            std::string u = decode(std::string(1, (char)b), e);
            size_t i = 0;
            uint32_t cp = u.empty() ? 0xFFFD : next_codepoint(u, i);
            if (cp != 0xFFFD && !back.count(cp))
                back[cp] = (char)b;
        }
        built = e;
    }
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        uint32_t cp = next_codepoint(utf8, i);
        if (cp < 0x80)
            out += (char)cp;
        else if (back.count(cp))
            out += back[cp];
        else
            out += "&#" + std::to_string(cp) + ";";
    }
    return out;
}

static bool valid_utf8(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
              : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (n == 1 && c < 0xC2) || i + n >= s.size())
            return false;
        for (int k = 1; k <= n; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return false;
        i += n + 1;
    }
    return true;
}

// The charset a Content-Type names: "text/html; charset=windows-1250"
static encoding http_charset(const std::string& type)
{
    std::string t;
    for (char c : type)
        t += (char)tolower((unsigned char)c);
    size_t at = t.find("charset=");
    if (at == std::string::npos)
        return encoding::null;
    std::string label = t.substr(at + 8);
    label = label.substr(0, label.find_first_of("; \t"));
    if (!label.empty() && (label[0] == '"' || label[0] == '\''))
        label = label.substr(1, label.find_first_of("\"'", 1) - 1);
    return get_encoding(label);
}

// What an undeclared page that is not UTF-8 is taken to be: the old
// single-byte set of the system's language, as browsers decide it
static encoding legacy_default()
{
    struct cfg_entry c;
    for (uint32_t i = 0; sys_cfg_get(i, &c) == 0; i++)
        if (!strcmp(c.name, "language") && !strcmp(c.value, "pl"))
            return encoding::windows_1250;
    return encoding::windows_1252;
}

// The page's bytes with what they are in: the server's word first; then
// what litehtml finds itself (a byte order mark, a <meta>), which it
// does in createFromString; and for a page saying nothing that is not
// UTF-8, the system's default
static estring with_encoding(const std::string& bytes, const std::string& type)
{
    encoding e = http_charset(type);
    if (e != encoding::null)
        return estring(bytes, e, confidence::certain);
    estring probe(bytes, encoding::null, confidence::tentative);
    encoding_sniffing_algorithm(probe);
    if (probe.encoding == encoding::utf_8 &&
        probe.confidence == confidence::tentative && !valid_utf8(bytes))
        return estring(bytes, legacy_default(), confidence::tentative);
    return estring(bytes, encoding::null, confidence::certain);
}

static bool looks_like_words(const std::string& s)
{
    if (s.find(' ') != std::string::npos)
        return true;
    if (s.find_first_of("./:") != std::string::npos)
        return false;
    FILE* f = std::fopen(s.c_str(), "rb");      // a file here by that name
    if (f)
        std::fclose(f);
    return !f;
}

static void open_page(const std::string& where0, bool remember,
                      const std::string* post)
{
    stopping = false;
    std::string where = where0;
    if (!where.empty() && where.compare(0, 6, "about:") != 0 &&
        looks_like_words(where))
        where = search_for(where);
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
        html = start_html();
        ok = true;
    } else if (network) {
        // Said before the wait, not after it - the fetch holds the
        // window until the page is in
        status_line = "Loading " + target + " ...";
        draw();
        ok = fetch(target, html, ".web-page.htm", post);
    } else {
        ok = read_file(where.c_str(), html);
    }
    if (!ok && stopping) {
        // Stopped: the page that was there stays
        stopping = false;
        status_line = "Stopped";
        std::printf("[web] stopped loading %s after %ld bytes\n",
                    target.c_str(), stopped_after);
        std::fflush(stdout);
        return;
    }
    if (!ok) {
        status_line = "Could not open " + where;
        return;
    }
    if (remember && !cont.base.empty()) {
        history.push_back(cont.base);
        ahead.clear();                 // a new way: the old forward is gone
    }
    cont.base = target;
    cont.title.clear();
    cont.scroll = 0;
    address = target;
    address_picked = false;
    std::string type;
    if (network)
        read_file(TYPE_FILE, type);
    estring page = with_encoding(html, type);
    if (target == "about:start")
        page = estring(html, encoding::utf_8, confidence::certain);
    // What litehtml reads the page as, for the forms: what was given; else
    // its own sniffing; and after the parse, a <meta> it went by (below)
    page_charset = page.encoding;
    bool charset_certain = page.encoding != encoding::null &&
                           page.confidence == confidence::certain;
    if (page_charset == encoding::null) {
        estring probe(html, encoding::null, confidence::tentative);
        encoding_sniffing_algorithm(probe);
        page_charset = probe.encoding;
        charset_certain = probe.confidence == confidence::certain;
    }
    if (extension(target) == "txt") {
        // Plain text is shown as it is: < and & in it are letters - in
        // UTF-8, whatever it came in
        std::string text = page.encoding == encoding::null
                               ? html : decode(html, page.encoding);
        html = "<html><head><title>" + target.substr(target.rfind('/') + 1) +
               "</title></head><body><pre style='white-space:pre-wrap'>";
        for (char ch : text)
            html += ch == '<' ? std::string("&lt;")
                  : ch == '&' ? std::string("&amp;") : std::string(1, ch);
        html += "</pre></body></html>";
        page = estring(html, encoding::utf_8, confidence::certain);
        page_charset = encoding::utf_8;
    }
    typing.reset();
    list_open.reset();
    inputs.clear();
    sel_any = sel_drag = false;
    matches.clear();
    doc = document::createFromString(page, &cont,
                                     std::string(master_css) + FORM_CSS);
    if (!charset_certain) {
        // litehtml parses again when a <meta> names another set; the
        // forms must follow it there
        encoding meta = encoding::null;
        if (auto m = doc->root()->select_one("meta[charset]"))
            meta = get_encoding(m->get_attr("charset", ""));
        else if (auto m = doc->root()->select_one("meta[http-equiv][content]"))
            meta = extract_encoding_from_meta_element(m->get_attr("content", ""));
        if (meta == encoding::utf_16le || meta == encoding::utf_16be)
            meta = encoding::utf_8;
        if (meta != encoding::null)
            page_charset = meta;
    }
    layout();
    if (finding)
        find_all();                    // the find field stays, on the new page
    // (stopped while its pictures came: the page shows, without the rest)
    status_line = stopping ? "Stopped" : network ? "Done" : "";
    stopping = false;
    show_title();
    if (target != "about:start")
        remember_visit(target, cont.title);
    std::printf("[web] title: %s\n", cont.title.c_str());
    page_marked = target != "about:start" &&
                  find_visit(load_list(BOOKMARKS), target) >= 0;
    page_icon = network ? load_icon(target) : "";
    save_session();
    std::printf("[web] opened %s (%d x %d)\n", target.c_str(),
                (int)doc->width(), (int)doc->height());
    std::fflush(stdout);
}

// A place on this page (#name): scrolled to, not fetched again
static bool go_to_anchor(const std::string& link)
{
    std::string id = link.substr(link.find('#') + 1);
    if (!doc || id.empty())
        return false;
    element::ptr el = doc->root()->select_one("[id=\"" + id + "\"]");
    if (!el)
        el = doc->root()->select_one("a[name=\"" + id + "\"]");
    if (!el)
        return false;
    cont.scroll = (int)el->get_placement().y;
    scroll_by(0);
    std::printf("[web] went to #%s\n", id.c_str());
    std::fflush(stdout);
    return true;
}

// A link into a window of its own: another browser, started with the
// address. Nobody waits for it - the kernel clears away what has ended.
static void open_in_new_window(const std::string& target)
{
    pid_t pid = fork();
    if (pid == 0) {
        char* args[] = {(char*)"web", (char*)target.c_str(), nullptr};
        execv("web", args);
        _exit(127);
    }
    status_line = pid > 0 ? "Opened in a new window"
                          : "Could not open a new window";
    std::printf("[web] new window: %s\n", target.c_str());
    std::fflush(stdout);
}

// The site's icon: where the page's <link rel=icon> says, or the
// /favicon.ico every browser has asked for since there were icons.
// Fetched once a run for each address, laid on the toolbar's colour.
// Returns its key in 'pictures', or "" for none.
static std::string load_icon(const std::string& target)
{
    element::ptr link;
    for (const char* sel : {"link[rel=icon]", "link[rel=\"shortcut icon\"]",
                            "link[rel=\"icon shortcut\"]"})
        if (!link)
            link = doc->root()->select_one(sel);
    url u(target);
    const char* href = link ? link->get_attr("href") : nullptr;
    std::string where = href && *href
        ? resolve(url(cont.base), url(href)).str()
        : u.scheme() + "://" + u.authority() + "/favicon.ico";
    std::string key = "icon:" + where;
    if (!pictures.count(key)) {
        picture& pic = pictures[key];
        std::string data;
        struct ud_look l;
        ud_look(&l);
        backdrop = l.field;
        if (!get_url(where, data, ".web-icon") ||
            (!decode_png(data, pic) && !decode_jpeg(data, pic) &&
             !decode_ico(data, pic)))
            pic = picture();
        backdrop = 0xFFFFFF;
        std::printf("[web] icon %s: %dx%d\n", where.c_str(), pic.w, pic.h);
        std::fflush(stdout);
    }
    return pictures[key].w ? key : "";
}

// An icon at 16 by 16, from the nearest of its pixels
static void draw_icon(int w, int x0, int y0, const std::string& key)
{
    auto it = pictures.find(key);
    if (key.empty() || it == pictures.end() || !it->second.w)
        return;
    const picture& p = it->second;
    uint32_t row[16];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++)
            row[x] = p.px[(size_t)(y * p.h / 16) * p.w + x * p.w / 16];
        win_blit32(w, x0, y0 + y, 16, 1, row);
    }
}

// The star: the page into the bookmarks, or out of them again
static void toggle_bookmark()
{
    if (cont.base.empty() || cont.base == "about:start")
        return;
    std::vector<visit> b = load_list(BOOKMARKS);
    int at = find_visit(b, cont.base);
    if (at >= 0) {
        b.erase(b.begin() + at);
        status_line = "Bookmark removed";
    } else {
        b.insert(b.begin(), {cont.base,
                             cont.title.empty() ? cont.base : cont.title});
        status_line = "Bookmarked";
    }
    save_list(BOOKMARKS, b);
    page_marked = at < 0;
    std::printf("[web] %s %s\n", at >= 0 ? "bookmark removed" : "bookmarked",
                cont.base.c_str());
    std::fflush(stdout);
}

// The last part of an address's path, in lower case: "zip", "html", ""
static std::string extension(const std::string& target)
{
    url u(target);
    std::string path = u.path();
    std::string last = path.substr(path.rfind('/') + 1);
    size_t dot = last.rfind('.');
    std::string ext = dot == std::string::npos ? "" : last.substr(dot + 1);
    for (auto& c : ext)
        c = (char)tolower((unsigned char)c);
    return ext;
}

// What a link leads to is shown when it is a page, plain text or a
// picture; anything else - an archive, a program, a document - is a
// file to keep
static bool is_download(const std::string& target)
{
    static const char* shown[] = {"", "html", "htm", "xhtml", "shtml", "php",
                                  "asp", "aspx", "jsp", "cgi", "txt", "png",
                                  "jpg", "jpeg"};
    std::string ext = extension(target);
    for (const char* e : shown)
        if (ext == e)
            return false;
    return true;
}

// A file from the network, onto the disk where the browser runs (the
// person's home), under the name its address ends in - with a number
// added rather than over a file already there
static void download(const std::string& target)
{
    url u(target);
    std::string path = u.path();
    std::string name;
    for (char ch : path.substr(path.rfind('/') + 1))
        name += isalnum((unsigned char)ch) || ch == '.' || ch == '-' ||
                        ch == '_' ? ch : '_';
    if (name.empty() || name[0] == '.')
        name = "download" + name;
    if (name.size() > 50)
        name = name.substr(name.size() - 50);
    std::string base = name, ext;
    if (base.rfind('.') != std::string::npos && base.rfind('.') > 0) {
        ext = base.substr(base.rfind('.'));
        base.resize(base.rfind('.'));
    }
    for (int k = 1; k < 100; k++) {
        FILE* f = std::fopen(name.c_str(), "rb");
        if (!f)
            break;
        std::fclose(f);
        name = base + "-" + std::to_string(k) + ext;
    }
    status_line = "Saving " + name + " ...";
    draw();
    stopping = false;
    if (!fetch_to(target, name.c_str())) {
        status_line = stopping ? "Stopped" : "Could not save " + name;
        stopping = false;
        std::remove(name.c_str());
        return;
    }
    long size = 0;
    if (FILE* f = std::fopen(name.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        size = std::ftell(f);
        std::fclose(f);
    }
    char cwd[256] = "";
    if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
    std::string where = std::string(cwd) +
                        (cwd[0] && cwd[std::strlen(cwd) - 1] != '/' ? "/" : "") +
                        name;
    status_line = "Saved " + where + " (" + std::to_string(size) + " bytes)";
    std::printf("[web] saved %s (%ld bytes)\n", where.c_str(), size);
    std::fflush(stdout);
}

static void follow(const std::string& link, bool keep = false)
{
    std::string target = resolve(url(cont.base), url(link)).str();
    // The same page with a fragment: a jump within it
    std::string here = cont.base.substr(0, cont.base.find('#'));
    if (link[0] == '#' || target.compare(0, here.size() + 1, here + "#") == 0) {
        if (go_to_anchor(link))
            return;
    }
    if (keep || is_download(target))
        download(target);
    else
        open_page(target, true);
}

static void go_back()
{
    if (history.empty())
        return;
    std::string to = history.back();
    history.pop_back();
    ahead.push_back(cont.base);
    open_page(to, false);
}

static void go_forward()
{
    if (ahead.empty())
        return;
    std::string to = ahead.back();
    ahead.pop_back();
    history.push_back(cont.base);
    open_page(to, false);
}

static void scroll_by(int dy)
{
    int max = doc ? (int)doc->height() - cont.view_h() : 0;
    cont.scroll += dy * 100 / cont.zoom;      // (the screen's px, the page's)
    if (cont.scroll > max)
        cont.scroll = max;
    if (cont.scroll < 0)
        cont.scroll = 0;
}

// The page's text in reading order, a piece per text element - the
// hidden parts (the head, display:none) left out
struct piece {
    element::ptr el;
    size_t at;
};

static void collect(const element::ptr& el, std::string& text,
                    std::vector<piece>& pieces)
{
    for (auto& c : el->children()) {
        if (c->css().get_display() == display_none)
            continue;
        if (c->is_text()) {
            std::string t;
            c->get_text(t);
            if (t.find_first_not_of(" \t\r\n") == std::string::npos)
                t = " ";               // a space, however it was written
            pieces.push_back({c, text.size()});
            for (auto& ch : t)
                text += (char)tolower((unsigned char)ch);
        } else {
            collect(c, text, pieces);
        }
    }
}

static void show_match()
{
    if (matches.empty())
        return;
    auto el = matches[match_at][0].lock();
    if (!el)
        return;
    int y = (int)el->get_placement().y;
    // Brought into sight, a third of the way down, if it is not there
    if (y < cont.scroll || y > cont.scroll + cont.view_h() - LINE_STEP) {
        cont.scroll = y - cont.view_h() / 3;
        scroll_by(0);
    }
    std::printf("[web] find '%s': %d of %d\n", find_q.c_str(), match_at + 1,
                (int)matches.size());
    std::fflush(stdout);
}

static void find_all()
{
    matches.clear();
    match_at = 0;
    if (!doc || find_q.empty())
        return;
    std::string text, q;
    std::vector<piece> pieces;
    collect(doc->root(), text, pieces);
    for (auto ch : find_q)
        q += (char)tolower((unsigned char)ch);
    for (size_t at = text.find(q); at != std::string::npos;
         at = text.find(q, at + q.size())) {
        std::vector<std::weak_ptr<element>> m;
        for (size_t i = 0; i < pieces.size(); i++) {
            size_t end = i + 1 < pieces.size() ? pieces[i + 1].at : text.size();
            if (end > at && pieces[i].at < at + q.size())
                m.push_back(pieces[i].el);
        }
        if (!m.empty())
            matches.push_back(m);
    }
    if (matches.empty()) {
        std::printf("[web] find '%s': none\n", find_q.c_str());
        std::fflush(stdout);
    }
    show_match();
}

// What is selected, as text: the runs in the order they were drawn, a
// line break where one starts below the last, onto the clipboard
static void copy_selection()
{
    std::string out;
    const text_run* last = nullptr;
    for (auto& r : runs) {
        if (!selected(r.x, r.y, r.w, r.h))
            continue;
        if (last && r.y >= last->y + last->h / 2) {
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            out += '\n';
        }
        if (!(out.empty() || out.back() == '\n') || r.text != " ")
            out += r.text;
        last = &r;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n'))
        out.pop_back();
    if (out.empty())
        return;
    if (out.size() > CLIP_MAX)
        out.resize(CLIP_MAX);
    sys_clip_set(out.c_str(), (uint32_t)out.size());
    status_line = "Copied";
    std::printf("[web] copied: %s\n", out.c_str());
    std::fflush(stdout);
}

// A field among the page's, next after the one with the keys - Tab's
static void next_field()
{
    bool past = !typing;
    for (auto& w : inputs) {
        auto in = w.lock();
        if (!in)
            continue;
        if (past && (in->is_text() || in->type == "textarea")) {
            typing = in;
            return;
        }
        if (in == typing)
            past = true;
    }
    typing.reset();                    // the last one: out of the form
}

// --- tabs ---------------------------------------------------------------

// The page in the window, and what goes with it, is the current tab's;
// the others keep theirs here until they are chosen. Switching is a
// swap: what is shown goes into its tab, the chosen tab's comes out.
struct tab {
    document::ptr doc;
    std::string base, title, address, status;
    int scroll = 0;
    std::vector<std::string> history, ahead;
    std::vector<std::weak_ptr<el_input>> inputs;
    encoding charset = encoding::utf_8;
    bool marked = false;
    std::string icon;
};
static std::vector<tab> tabs(1);
static int cur_tab;
// The tabs open, kept for the next run: by the browser started from the
// dock, not by one a link opened (that one would take their place)
static const char TABS_FILE[] = ".web-tabs.txt";
static bool keeps_session;

static void save_tab()
{
    tab& t = tabs[cur_tab];
    t.doc = doc;
    t.base = cont.base;
    t.title = cont.title;
    t.address = address;
    t.status = status_line;
    t.scroll = cont.scroll;
    t.history = history;
    t.ahead = ahead;
    t.inputs = inputs;
    t.charset = page_charset;
    t.marked = page_marked;
    t.icon = page_icon;
}

// The strip shows with two tabs or more, and the page moves down for it
static void set_strip()
{
    int was = cont.strip;
    cont.strip = tabs.size() > 1 ? TAB_H : 0;
    if (was != cont.strip)
        scroll_by(0);
}

static void save_session()
{
    if (!keeps_session)
        return;
    save_tab();
    std::string text = std::to_string(cur_tab) + "\n";
    for (auto& t : tabs)
        text += (t.base.empty() ? std::string("about:start") : t.base) + "\n";
    write_file(TABS_FILE, text);
}

static void say_tabs()
{
    save_session();
    std::printf("[web] tabs: %d, at %d\n", (int)tabs.size(), cur_tab + 1);
    std::fflush(stdout);
}

static void load_tab(int i)
{
    cur_tab = i;
    tab& t = tabs[i];
    doc = t.doc;
    cont.base = t.base;
    cont.title = t.title;
    address = t.address;
    status_line = t.status;
    cont.scroll = t.scroll;
    history = t.history;
    ahead = t.ahead;
    inputs = t.inputs;
    page_charset = t.charset;
    page_marked = t.marked;
    page_icon = t.icon;
    typing.reset();
    list_open.reset();
    sel_any = sel_drag = false;
    address_picked = false;
    set_strip();
    layout();                          // (the width may have changed since)
    scroll_by(0);
    if (finding)
        find_all();
    show_title();
}

static void switch_tab(int i)
{
    if (i == cur_tab || i < 0 || i >= (int)tabs.size())
        return;
    save_tab();
    load_tab(i);
    say_tabs();
}

// A new tab after the current one, with an address, and in front
static void new_tab(const std::string& where)
{
    save_tab();
    tabs.insert(tabs.begin() + cur_tab + 1, tab());
    load_tab(cur_tab + 1);
    doc.reset();
    cont.base.clear();
    open_page(where, false);
    say_tabs();
}

// The middle button's: opened where it would be, and this one shown again
static void open_in_tab_behind(const std::string& where)
{
    int back = cur_tab;
    new_tab(where);
    save_tab();
    load_tab(back);
    say_tabs();
}

static void close_tab(int i)
{
    if (tabs.size() < 2)
        return;
    if (i == cur_tab) {
        tabs.erase(tabs.begin() + i);
        load_tab(i < (int)tabs.size() ? i : i - 1);
    } else {
        save_tab();
        tabs.erase(tabs.begin() + i);
        load_tab(cur_tab > i ? cur_tab - 1 : cur_tab);
    }
    say_tabs();
}

// Where the tabs are on the strip: as wide as they fit, up to TAB_MAX
static int tab_w()
{
    int n = (int)tabs.size();
    int w = (cont.width - 16) / (n ? n : 1);
    return w < TAB_MAX ? w : TAB_MAX;
}

static void click_strip(int x)
{
    int w = tab_w(), i = (x - 8) / w;
    if (x < 8 || i >= (int)tabs.size())
        return;
    if (x >= 8 + i * w + w - 22)       // its cross
        close_tab(i);
    else
        switch_tab(i);
}

static void draw_strip(int w, const struct ud_look& l)
{
    if (!cont.strip)
        return;
    int y = BAR_H, tw = tab_w();
    ud_fill(w, 0, y, cont.width, TAB_H, l.bg);
    ud_fill(w, 0, y + TAB_H - 1, cont.width, 1, l.line);
    for (int i = 0; i < (int)tabs.size(); i++) {
        int x = 8 + i * tw;
        bool on = i == cur_tab;
        ud_round(w, x + 1, y + 3, tw - 2, TAB_H - 6, 8, on ? l.field : l.row);
        std::string t = on ? cont.title : tabs[i].title;
        if (t.empty())
            t = on ? address : tabs[i].address;
        const std::string& icon = on ? page_icon : tabs[i].icon;
        int tx = icon.empty() ? x + 10 : x + 30;
        draw_icon(w, x + 9, y + (TAB_H - 16) / 2, icon);
        while (!t.empty() && ud_width(t.c_str(), UD_SANS, 11) > x + tw - 24 - tx)
            t.pop_back();
        ud_text(w, tx, ud_baseline(y + 3, TAB_H - 6, 11), t.c_str(),
                on ? l.text : l.dim, UD_SANS, 11);
        int cx = x + tw - 15, cy = y + TAB_H / 2;   // its cross
        ud_line(w, cx - 3, cy - 3, cx + 3, cy + 3, 16, l.dim);
        ud_line(w, cx + 3, cy - 3, cx - 3, cy + 3, 16, l.dim);
    }
}

// A five-pointed star - drawn, since the system's lettering has no ★.
// Full: lines fanned from its middle to every point of its edge.
static void draw_star(int w, int cx, int cy, bool full, uint32_t ink)
{
    int px[11], py[11];
    for (int k = 0; k <= 10; k++) {
        double a = (-90.0 + k * 36.0) * 3.14159265 / 180.0;
        double r = k % 2 ? 3.6 : 8.5;
        px[k] = cx + (int)std::lround(r * std::cos(a));
        py[k] = cy + (int)std::lround(r * std::sin(a));
    }
    for (int k = 0; k < 10; k++) {
        if (full)
            for (int t = 0; t <= 8; t++)
                ud_line(w, cx, cy, px[k] + (px[k + 1] - px[k]) * t / 8,
                        py[k] + (py[k + 1] - py[k]) * t / 8, 24, ink);
        ud_line(w, px[k], py[k], px[k + 1], py[k + 1], 20, ink);
    }
}

// "-  100%  +" at the status line's right end
static void draw_zoom(int w, int fy, const struct ud_look& l)
{
    int x0 = cont.width - ZOOM_W, base = ud_baseline(fy, STATUS_H, 11);
    ud_fill(w, x0, fy + 1, ZOOM_W, STATUS_H - 1, l.bg);
    int my = fy + STATUS_H / 2;
    ud_fill(w, x0 + ZOOM_BTN / 2 - 4, my, 9, 1, l.text);           // -
    int px = cont.width - ZOOM_BTN / 2;
    ud_fill(w, px - 4, my, 9, 1, l.text);                          // +
    ud_fill(w, px, my - 4, 1, 9, l.text);
    std::string pct = std::to_string(cont.zoom) + "%";
    int tw = ud_width(pct.c_str(), UD_SANS, 11);
    ud_text(w, x0 + (ZOOM_W - tw) / 2, base, pct.c_str(),
            cont.zoom == 100 ? l.dim : l.text, UD_SANS, 11);
}

static void draw()
{
    int w = cont.win;
    runs.clear();
    lit_all.clear();
    lit_now.clear();
    for (size_t i = 0; finding && i < matches.size(); i++)
        for (auto& wp : matches[i])
            if (auto el = wp.lock())
                ((int)i == match_at ? lit_now : lit_all)
                    .push_back(el->get_placement());
    struct ud_look l;
    ud_look(&l);
    // The page is white, as pages are, whatever the theme around it
    ud_fill(w, 0, cont.page_top(), cont.width, cont.page_h(), 0xFFFFFF);
    if (doc) {
        position clip(0, cont.scroll, cont.view_w(), cont.view_h());
        doc->draw(0, 0, 0, &clip);
        draw_list();
    }
    // Where in the page the window is, when the page is longer than it:
    // a thumb as long as the share that shows, at the right edge
    int total = doc ? (int)doc->height() : 0;
    if (total > cont.view_h()) {
        int ph = cont.page_h(), th = ph * cont.view_h() / total;
        if (th < 20)
            th = 20;
        int ty = cont.page_top() +
                 (ph - th) * cont.scroll / (total - cont.view_h());
        ud_round(w, cont.width - SCROLL_W - 2, ty + 2, SCROLL_W, th - 4,
                 SCROLL_W / 2, 0x9A9AA0);
    }
    draw_chrome();
}

// The toolbar and the status line - over whatever the page drew past
// its edges. Alone while a page loads: the page is not to be touched
// while litehtml builds the next one.
static void draw_chrome()
{
    int w = cont.win;
    struct ud_look l;
    ud_look(&l);
    ud_fill(w, 0, 0, cont.width, BAR_H, l.bg);
    ud_fill(w, 0, BAR_H - 1, cont.width, 1, l.line);
    // Back and forward: lit when there is somewhere to go
    for (int k = 0; k < 2; k++) {
        bool can = k == 0 ? !history.empty() : !ahead.empty();
        int bx = 10 + k * BTN_STEP, mid = bx + BACK_W / 2;
        int d = k == 0 ? -1 : 1;
        uint32_t ink = can ? l.on_accent : l.dim;
        ud_round(w, bx, 8, BACK_W, 24, 12, can ? l.accent : l.row);
        ud_line(w, mid - 3 * d, 13, mid + 4 * d, 20, 20, ink);
        ud_line(w, mid + 4 * d, 20, mid - 3 * d, 27, 20, ink);
    }
    // Reload: a ring with a gap, and the head of an arrow on it - or
    // while a page loads, a cross that stops it
    int rx = 10 + 2 * BTN_STEP, rm = rx + BACK_W / 2;
    ud_round(w, rx, 8, BACK_W, 24, 12, l.row);
    if (loading) {
        ud_line(w, rm - 5, 15, rm + 5, 25, 24, l.text);
        ud_line(w, rm + 5, 15, rm - 5, 25, 24, l.text);
    } else {
        ud_ring(w, rm - 6, 14, 12, 12, 6, l.text);
        ud_fill(w, rm + 1, 13, 6, 5, l.row);
        ud_line(w, rm + 1, 14, rm + 6, 15, 16, l.text);
        ud_line(w, rm + 6, 15, rm + 5, 20, 16, l.text);
    }
    // A new tab: a plus
    int nx = 10 + 3 * BTN_STEP + BACK_W / 2;
    ud_round(w, nx - BACK_W / 2, 8, BACK_W, 24, 12, l.row);
    ud_fill(w, nx - 5, 19, 11, 2, l.text);
    ud_fill(w, nx - 1, 15, 2, 11, l.text);
    draw_strip(w, l);
    ud_round(w, ADDR_X, 8, cont.width - ADDR_X - 10, 24, 12, l.field);
    ud_ring(w, ADDR_X, 8, cont.width - ADDR_X - 10, 24, 12, l.line);
    int tx = ADDR_X + 12, ty = ud_baseline(8, 24, UD_PX);
    if (!page_icon.empty()) {
        draw_icon(w, ADDR_X + 10, 12, page_icon);   // the site's, before it
        tx += 22;
    }
    if (address_picked) {
        // Picked whole, the way a browser's address bar takes a click
        int aw = ud_width(address.c_str(), UD_SANS, UD_PX);
        ud_fill(w, tx - 1, 12, aw + 2, 16, l.accent);
        ud_text(w, tx, ty, address.c_str(), l.on_accent, UD_SANS, UD_PX);
    } else {
        int end = ud_text(w, tx, ty, address.c_str(), l.text, UD_SANS,
                          UD_PX);
        if (!typing)                   // the keys go here, not to a field
            ud_fill(w, end + 1, 13, 1, 14, l.accent);
    }
    // The star: full and lit for a page among the bookmarks. Over the
    // field's end, so a long address goes under it
    int sx0 = cont.width - 10 - STAR_W;
    ud_fill(w, sx0, 10, STAR_W - 8, 20, l.field);
    draw_star(w, sx0 + 9, 20, page_marked, page_marked ? l.accent : l.dim);
    int fy = cont.height - STATUS_H;
    ud_fill(w, 0, fy, cont.width, STATUS_H, l.bg);
    ud_fill(w, 0, fy, cont.width, 1, l.line);
    if (finding) {
        draw_zoom(w, fy, l);
        // The find field takes the status line's place while it is open
        std::string say = "Find: " + find_q;
        int end = ud_text(w, 10, ud_baseline(fy, STATUS_H, 11), say.c_str(),
                          l.text, UD_SANS, 11);
        ud_fill(w, end + 1, fy + 4, 1, STATUS_H - 8, l.accent);
        std::string n = find_q.empty() ? ""
                      : matches.empty() ? "no matches"
                      : std::to_string(match_at + 1) + " of " +
                            std::to_string(matches.size());
        ud_text(w, end + 16, ud_baseline(fy, STATUS_H, 11), n.c_str(), l.dim,
                UD_SANS, 11);
        return;
    }
    ud_text(w, 10, ud_baseline(fy, STATUS_H, 11), status_line.c_str(),
            l.dim, UD_SANS, 11);
    draw_zoom(w, fy, l);
}

int main(int argc, char** argv)
{
    cont.win = (int)win_create(W0, H0, "Web");
    if (cont.win < 0)
        return 1;
    // Real colours: a page is not drawn in the desktop's palette
    win_truecolour(cont.win);
    keeps_session = argc < 2;
    std::string saved;
    if (keeps_session && read_file(TABS_FILE, saved) && !saved.empty()) {
        // The tabs of the last run, in their order, at the one that was
        // in front
        int at = std::atoi(saved.c_str());
        std::vector<std::string> list;
        size_t p = saved.find('\n');
        while (p != std::string::npos && p + 1 < saved.size()) {
            size_t e = saved.find('\n', p + 1);
            std::string one = saved.substr(p + 1, e == std::string::npos
                                                      ? std::string::npos
                                                      : e - p - 1);
            if (!one.empty())
                list.push_back(one);
            p = e;
        }
        for (size_t i = 0; i < list.size(); i++) {
            if (i == 0)
                open_page(list[0], false);
            else
                new_tab(list[i]);
        }
        if (list.empty())
            open_page("about:start", false);
        else
            switch_tab(at);
        std::printf("[web] restored %d tabs\n", (int)list.size());
        say_tabs();
    } else {
        open_page(argc > 1 ? argv[1] : "about:start", false);
    }
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
            } else if (ev.type == WEV_PRESS) {
                // Perhaps the start of a selection; a click clears one
                sel_any = false;
                sel_drag = doc && !list_open && ev.y >= cont.page_top() &&
                           ev.y < cont.height - STATUS_H;
                sel_x0 = sel_x1 = page_x(ev.x);
                sel_y0 = sel_y1 = page_y(ev.y);
            } else if (ev.type == WEV_DRAG || ev.type == WEV_RELEASE) {
                if (sel_drag) {
                    sel_x1 = page_x(ev.x);
                    sel_y1 = page_y(ev.y);
                    sel_any = std::abs(sel_x1 - sel_x0) > 3 ||
                              std::abs(sel_y1 - sel_y0) > 3;
                }
                if (ev.type == WEV_RELEASE && sel_drag) {
                    sel_drag = false;
                    // Selecting is copying, as in the terminal - drawn
                    // first, so the runs are those now lit
                    if (sel_any) {
                        draw();
                        copy_selection();
                    }
                }
            } else if (ev.type == WEV_ALTCLICK) {
                // The right button on a link: a new window; the middle
                // one: a tab behind this one
                if (doc && !list_open && ev.y >= cont.page_top() &&
                    ev.y < cont.height - STATUS_H) {
                    int px = page_x(ev.x), py = page_y(ev.y);
                    cont.clicked.clear();
                    doc->on_lbutton_down(px, py, ev.x, ev.y,
                                         [](const position&) {});
                    doc->on_lbutton_up(px, py, ev.x, ev.y,
                                       [](const position&) {});
                    std::string to =
                        resolve(url(cont.base), url(cont.clicked)).str();
                    if (!cont.clicked.empty() && (ev.val & 4))
                        open_in_tab_behind(to);
                    else if (!cont.clicked.empty())
                        open_in_new_window(to);
                }
            } else if (ev.type == WEV_CLICK) {
                if (list_open) {
                    click_list(ev.x, ev.y);
                } else if (ev.y >= cont.height - STATUS_H &&
                           ev.x >= cont.width - ZOOM_W) {
                    click_zoom(ev.x);
                } else if (ev.y < BAR_H) {
                    typing.reset();
                    int b = (ev.x - 10) / BTN_STEP;
                    if (ev.x >= cont.width - 10 - STAR_W)
                        toggle_bookmark();
                    else if (ev.x >= ADDR_X)
                        address_picked = !address_picked;
                    else if (ev.x >= 10 && b == 0)
                        go_back();
                    else if (b == 1)
                        go_forward();
                    else if (b == 2 && !cont.base.empty()) {
                        int keep = cont.scroll;
                        open_page(cont.base, false);
                        cont.scroll = keep;   // the same place, freshly
                        scroll_by(0);
                    } else if (b == 3)
                        new_tab("about:start");
                } else if (ev.y < cont.page_top()) {
                    click_strip(ev.x);
                } else if (doc && ev.y < cont.height - STATUS_H) {
                    int px = page_x(ev.x), py = page_y(ev.y);
                    cont.clicked.clear();
                    doc->on_lbutton_down(px, py, ev.x, ev.y,
                                         [](const position&) {});
                    doc->on_lbutton_up(px, py, ev.x, ev.y,
                                       [](const position&) {});
                    if (!cont.clicked.empty())
                        follow(cont.clicked, cont.clicked_download);
                    else
                        click_form(px, py);
                }
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (list_open) {
                    int& k = list_open->chosen;
                    if (c == (char)KEY_DOWN &&
                        k + 1 < (int)list_open->options.size())
                        k++;
                    else if (c == (char)KEY_UP && k > 0)
                        k--;
                    else if (c == '\n' || c == 27)
                        list_open.reset();
                    continue;
                }
                if (c == KEY_FIND) {
                    if (!finding)
                        find_q.clear();        // a new search
                    finding = true;
                    typing.reset();
                    find_all();
                    continue;
                }
                // While the find field is open the keys are its - but for
                // the ones that move about the page
                if (finding && (c == '\n' || c == '\b' || c == 0x1B ||
                                (c >= 0x20 && c < 0x7F))) {
                    if (c == 0x1B) {
                        finding = false;       // Escape: closed
                    } else if (c == '\n') {
                        if (!matches.empty()) {
                            match_at = (match_at + 1) % (int)matches.size();
                            show_match();
                        }
                    } else {
                        if (c == '\b') {
                            if (!find_q.empty())
                                find_q.pop_back();
                        } else {
                            find_q += c;
                        }
                        find_all();
                    }
                    continue;
                }
                if (c == KEY_COPY) {          // Ctrl+Insert: again
                    copy_selection();
                    continue;
                }
                if (c == '\t' && !inputs.empty()) {
                    next_field();
                    continue;
                }
                // (Shift+Insert, the paste, arrives as 0x1B - which is
                // Escape's code too; in a field it means the paste)
                if (typing && (c == '\n' || c == '\b' || c == KEY_PASTE ||
                               (c >= 0x20 && c < 0x7F))) {
                    if (c == '\n' && typing->type == "textarea")
                        typing->value += c;    // a new line, not a send
                    else if (c == '\n')
                        submit(typing);
                    else if (c == KEY_PASTE) {
                        char got[CLIP_MAX + 1];
                        int n = (int)sys_clip_get(got, sizeof(got));
                        for (int i = 0; i < n; i++) {
                            char ch = got[i];
                            if (ch == '\n' && typing->type != "textarea")
                                ch = ' ';      // one line: the breaks go
                            if (ch != '\r')
                                typing->value += ch;
                        }
                    } else if (c == '\b') {
                        if (!typing->value.empty())
                            typing->value.pop_back();
                    } else
                        typing->value += c;
                    continue;
                }
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
