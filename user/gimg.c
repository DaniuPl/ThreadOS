/* gimg - looking at a picture (ring 3)
 *
 * The machine can now save what is on its screen: Print Screen writes
 * a file of palette indices - exactly the bytes the compositor drew.
 * A machine that can take a picture and not show it is only half of a
 * machine, so this puts the bytes back into a window.
 *
 * There is nothing to interpret. The file's numbers are the palette's
 * numbers, and the window's content is written in the same ones, so
 * showing a picture is copying it - through win_blit, which takes a
 * row at a time instead of a system call per pixel.
 *
 * A picture from anywhere ELSE speaks in colours, so the window asks
 * to hold colours too (win_truecolour) and the decoders hand them over
 * whole, through win_blit32. What used to happen here - every colour
 * traded for the nearest of two hundred and fifty six - does not
 * happen any more.
 *
 * The picture is bigger than the window (a screen does not fit inside
 * a window on the same screen), so it is shown every second or third
 * pixel, whichever makes it fit. Nearest neighbour: the point is to
 * SEE what was on the screen, and a blurred half-size picture of a
 * screen full of eight-pixel letters would show less, not more.
 */

#include "udraw.h"
#include "ulocale.h"
#include "uinflate.h"
#include "ujpeg.h"

#define W0 420
#define H0 300
#define STATUS_H 22           /* the status line along the bottom */
#define STATUS_PX 11
#define NAME_MAX_LEN 31
#define ROW_MAX 1024

/* "TSCR", width, height, 256 colours of three bytes, then the pixels */
#define HDR (4 + 2 + 2 + 256 * 3)

static char fname[NAME_MAX_LEN + 1];
static char note[80];
static int img_w, img_h;          /* the picture's own size */
static int step = 1;              /* show every 'step'-th pixel */
static int cw = W0, ch = H0;
static int winid = -1;

/* Asking for a name in the status line, the same way the editor does */
static int asking;
static char answer[NAME_MAX_LEN + 1];
static int alen;

static uint8_t row[ROW_MAX * 3];   /* a source row, up to three bytes a pixel */
static uint8_t out[ROW_MAX];       /* palette indices - for the machine's own files */
static uint32_t out32[ROW_MAX];    /* colours - for everything from outside */

/* --- colours ---------------------------------------------------------
 *
 * A picture from outside speaks in colours, and so, now, does this
 * window: it asks the compositor for pixels that ARE colours, and then
 * a photograph is a photograph rather than a photograph seen through
 * thirty-six crayons. The trading of each colour for the nearest index
 * the palette had - a search, and a table of thirty-two thousand
 * answers to make it bearable - is simply gone.
 *
 * The machine's own screenshots stay as they are: their bytes are
 * palette indices and the palette is this screen's, so win_blit puts
 * them back exactly. */
static int colour_win;             /* did the window agree to hold colours */

static uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void ecopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

/* A picture from outside needs the window to hold colours. The only
   way that is refused is no memory for the buffer, and then saying so
   beats drawing nothing. */
static int need_colour(void)
{
    if (colour_win)
        return 1;
    ecopy(note, "no memory for the colours", sizeof(note));
    return 0;
}

static int num(char *d, int v)
{
    char tmp[8];
    int n = 0;
    if (v < 0)
        v = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    for (int i = 0; i < n; i++)
        d[i] = tmp[n - 1 - i];
    d[n] = '\0';
    return n;
}

