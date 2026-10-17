/* gpaint - drawing with the mouse (ring 3)
 *
 * A canvas, a brush in one of eight colours and three sizes, a rubber,
 * and two buttons: one clears the canvas, one saves it - as a BMP,
 * which the picture viewer opens (Ctrl+S does the same). The brush has
 * a soft edge, the way everything on this desktop has: how much of a
 * pixel the round tip covers is how much of the colour it takes.
 *
 * The picture lives in the program, not in the window: a window's
 * pixels cannot be read back, and a picture that cannot be read back
 * cannot be saved. So the brush paints into the program's own copy,
 * and only the part that changed goes to the window.
 *
 * One program for both kernels (uapi.h).
 */

#include "udraw.h"

#define W0 440
#define H0 340
#define TOOL_H 44             /* the tools, along the top */
#define CW (W0)               /* the canvas: the rest of the window */
#define CH (H0 - TOOL_H)

#define SW_D   22             /* a colour's swatch */
#define SW_X0  12
#define SW_STEP 28
#define SW_Y   ((TOOL_H - SW_D) / 2)
#define SIZE_X0 248           /* the three sizes */
#define SIZE_STEP 26
#define BTN_Y  9
#define BTN_H  26
#define CLEAR_X 330
#define CLEAR_W 48
#define SAVE_X  384
#define SAVE_W  48

#define FILE_NAME "drawing.bmp"

static const uint32_t colours[8] = {
    0x1C1C1E, 0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x0A84FF, 0xAF52DE,
    0xFFFFFF                    /* the last one is the rubber: the paper */
};
static const int sizes[3] = { 2, 5, 10 };   /* the tip's radius */

static uint32_t canvas[CW * CH];
static int colour = 0, size = 1;
static int painting, last_x, last_y;
static int dx0, dy0, dx1, dy1;    /* what changed since it was shown */

static void dirty(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CW) x1 = CW;
    if (y1 > CH) y1 = CH;
    if (x0 >= x1 || y0 >= y1)
        return;
    if (dx1 <= dx0) {
        dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1;
        return;
    }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}

static uint32_t mix(uint32_t over, uint32_t under, int a)
{
    uint32_t r = (((over >> 16) & 255) * (uint32_t)a +
                  ((under >> 16) & 255) * (uint32_t)(255 - a) + 127) / 255;
    uint32_t g = (((over >> 8) & 255) * (uint32_t)a +
                  ((under >> 8) & 255) * (uint32_t)(255 - a) + 127) / 255;
    uint32_t b = ((over & 255) * (uint32_t)a +
                  (under & 255) * (uint32_t)(255 - a) + 127) / 255;
    return (r << 16) | (g << 8) | b;
}

/* The round tip, once, centred on (cx, cy) in sixteenths of a pixel.
   Inside r - 1/2 it covers the pixel; past r + 1/2 not at all; in
   between, as much as the distance says - measured in squares, which
   needs no square root and is near enough at these sizes. */
static void stamp(int cx, int cy, int r)
{
    int in = (r * 16 - 8) * (r * 16 - 8), out = (r * 16 + 8) * (r * 16 + 8);
    if (in < 0)
        in = 0;
    int x0 = (cx >> 4) - r - 1, x1 = (cx >> 4) + r + 2;
    int y0 = (cy >> 4) - r - 1, y1 = (cy >> 4) + r + 2;
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= CH)
            continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= CW)
                continue;
            int ddx = x * 16 + 8 - cx, ddy = y * 16 + 8 - cy;
            int d2 = ddx * ddx + ddy * ddy;
            if (d2 >= out)
                continue;
            int a = d2 <= in ? 255 : 255 * (out - d2) / (out - in);
            uint32_t *p = &canvas[y * CW + x];
            *p = mix(colours[colour], *p, a);
        }
    }
    dirty(x0, y0, x1, y1);
}

/* A stroke from the last point to this one, a stamp every half radius */
static void stroke(int x, int y)
{
    int r = sizes[size];
    int ax = last_x * 16 + 8, ay = last_y * 16 + 8;
    int bx = x * 16 + 8, by = y * 16 + 8;
    int ddx = bx - ax, ddy = by - ay;
    int len = (ddx < 0 ? -ddx : ddx) > (ddy < 0 ? -ddy : ddy)
                  ? (ddx < 0 ? -ddx : ddx) : (ddy < 0 ? -ddy : ddy);
    int steps = len / (r * 8 > 8 ? r * 8 : 8) + 1;
    for (int i = 1; i <= steps; i++)
        stamp(ax + ddx * i / steps, ay + ddy * i / steps, r);
    last_x = x;
    last_y = y;
}

static void clear_canvas(void)
{
    for (int i = 0; i < CW * CH; i++)
        canvas[i] = 0xFFFFFF;
    dirty(0, 0, CW, CH);
}

/* --- the file: a 24-bit BMP, bottom row first ------------------------- */

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void save(void)
{
    int fd = (int)sys_open(FILE_NAME, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) {
        sys_notify("Paint", "The drawing cannot be written - no disk?");
        return;
    }
    uint32_t row = (uint32_t)(CW * 3 + 3) & ~3u;
    uint8_t h[54] = { 'B', 'M' };
    le32(h + 2, 54 + row * CH);
    le32(h + 10, 54);
    le32(h + 14, 40);
    le32(h + 18, CW);
    le32(h + 22, CH);
    le16(h + 26, 1);
    le16(h + 28, 24);
    le32(h + 34, row * CH);
    sys_write(fd, h, sizeof(h));
    static uint8_t line[CW * 3 + 4];
    for (int y = CH - 1; y >= 0; y--) {
        for (int x = 0; x < CW; x++) {
            uint32_t c = canvas[y * CW + x];
            line[x * 3] = (uint8_t)c;
            line[x * 3 + 1] = (uint8_t)(c >> 8);
            line[x * 3 + 2] = (uint8_t)(c >> 16);
        }
        for (uint32_t k = (uint32_t)CW * 3; k < row; k++)
            line[k] = 0;
        sys_write(fd, line, row);
    }
    sys_close(fd);
    sys_notify("Paint", "Saved as " FILE_NAME);
}

