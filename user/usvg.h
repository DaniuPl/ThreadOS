/* usvg - an SVG picture, drawn
 *
 * The part of SVG that icons and logos are made of: the shapes (rect,
 * circle, ellipse, line, polyline, polygon, path with all its commands,
 * arcs too), groups and their transforms, nested <svg> and viewBox with
 * preserveAspectRatio, <use> of an element or a <symbol>, fill and
 * stroke in any colour, with opacity, nonzero or evenodd, stroke joins
 * and caps, linear and radial gradients, and the class rules of a
 * <style> - what drawing programs write their colours into.
 * Not here: text, filters, masks, clipping, patterns, dashes, CSS
 * beyond simple selectors. What is not understood is left out rather
 * than drawn wrong.
 *
 * How it draws: every shape becomes polygons - curves cut into short
 * lines, a stroke into the quadrilaterals along its segments and the
 * pieces at its joins and ends, all of one orientation, so that filled
 * by the nonzero rule they are their union. A polygon is filled along
 * scanlines, four per row of pixels, each counting the exact share of
 * a pixel it covers - so edges are smooth - and the colour is laid on
 * with that share as its alpha.
 *
 * Needs a C library (malloc, math): the browser's, which is built
 * against musl. Out of a network, so every length is checked; a
 * picture that makes no sense gives what was drawn of it.
 *
 * The result is four bytes a pixel, red, green, blue and alpha, not
 * premultiplied.
 */

#ifndef USVG_H
#define USVG_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SVG_MAX_SIDE 4096
#define SVG_MAX_NODES 20000
#define SVG_MAX_DEPTH 48

/* --- the tree ------------------------------------------------------------ */

struct svg_attr {
    const char *name, *value;          /* in the document's copy */
};

struct svg_node {
    char tag[16];                      /* without a namespace prefix */
    int parent, first_child, next;     /* -1: none */
    int attr0, nattr;
    int text;                          /* <style>'s text: an offset, or -1 */
};

struct svg_rule {                      /* one selector of a <style> */
    char kind;                         /* '.', '#' or 't' (a tag) */
    char name[32];
    const char *decls;                 /* "fill:#fff;stroke:none" */
};

struct svg_doc {
    char *buf;                         /* the text, NULs written into it */
    struct svg_node *nodes;
    int nnodes, cap_nodes;
    struct svg_attr *attrs;
    int nattrs, cap_attrs;
    struct svg_rule *rules;
    int nrules, cap_rules;
};

static void *svg_grow(void *p, int *cap, int need, size_t size)
{
    if (need <= *cap)
        return p;
    int c = *cap ? *cap * 2 : 64;
    while (c < need)
        c *= 2;
    void *q = realloc(p, (size_t)c * size);
    if (q)
        *cap = c;
    return q;
}

static int svg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Character references in place: &amp; &lt; &gt; &quot; &apos; &#n; &#xn; */
static void svg_unescape(char *s)
{
    char *o = s;
    while (*s) {
        if (*s != '&') {
            *o++ = *s++;
            continue;
        }
        char *e = strchr(s, ';');
        if (!e || e - s > 10) {
            *o++ = *s++;
            continue;
        }
        long v = -1;
        if (s[1] == '#')
            v = s[2] == 'x' || s[2] == 'X' ? strtol(s + 3, 0, 16)
                                           : strtol(s + 2, 0, 10);
        else if (!strncmp(s, "&amp;", 5)) v = '&';
        else if (!strncmp(s, "&lt;", 4)) v = '<';
        else if (!strncmp(s, "&gt;", 4)) v = '>';
        else if (!strncmp(s, "&quot;", 6)) v = '"';
        else if (!strncmp(s, "&apos;", 6)) v = '\'';
        if (v < 0) {
            *o++ = *s++;
            continue;
        }
        /* (only what a number or a colour can be made of matters here) */
        *o++ = v > 0 && v < 128 ? (char)v : '?';
        s = e + 1;
    }
    *o = 0;
}

static int svg_new_node(struct svg_doc *d, const char *tag, int parent)
{
    if (d->nnodes >= SVG_MAX_NODES)
        return -1;
    struct svg_node *n = (struct svg_node *)svg_grow(d->nodes, &d->cap_nodes,
                                                     d->nnodes + 1,
                                                     sizeof(struct svg_node));
    if (!n)
        return -1;
    d->nodes = n;
    int i = d->nnodes++;
    struct svg_node *x = &d->nodes[i];
    const char *colon = strchr(tag, ':');
    if (colon)
        tag = colon + 1;
    strncpy(x->tag, tag, sizeof(x->tag) - 1);
    x->tag[sizeof(x->tag) - 1] = 0;
    x->parent = parent;
    x->first_child = x->next = -1;
    x->attr0 = d->nattrs;
    x->nattr = 0;
    x->text = -1;
    if (parent >= 0) {
        int *link = &d->nodes[parent].first_child;
        while (*link >= 0)
            link = &d->nodes[*link].next;
        *link = i;
    }
    return i;
}

/* Parses the XML in d->buf into nodes. 0, or -1 with nothing usable. */
static int svg_parse_xml(struct svg_doc *d)
{
    char *p = d->buf;
    int cur = -1, depth = 0;
    while (*p) {
        if (*p != '<') {
            p++;
            continue;
        }
        if (!strncmp(p, "<!--", 4)) {
            char *e = strstr(p + 4, "-->");
            if (!e)
                break;
            p = e + 3;
            continue;
        }
        if (!strncmp(p, "<![CDATA[", 9)) {
            char *e = strstr(p + 9, "]]>");
            if (!e)
                break;
            /* A <style>'s rules may sit in CDATA: they are its text */
            if (cur >= 0 && d->nodes[cur].text < 0) {
                d->nodes[cur].text = (int)(p + 9 - d->buf);
                *e = 0;
            }
            p = e + 3;
            continue;
        }
        if (p[1] == '?' || p[1] == '!') {
            char *e = strchr(p, '>');
            if (!e)
                break;
            p = e + 1;
            continue;
        }
        if (p[1] == '/') {                     /* an end tag */
            char *e = strchr(p, '>');
            if (!e)
                break;
            *p = 0;                            /* (ends a <style>'s text) */
            if (cur >= 0) {
                cur = d->nodes[cur].parent;
                depth--;
            }
            p = e + 1;
            continue;
        }
        /* A start tag: its name, its attributes, perhaps "/>" */
        char *name = ++p;
        while (*p && !svg_space(*p) && *p != '>' && *p != '/')
            p++;
        char endc = *p;
        *p = 0;
        int node = svg_new_node(d, name, cur);
        if (node < 0)
            break;
        if (endc)
            p++;
        int closed = endc == '/', open_tag = endc != '>' && endc != '/';
        if (endc == '/' && *p == '>')
            p++;
        while (open_tag && *p) {
            while (svg_space(*p))
                p++;
            if (*p == '>') {
                p++;
                break;
            }
            if (*p == '/') {
                closed = 1;
                p++;
                continue;
            }
            char *an = p;
            while (*p && *p != '=' && !svg_space(*p) && *p != '>' && *p != '/')
                p++;
            char *ae = p;
            while (svg_space(*p))
                p++;
            if (*p != '=') {                   /* a bare name: no value */
                if (ae == an && *p)
                    p++;                       /* (and never past the end) */
                continue;
            }
            p++;
            while (svg_space(*p))
                p++;
            char q = *p;
            if (q != '"' && q != '\'')
                continue;
            char *av = ++p;
            while (*p && *p != q)
                p++;
            if (!*p)
                break;
            *ae = 0;
            *p++ = 0;
            svg_unescape(av);
            struct svg_attr *a = (struct svg_attr *)svg_grow(
                d->attrs, &d->cap_attrs, d->nattrs + 1, sizeof(struct svg_attr));
            if (!a)
                return -1;
            d->attrs = a;
            const char *colon = strchr(an, ':');
            /* xlink:href and href alike; xml:space and the rest do not
               matter */
            a[d->nattrs].name = colon && !strcmp(colon + 1, "href") ? colon + 1 : an;
            a[d->nattrs].value = av;
            d->nattrs++;
            d->nodes[node].nattr++;
        }
        if (!closed && depth < SVG_MAX_DEPTH) {
            cur = node;
            depth++;
            if (!strcmp(d->nodes[node].tag, "style"))
                d->nodes[node].text = (int)(p - d->buf);
        }
    }
    return d->nnodes ? 0 : -1;
}

static const char *svg_attr(const struct svg_doc *d, int n, const char *name)
{
    const struct svg_node *x = &d->nodes[n];
    for (int i = 0; i < x->nattr; i++)
        if (!strcmp(d->attrs[x->attr0 + i].name, name))
            return d->attrs[x->attr0 + i].value;
    return 0;
}

static int svg_find_id(const struct svg_doc *d, const char *id)
{
    if (!id || !*id)
        return -1;
    for (int i = 0; i < d->nnodes; i++) {
        const char *v = svg_attr(d, i, "id");
        if (v && !strcmp(v, id))
            return i;
    }
    return -1;
}

