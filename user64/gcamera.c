/* gcamera - the camera in the lid
 *
 * A picture of whatever the camera sees, as it sees it, and a button
 * that keeps one: in the Gallery folder of the home, where the Gallery
 * shows it. The kernel's driver (kernel/uvc.c) does the USB; this asks
 * it for the newest frame thirty times a second (SYS64_CAMERA) and
 * shows it - uncompressed YUY2 turned into colours the BT.601 way, an
 * MJPEG frame through the system's own JPEG decoder (user/ujpeg.h,
 * which knows the Huffman tables a webcam leaves out).
 *
 * A photo is the frame itself, not the preview: an MJPEG frame is
 * saved as the JPEG it is, with the standard Huffman tables written
 * into it when the camera left them out, so any viewer can open it;
 * a YUY2 frame is saved as a PNG. Nothing is decoded twice or
 * compressed again on the way.
 *
 * While the window is open the camera is on, and the desktop shows a
 * green dot on the bar; closing the window - or the program ending in
 * any way - turns it off.
 *
 * Built against musl, for zlib and for memory given back.
 */

#include "uapi.h"
#include "udraw.h"
#include "ulocale.h"
#include "ucam.h"
#include "ujpeg.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define W0 480
#define H0 400
#define PREVIEW_H 352         /* the picture; the strip below has the rest */
#define STRIP_H 48
#define SHUTTER_D 34          /* the round button, in the middle */
#define MODE_W 120            /* the mode, at the left, which changes it */
#define FRAME_MAX (4 * 1024 * 1024)

static int win = -1;
static int cw = W0, ch = H0;
static struct ud_look l;
static struct cam_info info;
static int usable[CAM_MODES], nusable, pick;   /* the modes that stream */
static int open_mode = -1;
static const char *trouble;    /* why there is no picture, or 0 */
static char said[64];          /* "Saved Photo-1.jpg" */