static int text_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static const char *note_for_language(const char *text)
{
    if (!ulocale_language_polish())
        return text;
    if (text_equal(text, "no memory for the colours"))
        return "Brak pamięci na kolory";
    if (text_equal(text, "no memory for the picture"))
        return "Brak pamięci na obraz";
    if (text_equal(text, "odd dimensions"))
        return "Nieprawidłowe wymiary";
    if (text_equal(text, "I read PNG with 8 bits per channel"))
        return "Obsługuję PNG z 8 bitami na kanał";
    if (text_equal(text, "I cannot read an interlaced PNG"))
        return "Nie obsługuję przeplotu w PNG";
    if (text_equal(text, "an unknown kind of PNG"))
        return "Nieznany typ pliku PNG";
    if (text_equal(text, "the PNG stream does not add up"))
        return "Dane PNG są niepoprawne";
    if (text_equal(text, "it did not work"))
        return "Operacja nie powiodła się";
    if (text_equal(text, "I only read 24- and 8-bit BMP with no compression"))
        return "Obsługuję nieskompresowane BMP 24- i 8-bitowe";
    if (text_equal(text, "the picture's palette cannot be read"))
        return "Nie można odczytać palety obrazu";
    if (text_equal(text, "this is not a picture"))
        return "To nie jest obraz";
    if (text_equal(text, "no colours here"))
        return "Brak danych o kolorach";
    if (text_equal(text, "there is no such file"))
        return "Nie ma takiego pliku";
    if (text_equal(text, "loaded"))
        return "Wczytano";
    return text;
}

static void retitle(void)
{
    char t[NAME_MAX_LEN + 10];
    int p = 0;
    const char *pre = ulocale_language_polish() ? "Obraz" : "Picture";
    for (int i = 0; pre[i]; i++)
        t[p++] = pre[i];
    if (fname[0]) {
        t[p++] = ':';
        t[p++] = ' ';
        for (int i = 0; fname[i] && p < (int)sizeof(t) - 1; i++)
            t[p++] = fname[i];
    }
    t[p] = '\0';
    if (winid >= 0)
        win_title(winid, t);
}

static void draw_status(void)
{
    ulocale_window_title(winid, "Picture", "Obraz");
    char line[80];
    int p = 0;
    if (asking) {
        const char *q = ulocale_language_polish() ? "Otwórz: " : "Open: ";
        for (int i = 0; q[i]; i++)
            line[p++] = q[i];
        for (int i = 0; i < alen; i++)
            line[p++] = answer[i];
        line[p++] = '_';
    } else if (img_w) {
        p += num(line + p, img_w);
        line[p++] = 'x';
        p += num(line + p, img_h);
        /* How many pixels are skipped between the ones drawn. Said as
           a step rather than an ordinal, because "every 1st / 2nd /
           3rd" would need the suffix worked out from the number. */
        const char *sc = ulocale_language_polish() ? ", krok " : ", step ";
        for (int i = 0; sc[i]; i++)
            line[p++] = sc[i];
        p += num(line + p, step);
    } else {
        const char *hint = ulocale_language_polish()
                               ? "Ctrl+O otwiera obraz"
                               : "Ctrl+O opens a picture";
        for (int i = 0; hint[i]; i++)
            line[p++] = hint[i];
    }
    if (note[0] && !asking) {
        line[p++] = ' ';
        line[p++] = '-';
        line[p++] = ' ';
        const char *shown_note = note_for_language(note);
        for (int i = 0; shown_note[i] && p < 70; i++)
            line[p++] = shown_note[i];
    }
    line[p] = '\0';
    struct ud_look l;
    ud_look(&l);
    ud_fill(winid, 0, ch - STATUS_H, cw, STATUS_H, l.row);
    ud_fill(winid, 0, ch - STATUS_H, cw, 1, l.line);
    ud_text(winid, 8, ud_baseline(ch - STATUS_H, STATUS_H, STATUS_PX), line,
            l.text, UD_SANS, STATUS_PX);
}

/* The window's own background, in the theme's colour */
static void clear(int winid)
{
    struct ud_look l;
    ud_look(&l);
    ud_fill(winid, 0, 0, cw, ch, l.bg);
}

