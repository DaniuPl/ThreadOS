/* ggallery - the pictures in the Gallery folder
 *
 * A person's pictures are in one place, the Gallery folder in their
 * home, and the Camera saves into it too. This shows them as they
 * look rather than as names: a square for each, the picture shrunk to
 * fit it, the name under it. A click opens one over the whole window;
 * the arrow keys, or a click at either side of it, go to the one
 * before or after, and Back (or Backspace) returns to all of them.
 *
 * Every picture is read by the system's own decoders - PNG, JPEG, GIF,
 * WebP and SVG, the ones the viewer and the browser use - into four
 * bytes a pixel, and shrunk by averaging every pixel that falls into a
 * pixel of the square: a nearest-neighbour thumbnail of a photograph
 * is noise. A picture is decoded once for its square, one per pass of
 * the loop, so the window is there at once and fills as it goes; the
 * folder is looked at again every two seconds, so a photo just taken
 * appears without asking.
 *
 * Built against musl: decoding a folder of pictures needs memory given
 * back, which the plain programs' allocator never does.
 */

#include "uapi.h"
#include "udraw.h"
#include "ulocale.h"
#include "upng.h"
#include "ujpeg.h"
#include "ugif.h"
#include "uwebp.h"
#include "usvg.h"

#include <stdlib.h>
#include <string.h>

#define W0 520
#define H0 380
#define PAD 12                /* round the grid */
#define HEAD_H 44             /* the name and the count, over the grid */
#define THUMB 104             /* a picture's square */
#define CELL_W 120            /* one picture: its square and its name */
#define CELL_H 132
#define NAME_Y 108            /* the name's row, from the square's top */
#define NAME_H 16
#define VIEW_BAR_H 32         /* one picture: Back, its name, which one */
#define BACK_W 64
#define PICS_MAX 200
#define FILE_MAX (24 * 1024 * 1024)
#define PIXELS_MAX (16 * 1024 * 1024)
#define RESCAN_TICKS 25       /* the folder again: 25 passes of 80 ms */

struct pic {
    char name[NAME_MAX64 + 1];
    uint32_t size;
    int state;                /* 0 not read yet, 1 shown, -1 cannot be */
    int tw, th;               /* the shrunk picture, inside the square */
    uint32_t *thumb;
};

static struct pic pics[PICS_MAX];
static int npics;
static int win = -1;
static int cw = W0, ch = H0;
static int scroll;            /* how far the grid has moved up */
static int open_i = -1;       /* the picture shown on its own, or -1 */
static struct ud_look l;
static char gallery[256];     /* its full path */

/* The view of one picture, kept until the picture or the window
   changes: shrinking a photo again on every redraw would be slow */
static uint32_t *view;
static int view_of = -1, view_cw, view_ch, view_w, view_h;
static const char *view_why;

/* --- reading pictures ------------------------------------------------- */

static int ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail);
    if (n < t)
        return 0;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (a != tail[i])
            return 0;
    }
    return 1;
}

static int is_picture(const char *name)
{
    return ends_with(name, ".png") || ends_with(name, ".jpg") ||
           ends_with(name, ".jpeg") || ends_with(name, ".gif") ||
           ends_with(name, ".webp") || ends_with(name, ".svg");
}

static unsigned char *read_file(const char *name, uint32_t size,
                                uint32_t *got)
{
    if (!size || size > FILE_MAX)
        return 0;
    int fd = (int)sys_open(name, 0);
    if (fd < 0)
        return 0;
    unsigned char *d = malloc(size + 1);
    uint32_t have = 0;
    while (d && have < size) {
        int64_t n = sys_read(fd, d + have, size - have);
        if (n <= 0)
            break;
        have += (uint32_t)n;
    }
    sys_close(fd);
    if (d)
        d[have] = 0;          /* an SVG is text */
    *got = have;
    return d;
}