static uint8_t *frame;         /* the newest frame, as the camera sent it */
static uint32_t frame_len, frame_seq, frame_cc;
static uint16_t frame_w, frame_h;
static uint32_t *rgb;          /* ... and in colours */
static uint8_t *jmem;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | \
                            (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

/* --- the camera ----------------------------------------------------------- */

/* Which modes stream (an isochronous alternate carries them), the
   largest that fits a 640x480 preview first, MJPEG before YUY2 at the
   same size: it leaves the bus room for more */
static void list_modes(void)
{
    nusable = 0;
    for (int k = 0; k < info.nmodes; k++) {
        const struct cam_mode_info *m = &info.modes[k];
        if (m->alt > 0 && m->w <= 640 && m->h <= 480 &&
            (m->fourcc == FOURCC('M', 'J', 'P', 'G') ||
             m->fourcc == FOURCC('Y', 'U', 'Y', '2')))
            usable[nusable++] = k;
    }
    for (int i = 1; i < nusable; i++) {
        int t = usable[i], j = i;
        const struct cam_mode_info *a = &info.modes[t];
        while (j > 0) {
            const struct cam_mode_info *b = &info.modes[usable[j - 1]];
            long sa = (long)a->w * a->h, sb = (long)b->w * b->h;
            int before = sa > sb || (sa == sb &&
                         a->fourcc == FOURCC('M', 'J', 'P', 'G') &&
                         b->fourcc != a->fourcc);
            if (!before)
                break;
            usable[j] = usable[j - 1];
            j--;
        }
        usable[j] = t;
    }
}

static void start(void)
{
    if (open_mode >= 0)
        sys_camera(CAM_CLOSE, 0);
    open_mode = -1;
    frame_len = 0;
    frame_seq = 0;
    if (!nusable) {
        trouble = info.present ? "The camera has no mode to show."
                               : "There is no camera.";
        return;
    }
    int r = sys_camera(CAM_OPEN, usable[pick]);
    if (r == -6)
        trouble = "Another program is using the camera.";
    else if (r == -2)
        trouble = "No room on the USB bus for the camera.";
    else if (r < 0)
        trouble = "The camera did not start.";
    else {
        trouble = 0;
        open_mode = usable[pick];
    }
}

static int clamp(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* A new frame into colours: 1 when it could be */
static int decode(void)
{
    int w = frame_w, h = frame_h;
    if (frame_cc == FOURCC('Y', 'U', 'Y', '2')) {
        if (frame_len != (uint32_t)w * h * 2)
            return 0;
        /* BT.601, limited range, in integers - as cameras mean it */
        for (int i = 0; i < w * h; i += 2) {
            const uint8_t *p = frame + i * 2;
            int d = p[1] - 128, e = p[3] - 128;
            for (int k = 0; k < 2; k++) {
                int c = p[k * 2] - 16;
                int r = clamp((298 * c + 409 * e + 128) >> 8);
                int g = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
                int b = clamp((298 * c + 516 * d + 128) >> 8);
                rgb[i + k] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            }
        }
        return 1;
    }
    const char *why;
    int jw = 0, jh = 0;
    if (jpeg_decode(frame, frame_len, jmem, 3 * 640 * 480 + 65536, &jw, &jh,
                    &why) < 0 || jw > 640 || jh > 480)
        return 0;
    frame_w = (uint16_t)jw;
    frame_h = (uint16_t)jh;
    for (int y = 0; y < jh; y++)
        for (int x = 0; x < jw; x++) {
            int r, g, b;
            jpeg_pixel(x, y, &r, &g, &b);
            rgb[y * jw + x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    return 1;
}

/* --- keeping one ------------------------------------------------------------ */

/* All of it: a write takes at most a page at a time */
static int write_all(int fd, const uint8_t *p, uint32_t n)
{
    while (n) {
        int64_t w = sys_write(fd, p, n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (uint32_t)w;
    }
    return 0;
}

static int write_file(const char *name, const uint8_t *a, uint32_t na,
                      const uint8_t *b, uint32_t nb, const uint8_t *c,
                      uint32_t nc)
{
    int fd = (int)sys_open(name, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0)
        return -1;
    int ok = write_all(fd, a, na) == 0 && write_all(fd, b, nb) == 0 &&
             write_all(fd, c, nc) == 0;
    sys_close(fd);
    return ok ? 0 : -1;
}

/* The JPEG's DHT, as the standard has the tables (ujpeg.h's), for a
   frame that left them out */
static uint32_t make_dht(uint8_t *o)
{
    uint32_t n = 4;
    o[0] = 0xFF;
    o[1] = 0xC4;
    for (int t = 0; t < 4; t++) {
        o[n++] = (uint8_t)((t >> 1) << 4 | (t & 1));   /* class, id */
        int total = 0;
        for (int i = 0; i < 16; i++) {
            o[n++] = jpg_k_bits[t][i];
            total += jpg_k_bits[t][i];
        }
        const unsigned char *v = t < 2 ? jpg_k_dc_vals :
                                 t == 2 ? jpg_k_ac0_vals : jpg_k_ac1_vals;
        for (int i = 0; i < total; i++)
            o[n++] = v[i];
    }
    o[2] = (uint8_t)((n - 2) >> 8);
    o[3] = (uint8_t)(n - 2);
    return n;
}

static void put32be(uint8_t *o, uint32_t v)
{
    o[0] = (uint8_t)(v >> 24);
    o[1] = (uint8_t)(v >> 16);
    o[2] = (uint8_t)(v >> 8);
    o[3] = (uint8_t)v;
}

static int save_png(const char *name)
{
    int w = frame_w, h = frame_h;
    uLong raw_n = (uLong)h * (1 + (uLong)w * 3);
    uint8_t *raw = malloc(raw_n);
    uLong z_n = compressBound(raw_n);
    uint8_t *z = malloc(z_n + 12);
    if (!raw || !z) {
        free(raw);
        free(z);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        uint8_t *o = raw + (uLong)y * (1 + (uLong)w * 3);
        *o++ = 0;                               /* no filter */
        for (int x = 0; x < w; x++) {
            uint32_t p = rgb[y * w + x];
            *o++ = (uint8_t)(p >> 16);
            *o++ = (uint8_t)(p >> 8);
            *o++ = (uint8_t)p;
        }
    }
    compress2(z + 8, &z_n, raw, raw_n, 6);
    free(raw);
    /* IDAT: length, name, the stream, its CRC */
    put32be(z, (uint32_t)z_n);
    memcpy(z + 4, "IDAT", 4);
    put32be(z + 8 + z_n, (uint32_t)crc32(0, z + 4, (uInt)z_n + 4));
    static uint8_t head[8 + 25];
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    memcpy(head, sig, 8);
    put32be(head + 8, 13);
    memcpy(head + 12, "IHDR", 4);
    put32be(head + 16, (uint32_t)w);
    put32be(head + 20, (uint32_t)h);
    head[24] = 8;                               /* bits a channel */
    head[25] = 2;                               /* RGB */
    head[26] = head[27] = head[28] = 0;
    put32be(head + 29, (uint32_t)crc32(0, head + 12, 17));
    static const uint8_t end[12] = { 0, 0, 0, 0, 'I', 'E', 'N', 'D',
                                     0xAE, 0x42, 0x60, 0x82 };
    int r = write_file(name, head, sizeof(head), z, (uint32_t)z_n + 12, end,
                       sizeof(end));
    free(z);
    return r;
}

static int save_jpeg(const char *name)
{
    /* Where the scan begins, and whether a DHT came before it */
    uint32_t sos = 0;
    int dht = 0;
    for (uint32_t i = 2; i + 4 <= frame_len;) {
        if (frame[i] != 0xFF)
            break;
        uint8_t m = frame[i + 1];
        if (m == 0xC4)
            dht = 1;
        if (m == 0xDA) {
            sos = i;
            break;
        }
        i += 2 + ((uint32_t)frame[i + 2] << 8 | frame[i + 3]);
    }
    if (!sos)
        return -1;
    if (dht)
        return write_file(name, frame, frame_len, 0, 0, 0, 0);
    static uint8_t table[512];
    uint32_t n = make_dht(table);
    return write_file(name, frame, sos, table, n, frame + sos,
                      frame_len - sos);
}

static void shutter(void)
{
    if (!frame_len) {
        strcpy(said, "No picture yet");
        return;
    }
    char home[200];
    if (sys_getcwd(home, sizeof(home) - 40) < 0)
        strcpy(home, "/");
    size_t n = strlen(home);
    if (n && home[n - 1] != '/')
        home[n++] = '/';
    strcpy(home + n, "Gallery");
    sys_mkdir(home);
    n = strlen(home);
    home[n++] = '/';
    const char *ext = frame_cc == FOURCC('Y', 'U', 'Y', '2') ? ".png"
                                                              : ".jpg";
    /* The first number not taken */
    for (int k = 1; k < 10000; k++) {
        char *p = home + n;
        strcpy(p, "Photo-");
        p += 6;
        char digits[8];
        int d = 0, v = k;
        do {
            digits[d++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (d)
            *p++ = digits[--d];
        /* a number is taken whichever kind of picture has it: the
           photos are counted together */
        int taken = 0;
        static const char *const kinds[2] = { ".jpg", ".png" };
        for (int t = 0; t < 2 && !taken; t++) {
            strcpy(p, kinds[t]);
            int fd = (int)sys_open(home, O_READ);
            if (fd >= 0) {
                sys_close(fd);
                taken = 1;
            }
        }
        if (taken)
            continue;
        strcpy(p, ext);
        int r = frame_cc == FOURCC('Y', 'U', 'Y', '2') ? save_png(home)
                                                        : save_jpeg(home);
        strcpy(said, r == 0 ? "Saved " : "Could not save ");
        strncat(said, home + n, sizeof(said) - strlen(said) - 1);
        return;
    }
}

/* --- drawing ---------------------------------------------------------------- */

static void blit(int x, int y, int w, int h, const uint32_t *px, int stride)
{
    static uint32_t row[1024];
    for (int r = 0; r < h; r++) {
        if (w <= 1024 && stride == w) {
            int step = 1024 / w;
            int n = h - r < step ? h - r : step;
            win_blit32(win, x, y + r, w, n, px + (long)r * stride);
            r += n - 1;
        } else {
            for (int c = 0; c < w; c += 1024) {
                int n = w - c < 1024 ? w - c : 1024;
                memcpy(row, px + (long)r * stride + c, (size_t)n * 4);
                win_blit32(win, x + c, y + r, n, 1, row);
            }
        }
    }
}

static void draw_preview(int have)
{
    int ph = ch - STRIP_H;
    ud_fill(win, 0, 0, cw, ph, 0x000000);
    if (trouble) {
        ud_text_mid_l(win, 0, 0, cw, ph, trouble, 0xC8C8C8, UD_SANS, UD_PX);
        return;
    }
    if (!have) {
        ud_text_mid_l(win, 0, 0, cw, ph, "Starting the camera...", 0xC8C8C8,
                      UD_SANS, UD_PX);
        return;
    }
    int w = frame_w, h = frame_h;
    if (w <= cw && h <= ph) {
        /* at its own size, in the middle: a camera's picture is not
           made larger than it is */
        blit((cw - w) / 2, (ph - h) / 2, w, h, rgb, w);
        return;
    }
    /* larger than the window: every n-th pixel, n the least that fits */
    int n = 1;
    while (w / n > cw || h / n > ph)
        n++;
    int sw = w / n, sh = h / n;
    static uint32_t small[640 * 480];
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            small[y * sw + x] = rgb[(y * n) * w + x * n];
    blit((cw - sw) / 2, (ph - sh) / 2, sw, sh, small, sw);
}

static void draw_strip(void)
{
    int y = ch - STRIP_H;
    ud_fill(win, 0, y, cw, STRIP_H, l.bg);
    /* the mode, which a click changes */
    if (open_mode >= 0) {
        const struct cam_mode_info *m = &info.modes[open_mode];
        char t[32];
        int n = 0;
        for (int i = 0; i < 4; i++)
            t[n++] = (char)(m->fourcc >> (8 * i));
        t[n++] = ' ';
        int parts[2] = { m->w, m->h };
        for (int k = 0; k < 2; k++) {
            char d[8];
            int dn = 0, v = parts[k];
            do {
                d[dn++] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            while (dn)
                t[n++] = d[--dn];
            if (!k)
                t[n++] = 'x';
        }
        t[n] = 0;
        ud_round(win, 8, y + 10, MODE_W, STRIP_H - 20, 8, l.row);
        ud_text_mid(win, 8, y + 10, MODE_W, STRIP_H - 20, t, l.text, UD_SANS,
                    UD_PX);
    }
    /* the shutter */
    int sx = (cw - SHUTTER_D) / 2, sy = y + (STRIP_H - SHUTTER_D) / 2;
    ud_round(win, sx, sy, SHUTTER_D, SHUTTER_D, SHUTTER_D / 2, l.text);
    ud_round(win, sx + 3, sy + 3, SHUTTER_D - 6, SHUTTER_D - 6,
             SHUTTER_D / 2 - 3, l.bg);
    ud_round(win, sx + 5, sy + 5, SHUTTER_D - 10, SHUTTER_D - 10,
             SHUTTER_D / 2 - 5, open_mode >= 0 ? l.text : l.dim);
    if (said[0])
        ud_text_right(win, cw - 10, ud_baseline(y, STRIP_H, 11), said, l.dim,
                      UD_SANS, 11);
}

/* --- the program ---------------------------------------------------------- */

static void click(int x, int y)
{
    int sy = ch - STRIP_H;
    if (y < sy)
        return;
    if (x >= 8 && x < 8 + MODE_W && nusable > 1) {
        pick = (pick + 1) % nusable;
        start();
        said[0] = 0;
        return;
    }
    if (x >= (cw - SHUTTER_D) / 2 && x < (cw + SHUTTER_D) / 2)
        shutter();
}

int main(void)
{
    win = (int)win_create(W0, H0, "Camera");
    if (win < 0 || win_truecolour(win) < 0)
        return 1;
    ulocale_window_title(win, "Camera", "Aparat");
    ud_look(&l);
    frame = malloc(FRAME_MAX);
    rgb = malloc(640 * 480 * 4);     /* the modes shown are 640x480 at most */
    jmem = malloc(3 * 640 * 480 + 65536);
    if (!frame || !rgb || !jmem)
        return 1;
    sys_camera(CAM_INFO, (int64_t)(uintptr_t)&info);
    list_modes();
    start();
    draw_preview(0);
    draw_strip();

    int have = 0;
    for (;;) {
        struct win_event ev;
        int again = 0;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE) {
                sys_camera(CAM_CLOSE, 0);
                return 0;
            }
            if (ev.type == WEV_SIZE) {
                cw = ev.x;
                ch = ev.y;
            } else if (ev.type == WEV_CLICK) {
                click(ev.x, ev.y);
            } else if (ev.type == WEV_KEY && ev.val == ' ') {
                shutter();
            }
            again = 1;
        }
        if (open_mode >= 0) {
            struct cam_frame f;
            memset(&f, 0, sizeof(f));
            f.buf = (uint64_t)(uintptr_t)frame;
            f.cap = FRAME_MAX;
            f.seq = frame_seq;
            int n = sys_camera(CAM_FRAME, (int64_t)(uintptr_t)&f);
            if (n > 0) {
                frame_len = (uint32_t)n;
                frame_seq = f.seq;
                frame_cc = f.fourcc;
                frame_w = f.w;
                frame_h = f.h;
                if (decode()) {
                    have = 1;
                    draw_preview(1);
                }
            }
        }
        if (again) {
            ud_look(&l);
            draw_preview(have);
            draw_strip();
        }
        sys_sleep(30);
    }
}