/* "url(#id)", "#id": the element named */
static int svg_ref(const struct svg_doc *d, const char *v)
{
    if (!v)
        return -1;
    while (svg_space(*v))
        v++;
    char id[64];
    const char *s = v;
    if (!strncmp(s, "url(", 4)) {
        s += 4;
        while (svg_space(*s) || *s == '\'' || *s == '"')
            s++;
    }
    if (*s != '#')
        return -1;
    s++;
    int n = 0;
    while (s[n] && s[n] != ')' && s[n] != '\'' && s[n] != '"' &&
           !svg_space(s[n]) && n < 63) {
        id[n] = s[n];
        n++;
    }
    id[n] = 0;
    return svg_find_id(d, id);
}

/* The rules of every <style>: "sel, sel { decls }" with selectors that
   are a class, an id or a tag. Anything else (descendants, pseudo
   classes, @media) is skipped. */
static void svg_parse_styles(struct svg_doc *d)
{
    for (int i = 0; i < d->nnodes; i++) {
        if (strcmp(d->nodes[i].tag, "style") || d->nodes[i].text < 0)
            continue;
        char *p = d->buf + d->nodes[i].text;
        for (;;) {
            char *open = strchr(p, '{');
            if (!open)
                break;
            char *close = strchr(open, '}');
            if (!close)
                break;
            *open = 0;
            *close = 0;
            char *sel = p;
            p = close + 1;
            if (strchr(sel, '@'))
                continue;
            for (char *s = strtok(sel, ","); s; s = strtok(0, ",")) {
                while (svg_space(*s))
                    s++;
                char *e = s + strlen(s);
                while (e > s && svg_space(e[-1]))
                    *--e = 0;
                if (!*s || strpbrk(s, " >+~:[*"))
                    continue;
                struct svg_rule *r = (struct svg_rule *)svg_grow(
                    d->rules, &d->cap_rules, d->nrules + 1, sizeof(struct svg_rule));
                if (!r)
                    return;
                d->rules = r;
                struct svg_rule *x = &r[d->nrules++];
                x->kind = *s == '.' || *s == '#' ? *s : 't';
                strncpy(x->name, x->kind == 't' ? s : s + 1, sizeof(x->name) - 1);
                x->name[sizeof(x->name) - 1] = 0;
                x->decls = open + 1;
            }
        }
    }
}

/* --- numbers, lengths, colours, transforms ------------------------------- */

static const char *svg_num(const char *s, double *out)
{
    while (svg_space(*s) || *s == ',')
        s++;
    char *e;
    double v = strtod(s, &e);
    if (e == s)
        return 0;
    *out = isfinite(v) ? v : 0;
    return e;
}

/* A length in user units; 'ref' is what 100% is */
static double svg_len(const char *v, double ref, double dflt)
{
    double x;
    const char *e;
    if (!v || !(e = svg_num(v, &x)))
        return dflt;
    if (*e == '%') return x * ref / 100;
    if (!strncmp(e, "pt", 2)) return x * 96 / 72;
    if (!strncmp(e, "pc", 2)) return x * 16;
    if (!strncmp(e, "mm", 2)) return x * 96 / 25.4;
    if (!strncmp(e, "cm", 2)) return x * 96 / 2.54;
    if (!strncmp(e, "in", 2)) return x * 96;
    if (!strncmp(e, "em", 2) || !strncmp(e, "ex", 2)) return x * 16;
    return x;
}

struct svg_named {
    const char *name;
    unsigned rgb;
};

static const struct svg_named svg_colours[] = {
    {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
    {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
    {"cyan", 0x00FFFF}, {"aqua", 0x00FFFF}, {"magenta", 0xFF00FF},
    {"fuchsia", 0xFF00FF}, {"gray", 0x808080}, {"grey", 0x808080},
    {"silver", 0xC0C0C0}, {"maroon", 0x800000}, {"olive", 0x808000},
    {"lime", 0x00FF00}, {"teal", 0x008080}, {"navy", 0x000080},
    {"purple", 0x800080}, {"orange", 0xFFA500}, {"pink", 0xFFC0CB},
    {"brown", 0xA52A2A}, {"gold", 0xFFD700}, {"indigo", 0x4B0082},
    {"violet", 0xEE82EE}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9},
    {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3},
    {"dimgray", 0x696969}, {"darkred", 0x8B0000}, {"darkgreen", 0x006400},
    {"darkblue", 0x00008B}, {"lightblue", 0xADD8E6},
    {"skyblue", 0x87CEEB}, {"steelblue", 0x4682B4},
    {"royalblue", 0x4169E1}, {"dodgerblue", 0x1E90FF},
    {"tomato", 0xFF6347}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072},
    {"crimson", 0xDC143C}, {"orangered", 0xFF4500},
    {"darkorange", 0xFF8C00}, {"khaki", 0xF0E68C}, {"beige", 0xF5F5DC},
    {"tan", 0xD2B48C}, {"chocolate", 0xD2691E}, {"sienna", 0xA0522D},
    {"limegreen", 0x32CD32}, {"forestgreen", 0x228B22},
    {"seagreen", 0x2E8B57}, {"turquoise", 0x40E0D0},
    {"slategray", 0x708090}, {"whitesmoke", 0xF5F5F5},
    {"gainsboro", 0xDCDCDC}, {"ivory", 0xFFFFF0}, {"lavender", 0xE6E6FA},
    {"plum", 0xDDA0DD}, {"orchid", 0xDA70D6}, {"hotpink", 0xFF69B4},
    {"deeppink", 0xFF1493}, {"firebrick", 0xB22222},
};

/* A colour: 1 with *rgb and *alpha set, 0 for "none" or nonsense, 2 for
   currentColor, 3 for a paint server (url(#...)) */
static int svg_colour(const char *v, unsigned *rgb, double *alpha)
{
    while (v && svg_space(*v))
        v++;
    if (!v || !*v || !strncmp(v, "none", 4) || !strncmp(v, "transparent", 11))
        return 0;
    *alpha = 1;
    if (!strncmp(v, "url(", 4))
        return 3;
    if (!strncmp(v, "currentColor", 12) || !strncmp(v, "currentcolor", 12))
        return 2;
    if (*v == '#') {
        unsigned x = 0;
        int n = 0;
        while (n < 8 && ((v[n + 1] >= '0' && v[n + 1] <= '9') ||
                         ((v[n + 1] | 32) >= 'a' && (v[n + 1] | 32) <= 'f'))) {
            char c = v[n + 1];
            x = x * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            n++;
        }
        if (n == 3 || n == 4) {
            unsigned a = n == 4 ? (x & 15) * 17 : 255;
            if (n == 4)
                x >>= 4;
            *rgb = ((x >> 8 & 15) * 17) << 16 | ((x >> 4 & 15) * 17) << 8 |
                   (x & 15) * 17;
            *alpha = a / 255.0;
            return 1;
        }
        if (n == 6 || n == 8) {
            *alpha = n == 8 ? (x & 255) / 255.0 : 1;
            *rgb = n == 8 ? x >> 8 : x;
            return 1;
        }
        return 0;
    }
    if (!strncmp(v, "rgb", 3)) {
        const char *s = strchr(v, '(');
        if (!s)
            return 0;
        s++;
        double c[4] = {0, 0, 0, 1};
        for (int i = 0; i < 4; i++) {
            while (svg_space(*s) || *s == ',' || *s == '/')
                s++;
            const char *e = svg_num(s, &c[i]);
            if (!e)
                break;
            if (*e == '%')
                c[i] = c[i] * (i < 3 ? 2.55 : 0.01), e++;
            s = e;
        }
        unsigned r[3];
        for (int i = 0; i < 3; i++)
            r[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : (unsigned)(c[i] + 0.5);
        *rgb = r[0] << 16 | r[1] << 8 | r[2];
        *alpha = c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3];
        return 1;
    }
    for (size_t i = 0; i < sizeof(svg_colours) / sizeof(svg_colours[0]); i++) {
        size_t n = strlen(svg_colours[i].name);
        if (!strncmp(v, svg_colours[i].name, n) &&
            (v[n] == 0 || svg_space(v[n]) || v[n] == ';')) {
            *rgb = svg_colours[i].rgb;
            return 1;
        }
    }
    return 0;
}

struct svg_m {                          /* x' = a x + c y + e, y' = b x + d y + f */
    double a, b, c, d, e, f;
};

static struct svg_m svg_mul(struct svg_m p, struct svg_m q)   /* p after q */
{
    struct svg_m r;
    r.a = p.a * q.a + p.c * q.b;
    r.b = p.b * q.a + p.d * q.b;
    r.c = p.a * q.c + p.c * q.d;
    r.d = p.b * q.c + p.d * q.d;
    r.e = p.a * q.e + p.c * q.f + p.e;
    r.f = p.b * q.e + p.d * q.f + p.f;
    return r;
}