/* A picture as four bytes a pixel: 0 with a malloc'd 'rgba', or -1 */
static int decode(const char *name, uint32_t size, unsigned char **rgba,
                  int *w, int *h, const char **why)
{
    uint32_t n = 0;
    unsigned char *d = read_file(name, size, &n);
    *rgba = 0;
    *why = "it cannot be read";
    if (!d)
        return -1;
    int r = -1;
    if (png_sniff(d, n)) {
        int pw, ph;
        if (png_size(d, n, &pw, &ph) == 0 && pw > 0 && ph > 0 &&
            (long)pw * ph <= PIXELS_MAX) {
            unsigned int out_max = (unsigned int)pw * (unsigned int)ph * 4;
            unsigned int work_max = (unsigned int)png_work_size(pw, ph, n);
            unsigned char *out = malloc(out_max), *work = malloc(work_max);
            if (out && work &&
                png_decode(d, n, out, out_max, work, work_max, w, h, why) == 0) {
                *rgba = out;
                out = 0;
                r = 0;
            }
            free(out);
            free(work);
        } else {
            *why = "too large";
        }
    } else if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) {
        unsigned int mem_len = 3 * PIXELS_MAX / 2;
        unsigned char *mem = malloc(mem_len);
        if (mem && jpeg_decode(d, n, mem, mem_len, w, h, why) == 0 &&
            (long)*w * *h <= PIXELS_MAX) {
            unsigned char *out = malloc((size_t)*w * (size_t)*h * 4);
            if (out) {
                unsigned char *o = out;
                for (int y = 0; y < *h; y++)
                    for (int x = 0; x < *w; x++, o += 4) {
                        int cr, cg, cb;
                        jpeg_pixel(x, y, &cr, &cg, &cb);
                        o[0] = (unsigned char)cr;
                        o[1] = (unsigned char)cg;
                        o[2] = (unsigned char)cb;
                        o[3] = 255;
                    }
                *rgba = out;
                r = 0;
            }
        }
        free(mem);
    } else if (n > 10 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') {
        int gw = d[6] | d[7] << 8, gh = d[8] | d[9] << 8;
        if (gw > 0 && gh > 0 && (long)gw * gh <= PIXELS_MAX) {
            unsigned int max = (unsigned int)gw * (unsigned int)gh * 4;
            unsigned char *out = malloc(max);
            if (out && gif_decode(d, n, out, max, w, h, why) == 0) {
                *rgba = out;
                r = 0;
            } else {
                free(out);
            }
        }
    } else if (webp_sniff(d, n)) {
        r = webp_decode(d, n, rgba, w, h, why);
    } else if (svg_sniff((const char *)d, n)) {
        /* Drawn at its own size, but no larger than a screen: an SVG
           may say it is ten thousand pixels across and be a small
           drawing all the same */
        int nw = 0, nh = 0, rw = 0, rh = 0;
        if (svg_size((const char *)d, n, &nw, &nh) == 0 &&
            (nw > 1024 || nh > 1024)) {
            rw = nw >= nh ? 1024 : nw * 1024 / nh;
            rh = nw >= nh ? nh * 1024 / nw : 1024;
        }
        r = svg_render((const char *)d, n, rw, rh, rgba, w, h, why);
        if (r == 0 && rw) {
            *w = rw;           /* (it says the natural size) */
            *h = rh;
        }
    } else {
        *why = "not a kind of picture I know";
    }
    free(d);
    if (r < 0) {
        free(*rgba);
        *rgba = 0;
    }
    return r;
}

/* The picture shrunk into a box of bw by bh, keeping its shape and
   never made larger than it is: every pixel of the result the average
   of the ones under it, laid on 'ground' where the picture is clear */
