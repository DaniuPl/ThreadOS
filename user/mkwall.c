/* mkwall - makes a wallpaper file for the desktop
 *
 * The desktop can put a picture behind everything, and this is where
 * one comes from: a machine with no camera and no image editor still
 * has arithmetic, so the pictures here are computed rather than drawn.
 *
 * The file is what kernel/desktop.c reads: "PWAL", the size, 192
 * colours of three bytes, then one byte per pixel - an index into
 * those colours. Small on purpose (160x120): the kernel stretches it
 * over the screen, and a screen-sized image would be a third of the
 * kernel's heap.
 *
 * Four patterns, because four is what one afternoon of arithmetic is
 * worth: a two-colour slope, rings, a weave and a grid. Any of them
 * can be had in any hue - the hue is what a person actually chooses.
 */

#include "usys.h"

#define W 160
#define H 120
#define COLS 192
#define HDR (4 + 2 + 2 + COLS * 3)

static unsigned char out[HDR + W * H];

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

void _start(int argc, char **argv)
{
    /* mkwall <plik> [wzor 0-3] [barwa 0-5] */
    if (argc < 2) {
        print("uzycie: mkwall <plik> [wzor 0-3] [barwa 0-5]\n");
        print("  wzory: 0 zbocze, 1 pierscienie, 2 plecionka, 3 krata\n");
        sys_exit(1);
    }
    int pattern = argc > 2 ? text_to_num(argv[2]) % 4 : 0;
    int hue = argc > 3 ? text_to_num(argv[3]) % 6 : 0;

    out[0] = 'P'; out[1] = 'W'; out[2] = 'A'; out[3] = 'L';
    out[4] = W & 0xFF; out[5] = (W >> 8) & 0xFF;
    out[6] = H & 0xFF; out[7] = (H >> 8) & 0xFF;

    /* The palette: 192 steps from dark to light in the chosen hue.
       Two hues get a second component so that 'teal' is teal and not
       a blue with a name. */
    for (int i = 0; i < COLS; i++) {
        int v = 40 + i * 215 / (COLS - 1);        /* 40..255 */
        int r = 0, g = 0, b = 0;
        switch (hue) {
        case 0: g = v * 3 / 4; b = v; break;      /* teal */
        case 1: b = v; break;                     /* blue */
        case 2: r = v * 3 / 5; b = v; break;      /* violet */
        case 3: g = v; break;                     /* green */
        case 4: r = v; g = v * 2 / 5; break;      /* amber */
        default: r = g = b = v; break;            /* grey */
        }
        out[8 + i * 3] = (unsigned char)r;
        out[9 + i * 3] = (unsigned char)g;
        out[10 + i * 3] = (unsigned char)b;
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int v;
            switch (pattern) {
            case 1: {                             /* rings from the middle */
                int dx = x - W / 2, dy = (y - H / 2) * 4 / 3;
                uint32_t d = isqrt((uint32_t)(dx * dx + dy * dy));
                v = (int)((d * 6) % COLS);
                break;
            }
            case 2:                               /* a weave */
                v = ((x * 5 / 4 + y * 3 / 4) % 48) * COLS / 48;
                break;
            case 3:                               /* a grid */
                v = (x % 20 < 2 || y % 20 < 2) ? COLS - 1
                                               : (y * COLS / H) / 3;
                break;
            default:                              /* a slope */
                v = COLS - 1 - (y * COLS / H);
                break;
            }
            if (v < 0)
                v = 0;
            if (v >= COLS)
                v = COLS - 1;
            out[HDR + y * W + x] = (unsigned char)v;
        }
    }

    int fd = sys_open(argv[1], O_WRITE | O_CREATE | O_TRUNC);
    if (fd == -2) {
        print("mkwall: brak uprawnien do '");
        print(argv[1]);
        print("'\n");
        sys_exit(2);
    }
    if (fd < 0) {
        print("mkwall: nie moge utworzyc '");
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
    print(" gotowa (ustaw 'set tapeta ");
    print(argv[1]);
    print("')\n");
    sys_exit(at == sizeof(out) ? 0 : 1);
}