static struct svg_m svg_mat(double a, double b, double c, double d, double e,
                            double f)
{
    struct svg_m m;
    m.a = a; m.b = b; m.c = c; m.d = d; m.e = e; m.f = f;
    return m;
}

static struct svg_m svg_transform(struct svg_m m, const char *v)
{
    const double pi = 3.14159265358979323846;
    while (v && *v) {
        while (svg_space(*v) || *v == ',')
            v++;
        const char *open = strchr(v, '(');
        if (!open)
            break;
        double a[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        const char *s = open + 1, *e;
        while (n < 6 && (e = svg_num(s, &a[n])))
            s = e, n++;
        struct svg_m t = svg_mat(1, 0, 0, 1, 0, 0);
        if (!strncmp(v, "matrix", 6) && n == 6)
            t = svg_mat(a[0], a[1], a[2], a[3], a[4], a[5]);
        else if (!strncmp(v, "translate", 9) && n >= 1)
            t = svg_mat(1, 0, 0, 1, a[0], n > 1 ? a[1] : 0);
        else if (!strncmp(v, "scale", 5) && n >= 1)
            t = svg_mat(a[0], 0, 0, n > 1 ? a[1] : a[0], 0, 0);
        else if (!strncmp(v, "rotate", 6) && n >= 1) {
            double r = a[0] * pi / 180, cs = cos(r), sn = sin(r);
            t = svg_mat(cs, sn, -sn, cs, 0, 0);
            if (n >= 3)
                t = svg_mul(svg_mat(1, 0, 0, 1, a[1], a[2]),
                            svg_mul(t, svg_mat(1, 0, 0, 1, -a[1], -a[2])));
        } else if (!strncmp(v, "skewX", 5) && n >= 1)
            t = svg_mat(1, 0, tan(a[0] * pi / 180), 1, 0, 0);
        else if (!strncmp(v, "skewY", 5) && n >= 1)
            t = svg_mat(1, tan(a[0] * pi / 180), 0, 1, 0, 0);
        m = svg_mul(m, t);
        const char *close = strchr(open, ')');
        if (!close)
            break;
        v = close + 1;
    }
    return m;
}

/* --- polygons ------------------------------------------------------------ */

struct svg_pt {
    double x, y;
};

struct svg_poly {                       /* subpaths, one after another */
    struct svg_pt *pt;
    int n, cap;
    int *start;                         /* where each subpath begins */
    char *closed;
    int nsub, cap_sub;
};

static void svg_poly_free(struct svg_poly *p)
{
    free(p->pt);
    free(p->start);
    free(p->closed);
    memset(p, 0, sizeof(*p));
}

static void svg_move(struct svg_poly *p, double x, double y)
{
    int c = p->cap_sub;
    int *s = (int *)svg_grow(p->start, &c, p->nsub + 1, sizeof(int));
    if (!s)
        return;
    p->start = s;
    c = p->cap_sub;
    char *cl = (char *)svg_grow(p->closed, &c, p->nsub + 1, 1);
    if (!cl)
        return;
    p->closed = cl;
    p->cap_sub = c;
    p->start[p->nsub] = p->n;
    p->closed[p->nsub] = 0;
    p->nsub++;
    struct svg_pt *q = (struct svg_pt *)svg_grow(p->pt, &p->cap, p->n + 1,
                                                 sizeof(struct svg_pt));
    if (!q)
        return;
    p->pt = q;
    p->pt[p->n].x = x;
    p->pt[p->n].y = y;
    p->n++;
}

static void svg_line(struct svg_poly *p, double x, double y)
{
    if (!p->nsub)
        svg_move(p, x, y);
    if (p->n >= 200000)                /* (a picture of more is an attack) */
        return;
    struct svg_pt *q = (struct svg_pt *)svg_grow(p->pt, &p->cap, p->n + 1,
                                                 sizeof(struct svg_pt));
    if (!q)
        return;
    p->pt = q;
    p->pt[p->n].x = x;
    p->pt[p->n].y = y;
    p->n++;
}

static int svg_sub_len(const struct svg_poly *p, int i)
{
    return (i + 1 < p->nsub ? p->start[i + 1] : p->n) - p->start[i];
}

/* How many pieces a curve is cut into: by how long it is, in pixels */
static int svg_steps(double len_px)
{
    int n = (int)(sqrt(len_px) * 2.5) + 2;
    return n > 200 ? 200 : n;
}

static void svg_cubic(struct svg_poly *p, double x0, double y0, double x1,
                      double y1, double x2, double y2, double x3, double y3,
                      double scale)
{
    double len = (hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) +
                  hypot(x3 - x2, y3 - y2)) * scale;
    int n = svg_steps(len);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        svg_line(p, a * x0 + b * x1 + c * x2 + d * x3,
                 a * y0 + b * y1 + c * y2 + d * y3);
    }
}

/* An elliptical arc from the current point to (x, y), as the
   specification turns its endpoints into a centre (F.6.5) */
static void svg_arc(struct svg_poly *p, double x0, double y0, double rx,
                    double ry, double angle, int large, int sweep, double x,
                    double y, double scale)
{
    const double pi = 3.14159265358979323846;
    rx = fabs(rx);
    ry = fabs(ry);
    if ((x0 == x && y0 == y))
        return;
    if (rx == 0 || ry == 0) {
        svg_line(p, x, y);
        return;
    }
    double phi = angle * pi / 180, cs = cos(phi), sn = sin(phi);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
    double l = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (l > 1) {
        rx *= sqrt(l);
        ry *= sqrt(l);
    }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den > 0 && num > 0 ? sqrt(num / den) : 0;
    if (large == sweep)
        k = -k;
    double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
    double cx = cs * cx1 - sn * cy1 + (x0 + x) / 2;
    double cy = sn * cx1 + cs * cy1 + (y0 + y) / 2;
    double t1 = atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
    double t2 = atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx);
    double dt = t2 - t1;
    if (sweep && dt < 0)
        dt += 2 * pi;
    else if (!sweep && dt > 0)
        dt -= 2 * pi;
    int n = svg_steps(fabs(dt) * (rx > ry ? rx : ry) * scale) + 2;
    for (int i = 1; i <= n; i++) {
        double t = t1 + dt * i / n;
        double ex = rx * cos(t), ey = ry * sin(t);
        svg_line(p, cs * ex - sn * ey + cx, sn * ex + cs * ey + cy);
    }
    p->pt[p->n - 1].x = x;             /* exactly where it was to end */
    p->pt[p->n - 1].y = y;
}

static void svg_close(struct svg_poly *p)
{
    if (p->nsub)
        p->closed[p->nsub - 1] = 1;
}

/* An arc flag: one digit, which may stand right against the next number */
static const char *svg_flag(const char *s, int *f)
{
    while (svg_space(*s) || *s == ',')
        s++;
    if (*s != '0' && *s != '1')
        return 0;
    *f = *s == '1';
    return s + 1;
}

static void svg_path(struct svg_poly *p, const char *d, double scale)
{
    double x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;   /* cx: last control */
    char cmd = 0, prev = 0;
    const char *s = d;
    while (s && *s) {
        while (svg_space(*s) || *s == ',')
            s++;
        if (!*s)
            break;
        if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
            cmd = *s++;
            if (cmd == 'Z' || cmd == 'z') {
                svg_close(p);
                x = sx;
                y = sy;
                prev = cmd;
                cmd = 0;
                continue;
            }
        } else if (!cmd) {
            break;                     /* numbers with no command: nonsense */
        }
        int rel = cmd >= 'a';
        double bx = rel ? x : 0, by = rel ? y : 0, a[7];
        const char *e = s;
        char up = (char)(cmd & ~32);
        int need = up == 'M' || up == 'L' || up == 'T' ? 2 : up == 'H' || up == 'V' ? 1
                 : up == 'C' ? 6 : up == 'S' || up == 'Q' ? 4 : up == 'A' ? 7 : 0;
        if (!need)
            break;
        int ok = 1;
        for (int i = 0; i < need && ok; i++) {
            if (up == 'A' && (i == 3 || i == 4)) {
                int f = 0;
                e = svg_flag(e, &f);
                a[i] = f;
            } else {
                e = svg_num(e, &a[i]);
            }
            ok = e != 0;
        }
        if (!ok)
            break;
        s = e;
        switch (up) {
        case 'M':
            x = bx + a[0];
            y = by + a[1];
            svg_move(p, x, y);
            sx = x;
            sy = y;
            cmd = rel ? 'l' : 'L';     /* more pairs after a moveto: lines */
            break;
        case 'L':
            x = bx + a[0];
            y = by + a[1];
            svg_line(p, x, y);
            break;
        case 'H':
            x = bx + a[0];
            svg_line(p, x, y);
            break;
        case 'V':
            y = (rel ? y : 0) + a[0];
            svg_line(p, x, y);
            break;
        case 'C':
            svg_cubic(p, x, y, bx + a[0], by + a[1], bx + a[2], by + a[3],
                      bx + a[4], by + a[5], scale);
            cx = bx + a[2];
            cy = by + a[3];
            x = bx + a[4];
            y = by + a[5];
            break;
        case 'S': {
            char pu = (char)(prev & ~32);
            double c1x = pu == 'C' || pu == 'S' ? 2 * x - cx : x;
            double c1y = pu == 'C' || pu == 'S' ? 2 * y - cy : y;
            svg_cubic(p, x, y, c1x, c1y, bx + a[0], by + a[1], bx + a[2],
                      by + a[3], scale);
            cx = bx + a[0];
            cy = by + a[1];
            x = bx + a[2];
            y = by + a[3];
            break;
        }
        case 'Q':
        case 'T': {
            char pu = (char)(prev & ~32);
            double qx, qy, ex, ey;
            if (up == 'Q') {
                qx = bx + a[0];
                qy = by + a[1];
                ex = bx + a[2];
                ey = by + a[3];
            } else {
                qx = pu == 'Q' || pu == 'T' ? 2 * x - cx : x;
                qy = pu == 'Q' || pu == 'T' ? 2 * y - cy : y;
                ex = bx + a[0];
                ey = by + a[1];
            }
            /* A quadratic is a cubic with its controls two thirds along */
            svg_cubic(p, x, y, x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y),
                      ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex,
                      ey, scale);
            cx = qx;
            cy = qy;
            x = ex;
            y = ey;
            break;
        }
        case 'A':
            svg_arc(p, x, y, a[0], a[1], a[2], (int)a[3], (int)a[4],
                    bx + a[5], by + a[6], scale);
            x = bx + a[5];
            y = by + a[6];
            break;
        }
        prev = cmd;
    }
}