static uint32_t *shrink(const unsigned char *rgba, int w, int h, int bw,
                        int bh, uint32_t ground, int *ow, int *oh)
{
    int tw = w, th = h;
    if (tw > bw) {
        th = (int)((long)th * bw / tw);
        tw = bw;
    }
    if (th > bh) {
        tw = (int)((long)tw * bh / th);
        th = bh;
    }
    if (tw < 1)
        tw = 1;
    if (th < 1)
        th = 1;
    uint32_t *out = malloc((size_t)tw * (size_t)th * 4);
    if (!out)
        return 0;
    int gr = (int)(ground >> 16) & 0xFF, gg = (int)(ground >> 8) & 0xFF,
        gb = (int)ground & 0xFF;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long)y * h / th), y1 = (int)((long)(y + 1) * h / th);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long)x * w / tw), x1 = (int)((long)(x + 1) * w / tw);
            if (x1 <= x0)
                x1 = x0 + 1;
            long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = y0; sy < y1; sy++) {
                const unsigned char *p = rgba + ((long)sy * w + x0) * 4;
                for (int sx = x0; sx < x1; sx++, p += 4) {
                    /* (colour weighted by its alpha, so a clear pixel's
                       colour - often black - does not darken the edge) */
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    cnt++;
                }
            }
            int pr = a ? (int)(r / a) : 0, pg = a ? (int)(g / a) : 0,
                pb = a ? (int)(b / a) : 0;
            int pa = (int)(a / cnt);
            pr = (pr * pa + gr * (255 - pa)) / 255;
            pg = (pg * pa + gg * (255 - pa)) / 255;
            pb = (pb * pa + gb * (255 - pa)) / 255;
            out[(long)y * tw + x] = ((uint32_t)pr << 16) | ((uint32_t)pg << 8) |
                                    (uint32_t)pb;
        }
    }
    *ow = tw;
    *oh = th;
    return out;
}

/* --- the folder --------------------------------------------------------- */

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct pic *)a)->name, ((const struct pic *)b)->name);
}

/* The folder's pictures; a picture already shrunk keeps its square.
   1 when the list changed. */
static int scan(void)
{
    if (sys_chdir(gallery) < 0)
        return 0;
    static struct pic fresh[PICS_MAX];
    int n = 0;
    struct dirent64 e;
    for (int i = 0; n < PICS_MAX && sys_readdir(i, &e) == 0; i++) {
        if (e.is_dir || !is_picture(e.name))
            continue;
        memset(&fresh[n], 0, sizeof(fresh[n]));
        memcpy(fresh[n].name, e.name, sizeof(fresh[n].name) - 1);
        fresh[n].size = e.size;
        n++;
    }
    qsort(fresh, (size_t)n, sizeof(fresh[0]), by_name);
    int changed = n != npics;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < npics; k++) {
            /* (one that could not be shown keeps that too, until the
               file changes: it is not read again every two seconds) */
            if (pics[k].state && !strcmp(pics[k].name, fresh[i].name) &&
                pics[k].size == fresh[i].size) {
                fresh[i] = pics[k];
                pics[k].thumb = 0;    /* moved, not to be freed */
                break;
            }
        }
        if (!changed && (strcmp(fresh[i].name, pics[i].name) ||
                         fresh[i].size != pics[i].size))
            changed = 1;
    }
    for (int k = 0; k < npics; k++)
        free(pics[k].thumb);
    memcpy(pics, fresh, sizeof(pics[0]) * (size_t)n);
    npics = n;
    if (changed) {
        view_of = -1;
        if (open_i >= npics)
            open_i = -1;
    }
    return changed;
}

/* One picture not shrunk yet, shrunk: 1 when there was one */
static int shrink_next(void)
{
    for (int i = 0; i < npics; i++) {
        struct pic *p = &pics[i];
        if (p->state)
            continue;
        unsigned char *rgba;
        int w, h;
        const char *why;
        p->state = -1;
        if (decode(p->name, p->size, &rgba, &w, &h, &why) == 0) {
            p->thumb = shrink(rgba, w, h, THUMB, THUMB, l.field, &p->tw,
                              &p->th);
            if (p->thumb)
                p->state = 1;
            free(rgba);
        }
        return 1;
    }
    return 0;
}

/* --- drawing ------------------------------------------------------------ */

/* Pixels into the window, rows y0 to y1 of a picture w wide placed at
   x, y: as many rows a call as fit in what one call takes (1024
   pixels), and a row at a time for a picture wider than that */
static void blit_rows(int x, int y, int w, int y0, int y1, const uint32_t *px)
{
    if (w <= 0)
        return;
    int step = 1024 / w;
    if (step < 1)
        step = 1;
    for (int r = y0; r < y1; r += step) {
        int n = y1 - r < step ? y1 - r : step;
        if (w <= 1024) {
            win_blit32(win, x, y + r, w, n, px + (long)r * w);
        } else {
            for (int k = 0; k < n; k++)
                for (int c = 0; c < w; c += 1024)
                    win_blit32(win, x + c, y + r + k, w - c < 1024 ? w - c : 1024,
                               1, px + (long)(r + k) * w + c);
        }
    }
}

