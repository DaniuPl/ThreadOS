#include <stdint.h>
#include "include/bootsplash.h"
#include "include/font8x8.h"
#include "include/printk.h"
#include "include/video.h"

#define SPLASH_BG       0x08111F
#define SPLASH_TEXT     0xE8F0FA
#define SPLASH_MUTED    0x8EA2BA
#define SPLASH_ACCENT   0x57B8FF
#define SPLASH_BAR_BG   0x24344A

static int active;
static int scale;
static int screen_w;
static int screen_h;
static int center_y;
static int bar_x;
static int bar_w;
static int bar_y;

static void draw_text(int x, int y, int size, const char *text, uint32_t color)
{
    for (int i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c > 0x7E)
            continue;
        const uint8_t *glyph = font8x8[c - 0x20];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                if (glyph[row] & (1u << col))
                    video_fill_rect_rgb(x + (i * 8 + col) * size,
                                        y + row * size, size, size, color);
            }
        }
    }
}

static int text_width(const char *text, int size)
{
    int n = 0;
    while (text[n])
        n++;
    return n * 8 * size;
}

static void draw_title(void)
{
    const char *title = "ThreadOS";
    int title_size = scale;
    int x = (screen_w - text_width(title, title_size)) / 2;
    int y = center_y - 58 * scale / 2;
    draw_text(x, y, title_size, title, SPLASH_TEXT);
}

void boot_splash_start(int requested)
{
    if (!requested || !video_have_loader_lfb())
        return;

    screen_w = video_w();
    screen_h = video_h();
    if (screen_w < 1 || screen_h < 1)
        return;

    active = 1;
    scale = screen_w >= 640 ? 3 : 2;
    center_y = screen_h / 2;
    bar_w = screen_w - 64;
    if (bar_w > 480)
        bar_w = 480;
    if (bar_w < 80)
        bar_w = screen_w - 16;
    bar_x = (screen_w - bar_w) / 2;
    bar_y = center_y + 38;

    printk_screen_output(0);
    video_fill_rect_rgb(0, 0, screen_w, screen_h, SPLASH_BG);
    draw_title();
    video_present();
    printk("[boot] visual loading screen active\n");
    boot_splash_update(3, "Starting the kernel");
}

void boot_splash_update(int progress, const char *status)
{
    if (!active || !status)
        return;
    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;

    int status_size = scale > 2 ? 2 : 1;
    int width = text_width(status, status_size);
    if (width > screen_w - 16) {
        status_size = 1;
        width = text_width(status, status_size);
    }
    int status_x = (screen_w - width) / 2;
    int status_y = bar_y - 25;
    video_fill_rect_rgb(0, status_y - 2, screen_w, 18,
                        SPLASH_BG);
    draw_text(status_x, status_y, status_size, status, SPLASH_MUTED);

    video_fill_rect_rgb(bar_x, bar_y, bar_w, 6, SPLASH_BAR_BG);
    int filled = bar_w * progress / 100;
    if (filled > 0)
        video_fill_rect_rgb(bar_x, bar_y, filled, 6, SPLASH_ACCENT);
    video_present();
}