/* A PNG.
 *
 * Three things in a trench coat: chunks, a deflate stream, and one
 * filter byte per row. The chunks are easy (a length, a name, the
 * bytes, a checksum we do not need to look at, because the deflate
 * stream will not hold together if the bytes are wrong). The deflate
 * stream is in uinflate.h, written for this and useful again the next
 * time something is packed. What is left is the filters - five of
 * them, each saying "this row was written as the difference from
 * something", and each undone by adding that something back:
 *
 *   0 none, 1 the pixel to the left, 2 the pixel above,
 *   3 the average of those two, 4 Paeth - whichever of left, above and
 *     above-left is nearest to their sum minus the corner.
 *
 * Interlaced pictures are refused rather than half-drawn: Adam7 is
 * seven passes of this same work in a different order, and a picture
 * shown wrong is worse than a picture not shown.
 */
#define PNG_MAX_PIXELS (1024 * 1024)

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

static int show_png(int fd, const uint8_t *head, int got)
{
    if (!need_colour())
        return -1;
    /* The whole file first: the deflate stream is spread across as
       many IDAT chunks as the writer felt like, and they have to be
       fed to the decoder as one. There is no call that says how long a
       file is, so we take a ceiling and read until it ends. */
#define PNG_MAX_FILE (2 * 1024 * 1024)
    uint8_t *file = malloc(PNG_MAX_FILE);
    if (!file) {
        ecopy(note, "no memory for the picture", sizeof(note));
        return -1;
    }
    for (int i = 0; i < got && i < PNG_MAX_FILE; i++)
        file[i] = head[i];
    int have = got;
    while (have < PNG_MAX_FILE) {
        int n = (int)sys_read(fd, file + have,
                              (uint32_t)(PNG_MAX_FILE - have));
        if (n <= 0)
            break;
        have += n;
    }
    int size = have;

    int w = 0, h = 0, depth = 0, colour = 0, interlace = 0;
    uint8_t plte[256 * 3];
    int plte_n = 0;
    uint8_t *idat = malloc((uint32_t)size);   /* never larger than the file */
    if (!idat) {
        ecopy(note, "no memory for the picture", sizeof(note));
        return -1;
    }
    int idat_n = 0;

    int pos = 8;                              /* past the signature */
    while (pos + 8 <= have) {
        uint32_t len = ((uint32_t)file[pos] << 24) |
                       ((uint32_t)file[pos + 1] << 16) |
                       ((uint32_t)file[pos + 2] << 8) | file[pos + 3];
        const uint8_t *name = file + pos + 4;
        const uint8_t *body = file + pos + 8;
        if (pos + 8 + (int)len + 4 > have)
            break;

        if (name[0] == 'I' && name[1] == 'H' && name[2] == 'D' &&
            name[3] == 'R' && len >= 13) {
            w = (int)(((uint32_t)body[0] << 24) | ((uint32_t)body[1] << 16) |
                      ((uint32_t)body[2] << 8) | body[3]);
            h = (int)(((uint32_t)body[4] << 24) | ((uint32_t)body[5] << 16) |
                      ((uint32_t)body[6] << 8) | body[7]);
            depth = body[8];
            colour = body[9];
            interlace = body[12];
        } else if (name[0] == 'P' && name[1] == 'L' && name[2] == 'T' &&
                   name[3] == 'E') {
            plte_n = (int)(len / 3);
            if (plte_n > 256)
                plte_n = 256;
            for (int i = 0; i < plte_n * 3; i++)
                plte[i] = body[i];
        } else if (name[0] == 'I' && name[1] == 'D' && name[2] == 'A' &&
                   name[3] == 'T') {
            for (uint32_t i = 0; i < len; i++)
                idat[idat_n++] = body[i];
        } else if (name[0] == 'I' && name[1] == 'E' && name[2] == 'N' &&
                   name[3] == 'D') {
            break;
        }
        pos += 8 + (int)len + 4;
    }

    if (w <= 0 || h <= 0 || w > ROW_MAX || (long)w * h > PNG_MAX_PIXELS) {
        ecopy(note, "odd dimensions", sizeof(note));
        return -1;
    }
    if (depth != 8) {
        ecopy(note, "I read PNG with 8 bits per channel", sizeof(note));
        return -1;
    }
    if (interlace) {
        ecopy(note, "I cannot read an interlaced PNG", sizeof(note));
        return -1;
    }
    int chan = colour == 2 ? 3 : colour == 6 ? 4 : colour == 4 ? 2 :
               colour == 0 ? 1 : colour == 3 ? 1 : 0;
    if (!chan || (colour == 3 && !plte_n)) {
        ecopy(note, "an unknown kind of PNG", sizeof(note));
        return -1;
    }

    uint32_t raw_len = (uint32_t)h * (uint32_t)(1 + w * chan);
    uint8_t *raw = malloc(raw_len);
    if (!raw) {
        ecopy(note, "no memory for the picture", sizeof(note));
        return -1;
    }
    if (inflate_zlib(idat, (unsigned)idat_n, raw, raw_len) < 0) {
        ecopy(note, "the PNG stream does not add up", sizeof(note));
        return -1;
    }

    img_w = w;
    img_h = h;
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;
    clear(winid);

    /* Undoing the filters needs the row above, so the rows are walked
       in order and the previous one kept. */
    int stride = w * chan;
    for (int y = 0; y < h; y++) {
        uint8_t *cur = raw + (uint32_t)y * (uint32_t)(1 + stride);
        int filter = cur[0];
        uint8_t *line = cur + 1;
        uint8_t *prev = y ? raw + (uint32_t)(y - 1) * (uint32_t)(1 + stride)
                            + 1 : (uint8_t *)0;
        for (int i = 0; i < stride; i++) {
            int a = i >= chan ? line[i - chan] : 0;
            int b = prev ? prev[i] : 0;
            int c = (prev && i >= chan) ? prev[i - chan] : 0;
            switch (filter) {
            case 1: line[i] = (uint8_t)(line[i] + a); break;
            case 2: line[i] = (uint8_t)(line[i] + b); break;
            case 3: line[i] = (uint8_t)(line[i] + ((a + b) >> 1)); break;
            case 4: line[i] = (uint8_t)(line[i] + paeth(a, b, c)); break;
            default: break;                       /* 0: as it stands */
            }
        }

        if (y % step)
            continue;
        int dest = y / step;
        if (dest >= room_h)
            break;
        int n = 0;
        for (int x = 0; x < w && n < ROW_MAX; x += step) {
            int r, g, b;
            const uint8_t *px = line + (long)x * chan;
            if (colour == 2 || colour == 6) {
                r = px[0]; g = px[1]; b = px[2];
            } else if (colour == 3) {
                const uint8_t *c = plte + (px[0] % (plte_n ? plte_n : 1)) * 3;
                r = c[0]; g = c[1]; b = c[2];
            } else {
                r = g = b = px[0];                /* grey, with or without alpha */
            }
            out32[n++] = rgb(r, g, b);
        }
        win_blit32(winid, 0, dest, n, 1, out32);
    }
    return 0;
}

