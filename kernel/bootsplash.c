/* The loading screen of a boot to the desktop or the installer
 *
 * The mark and a bar, as a Mac starts: black over the whole framebuffer
 * the loader set, ThreadOS's mark (art/threados-mark.svg - a cog with
 * threads running out of it) large and white in the middle, and under
 * it a thin bar with round ends that fills as the boot goes on. No
 * words: what the kernel is doing goes to the serial port, where it
 * always has. It used to be the 8x8 bitmap font blown up three times on
 * flat navy, the one screen that looked like none of the system.
 *
 * The mark is kernel/mark.c's, made from the SVG at build time.
 * Nothing needs the initrd, so all of it is there from the first
 * moment. It is drawn
 * straight onto the loader's framebuffer, whatever its format; logical
 * pixels (video_w(), video_h()) place things, and on a panel of more
 * pixels than that the mark and the bar are drawn at its size, as the
 * desktop draws them.
 */

#include <stdint.h>
#include "include/bootsplash.h"
#include "include/gfx.h"
#include "include/printk.h"
#include "include/video.h"
#include "include/mark.h"

/* Logical pixels, from the middle of the screen; t_bootsplash reads them */
#define SPLASH_LOGO_D     96       /* the mark's box (its longer side) */
#define SPLASH_LOGO_DY    24       /* its middle above the screen's */
#define SPLASH_BAR_W      200
#define SPLASH_BAR_H      6
#define SPLASH_BAR_DY     64       /* the bar's top below the middle */

#define SPLASH_BG         0x000000
#define SPLASH_MARK       0xFFFFFF
#define SPLASH_FILL       0xFFFFFF
#define SPLASH_TRACK_A    56       /* the bar's empty part: white this strong */

static int active;
static int lw, lh, cy;
static int bar_x, bar_w, bar_y;

void boot_splash_start(int requested)
{
    if (!requested || !video_have_loader_lfb())
        return;
    lw = video_w();
    lh = video_h();
    if (lw < 1 || lh < 1)
        return;

    active = 1;
    cy = lh / 2;
    bar_w = lw - 64 < SPLASH_BAR_W ? lw - 64 : SPLASH_BAR_W;
    if (bar_w < 80)
        bar_w = lw - 16;
    bar_x = (lw - bar_w) / 2;
    bar_y = cy + SPLASH_BAR_DY;
    int d = lh >= 400 ? SPLASH_LOGO_D : SPLASH_LOGO_D * 2 / 3;

    printk_screen_output(0);
    video_fill_rect_rgb(0, 0, lw, lh, SPLASH_BG);
    mark_draw((const struct gfx_surface *)0, (lw - d) / 2,
              cy - SPLASH_LOGO_DY - d / 2, d, SPLASH_MARK, 255);
    video_present();
    printk("[boot] visual loading screen active\n");
    boot_splash_update(3, "Starting the kernel");
}

/* 'status' is the step's name; the screen shows only how far along it
   is, and the serial port has the words */
void boot_splash_update(int progress, const char *status)
{
    if (!active || !status)
        return;
    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;

    /* The bar from the background up: the track, then the filled part */
    video_fill_rect_rgb(bar_x - 2, bar_y - 2, bar_w + 4, SPLASH_BAR_H + 4,
                        SPLASH_BG);
    gfx_rrect(bar_x, bar_y, bar_w, SPLASH_BAR_H, SPLASH_BAR_H / 2, 0xFFFFFF,
              SPLASH_TRACK_A);
    int filled = bar_w * progress / 100;
    if (filled > 0) {
        if (filled < SPLASH_BAR_H)
            filled = SPLASH_BAR_H;         /* a dot, not a sliver */
        gfx_rrect(bar_x, bar_y, filled, SPLASH_BAR_H, SPLASH_BAR_H / 2,
                  SPLASH_FILL, 255);
    }
    video_present();
}
