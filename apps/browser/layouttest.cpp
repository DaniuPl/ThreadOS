// layouttest - litehtml laying out pages, with a font whose sizes are
// known in advance
//
// Plain portable C++: the same source is built against musl for
// ThreadOS and against the host's own C++ library on Linux, and
// scripts/tests/t_browser.py compares what the two print. litehtml is a
// lot of C++ - strings, maps, shared pointers, std::function, floating
// point in every box - so the same boxes on both sides is the proof
// that the C++ runtime under it on ThreadOS behaves.
//
// The font is made up: every character is half the font's size wide.
// That keeps the numbers the same everywhere - a real font's widths
// would depend on its rasteriser - and the layout is still a real one:
// lines break, floats push text aside, tables share out their width.

#include <litehtml.h>
#include <cstdio>
#include <cstring>
#include <string>

using namespace litehtml;

struct fake_font {
    float size;
    int weight;
};

class test_container : public document_container {
  public:
    uint_ptr create_font(const font_description& d, const document*,
                         font_metrics* fm) override
    {
        fake_font *f = new fake_font{d.size, d.weight};
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = d.size * 0.8f;
            fm->descent = d.size * 0.2f;
            fm->height = d.size * 1.2f;
            fm->x_height = d.size * 0.5f;
            fm->ch_width = d.size * 0.5f;
            fm->draw_spaces = true;
        }
        return (uint_ptr)f;
    }
    void delete_font(uint_ptr h) override { delete (fake_font *)h; }
    pixel_t text_width(const char *text, uint_ptr h) override
    {
        fake_font *f = (fake_font *)h;
        int chars = 0;
        for (const char *p = text; *p; p++)
            if ((*p & 0xC0) != 0x80)
                chars++;
        return chars * f->size * (f->weight >= 600 ? 0.55f : 0.5f);
    }
    void draw_text(uint_ptr, const char *text, uint_ptr h, web_color c,
                   const position& pos) override
    {
        fake_font *f = (fake_font *)h;
        std::printf("text %.2f,%.2f %.0f%s #%02x%02x%02x \"%s\"\n",
                    (double)pos.x, (double)pos.y, (double)f->size,
                    f->weight >= 600 ? "b" : "", c.red, c.green, c.blue, text);
    }
    pixel_t pt_to_px(float pt) const override { return pt * 96 / 72; }
    pixel_t get_default_font_size() const override { return 16; }
    const char *get_default_font_name() const override { return "sans-serif"; }
    void draw_list_marker(uint_ptr, const list_marker& m) override
    {
        std::printf("marker %.2f,%.2f %.2fx%.2f type %d\n",
                    (double)m.pos.x, (double)m.pos.y, (double)m.pos.width,
                    (double)m.pos.height, (int)m.marker_type);
    }
    void load_image(const char *, const char *, bool) override {}
    void get_image_size(const char *, const char *, size& sz) override
    {
        sz.width = 40;
        sz.height = 30;
    }
    void draw_image(uint_ptr, const background_layer&, const std::string&,
                    const std::string&) override {}
    void draw_solid_fill(uint_ptr, const background_layer& l,
                         const web_color& c) override
    {
        std::printf("fill %.2f,%.2f %.2fx%.2f #%02x%02x%02x\n",
                    (double)l.border_box.x, (double)l.border_box.y,
                    (double)l.border_box.width, (double)l.border_box.height,
                    c.red, c.green, c.blue);
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
        std::printf("border %.2f,%.2f %.2fx%.2f top %.2f\n", (double)p.x,
                    (double)p.y, (double)p.width, (double)p.height,
                    (double)b.top.width);
    }
    void set_caption(const char *c) override { std::printf("title \"%s\"\n", c); }
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<document>&, const element::ptr&) override {}
    void on_anchor_click(const char *, const element::ptr&) override {}
    void on_mouse_event(const element::ptr&, mouse_event) override {}
    void set_cursor(const char *) override {}
    void transform_text(std::string& text, text_transform tt) override
    {
        if (tt == text_transform_uppercase)
            for (auto& c : text)
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 32);
    }
    void import_css(std::string&, const std::string&, std::string&) override {}
    void set_clip(const position&, const border_radiuses&) override {}
    void del_clip() override {}
    void get_viewport(position& vp) const override
    {
        vp = position(0, 0, 400, 300);
    }
    element::ptr create_element(const char *, const string_map&,
                                const std::shared_ptr<document>&) override
    {
        return nullptr;
    }
    void get_media_features(media_features& m) const override
    {
        m.type = media_type_screen;
        m.width = m.device_width = 400;
        m.height = m.device_height = 300;
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

static void boxes(const element::ptr& el, int depth)
{
    if (!el->is_text() && el->get_tagName() && *el->get_tagName()) {
        position p = el->get_placement();
        std::printf("%*s%s %.2f,%.2f %.2fx%.2f\n", depth * 2, "",
                    el->get_tagName(), (double)p.x, (double)p.y,
                    (double)p.width, (double)p.height);
    }
    for (auto& c : el->children())
        boxes(c, depth + 1);
}

static const char *pages[] = {
    // Blocks, margins that collapse, text that wraps, inline styles
    "<html><head><title>Blocks</title><style>"
    "body{margin:8px} h1{font-size:24px;margin:10px 0} p{margin:12px 0}"
    ".box{background:#ddeeff;padding:6px;border:2px solid #345}"
    "</style></head><body><h1>ThreadOS</h1>"
    "<p>A hobby operating system for PC, written from scratch in C and "
    "assembly, with <b>two kernels</b> and <i>one set of sources</i>.</p>"
    "<div class=box>Zażółć gęślą jaźń - a box with padding and a border "
    "that wraps its text onto several lines.</div></body></html>",

    // Floats and a centred line
    "<html><body style='margin:0'>"
    "<div style='float:left;width:100px;height:60px;background:red'></div>"
    "<div style='float:right;width:80px;height:40px;background:blue'></div>"
    "<p style='margin:0'>Text flows between two floats until it has gone "
    "past both of them and takes the whole width again.</p>"
    "<p style='clear:both;text-align:center'>centred after the clear</p>"
    "</body></html>",

    // Lists and a table
    "<html><body><ul><li>first</li><li>second item</li></ul>"
    "<ol start=3><li>three</li><li>four</li></ol>"
    "<table border=1 cellpadding=4 style='width:300px'>"
    "<tr><th>Kernel</th><th>Mode</th></tr>"
    "<tr><td>32-bit</td><td>protected</td></tr>"
    "<tr><td colspan=2>64-bit long mode, UEFI</td></tr></table>"
    "</body></html>",

    // Flexbox, percentages, em, display:none, selectors
    "<html><head><style>"
    ".row{display:flex;justify-content:space-between;width:90%}"
    ".row div{width:5em;background:#eee} .row div:first-child{width:8em}"
    ".gone{display:none} p span + span{font-weight:bold}"
    "h2{text-transform:uppercase;font-size:1.5em}"
    "</style></head><body><h2>flex row</h2><div class=row><div>a</div>"
    "<div>b</div><div>c</div></div><p class=gone>hidden</p>"
    "<p><span>one</span> <span>two</span></p>"
    "<div style='position:relative;left:20px;top:5px'>moved</div>"
    "</body></html>",
};

int main()
{
    test_container c;
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        std::printf("== page %u\n", i + 1);
        document::ptr doc = document::createFromString(pages[i], &c);
        pixel_t w = doc->render(400);
        std::printf("rendered %.2f, %.2fx%.2f\n", (double)w,
                    (double)doc->width(), (double)doc->height());
        boxes(doc->root(), 0);
        position clip(0, 0, 400, 2000);
        doc->draw(0, 0, 0, &clip);
    }
    std::printf("done\n");
    return 0;
}