static int columns(void)
{
    int c = (cw - 2 * PAD) / CELL_W;
    return c < 1 ? 1 : c;
}

static int grid_x0(void)
{
    return (cw - columns() * CELL_W) / 2;
}

static void draw_grid(void)
{
    ud_text_l(win, PAD, 28, "Gallery", l.text, UD_BOLD, 16);
    if (!npics) {
        ud_text_l(win, PAD, HEAD_H + 30, "No pictures yet.", l.text, UD_SANS,
                  UD_PX);
        ud_text_l(win, PAD, HEAD_H + 50,
                  "Put them in the Gallery folder in your home,", l.dim,
                  UD_SANS, UD_PX);
        ud_text_l(win, PAD, HEAD_H + 68, "or take some with the Camera.",
                  l.dim, UD_SANS, UD_PX);
        ud_text(win, PAD, HEAD_H + 96, gallery, l.dim, UD_SANS, 11);
        return;
    }
    char num[12];
    int k = 0, v = npics;
    char tmp[12];
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    int n = 0;
    while (k)
        num[n++] = tmp[--k];
    num[n] = 0;
    int nw = ud_width(num, UD_SANS, UD_PX);
    ud_text(win, cw - PAD - nw, 28, num, l.dim, UD_SANS, UD_PX);
    ud_text_right_l(win, cw - PAD - nw - 5, 28, "Pictures:", l.dim, UD_SANS,
                    UD_PX);

    int cols = columns(), x0 = grid_x0();
    int rows = (npics + cols - 1) / cols;
    int room = ch - HEAD_H;
    int most = rows * CELL_H - room;
    if (scroll > most)
        scroll = most;
    if (scroll < 0)
        scroll = 0;
    ud_clip(win, 0, HEAD_H, cw, room);
    for (int i = 0; i < npics; i++) {
        const struct pic *p = &pics[i];
        int x = x0 + (i % cols) * CELL_W + (CELL_W - THUMB) / 2;
        int y = HEAD_H + (i / cols) * CELL_H - scroll;
        if (y + CELL_H < HEAD_H || y > ch)
            continue;
        ud_round(win, x, y, THUMB, THUMB, 8, l.field);
        if (p->state == 1) {
            int tx = x + (THUMB - p->tw) / 2, ty = y + (THUMB - p->th) / 2;
            /* (rows above the grid's top are the header's) */
            int r0 = HEAD_H - ty > 0 ? HEAD_H - ty : 0;
            int r1 = ch - ty < p->th ? ch - ty : p->th;
            blit_rows(tx, ty, p->tw, r0, r1, p->thumb);
        } else if (p->state < 0) {
            ud_text_mid_l(win, x, y, THUMB, THUMB, "cannot be shown", l.dim,
                          UD_SANS, 11);
        }
        ud_clip(win, x0 + (i % cols) * CELL_W + 2, HEAD_H,
                CELL_W - 4, room);
        ud_text_mid(win, x0 + (i % cols) * CELL_W, y + NAME_Y, CELL_W,
                    NAME_H, p->name, l.text, UD_SANS, 11);
        ud_clip(win, 0, HEAD_H, cw, room);
    }
    ud_unclip(win);
}