static void svg_ellipse(struct svg_poly *p, double cx, double cy, double rx,
                        double ry, double scale)
{
    const double pi = 3.14159265358979323846;
    if (rx <= 0 || ry <= 0)
        return;
    int n = svg_steps(2 * pi * (rx > ry ? rx : ry) * scale) * 2;
    if (n < 16)
        n = 16;
    svg_move(p, cx + rx, cy);
    for (int i = 1; i < n; i++)
        svg_line(p, cx + rx * cos(2 * pi * i / n), cy + ry * sin(2 * pi * i / n));
    svg_close(p);
}

/* A quarter of an ellipse, for a rectangle's rounded corners */
static void svg_corner(struct svg_poly *p, double cx, double cy, double rx,
                       double ry, double from, double scale)
{
    const double pi = 3.14159265358979323846;
    int n = svg_steps(pi / 2 * (rx > ry ? rx : ry) * scale) + 2;
    for (int i = 0; i <= n; i++) {
        double t = from + pi / 2 * i / n;
        svg_line(p, cx + rx * cos(t), cy + ry * sin(t));
    }
}

/* --- filling ------------------------------------------------------------- */

#define SVG_SUB 4                       /* scanlines to a row of pixels */

struct svg_edge {
    double x0, y0, x1, y1;
    int dir;                            /* +1 down, -1 up */
};

struct svg_canvas {
    int w, h;
    unsigned char *rgba;
    float *cover;                       /* what the shape covers of each pixel */
    struct svg_edge *edges;
    int nedges, cap_edges;
    double *xs;                         /* crossings of one scanline */
    int *ws;
    int cap_xs;
};

/* Takes a polygon's edges, in pixels, through 'm' */
static void svg_add_edges(struct svg_canvas *cv, const struct svg_poly *p,
                          struct svg_m m)
{
    for (int s = 0; s < p->nsub; s++) {
        int n = svg_sub_len(p, s), b = p->start[s];
        if (n < 2)
            continue;
        for (int i = 0; i < n; i++) {
            const struct svg_pt *u = &p->pt[b + i], *v = &p->pt[b + (i + 1) % n];
            double x0 = m.a * u->x + m.c * u->y + m.e, y0 = m.b * u->x + m.d * u->y + m.f;
            double x1 = m.a * v->x + m.c * v->y + m.e, y1 = m.b * v->x + m.d * v->y + m.f;
            if (y0 == y1 || !isfinite(x0) || !isfinite(y0) || !isfinite(x1) ||
                !isfinite(y1))
                continue;
            struct svg_edge *e = (struct svg_edge *)svg_grow(
                cv->edges, &cv->cap_edges, cv->nedges + 1, sizeof(struct svg_edge));
            if (!e)
                return;
            cv->edges = e;
            struct svg_edge *x = &e[cv->nedges++];
            if (y0 < y1) {
                x->x0 = x0; x->y0 = y0; x->x1 = x1; x->y1 = y1; x->dir = 1;
            } else {
                x->x0 = x1; x->y0 = y1; x->x1 = x0; x->y1 = y0; x->dir = -1;
            }
        }
    }
}

/* Adds to a row of coverage what [a, b) of it is inside, at one
   scanline's weight */
static void svg_span(float *row, int w, double a, double b)
{
    if (!(a >= 0)) a = 0;
    if (!(b <= w)) b = w;
    if (!(b > a))
        return;
    const float k = 1.0f / SVG_SUB;
    int ia = (int)a, ib = (int)b;
    if (ia == ib) {
        row[ia] += (float)(b - a) * k;
        return;
    }
    row[ia] += (float)(ia + 1 - a) * k;
    for (int i = ia + 1; i < ib; i++)
        row[i] += k;
    if (ib < w)
        row[ib] += (float)(b - ib) * k;
}