/* A JPEG, which is what a camera gives you.
 *
 * Baseline only, and that is a deliberate line: baseline is what
 * cameras, scanners and most of the web write. Progressive is the same
 * picture sent in several passes so that a slow line shows something
 * early - a decoder for it is a second decoder, and this one says so
 * plainly rather than showing half a picture.
 *
 * The work itself is in ujpeg.h. What is left here is the reading:
 * a JPEG has no length in its header either, so the file is read to
 * its end, decoded into planes, and then asked pixel by pixel - the
 * components have different sizes (the colour ones are usually half),
 * and asking hides that.
 */
#define JPEG_MAX_FILE (1024 * 1024)
#define JPEG_MAX_MEM  (3 * 1024 * 1024)

static uint8_t *jpeg_file;        /* kept between pictures: malloc never
                                     gives anything back, so opening a
                                     second JPEG must not ask again */
static uint8_t *jpeg_mem;

static int show_jpeg(int fd, const uint8_t *head, int got)
{
    if (!need_colour())
        return -1;
    if (!jpeg_file) {
        jpeg_file = malloc(JPEG_MAX_FILE);
        jpeg_mem = malloc(JPEG_MAX_MEM);
    }
    if (!jpeg_file || !jpeg_mem) {
        ecopy(note, "no memory for the picture", sizeof(note));
        return -1;
    }

    for (int i = 0; i < got && i < JPEG_MAX_FILE; i++)
        jpeg_file[i] = head[i];
    int have = got;
    while (have < JPEG_MAX_FILE) {
        int n = (int)sys_read(fd, jpeg_file + have,
                              (uint32_t)(JPEG_MAX_FILE - have));
        if (n <= 0)
            break;
        have += n;
    }

    int w = 0, h = 0;
    const char *why = "it did not work";
    if (jpeg_decode(jpeg_file, (unsigned)have, jpeg_mem, JPEG_MAX_MEM,
                    &w, &h, &why) < 0) {
        ecopy(note, why, sizeof(note));
        return -1;
    }
    if (w <= 0 || w > ROW_MAX || h <= 0) {
        ecopy(note, "odd dimensions", sizeof(note));
        return -1;
    }

    img_w = w;
    img_h = h;
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;
    clear(winid);

    for (int y = 0; y < h; y += step) {
        int dest = y / step;
        if (dest >= room_h)
            break;
        int n = 0;
        for (int x = 0; x < w && n < ROW_MAX; x += step) {
            int r, g, b;
            jpeg_pixel(x, y, &r, &g, &b);
            out32[n++] = rgb(r, g, b);
        }
        win_blit32(winid, 0, dest, n, 1, out32);
    }
    return 0;
}

