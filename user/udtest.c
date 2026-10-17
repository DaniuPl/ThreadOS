/* udtest - the drawing calls, drawn where a test can measure them (ring 3)
 *
 * user/udraw.h hands a program the desktop's own tools: soft-edged
 * shapes, TrueType text, the theme's colours. This program puts one of
 * each into a window - white on black, so that how much of a pixel is
 * white is how much of it the shape covers - and says on its standard
 * output what the calls answered: how wide a text is, where the pen
 * ended, what a call on somebody else's window got, and the theme's
 * colours. scripts/tests/t_udraw.py holds all of it against exact
 * areas, FreeType and kernel/include/look.h.
 *
 * It asks for the colours again twice a second and says them once more
 * when they change, which is how a switch of the theme is seen to reach
 * a program. One source for both kernels (uapi.h).
 */

#include "udraw.h"

#define W 200
#define H 120

static const char TEXT[] = "ThreadOS 1234";

static int put(char *d, const char *s)
{
    int n = 0;
    while (s[n]) {
        d[n] = s[n];
        n++;
    }
    return n;
}

static int dec(char *d, int v)
{
    char tmp[12];
    int n = 0, k = 0;
    if (v < 0) {
        d[k++] = '-';
        v = -v;
    }
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        d[k++] = tmp[--n];
    return k;
}

static int hex6(char *d, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++)
        d[i] = digits[(v >> (20 - 4 * i)) & 0xF];
    return 6;
}

static void say_look(const struct ud_look *l)
{
    const uint32_t *v = (const uint32_t *)l;
    char line[128];
    int n = put(line, "udtest: look");
    for (int i = 0; i < 11; i++) {
        line[n++] = ' ';
        n += hex6(line + n, v[i]);
    }
    line[n++] = '\n';
    line[n] = '\0';
    print(line);
}

void _start(void)
{
    int w = (int)win_create(W, H, "udtest");
    if (w < 0)
        sys_exit(1);

    ud_fill(w, 0, 0, W, H, 0x000000);
    ud_round(w, 4, 4, 40, 28, 8, 0xFFFFFF);
    ud_ring(w, 52, 4, 36, 28, 10, 0xFFFFFF);
    ud_disc(w, 96, 4, 25, 0xFFFFFF);
    ud_line(w, 130, 6, 190, 30, 24, 0xFFFFFF);
    ud_grad(w, 4, 40, 60, 24, 0, 0xFF0000, 0x0000FF);
    ud_fill_a(w, 70, 40, 30, 24, 0xFFFFFF, 128);
    int pen = ud_text(w, 4, 100, TEXT, 0xFFFFFF, UD_SANS, UD_PX);
    int width = ud_width(TEXT, UD_SANS, UD_PX);

    /* Window 0 is the kernel's own: drawing there is not ours to do */
    struct win_draw_cmd c = { UD_OP_FILL, 0, 0, 10, 10, 0, 0xFF0000, 0, 255,
                              0, 0, (const char *)0 };
    int foreign = sys_win_draw(0, &c);

    char line[96];
    int n = put(line, "udtest: width ");
    n += dec(line + n, width);
    n += put(line + n, " pen ");
    n += dec(line + n, pen);
    n += put(line + n, " foreign ");
    n += dec(line + n, foreign);
    line[n++] = '\n';
    line[n] = '\0';
    print(line);

    struct ud_look was;
    ud_look(&was);
    say_look(&was);

    for (;;) {
        struct win_event ev;
        while (win_poll(w, &ev) == 1)
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
        struct ud_look now;
        ud_look(&now);
        const uint32_t *a = (const uint32_t *)&now;
        const uint32_t *b = (const uint32_t *)&was;
        int changed = 0;
        for (int i = 0; i < 11; i++)
            if (a[i] != b[i])
                changed = 1;
        if (changed) {
            was = now;
            say_look(&now);
        }
        sys_sleep(500);
    }
}