/* --- drawing ------------------------------------------------------------- */

static void show_canvas(int win)
{
    if (dx1 <= dx0)
        return;
    int w = dx1 - dx0;
    static uint32_t buf[1024];
    int rows = 1024 / w;
    for (int y = dy0; y < dy1; y += rows) {
        int n = dy1 - y < rows ? dy1 - y : rows;
        for (int j = 0; j < n; j++)
            for (int i = 0; i < w; i++)
                buf[j * w + i] = canvas[(y + j) * CW + dx0 + i];
        win_blit32(win, dx0, TOOL_H + y, w, n, buf);
    }
    dx1 = dx0;
}

static void draw_tools(int win)
{
    struct ud_look l;
    ud_look(&l);
    ud_fill(win, 0, 0, W0, TOOL_H, l.bg);
    ud_fill(win, 0, TOOL_H - 1, W0, 1, l.line);
    for (int i = 0; i < 8; i++) {
        int x = SW_X0 + i * SW_STEP;
        if (i == colour)
            ud_ring(win, x - 3, SW_Y - 3, SW_D + 6, SW_D + 6, SW_D / 2 + 3,
                    l.accent);
        ud_disc(win, x, SW_Y, SW_D, colours[i]);
        ud_ring(win, x, SW_Y, SW_D, SW_D, SW_D / 2, l.line);
        if (i == 7) {                    /* the rubber: a slash across */
            ud_line(win, x + 6, SW_Y + SW_D - 6, x + SW_D - 6, SW_Y + 6, 24,
                    l.dim);
        }
    }
    for (int i = 0; i < 3; i++) {
        int x = SIZE_X0 + i * SIZE_STEP;
        int on = i == size;
        ud_round(win, x, BTN_Y, 22, BTN_H, 7, on ? l.accent : l.row);
        int d = 2 * sizes[i] > 14 ? 14 : 2 * sizes[i];
        ud_disc(win, x + 11 - d / 2, BTN_Y + BTN_H / 2 - d / 2, d,
                on ? l.on_accent : l.text);
    }
    ud_round(win, CLEAR_X, BTN_Y, CLEAR_W, BTN_H, 7, l.row);
    ud_text_mid(win, CLEAR_X, BTN_Y, CLEAR_W, BTN_H, "Clear", l.text,
                UD_SANS, UD_PX);
    ud_round(win, SAVE_X, BTN_Y, SAVE_W, BTN_H, 7, l.accent);
    ud_text_mid(win, SAVE_X, BTN_Y, SAVE_W, BTN_H, "Save", l.on_accent,
                UD_BOLD, UD_PX);
}

static void tool_click(int x, int y)
{
    for (int i = 0; i < 8; i++) {
        int sx = SW_X0 + i * SW_STEP;
        if (x >= sx && x < sx + SW_D && y >= SW_Y && y < SW_Y + SW_D)
            colour = i;
    }
    for (int i = 0; i < 3; i++) {
        int sx = SIZE_X0 + i * SIZE_STEP;
        if (x >= sx && x < sx + 22 && y >= BTN_Y && y < BTN_Y + BTN_H)
            size = i;
    }
    if (y >= BTN_Y && y < BTN_Y + BTN_H) {
        if (x >= CLEAR_X && x < CLEAR_X + CLEAR_W)
            clear_canvas();
        if (x >= SAVE_X && x < SAVE_X + SAVE_W)
            save();
    }
}

void _start(void)
{
    int win = (int)win_create(W0, H0, "Paint");
    if (win < 0)
        sys_exit(1);
    win_truecolour(win);
    clear_canvas();
    draw_tools(win);
    show_canvas(win);

    struct ud_look was;
    ud_look(&was);
    for (;;) {
        int tools = 0;
        struct win_event ev;
        while (win_poll(win, &ev) == 1) {
            if (ev.type == WEV_CLOSE)
                sys_exit(0);
            if (ev.type == WEV_PRESS) {
                if (ev.y >= TOOL_H) {
                    painting = 1;
                    last_x = ev.x;
                    last_y = ev.y - TOOL_H;
                    stroke(last_x, last_y);
                } else {
                    tool_click(ev.x, ev.y);
                    tools = 1;
                }
            } else if (ev.type == WEV_DRAG && painting) {
                stroke(ev.x, ev.y < TOOL_H ? 0 : ev.y - TOOL_H);
            } else if (ev.type == WEV_RELEASE) {
                painting = 0;
            } else if (ev.type == WEV_KEY) {
                char c = (char)ev.val;
                if (c == KEY_SAVE)
                    save();
                else if (c >= '1' && c <= '8')
                    colour = c - '1';
                tools = 1;
            }
        }
        struct ud_look now;
        ud_look(&now);
        if (now.dark != was.dark) {
            was = now;
            tools = 1;
        }
        if (tools)
            draw_tools(win);
        show_canvas(win);
        sys_sleep(20);
    }
}