/* A BMP, as it comes from anywhere else in the world.
 *
 * Only what is worth having: no compression (BI_RGB), twenty-four bits
 * a pixel or eight with a palette of its own. Rows are padded to four
 * bytes and, unless the height is negative, they run from the BOTTOM
 * up - which is why the destination row is counted from the other end.
 */
static int show_bmp(int fd, const uint8_t *head)
{
    if (!need_colour())
        return -1;
    uint32_t data_at = (uint32_t)head[10] | ((uint32_t)head[11] << 8) |
                       ((uint32_t)head[12] << 16) | ((uint32_t)head[13] << 24);
    uint32_t dib = (uint32_t)head[14] | ((uint32_t)head[15] << 8) |
                   ((uint32_t)head[16] << 16) | ((uint32_t)head[17] << 24);
    int w = (int)((uint32_t)head[18] | ((uint32_t)head[19] << 8) |
                  ((uint32_t)head[20] << 16) | ((uint32_t)head[21] << 24));
    int hh = (int)((uint32_t)head[22] | ((uint32_t)head[23] << 8) |
                   ((uint32_t)head[24] << 16) | ((uint32_t)head[25] << 24));
    int bpp = (int)((uint32_t)head[28] | ((uint32_t)head[29] << 8));
    uint32_t comp = (uint32_t)head[30] | ((uint32_t)head[31] << 8) |
                    ((uint32_t)head[32] << 16) | ((uint32_t)head[33] << 24);

    int upside = hh > 0;              /* the usual: rows from the bottom */
    if (hh < 0)
        hh = -hh;
    if (w <= 0 || w > ROW_MAX || hh <= 0 || dib < 40) {
        ecopy(note, "odd dimensions", sizeof(note));
        return -1;
    }
    if (comp != 0 || (bpp != 24 && bpp != 8)) {
        ecopy(note, "I only read 24- and 8-bit BMP with no compression",
              sizeof(note));
        return -1;
    }

    /* An eight-bit BMP carries its own palette, right after the header */
    static uint8_t own[256 * 4];
    uint32_t pos = 14 + dib;
    if (bpp == 8) {
        int colours = (int)(data_at - pos) / 4;
        if (colours > 256)
            colours = 256;
        if (colours < 0 || (int)sys_read(fd, own, (uint32_t)(colours * 4))
                           != colours * 4) {
            ecopy(note, "the picture's palette cannot be read",
                  sizeof(note));
            return -1;
        }
        pos += (uint32_t)(colours * 4);
    }
    /* Skip whatever else sits between here and the pixels */
    while (pos < data_at) {
        uint32_t step_n = data_at - pos;
        if (step_n > sizeof(row))
            step_n = sizeof(row);
        if ((uint32_t)sys_read(fd, row, step_n) != step_n)
            break;
        pos += step_n;
    }

    img_w = w;
    img_h = hh;
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;

    clear(winid);

    int stride = bpp == 24 ? (w * 3 + 3) & ~3 : (w + 3) & ~3;
    for (int y = 0; y < hh; y++) {
        if ((int)sys_read(fd, row, (uint32_t)stride) != stride)
            break;
        int dest = upside ? (hh - 1 - y) : y;
        if (dest % step)
            continue;
        dest /= step;
        if (dest >= room_h)
            continue;
        int n = 0;
        for (int x = 0; x < w && n < ROW_MAX; x += step) {
            int r, g, b;
            if (bpp == 24) {
                b = row[x * 3 + 0];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else {
                const uint8_t *c = own + row[x] * 4;
                b = c[0];
                g = c[1];
                r = c[2];
            }
            out32[n++] = rgb(r, g, b);
        }
        win_blit32(winid, 0, dest, n, 1, out32);
    }
    return 0;
}

/* Reads the picture and puts it in the window, row by row. Nothing is
   kept afterwards: the window's content IS the copy, and a second copy
   in this program's memory would be three hundred kilobytes held for
   no reason. */
static int show_tsc2(const char *name)
{
    int fd = (int)sys_open(name, O_READ);
    uint8_t h8[8];
    if (fd < 0 || (int)sys_read(fd, h8, 8) != 8) {
        if (fd >= 0)
            sys_close(fd);
        ecopy(note, "this is not a picture", sizeof(note));
        return -1;
    }
    img_w = h8[4] | (h8[5] << 8);
    img_h = h8[6] | (h8[7] << 8);
    if (img_w <= 0 || img_w > ROW_MAX || img_h <= 0 || !colour_win) {
        sys_close(fd);
        ecopy(note, colour_win ? "odd dimensions" : "no colours here",
              sizeof(note));
        return -1;
    }
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;

    clear(winid);
    int drawn = 0;
    for (int y = 0; y < img_h; y++) {
        if ((int)sys_read(fd, row, (uint32_t)img_w * 3) != img_w * 3)
            break;
        if (y % step)
            continue;
        int n = 0;
        for (int x = 0; x < img_w && n < ROW_MAX; x += step)
            out32[n++] = rgb(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
        win_blit32(winid, 0, drawn, n, 1, out32);
        drawn++;
        if (drawn >= room_h)
            break;
    }
    sys_close(fd);
    return 0;
}

static void show(const char *name)
{
    int fd = (int)sys_open(name, O_READ);
    if (fd < 0) {
        ecopy(note, "there is no such file", sizeof(note));
        img_w = 0;
        return;
    }

    /* Enough of the front of the file to tell the formats apart and
       to hold a whole BMP header (fourteen plus forty bytes). */
    uint8_t head[HDR];
    int got = (int)sys_read(fd, head, HDR);
    if (got < 54) {
        sys_close(fd);
        ecopy(note, "this is not a picture", sizeof(note));
        img_w = 0;
        return;
    }

    if (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' &&
        head[3] == 'G') {
        int ok = show_png(fd, head, got);
        sys_close(fd);
        if (ok < 0) {
            img_w = 0;
            return;
        }
        ecopy(fname, name, NAME_MAX_LEN);
        ecopy(note, "loaded", sizeof(note));
        retitle();
        return;
    }

    if (head[0] == 0xFF && head[1] == 0xD8) {
        int ok = show_jpeg(fd, head, got);
        sys_close(fd);
        if (ok < 0) {
            img_w = 0;
            return;
        }
        ecopy(fname, name, NAME_MAX_LEN);
        ecopy(note, "loaded", sizeof(note));
        retitle();
        return;
    }

    if (head[0] == 'B' && head[1] == 'M') {
        /* The reading above swallowed more than the BMP header, so we
           start again from the top and step forward properly. */
        sys_close(fd);
        fd = (int)sys_open(name, O_READ);
        if (fd < 0) {
            img_w = 0;
            return;
        }
        uint8_t bh[54];
        if ((int)sys_read(fd, bh, 54) != 54 || show_bmp(fd, bh) < 0) {
            sys_close(fd);
            img_w = 0;
            return;
        }
        sys_close(fd);
        ecopy(fname, name, NAME_MAX_LEN);
        ecopy(note, "loaded", sizeof(note));
        retitle();
        return;
    }

    /* The machine's own screenshot of a screen of real colours: "TSC2",
       the size, three bytes a pixel. Read from the top again, because
       the header is eight bytes and what was read above is more. */
    if (head[0] == 'T' && head[1] == 'S' && head[2] == 'C' &&
        head[3] == '2') {
        sys_close(fd);
        int ok = show_tsc2(name);
        if (ok < 0) {
            img_w = 0;
            return;
        }
        ecopy(fname, name, NAME_MAX_LEN);
        ecopy(note, "loaded", sizeof(note));
        retitle();
        return;
    }

    if (got != HDR || head[0] != 'T' || head[1] != 'S' || head[2] != 'C' ||
        head[3] != 'R') {
        sys_close(fd);
        ecopy(note, "this is not a picture", sizeof(note));
        img_w = 0;
        return;
    }
    img_w = head[4] | (head[5] << 8);
    img_h = head[6] | (head[7] << 8);
    if (img_w <= 0 || img_w > ROW_MAX || img_h <= 0) {
        sys_close(fd);
        ecopy(note, "odd dimensions", sizeof(note));
        img_w = 0;
        return;
    }

    /* How much of it fits: every pixel, or every second, or third... */
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;

    clear(winid);
    int drawn = 0;
    for (int y = 0; y < img_h; y++) {
        if ((int)sys_read(fd, row, (uint32_t)img_w) != img_w)
            break;
        if (y % step)
            continue;                  /* a row we are not showing */
        int n = 0;
        for (int x = 0; x < img_w && n < ROW_MAX; x += step)
            out[n++] = row[x];
        win_blit(winid, 0, drawn, n, 1, out);
        drawn++;
        if (drawn >= room_h)
            break;
    }
    sys_close(fd);
    ecopy(fname, name, NAME_MAX_LEN);
    ecopy(note, "loaded", sizeof(note));
    retitle();
}

void _start(int argc, char **argv)
{
    int win = (int)win_create(W0, H0,
                              ulocale_language_polish() ? "Obraz" : "Picture");
    if (win < 0)
        sys_exit(1);
    winid = win;

    /* Ask for a window whose pixels are colours. Everything the viewer
       draws for itself - the background, the status line - goes on
       being said in palette indices, and the compositor translates
       them; only the picture needs the other sixteen million. */
    colour_win = win_truecolour(win) == 0;

    clear(win);
    if (argc > 1 && argv && argv[1] && argv[1][0])
        show(argv[1]);
    draw_status();

    int language = ulocale_language_polish();
    for (;;) {
        struct win_event ev;
        int again = 0;
        int current_language = ulocale_language_polish();
        if (current_language != language) {
            language = current_language;
            again = 1;
        }
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
                if (fname[0])
                    show(fname);       /* a new size, a new fit */
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (asking) {
                    if (c == '\n') {
                        asking = 0;
                        answer[alen] = '\0';
                        if (alen)
                            show(answer);
                        alen = 0;
                    } else if (c == '\b') {
                        if (alen)
                            alen--;
                        else
                            asking = 0;
                    } else if (c >= 0x20 && c < 0x7F &&
                               alen < NAME_MAX_LEN) {
                        answer[alen++] = c;
                    }
                } else if (c == KEY_OPEN) {
                    asking = 1;
                    alen = 0;
                    note[0] = '\0';
                }
            }
            again = 1;
        }
        if (again)
            draw_status();
        sys_sleep(60);
    }
}