/* Fills cv->cover from the edges, by the rule; the box it touched */
static void svg_rasterize(struct svg_canvas *cv, int evenodd, int *y_lo, int *y_hi)
{
    double top = 1e30, bottom = -1e30;
    for (int i = 0; i < cv->nedges; i++) {
        if (cv->edges[i].y0 < top) top = cv->edges[i].y0;
        if (cv->edges[i].y1 > bottom) bottom = cv->edges[i].y1;
    }
    /* (written so that a NaN, or a number past what an int holds, ends
       up inside the picture or empties the range) */
    int r0 = !(top >= 0) ? 0 : top >= cv->h ? cv->h : (int)top;
    int r1 = !(bottom < cv->h) ? cv->h - 1 : bottom < 0 ? -1 : (int)bottom;
    *y_lo = r0;
    *y_hi = r1;
    if (!cv->nedges)
        *y_hi = -1;
    for (int r = r0; r <= r1; r++) {
        float *row = cv->cover + (size_t)r * cv->w;
        for (int s = 0; s < SVG_SUB; s++) {
            double y = r + (s + 0.5) / SVG_SUB;
            int n = 0;
            for (int i = 0; i < cv->nedges; i++) {
                const struct svg_edge *e = &cv->edges[i];
                if (y < e->y0 || y >= e->y1)
                    continue;
                if (n >= cv->cap_xs) {
                    int c = cv->cap_xs ? cv->cap_xs * 2 : 64;
                    double *xs = (double *)realloc(cv->xs, (size_t)c * sizeof(double));
                    if (!xs)
                        return;
                    cv->xs = xs;
                    int *ws = (int *)realloc(cv->ws, (size_t)c * sizeof(int));
                    if (!ws)
                        return;
                    cv->ws = ws;
                    cv->cap_xs = c;
                }
                cv->xs[n] = e->x0 + (y - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                cv->ws[n] = e->dir;
                n++;
            }
            /* (few crossings to a scanline: insertion sort will do) */
            for (int i = 1; i < n; i++) {
                double x = cv->xs[i];
                int wv = cv->ws[i], j = i - 1;
                while (j >= 0 && cv->xs[j] > x) {
                    cv->xs[j + 1] = cv->xs[j];
                    cv->ws[j + 1] = cv->ws[j];
                    j--;
                }
                cv->xs[j + 1] = x;
                cv->ws[j + 1] = wv;
            }
            int wind = 0;
            for (int i = 0; i + 1 < n; i++) {
                wind += cv->ws[i];
                int in = evenodd ? (i + 1) % 2 : wind != 0;
                if (in)
                    svg_span(row, cv->w, cv->xs[i], cv->xs[i + 1]);
            }
        }
    }
}

/* --- painting ------------------------------------------------------------ */

struct svg_stop {
    double at;
    unsigned rgb;
    double alpha;
};

struct svg_paint {
    int kind;                           /* 0 none, 1 colour, 2 linear, 3 radial */
    unsigned rgb;
    double alpha;
    struct svg_stop stops[16];
    int nstops;
    struct svg_m inv;                   /* pixels -> the gradient's space */
    double x1, y1, x2, y2, r;           /* linear: the line; radial: centre, r */
    int spread;                         /* 0 pad, 1 reflect, 2 repeat */
};

static int svg_invert(struct svg_m m, struct svg_m *out)
{
    double det = m.a * m.d - m.b * m.c;
    if (fabs(det) < 1e-12)
        return 0;
    out->a = m.d / det;
    out->b = -m.b / det;
    out->c = -m.c / det;
    out->d = m.a / det;
    out->e = (m.c * m.f - m.d * m.e) / det;
    out->f = (m.b * m.e - m.a * m.f) / det;
    return 1;
}

static void svg_stop_colour(const struct svg_paint *p, double t, unsigned *rgb,
                            double *alpha)
{
    if (p->spread == 2)
        t -= floor(t);
    else if (p->spread == 1) {
        t = fmod(fabs(t), 2);
        if (t > 1)
            t = 2 - t;
    }
    if (t <= p->stops[0].at) {
        *rgb = p->stops[0].rgb;
        *alpha = p->stops[0].alpha;
        return;
    }
    for (int i = 1; i < p->nstops; i++) {
        if (t <= p->stops[i].at) {
            const struct svg_stop *a = &p->stops[i - 1], *b = &p->stops[i];
            double k = b->at > a->at ? (t - a->at) / (b->at - a->at) : 1;
            unsigned r = 0;
            for (int sh = 16; sh >= 0; sh -= 8) {
                double c = (a->rgb >> sh & 255) * (1 - k) + (b->rgb >> sh & 255) * k;
                r |= (unsigned)(c + 0.5) << sh;
            }
            *rgb = r;
            *alpha = a->alpha * (1 - k) + b->alpha * k;
            return;
        }
    }
    *rgb = p->stops[p->nstops - 1].rgb;
    *alpha = p->stops[p->nstops - 1].alpha;
}

/* Lays the paint on where cv->cover says, and clears the cover */
static void svg_composite(struct svg_canvas *cv, const struct svg_paint *p,
                          double opacity, int y_lo, int y_hi)
{
    for (int y = y_lo; y <= y_hi; y++) {
        float *row = cv->cover + (size_t)y * cv->w;
        for (int x = 0; x < cv->w; x++) {
            float c = row[x];
            if (c <= 0)
                continue;
            row[x] = 0;
            if (c > 1)
                c = 1;
            unsigned rgb = p->rgb;
            double a = p->alpha;
            if (p->kind >= 2) {
                double px = x + 0.5, py = y + 0.5;
                double gx = p->inv.a * px + p->inv.c * py + p->inv.e;
                double gy = p->inv.b * px + p->inv.d * py + p->inv.f;
                double t;
                if (p->kind == 2) {
                    double dx = p->x2 - p->x1, dy = p->y2 - p->y1;
                    double l2 = dx * dx + dy * dy;
                    t = l2 > 0 ? ((gx - p->x1) * dx + (gy - p->y1) * dy) / l2 : 0;
                } else {
                    t = p->r > 0 ? hypot(gx - p->x1, gy - p->y1) / p->r : 0;
                }
                svg_stop_colour(p, t, &rgb, &a);
            }
            double sa = a * opacity * c;
            if (sa <= 0)
                continue;
            unsigned char *o = cv->rgba + ((size_t)y * cv->w + x) * 4;
            double da = o[3] / 255.0, oa = sa + da * (1 - sa);
            for (int i = 0; i < 3; i++) {
                double sc = (rgb >> (16 - 8 * i) & 255);
                double v = (sc * sa + o[i] * da * (1 - sa)) / oa;
                o[i] = (unsigned char)(v + 0.5);
            }
            o[3] = (unsigned char)(oa * 255 + 0.5);
        }
    }
}

/* --- the style of an element --------------------------------------------- */

struct svg_style {
    char fill[96], stroke[96];          /* as written: a colour, none, url() */
    double fill_op, stroke_op, opacity, width, miter;
    int evenodd, cap, join;             /* cap: 0 butt 1 round 2 square;
                                           join: 0 miter 1 round 2 bevel */
    unsigned colour;                    /* currentColor */
    int hidden;
};

static void svg_prop(struct svg_style *s, const char *name, size_t nl,
                     const char *v, size_t vl, int own)
{
    char val[96];
    if (vl >= sizeof(val))
        vl = sizeof(val) - 1;
    memcpy(val, v, vl);
    val[vl] = 0;
    while (vl && svg_space(val[vl - 1]))
        val[--vl] = 0;
    if (!strcmp(val, "inherit"))
        return;
#define SVG_IS(n) (nl == sizeof(n) - 1 && !strncmp(name, n, nl))
    if (SVG_IS("fill"))
        strcpy(s->fill, val);
    else if (SVG_IS("stroke"))
        strcpy(s->stroke, val);
    else if (SVG_IS("fill-opacity"))
        s->fill_op = atof(val);
    else if (SVG_IS("stroke-opacity"))
        s->stroke_op = atof(val);
    else if (SVG_IS("opacity") && own)
        s->opacity *= atof(val);
    else if (SVG_IS("stroke-width"))
        s->width = svg_len(val, 100, 1);
    else if (SVG_IS("stroke-miterlimit"))
        s->miter = atof(val);
    else if (SVG_IS("fill-rule"))
        s->evenodd = !strcmp(val, "evenodd");
    else if (SVG_IS("stroke-linecap"))
        s->cap = !strcmp(val, "round") ? 1 : !strcmp(val, "square") ? 2 : 0;
    else if (SVG_IS("stroke-linejoin"))
        s->join = !strcmp(val, "round") ? 1 : !strcmp(val, "bevel") ? 2 : 0;
    else if (SVG_IS("color")) {
        double a;
        unsigned rgb;
        if (svg_colour(val, &rgb, &a) == 1)
            s->colour = rgb;
    } else if ((SVG_IS("display") && !strcmp(val, "none")) ||
               (SVG_IS("visibility") && strcmp(val, "visible")))
        s->hidden = 1;
    else if (SVG_IS("visibility"))
        s->hidden = 0;
#undef SVG_IS
}

/* "a:b; c:d" */
static void svg_decls(struct svg_style *s, const char *d, int own)
{
    while (d && *d) {
        while (svg_space(*d) || *d == ';')
            d++;
        const char *colon = strchr(d, ':');
        if (!colon)
            break;
        const char *end = strchr(colon, ';');
        if (!end)
            end = colon + strlen(colon);
        const char *ne = colon;
        while (ne > d && svg_space(ne[-1]))
            ne--;
        const char *v = colon + 1;
        while (svg_space(*v))
            v++;
        svg_prop(s, d, (size_t)(ne - d), v, (size_t)(end - v), own);
        d = end;
    }
}

/* An element's style: its parent's, then its presentation attributes,
   then the <style> rules that match it, then its own style="" */
static void svg_style_of(const struct svg_doc *d, int n, struct svg_style *s)
{
    static const char *props[] = { "fill", "stroke", "fill-opacity",
        "stroke-opacity", "opacity", "stroke-width", "stroke-miterlimit",
        "fill-rule", "stroke-linecap", "stroke-linejoin", "color", "display",
        "visibility" };
    /* (a group's opacity is laid on its parts: exact where they do not
       overlap, which in icons they mostly do not) */
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        const char *v = svg_attr(d, n, props[i]);
        if (v)
            svg_prop(s, props[i], strlen(props[i]), v, strlen(v), 1);
    }
    const char *cls = svg_attr(d, n, "class"), *id = svg_attr(d, n, "id");
    for (int r = 0; r < d->nrules; r++) {
        const struct svg_rule *x = &d->rules[r];
        int match = 0;
        if (x->kind == 't')
            match = !strcmp(x->name, d->nodes[n].tag);
        else if (x->kind == '#')
            match = id && !strcmp(x->name, id);
        else if (cls) {
            size_t l = strlen(x->name);
            for (const char *c = cls; (c = strstr(c, x->name)); c += l)
                if ((c == cls || svg_space(c[-1])) && (!c[l] || svg_space(c[l]))) {
                    match = 1;
                    break;
                }
        }
        if (match)
            svg_decls(s, x->decls, 1);
    }
    svg_decls(s, svg_attr(d, n, "style"), 1);
}

/* --- drawing ------------------------------------------------------------- */

struct svg_ctx {
    struct svg_doc *doc;
    struct svg_canvas cv;
    int depth;                          /* of <use>s inside <use>s */
};

static void svg_poly_bounds(const struct svg_poly *p, double *b)
{
    b[0] = b[1] = 1e30;
    b[2] = b[3] = -1e30;
    for (int i = 0; i < p->n; i++) {
        if (p->pt[i].x < b[0]) b[0] = p->pt[i].x;
        if (p->pt[i].y < b[1]) b[1] = p->pt[i].y;
        if (p->pt[i].x > b[2]) b[2] = p->pt[i].x;
        if (p->pt[i].y > b[3]) b[3] = p->pt[i].y;
    }
}

static double svg_frac(const char *v, double dflt)
{
    double x;
    const char *e;
    if (!v || !(e = svg_num(v, &x)))
        return dflt;
    return *e == '%' ? x / 100 : x;
}

