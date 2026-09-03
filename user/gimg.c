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
 * The picture is bigger than the window (a screen does not fit inside
 * a window on the same screen), so it is shown every second or third
 * pixel, whichever makes it fit. Nearest neighbour: the point is to
 * SEE what was on the screen, and a blurred half-size picture of a
 * screen full of eight-pixel letters would show less, not more.
 */

#include "uapi.h"

#define C_BG    8
#define C_WHITE 4
#define C_GREEN 2
#define C_BLUE  3

#define W0 420
#define H0 300
#define STATUS_H 10
#define NAME_MAX_LEN 31
#define ROW_MAX 1024

/* "PSCR", width, height, 256 colours of three bytes, then the pixels */
#define HDR (4 + 2 + 2 + 256 * 3)

static char fname[NAME_MAX_LEN + 1];
static char note[48];
static int img_w, img_h;          /* the picture's own size */
static int step = 1;              /* show every 'step'-th pixel */
static int cw = W0, ch = H0;
static int winid = -1;

/* Asking for a name in the status line, the same way the editor does */
static int asking;
static char answer[NAME_MAX_LEN + 1];
static int alen;

static uint8_t row[ROW_MAX * 3];   /* a source row, up to three bytes a pixel */
static uint8_t out[ROW_MAX];

/* --- colours into numbers -------------------------------------------
 *
 * This screen speaks in palette indices; a picture that came from
 * anywhere else speaks in colours. So each colour has to be traded for
 * the nearest number the screen has - and "nearest" is honest here,
 * because the palette holds thirty-six colours of the desktop and
 * whatever the wallpaper left in the rest. A drawing comes out well; a
 * photograph comes out as a photograph seen through thirty-six
 * crayons, which is what an eight-bit screen is.
 *
 * Doing the search for every pixel would be two hundred and fifty six
 * comparisons three hundred thousand times. Instead the answer is
 * remembered for every colour rounded to five bits a channel: at most
 * thirty two thousand searches, and a picture rarely uses a tenth of
 * that. */
static uint32_t pal[256];
static uint8_t memo[32768];
static int memo_ready;

static void colours_start(void)
{
    sys_palette(pal);
    for (int i = 0; i < 32768; i++)
        memo[i] = 0xFF;
    memo_ready = 1;
}

static uint8_t nearest(int r, int g, int b)
{
    int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    if (memo[key] != 0xFF)
        return memo[key];

    int best = 0;
    long best_d = 1L << 30;
    for (int i = 0; i < 256; i++) {
        int pr = (int)((pal[i] >> 16) & 0xFF);
        int pg = (int)((pal[i] >> 8) & 0xFF);
        int pb = (int)(pal[i] & 0xFF);
        long dr = pr - r, dg = pg - g, db = pb - b;
        long d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
            if (!d)
                break;
        }
    }
    memo[key] = (uint8_t)best;
    return (uint8_t)best;
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

static void retitle(void)
{
    char t[NAME_MAX_LEN + 10];
    int p = 0;
    const char *pre = "Obraz";
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
    char line[80];
    int p = 0;
    if (asking) {
        const char *q = "Otworz: ";
        for (int i = 0; q[i]; i++)
            line[p++] = q[i];
        for (int i = 0; i < alen; i++)
            line[p++] = answer[i];
        line[p++] = '_';
    } else if (img_w) {
        p += num(line + p, img_w);
        line[p++] = 'x';
        p += num(line + p, img_h);
        const char *sc = ", co ";
        for (int i = 0; sc[i]; i++)
            line[p++] = sc[i];
        p += num(line + p, step);
        const char *px = ". pixel";
        for (int i = 0; px[i]; i++)
            line[p++] = px[i];
    } else {
        const char *hint = "Ctrl+O otwiera obraz";
        for (int i = 0; hint[i]; i++)
            line[p++] = hint[i];
    }
    if (note[0] && !asking) {
        line[p++] = ' ';
        line[p++] = '-';
        line[p++] = ' ';
        for (int i = 0; note[i] && p < 70; i++)
            line[p++] = note[i];
    }
    line[p] = '\0';
    win_rect(winid, 0, ch - STATUS_H, cw, STATUS_H, C_BLUE);
    win_text(winid, 4, ch - STATUS_H + 1, line, C_WHITE);
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
        ecopy(note, "dziwne wymiary", sizeof(note));
        return -1;
    }
    if (comp != 0 || (bpp != 24 && bpp != 8)) {
        ecopy(note, "umiem tylko BMP 24- i 8-bitowy bez kompresji",
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
            ecopy(note, "nie da sie przeczytac palety obrazu",
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

    if (!memo_ready)
        colours_start();
    win_rect(winid, 0, 0, cw, ch, C_BG);

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
            out[n++] = nearest(r, g, b);
        }
        win_blit(winid, 0, dest, n, 1, out);
    }
    return 0;
}

/* Reads the picture and puts it in the window, row by row. Nothing is
   kept afterwards: the window's content IS the copy, and a second copy
   in this program's memory would be three hundred kilobytes held for
   no reason. */
static void show(const char *name)
{
    int fd = (int)sys_open(name, O_READ);
    if (fd < 0) {
        ecopy(note, "nie ma takiego pliku", sizeof(note));
        img_w = 0;
        return;
    }

    /* Enough of the front of the file to tell the formats apart and
       to hold a whole BMP header (fourteen plus forty bytes). */
    uint8_t head[HDR];
    int got = (int)sys_read(fd, head, HDR);
    if (got < 54) {
        sys_close(fd);
        ecopy(note, "to nie jest obraz", sizeof(note));
        img_w = 0;
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
        ecopy(note, "wczytany", sizeof(note));
        retitle();
        return;
    }

    if (got != HDR || head[0] != 'P' || head[1] != 'S' || head[2] != 'C' ||
        head[3] != 'R') {
        sys_close(fd);
        ecopy(note, "to nie jest obraz", sizeof(note));
        img_w = 0;
        return;
    }
    img_w = head[4] | (head[5] << 8);
    img_h = head[6] | (head[7] << 8);
    if (img_w <= 0 || img_w > ROW_MAX || img_h <= 0) {
        sys_close(fd);
        ecopy(note, "dziwne wymiary", sizeof(note));
        img_w = 0;
        return;
    }

    /* How much of it fits: every pixel, or every second, or third... */
    int room_w = cw, room_h = ch - STATUS_H - 2;
    step = 1;
    while ((img_w / step > room_w || img_h / step > room_h) && step < 16)
        step++;

    win_rect(winid, 0, 0, cw, ch, C_BG);
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
    ecopy(note, "wczytany", sizeof(note));
    retitle();
}

void _start(int argc, char **argv)
{
    int win = (int)win_create(W0, H0, "Obraz");
    if (win < 0)
        sys_exit(1);
    winid = win;

    win_rect(win, 0, 0, cw, ch, C_BG);
    if (argc > 1 && argv && argv[1] && argv[1][0])
        show(argv[1]);
    draw_status();

    for (;;) {
        struct win_event ev;
        int again = 0;
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
