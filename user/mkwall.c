/* mkwall - makes a wallpaper file for the desktop
 *
 * The desktop can put a picture behind everything, and this is where
 * one comes from: a machine with no camera and no image editor still
 * has arithmetic, so the pictures here are computed rather than drawn.
 *
 * The file is what kernel/desktop.c reads: "PWL2", the size, then
 * three bytes a pixel. Small on purpose (160x120): the kernel
 * stretches it over the screen, and a screen-sized image would be a
 * large part of the kernel's heap.
 *
 * It used to write a palette of 192 colours and one index per pixel,
 * which was the right shape for a screen that had 256 colours in all.
 * For four of the five patterns here it made no difference - a slope
 * down 120 rows is 120 colours whatever you do. For the fifth it makes
 * all of it: a blend across BOTH axes is one colour per pixel, near
 * twenty thousand of them, and no palette of 192 holds that.
 *
 * Five patterns, then: a slope, rings, a weave, a grid and a blend.
 * Any of them can be had in any hue - the hue is what a person
 * actually chooses.
 */

#include "usys.h"

#define W 160
#define H 120
#define LEVELS 192          /* how many steps the patterns are drawn in */
#define HDR 8

static unsigned char out[HDR + W * H * 3];

static int text_to_num(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* An unsigned square root, for the rings - no floating point here and
   none wanted: the answer is a pixel number. */
static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* A hue at a brightness. Two of them get a second component so that
   'teal' is teal and not a blue with a name. */
static void hue_rgb(int hue, int v, int *r, int *g, int *b)
{
    *r = *g = *b = 0;
    switch (hue % 6) {
    case 0: *g = v * 3 / 4; *b = v; break;        /* teal */
    case 1: *b = v; break;                        /* blue */
    case 2: *r = v * 3 / 5; *b = v; break;        /* violet */
    case 3: *g = v; break;                        /* green */
    case 4: *r = v; *g = v * 2 / 5; break;        /* amber */
    default: *r = *g = *b = v; break;             /* grey */
    }
}

void _start(int argc, char **argv)
{
    /* mkwall <plik> [wzor 0-4] [barwa 0-5] */
    if (argc < 2) {
        print("usage: mkwall <file> [pattern 0-4] [hue 0-5]\n");
        print("  patterns: 0 slope, 1 rings, 2 weave, 3 lattice,\n");
        print("            4 blend (thousands of colours)\n");
        sys_exit(1);
    }
    int pattern = argc > 2 ? text_to_num(argv[2]) % 5 : 0;
    int hue = argc > 3 ? text_to_num(argv[3]) % 6 : 0;

    out[0] = 'P'; out[1] = 'W'; out[2] = 'L'; out[3] = '2';
    out[4] = W & 0xFF; out[5] = (W >> 8) & 0xFF;
    out[6] = H & 0xFF; out[7] = (H >> 8) & 0xFF;

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int r, g, b;
            if (pattern == 4) {
                /* The blend: brightness down the picture, and across
                   it a walk from the chosen hue to the one three
                   places along. Every pixel gets its own colour, which
                   is the thing a palette could not give. */
                int v = 40 + (H - 1 - y) * 215 / (H - 1);
                int mix = x * 255 / (W - 1);
                int r1, g1, b1, r2, g2, b2;
                hue_rgb(hue, v, &r1, &g1, &b1);
                hue_rgb(hue + 3, v, &r2, &g2, &b2);
                r = (r1 * (255 - mix) + r2 * mix) / 255;
                g = (g1 * (255 - mix) + g2 * mix) / 255;
                b = (b1 * (255 - mix) + b2 * mix) / 255;
            } else {
                int i;
                switch (pattern) {
                case 1: {                         /* rings from the middle */
                    int dx = x - W / 2, dy = (y - H / 2) * 4 / 3;
                    uint32_t d = isqrt((uint32_t)(dx * dx + dy * dy));
                    i = (int)((d * 6) % LEVELS);
                    break;
                }
                case 2:                           /* a weave */
                    i = ((x * 5 / 4 + y * 3 / 4) % 48) * LEVELS / 48;
                    break;
                case 3:                           /* a grid */
                    i = (x % 20 < 2 || y % 20 < 2) ? LEVELS - 1
                                                   : (y * LEVELS / H) / 3;
                    break;
                default:                          /* a slope */
                    i = LEVELS - 1 - (y * LEVELS / H);
                    break;
                }
                if (i < 0)
                    i = 0;
                if (i >= LEVELS)
                    i = LEVELS - 1;
                hue_rgb(hue, 40 + i * 215 / (LEVELS - 1), &r, &g, &b);
            }
            unsigned char *p = out + HDR + (y * W + x) * 3;
            p[0] = (unsigned char)r;
            p[1] = (unsigned char)g;
            p[2] = (unsigned char)b;
        }
    }

    int fd = sys_open(argv[1], O_WRITE | O_CREATE | O_TRUNC);
    if (fd == -2) {
        print("mkwall: no permission for '");
        print(argv[1]);
        print("'\n");
        sys_exit(2);
    }
    if (fd < 0) {
        print("mkwall: I cannot create '");
        print(argv[1]);
        print("'\n");
        sys_exit(1);
    }
    /* In pieces: the syscall takes what it takes, and a picture is
       bigger than that */
    uint32_t at = 0;
    while (at < sizeof(out)) {
        uint32_t n = sizeof(out) - at;
        if (n > 1024)
            n = 1024;
        int32_t wrote = sys_write(fd, out + at, n);
        if (wrote <= 0)
            break;
        at += (uint32_t)wrote;
    }
    sys_close(fd);

    print("mkwall: ");
    print(argv[1]);
    print(" is ready (set it with 'set wallpaper ");
    print(argv[1]);
    print("')\n");
    sys_exit(at == sizeof(out) ? 0 : 1);
}