/* A gradient's paint for a shape with these bounds, under 'm' */
static int svg_gradient(struct svg_ctx *c, int g, const double *bbox,
                        struct svg_m m, struct svg_paint *p)
{
    struct svg_doc *d = c->doc;
    int linear = !strcmp(d->nodes[g].tag, "linearGradient");
    /* Stops and attributes may come from the gradient it refers to */
    int src = g, stops_from = -1;
    for (int hop = 0; hop < 8 && src >= 0; hop++) {
        for (int k = d->nodes[src].first_child; k >= 0; k = d->nodes[k].next)
            if (!strcmp(d->nodes[k].tag, "stop")) {
                stops_from = src;
                break;
            }
        if (stops_from >= 0)
            break;
        src = svg_ref(d, svg_attr(d, src, "href"));
    }
    p->nstops = 0;
    if (stops_from >= 0)
        for (int k = d->nodes[stops_from].first_child; k >= 0 && p->nstops < 16;
             k = d->nodes[k].next) {
            if (strcmp(d->nodes[k].tag, "stop"))
                continue;
            struct svg_style st;
            memset(&st, 0, sizeof(st));
            strcpy(st.fill, "black");
            st.fill_op = 1;
            const char *sc = svg_attr(d, k, "stop-color");
            const char *so = svg_attr(d, k, "stop-opacity");
            /* (stop-color may be in its style="" too) */
            const char *style = svg_attr(d, k, "style");
            char colour[96] = "black";
            double op = so ? atof(so) : 1;
            if (sc)
                strncpy(colour, sc, sizeof(colour) - 1);
            if (style) {
                const char *x = strstr(style, "stop-color:");
                if (x) {
                    x += 11;
                    size_t l = strcspn(x, ";");
                    if (l > sizeof(colour) - 1)
                        l = sizeof(colour) - 1;
                    memcpy(colour, x, l);
                    colour[l] = 0;
                }
                x = strstr(style, "stop-opacity:");
                if (x)
                    op = atof(x + 13);
            }
            struct svg_stop *s = &p->stops[p->nstops];
            s->at = svg_frac(svg_attr(d, k, "offset"), 0);
            if (s->at < 0) s->at = 0;
            if (s->at > 1) s->at = 1;
            if (p->nstops && s->at < p->stops[p->nstops - 1].at)
                s->at = p->stops[p->nstops - 1].at;
            double a = 1;
            if (svg_colour(colour, &s->rgb, &a) != 1)
                s->rgb = 0, a = 1;
            s->alpha = a * (op < 0 ? 0 : op > 1 ? 1 : op);
            p->nstops++;
        }
    if (!p->nstops)
        return 0;
    if (p->nstops == 1) {
        p->kind = 1;
        p->rgb = p->stops[0].rgb;
        p->alpha = p->stops[0].alpha;
        return 1;
    }
    /* The attributes: the gradient's own, else those it refers to */
    const char *names[] = { "x1", "y1", "x2", "y2", "cx", "cy", "r",
                            "gradientUnits", "gradientTransform", "spreadMethod" };
    const char *v[10];
    for (int i = 0; i < 10; i++) {
        v[i] = 0;
        for (int at = g, hop = 0; at >= 0 && hop < 8 && !v[i]; hop++) {
            v[i] = svg_attr(d, at, names[i]);
            at = svg_ref(d, svg_attr(d, at, "href"));
        }
    }
    int user = v[7] && !strcmp(v[7], "userSpaceOnUse");
    struct svg_m gm = svg_mat(1, 0, 0, 1, 0, 0);
    if (!user) {
        double bw = bbox[2] - bbox[0], bh = bbox[3] - bbox[1];
        if (bw <= 0 || bh <= 0)
            return 0;
        gm = svg_mat(bw, 0, 0, bh, bbox[0], bbox[1]);
    }
    gm = svg_transform(gm, v[8]);
    if (!svg_invert(svg_mul(m, gm), &p->inv))
        return 0;
    p->spread = v[9] && !strcmp(v[9], "reflect") ? 1
              : v[9] && !strcmp(v[9], "repeat") ? 2 : 0;
    double ref = user ? 100 : 1;       /* (percentages of the box, or not) */
    if (linear) {
        p->kind = 2;
        p->x1 = user ? svg_len(v[0], ref, 0) : svg_frac(v[0], 0);
        p->y1 = user ? svg_len(v[1], ref, 0) : svg_frac(v[1], 0);
        p->x2 = user ? svg_len(v[2], ref, 100) : svg_frac(v[2], 1);
        p->y2 = user ? svg_len(v[3], ref, 0) : svg_frac(v[3], 0);
    } else {
        p->kind = 3;
        p->x1 = user ? svg_len(v[4], ref, 50) : svg_frac(v[4], 0.5);
        p->y1 = user ? svg_len(v[5], ref, 50) : svg_frac(v[5], 0.5);
        p->r = user ? svg_len(v[6], ref, 50) : svg_frac(v[6], 0.5);
    }
    return 1;
}

static int svg_paint_of(struct svg_ctx *c, const char *spec, double op,
                        const struct svg_style *st, const double *bbox,
                        struct svg_m m, struct svg_paint *p)
{
    memset(p, 0, sizeof(*p));
    double a = 1;
    int k = svg_colour(spec, &p->rgb, &a);
    if (k == 2) {
        p->rgb = st->colour;
        k = 1;
    }
    if (k == 3) {
        int g = svg_ref(c->doc, spec);
        if (g >= 0 && (!strcmp(c->doc->nodes[g].tag, "linearGradient") ||
                       !strcmp(c->doc->nodes[g].tag, "radialGradient")) &&
            svg_gradient(c, g, bbox, m, p))
            return 1;
        /* "url(#x) red": the colour after it, when x is not there */
        const char *after = strchr(spec, ')');
        if (!after || svg_colour(after + 1, &p->rgb, &a) != 1)
            return 0;
        k = 1;
    }
    if (k != 1)
        return 0;
    p->kind = 1;
    p->alpha = a * (op < 0 ? 0 : op > 1 ? 1 : op);
    return 1;
}

/* One of the polygons a stroke is made of, turned to the same
   orientation as all the others, so that nonzero makes their union */
static void svg_piece(struct svg_poly *out, const struct svg_pt *q, int n)
{
    double area = 0;
    for (int i = 0; i < n; i++)
        area += q[i].x * q[(i + 1) % n].y - q[(i + 1) % n].x * q[i].y;
    if (fabs(area) < 1e-12)
        return;
    svg_move(out, q[area > 0 ? 0 : n - 1].x, q[area > 0 ? 0 : n - 1].y);
    for (int i = 1; i < n; i++) {
        int k = area > 0 ? i : n - 1 - i;
        svg_line(out, q[k].x, q[k].y);
    }
    svg_close(out);
}

static void svg_dot(struct svg_poly *out, double x, double y, double r, double scale)
{
    struct svg_poly c;
    memset(&c, 0, sizeof(c));
    svg_ellipse(&c, x, y, r, r, scale);
    if (c.n >= 3)
        svg_piece(out, c.pt, c.n);
    svg_poly_free(&c);
}