static void draw_view(void)
{
    int vh = ch - VIEW_BAR_H;
    if (view_of != open_i || view_cw != cw || view_ch != ch) {
        free(view);
        view = 0;
        view_why = 0;
        unsigned char *rgba;
        int w, h;
        const char *why;
        const struct pic *p = &pics[open_i];
        if (decode(p->name, p->size, &rgba, &w, &h, &why) == 0) {
            view = shrink(rgba, w, h, cw, vh, l.bg, &view_w, &view_h);
            free(rgba);
        } else {
            view_why = why;
        }
        view_of = open_i;
        view_cw = cw;
        view_ch = ch;
    }
    if (view) {
        int x = (cw - view_w) / 2, y = (vh - view_h) / 2;
        blit_rows(x, y, view_w, 0, view_h, view);
    } else {
        ud_text_mid_l(win, 0, 0, cw, vh, "This picture cannot be shown.",
                      l.dim, UD_SANS, UD_PX);
    }
    int by = ch - VIEW_BAR_H;
    ud_fill(win, 0, by, cw, VIEW_BAR_H, l.field);
    ud_round(win, 8, by + 4, BACK_W, VIEW_BAR_H - 8, 7, l.row);
    ud_text_mid_l(win, 8, by + 4, BACK_W, VIEW_BAR_H - 8, "Back", l.text,
                  UD_SANS, UD_PX);
    ud_text_mid(win, BACK_W + 16, by, cw - 2 * (BACK_W + 16), VIEW_BAR_H,
                pics[open_i].name, l.text, UD_SANS, UD_PX);
    char which[24];
    int n = 0, parts[2] = { open_i + 1, npics };
    for (int k = 0; k < 2; k++) {
        char tmp[12];
        int t = 0, v = parts[k];
        do {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (t)
            which[n++] = tmp[--t];
        if (!k) {
            which[n++] = ' ';
            which[n++] = '/';
            which[n++] = ' ';
        }
    }
    which[n] = 0;
    ud_text_right(win, cw - 12, ud_baseline(by, VIEW_BAR_H, UD_PX), which,
                  l.dim, UD_SANS, UD_PX);
}

static void draw(void)
{
    ud_look(&l);
    ud_fill(win, 0, 0, cw, ch, l.bg);
    if (open_i >= 0 && open_i < npics)
        draw_view();
    else
        draw_grid();
}

/* --- the pointer and the keys --------------------------------------------- */

static void go(int by)
{
    if (open_i < 0 || !npics)
        return;
    open_i = (open_i + by + npics) % npics;
}

static void click(int x, int y)
{
    if (open_i >= 0) {
        int by = ch - VIEW_BAR_H;
        if (y >= by) {
            if (x < 8 + BACK_W)
                open_i = -1;
        } else if (x < cw / 4) {
            go(-1);
        } else if (x >= cw - cw / 4) {
            go(1);
        }
        return;
    }
    if (y < HEAD_H)
        return;
    int cols = columns(), x0 = grid_x0();
    int col = (x - x0) / CELL_W, row = (y - HEAD_H + scroll) / CELL_H;
    if (x < x0 || col >= cols)
        return;
    int i = row * cols + col;
    if (i < npics)
        open_i = i;
}

static void key(int c)
{
    if (open_i < 0)
        return;
    if (c == KEY_LEFT)
        go(-1);
    else if (c == KEY_RIGHT)
        go(1);
    else if (c == '\b')
        open_i = -1;
}

int main(void)
{
    win = (int)win_create(W0, H0, "Gallery");
    if (win < 0 || win_truecolour(win) < 0)
        return 1;
    ulocale_window_title(win, "Gallery", "Galeria");
    ud_look(&l);

    /* The folder is in the home this program started in; made the
       first time, so there is always somewhere to put pictures */
    if (sys_getcwd(gallery, sizeof(gallery) - 10) < 0)
        strcpy(gallery, "/");
    size_t n = strlen(gallery);
    if (n && gallery[n - 1] != '/')
        gallery[n++] = '/';
    strcpy(gallery + n, "Gallery");
    if (sys_chdir(gallery) < 0)
        sys_mkdir(gallery);
    scan();
    draw();

    int ticks = 0;
    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            again = 1;
            if (ev.type == WEV_CLOSE)
                return 0;
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_KEY) {
                key(ev.val);
            } else if (ev.type == WEV_SCROLL && open_i < 0) {
                scroll -= ev.val * CELL_H / 2;
            }
        }
        if (open_i < 0 && shrink_next())
            again = 1;
        if (++ticks >= RESCAN_TICKS) {
            ticks = 0;
            if (scan())
                again = 1;
        }
        if (again)
            draw();
        sys_sleep(again ? 10 : 80);
    }
}