/* The polygons of a stroke along 'p', in its own (user) space */
static void svg_stroke(const struct svg_poly *p, const struct svg_style *st,
                       double scale, struct svg_poly *out)
{
    double hw = st->width / 2;
    if (hw <= 0)
        return;
    for (int s = 0; s < p->nsub; s++) {
        int n = svg_sub_len(p, s), b = p->start[s], closed = p->closed[s];
        /* The points without repeats: a zero-length piece has no direction */
        struct svg_pt *v = (struct svg_pt *)malloc(sizeof(struct svg_pt) * (size_t)(n + 1));
        if (!v)
            return;
        int m = 0;
        for (int i = 0; i < n; i++)
            if (!m || fabs(p->pt[b + i].x - v[m - 1].x) > 1e-9 ||
                fabs(p->pt[b + i].y - v[m - 1].y) > 1e-9)
                v[m++] = p->pt[b + i];
        if (closed && m > 1 && fabs(v[0].x - v[m - 1].x) < 1e-9 &&
            fabs(v[0].y - v[m - 1].y) < 1e-9)
            m--;
        if (m == 1) {
            if (st->cap == 1)
                svg_dot(out, v[0].x, v[0].y, hw, scale);
            else if (st->cap == 2) {
                struct svg_pt q[4] = { {v[0].x - hw, v[0].y - hw}, {v[0].x + hw, v[0].y - hw},
                                       {v[0].x + hw, v[0].y + hw}, {v[0].x - hw, v[0].y + hw} };
                svg_piece(out, q, 4);
            }
            free(v);
            continue;
        }
        int segs = closed ? m : m - 1;
        for (int i = 0; i < segs; i++) {
            struct svg_pt a = v[i], c = v[(i + 1) % m];
            double dx = c.x - a.x, dy = c.y - a.y, l = hypot(dx, dy);
            double nx = -dy / l * hw, ny = dx / l * hw;
            double ex = 0, ey = 0, sx = 0, sy = 0;
            /* A square cap is the segment carried on half the width */
            if (!closed && st->cap == 2) {
                if (i == 0) sx = dx / l * hw, sy = dy / l * hw;
                if (i == segs - 1) ex = dx / l * hw, ey = dy / l * hw;
            }
            struct svg_pt q[4] = { {a.x + nx - sx, a.y + ny - sy}, {c.x + nx + ex, c.y + ny + ey},
                                   {c.x - nx + ex, c.y - ny + ey}, {a.x - nx - sx, a.y - ny - sy} };
            svg_piece(out, q, 4);
        }
        /* The joins: every inner point, and the first of a closed path */
        for (int i = closed ? 0 : 1; i < (closed ? m : m - 1); i++) {
            struct svg_pt pp = v[(i + m - 1) % m], o = v[i], nn = v[(i + 1) % m];
            double d0x = o.x - pp.x, d0y = o.y - pp.y, d1x = nn.x - o.x, d1y = nn.y - o.y;
            double l0 = hypot(d0x, d0y), l1 = hypot(d1x, d1y);
            d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
            double cross = d0x * d1y - d0y * d1x;
            if (fabs(cross) < 1e-9 && d0x * d1x + d0y * d1y > 0)
                continue;              /* straight on: nothing to fill */
            if (st->join == 1) {
                svg_dot(out, o.x, o.y, hw, scale);
                continue;
            }
            /* The outer side: where the two edges part */
            double side = cross > 0 ? -1 : 1;
            struct svg_pt e0 = { o.x - d0y * hw * side, o.y + d0x * hw * side };
            struct svg_pt e1 = { o.x - d1y * hw * side, o.y + d1x * hw * side };
            double cosv = d0x * d1x + d0y * d1y;
            double ml = 1 / sqrt((1 + cosv) / 2 > 1e-12 ? (1 + cosv) / 2 : 1e-12);
            if (st->join == 0 && ml <= (st->miter >= 1 ? st->miter : 4)) {
                double bx = (e0.x + e1.x) / 2 - o.x, by = (e0.y + e1.y) / 2 - o.y;
                double bl = hypot(bx, by);
                if (bl > 1e-12) {
                    struct svg_pt tip = { o.x + bx / bl * hw * ml, o.y + by / bl * hw * ml };
                    struct svg_pt q[4] = { o, e0, tip, e1 };
                    svg_piece(out, q, 4);
                    continue;
                }
            }
            struct svg_pt q[3] = { o, e0, e1 };
            svg_piece(out, q, 3);
        }
        if (!closed && st->cap == 1) {
            svg_dot(out, v[0].x, v[0].y, hw, scale);
            svg_dot(out, v[m - 1].x, v[m - 1].y, hw, scale);
        }
        free(v);
    }
}

/* Fills and strokes a shape: 'p' in user space, 'm' to pixels */
static void svg_draw_shape(struct svg_ctx *c, const struct svg_poly *p,
                           const struct svg_style *st, struct svg_m m, int fill)
{
    if (st->hidden || p->n < 2)
        return;
    double bbox[4];
    svg_poly_bounds(p, bbox);
    double scale = sqrt(fabs(m.a * m.d - m.b * m.c));
    struct svg_paint paint;
    int lo, hi;
    if (fill && svg_paint_of(c, st->fill, st->fill_op, st, bbox, m, &paint)) {
        c->cv.nedges = 0;
        svg_add_edges(&c->cv, p, m);
        svg_rasterize(&c->cv, st->evenodd, &lo, &hi);
        svg_composite(&c->cv, &paint, st->opacity, lo, hi);
    }
    if (st->width > 0 &&
        svg_paint_of(c, st->stroke, st->stroke_op, st, bbox, m, &paint)) {
        struct svg_poly out;
        memset(&out, 0, sizeof(out));
        svg_stroke(p, st, scale, &out);
        c->cv.nedges = 0;
        svg_add_edges(&c->cv, &out, m);
        svg_rasterize(&c->cv, 0, &lo, &hi);
        svg_composite(&c->cv, &paint, st->opacity, lo, hi);
        svg_poly_free(&out);
    }
}

/* The transform a viewBox and preserveAspectRatio make of a viewport */
static struct svg_m svg_viewbox(const char *vb, const char *par, double x,
                                double y, double w, double h)
{
    double v[4];
    const char *s = vb;
    for (int i = 0; i < 4; i++)
        if (!s || !(s = svg_num(s, &v[i])))
            return svg_mat(1, 0, 0, 1, x, y);
    if (v[2] <= 0 || v[3] <= 0)
        return svg_mat(1, 0, 0, 1, x, y);
    double sx = w / v[2], sy = h / v[3];
    double ax = 0.5, ay = 0.5;
    if (par && strstr(par, "none"))
        return svg_mat(sx, 0, 0, sy, x - v[0] * sx, y - v[1] * sy);
    if (par) {
        if (strstr(par, "xMin")) ax = 0;
        if (strstr(par, "xMax")) ax = 1;
        if (strstr(par, "YMin")) ay = 0;
        if (strstr(par, "YMax")) ay = 1;
    }
    double k = par && strstr(par, "slice") ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    return svg_mat(k, 0, 0, k, x + (w - v[2] * k) * ax - v[0] * k,
                   y + (h - v[3] * k) * ay - v[1] * k);
}

static void svg_node_draw(struct svg_ctx *c, int n, struct svg_m m,
                          struct svg_style st, int depth);

static void svg_children(struct svg_ctx *c, int n, struct svg_m m,
                         const struct svg_style *st, int depth)
{
    for (int k = c->doc->nodes[n].first_child; k >= 0; k = c->doc->nodes[k].next)
        svg_node_draw(c, k, m, *st, depth + 1);
}

static void svg_node_draw(struct svg_ctx *c, int n, struct svg_m m,
                          struct svg_style st, int depth)
{
    struct svg_doc *d = c->doc;
    const char *tag = d->nodes[n].tag;
    if (depth > SVG_MAX_DEPTH)
        return;
    /* What is never drawn where it stands */
    if (!strcmp(tag, "defs") || !strcmp(tag, "symbol") || !strcmp(tag, "style") ||
        !strcmp(tag, "linearGradient") || !strcmp(tag, "radialGradient") ||
        !strcmp(tag, "clipPath") || !strcmp(tag, "mask") || !strcmp(tag, "title") ||
        !strcmp(tag, "desc") || !strcmp(tag, "metadata") || !strcmp(tag, "pattern") ||
        !strcmp(tag, "marker") || !strcmp(tag, "filter") || !strcmp(tag, "text"))
        return;
    svg_style_of(d, n, &st);
    if (st.hidden)
        return;
    m = svg_transform(m, svg_attr(d, n, "transform"));
    double scale = sqrt(fabs(m.a * m.d - m.b * m.c));
    struct svg_poly p;
    memset(&p, 0, sizeof(p));
    int shape = 1, fill = 1;
#define A(name, dflt) svg_len(svg_attr(d, n, name), 100, dflt)
    if (!strcmp(tag, "g") || !strcmp(tag, "a") || !strcmp(tag, "switch")) {
        shape = 0;
        svg_children(c, n, m, &st, depth);
    } else if (!strcmp(tag, "svg")) {
        /* A nested viewport (the outermost was done by the caller) */
        shape = 0;
        if (d->nodes[n].parent >= 0) {
            double x = A("x", 0), y = A("y", 0), w = A("width", 100), h = A("height", 100);
            m = svg_mul(m, svg_viewbox(svg_attr(d, n, "viewBox"),
                                       svg_attr(d, n, "preserveAspectRatio"), x, y, w, h));
        }
        svg_children(c, n, m, &st, depth);
    } else if (!strcmp(tag, "use")) {
        shape = 0;
        int ref = svg_ref(d, svg_attr(d, n, "href"));
        if (ref >= 0 && ref != n && c->depth < 8) {
            struct svg_m u = svg_mul(m, svg_mat(1, 0, 0, 1, A("x", 0), A("y", 0)));
            c->depth++;
            if (!strcmp(d->nodes[ref].tag, "symbol")) {
                struct svg_style s2 = st;
                svg_style_of(d, ref, &s2);
                double w = A("width", 100), h = A("height", 100);
                u = svg_mul(u, svg_viewbox(svg_attr(d, ref, "viewBox"),
                                           svg_attr(d, ref, "preserveAspectRatio"),
                                           0, 0, w, h));
                if (!s2.hidden)
                    svg_children(c, ref, u, &s2, depth);
            } else {
                svg_node_draw(c, ref, u, st, depth + 1);
            }
            c->depth--;
        }
    } else if (!strcmp(tag, "rect")) {
        double x = A("x", 0), y = A("y", 0), w = A("width", 0), h = A("height", 0);
        const char *rxs = svg_attr(d, n, "rx"), *rys = svg_attr(d, n, "ry");
        double rx = svg_len(rxs ? rxs : rys, 100, 0), ry = svg_len(rys ? rys : rxs, 100, 0);
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (w > 0 && h > 0) {
            const double pi = 3.14159265358979323846;
            if (rx > 0 && ry > 0) {
                svg_move(&p, x + rx, y);
                svg_corner(&p, x + w - rx, y + ry, rx, ry, -pi / 2, scale);
                svg_corner(&p, x + w - rx, y + h - ry, rx, ry, 0, scale);
                svg_corner(&p, x + rx, y + h - ry, rx, ry, pi / 2, scale);
                svg_corner(&p, x + rx, y + ry, rx, ry, pi, scale);
            } else {
                svg_move(&p, x, y);
                svg_line(&p, x + w, y);
                svg_line(&p, x + w, y + h);
                svg_line(&p, x, y + h);
            }
            svg_close(&p);
        }
    } else if (!strcmp(tag, "circle")) {
        double r = A("r", 0);
        svg_ellipse(&p, A("cx", 0), A("cy", 0), r, r, scale);
    } else if (!strcmp(tag, "ellipse")) {
        svg_ellipse(&p, A("cx", 0), A("cy", 0), A("rx", 0), A("ry", 0), scale);
    } else if (!strcmp(tag, "line")) {
        svg_move(&p, A("x1", 0), A("y1", 0));
        svg_line(&p, A("x2", 0), A("y2", 0));
        fill = 0;                      /* a line has no inside */
    } else if (!strcmp(tag, "polyline") || !strcmp(tag, "polygon")) {
        const char *s = svg_attr(d, n, "points");
        double x, y;
        int first = 1;
        while (s && (s = svg_num(s, &x)) && (s = svg_num(s, &y))) {
            if (first)
                svg_move(&p, x, y);
            else
                svg_line(&p, x, y);
            first = 0;
        }
        if (!strcmp(tag, "polygon"))
            svg_close(&p);
    } else if (!strcmp(tag, "path")) {
        svg_path(&p, svg_attr(d, n, "d"), scale);
    } else {
        shape = 0;                     /* unknown: left out, children and all */
    }
#undef A
    if (shape)
        svg_draw_shape(c, &p, &st, m, fill);
    svg_poly_free(&p);
}

/* The picture's own size, in pixels: width and height, else the
   viewBox's, else 300 by 150 as for any replaced element. 0, or -1 when
   it is not an SVG. */
static int svg_natural(struct svg_doc *d, double *w, double *h)
{
    if (!d->nnodes || strcmp(d->nodes[0].tag, "svg"))
        return -1;
    double vb[4] = {0, 0, 0, 0};
    const char *s = svg_attr(d, 0, "viewBox");
    int have_vb = 1;
    for (int i = 0; i < 4; i++)
        if (!s || !(s = svg_num(s, &vb[i])))
            have_vb = 0;
    if (have_vb && (vb[2] <= 0 || vb[3] <= 0))
        have_vb = 0;
    const char *ws = svg_attr(d, 0, "width"), *hs = svg_attr(d, 0, "height");
    /* (a percentage has nothing to be a percentage of here) */
    if (ws && strchr(ws, '%')) ws = 0;
    if (hs && strchr(hs, '%')) hs = 0;
    *w = svg_len(ws, 0, 0);
    *h = svg_len(hs, 0, 0);
    if (*w <= 0 && *h <= 0) {
        *w = have_vb ? vb[2] : 300;
        *h = have_vb ? vb[3] : 150;
    } else if (*w <= 0) {
        *w = have_vb ? *h * vb[2] / vb[3] : 300;
    } else if (*h <= 0) {
        *h = have_vb ? *w * vb[3] / vb[2] : 150;
    }
    return 0;
}

static void svg_doc_free(struct svg_doc *d)
{
    free(d->buf);
    free(d->nodes);
    free(d->attrs);
    free(d->rules);
    memset(d, 0, sizeof(*d));
}

static int svg_load(struct svg_doc *d, const char *text, unsigned len)
{
    memset(d, 0, sizeof(*d));
    d->buf = (char *)malloc((size_t)len + 1);
    if (!d->buf)
        return -1;
    memcpy(d->buf, text, len);
    d->buf[len] = 0;
    for (unsigned i = 0; i < len; i++)
        if (!d->buf[i])
            d->buf[i] = ' ';           /* a NUL inside must not end it early */
    if (svg_parse_xml(d) < 0) {
        svg_doc_free(d);
        return -1;
    }
    svg_parse_styles(d);
    return 0;
}

/* Whether these bytes are an SVG picture at all: an <svg> element first,
   after the XML declaration, comments and a doctype */
static int svg_sniff(const char *t, unsigned len)
{
    unsigned i = 0;
    for (;;) {
        while (i < len && (svg_space(t[i]) || (unsigned char)t[i] == 0xEF ||
                           (unsigned char)t[i] == 0xBB || (unsigned char)t[i] == 0xBF))
            i++;
        if (i + 4 >= len || t[i] != '<')
            return 0;
        if (t[i + 1] == '?' || t[i + 1] == '!') {
            while (i < len && t[i] != '>')
                i++;
            i++;
            continue;
        }
        unsigned j = i + 1;
        while (j < len && t[j] != ' ' && t[j] != '>' && t[j] != '\n' && t[j] != '\t' &&
               t[j] != '\r' && t[j] != '/')
            j++;
        return (j - i - 1 == 3 && !strncmp(t + i + 1, "svg", 3)) ||
               (j - i - 1 > 4 && !strncmp(t + j - 4, ":svg", 4));
    }
}

/* Just the natural size, rounded to pixels: 0, or -1 when it is not an
   SVG or the size is out of range */
static int svg_size(const char *text, unsigned len, int *w, int *h)
{
    struct svg_doc doc;
    double fw, fh;
    if (!svg_sniff(text, len) || svg_load(&doc, text, len) < 0)
        return -1;
    int r = svg_natural(&doc, &fw, &fh);
    svg_doc_free(&doc);
    if (r < 0 || !(fw >= 0.5 && fh >= 0.5 && fw <= SVG_MAX_SIDE && fh <= SVG_MAX_SIDE))
        return -1;
    *w = (int)(fw + 0.5);
    *h = (int)(fh + 0.5);
    return 0;
}

/* Draws an SVG into a new RGBA buffer of out_w by out_h (0: its natural
   size), to be freed with free(). Its natural size goes to nat_w and
   nat_h. 0, or -1 with 'why'. */
static int svg_render(const char *text, unsigned len, int out_w, int out_h,
                      unsigned char **rgba, int *nat_w, int *nat_h,
                      const char **why)
{
    struct svg_doc doc;
    struct svg_ctx c;
    memset(&c, 0, sizeof(c));
    *rgba = 0;
    *why = "not an SVG";
    if (!svg_sniff(text, len) || svg_load(&doc, text, len) < 0)
        return -1;
    double w, h;
    if (svg_natural(&doc, &w, &h) < 0) {
        svg_doc_free(&doc);
        return -1;
    }
    *why = "a size out of range";
    if (!(w >= 0.5 && h >= 0.5 && w <= SVG_MAX_SIDE && h <= SVG_MAX_SIDE)) {
        svg_doc_free(&doc);
        return -1;
    }
    *nat_w = (int)(w + 0.5);
    *nat_h = (int)(h + 0.5);
    if (out_w <= 0 || out_h <= 0) {
        out_w = *nat_w;
        out_h = *nat_h;
    }
    if (out_w > SVG_MAX_SIDE || out_h > SVG_MAX_SIDE) {
        svg_doc_free(&doc);
        return -1;
    }
    *why = "out of memory";
    c.doc = &doc;
    c.cv.w = out_w;
    c.cv.h = out_h;
    c.cv.rgba = (unsigned char *)calloc((size_t)out_w * out_h, 4);
    c.cv.cover = (float *)calloc((size_t)out_w * out_h, sizeof(float));
    if (!c.cv.rgba || !c.cv.cover) {
        free(c.cv.rgba);
        free(c.cv.cover);
        svg_doc_free(&doc);
        return -1;
    }
    /* The picture's user space onto the pixels asked for */
    struct svg_m m;
    const char *vb = svg_attr(&doc, 0, "viewBox");
    double v;
    if (vb && svg_num(vb, &v))
        m = svg_viewbox(vb, svg_attr(&doc, 0, "preserveAspectRatio"), 0, 0,
                        out_w, out_h);
    else
        m = svg_mat(out_w / w, 0, 0, out_h / h, 0, 0);
    struct svg_style st;
    memset(&st, 0, sizeof(st));
    strcpy(st.fill, "black");
    strcpy(st.stroke, "none");
    st.fill_op = st.stroke_op = st.opacity = 1;
    st.width = 1;
    st.miter = 4;
    svg_node_draw(&c, 0, m, st, 0);
    free(c.cv.cover);
    free(c.cv.edges);
    free(c.cv.xs);
    free(c.cv.ws);
    svg_doc_free(&doc);
    *rgba = c.cv.rgba;
    return 0;
}

#endif
