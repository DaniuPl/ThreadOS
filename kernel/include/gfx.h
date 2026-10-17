#ifndef THREADOS_GFX_H
#define THREADOS_GFX_H

#include <stdint.h>

/* Drawing with soft edges and light let through (see kernel/gfx.c)
 *
 * Colours are 0x00RRGGBB; 'a' is how much of the colour goes on, 0 to
 * 255, the rest being what was there. Everything is drawn onto the
 * current target: the screen, unless gfx_target() was given a surface
 * of its own - which is how a picture is drawn once and kept.
 *
 * Shapes whose edges matter - round corners, circles, lines - take
 * their positions in sixteenths of a pixel (the ...16 calls), so an
 * edge can fall between two pixels and is then drawn as half of each.
 */

struct gfx_surface {
    uint32_t *px;
    int w, h, stride;         /* stride in pixels */
};

/* Where drawing goes; NULL = the screen */
void gfx_target(const struct gfx_surface *s);

/* The mix every soft edge in the system uses, text included:
   (over * a + under * (255 - a) + 127) / 255, per channel */
uint32_t gfx_mix(uint32_t over, uint32_t under, int a);

uint32_t gfx_get(int x, int y);
void gfx_put(int x, int y, uint32_t c);
void gfx_blend(int x, int y, uint32_t c, int a);

/* A rectangle, whole pixels */
void gfx_fill(int x, int y, int w, int h, uint32_t c, int a);
/* ... with a slope from 'top' to 'bottom' */
void gfx_fill_grad(int x, int y, int w, int h, uint32_t top,
                   uint32_t bottom, int a);

/* A rectangle with round corners. The whole-pixel form is the common
   one; the other takes sixteenths. */
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx_rrect16(int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx_rrect_grad(int x, int y, int w, int h, int r, uint32_t top,
                    uint32_t bottom, int a);
/* Only its edge, 't' sixteenths thick, drawn inside the shape */
void gfx_rring16(int x, int y, int w, int h, int r, int t, uint32_t c,
                 int a);
/* A circle: centre and radius, in sixteenths */
void gfx_disc16(int cx, int cy, int rad, uint32_t c, int a);
void gfx_circle16(int cx, int cy, int rad, int t, uint32_t c, int a);
/* A line with round ends, 'thick' sixteenths wide */
void gfx_line16(int x0, int y0, int x1, int y1, int thick, uint32_t c,
                int a);

/* How much of the pixel (px, py) a rounded rectangle covers, 0-255 -
   what the calls above draw with, and what the 'shape' command prints so
   the test can hold it against an exact area. Sixteenths again. */
int gfx_cov_rrect16(int px, int py, int x, int y, int w, int h, int r);
int gfx_cov_line16(int px, int py, int x0, int y0, int x1, int y1,
                   int thick);

/* The soft shadow a rounded rectangle casts, fallen 'dy' pixels below
   it: darkest at its edge, gone 's' pixels further out. Drawn only
   OUTSIDE the rectangle - what is inside is about to be covered by
   whatever casts it. */
void gfx_shadow(int x, int y, int w, int h, int r, int s, int a, int dy);

/* Blurs a rectangle of the target where it is: two passes of a box
   'radius' pixels to either side, which is near enough to a bell
   curve that the eye cannot tell. Pixels beyond the rectangle are
   read (that is what makes the edge of glass look like glass) and
   never written. Returns 0, or -1 when the rectangle is larger than
   the room set aside for it - then nothing is blurred. */
int gfx_blur(int x, int y, int w, int h, int radius);

/* Glass: blurred, tinted, and with round corners */
void gfx_frost(int x, int y, int w, int h, int r, int blur, uint32_t tint,
               int a);

/* A picture with round corners: what lies outside them is left alone */
void gfx_blit_round(int x, int y, int w, int h, int r, const uint32_t *src,
                    int stride, int a);

/* Rounding something already drawn. save() keeps what is in the four
   corners before it is drawn; apply() puts back, in each corner, as
   much of that as the round edge leaves uncovered. 'keep' holds
   4 * r * r pixels. */
void gfx_round_save(int x, int y, int w, int h, int r, uint32_t *keep);
void gfx_round_apply(int x, int y, int w, int h, int r,
                     const uint32_t *keep);

/* The same calls with the target said each time ('t', NULL = the
   screen) rather than taken from gfx_target(). What a program draws in
   its own window goes through these: it may be doing it on another
   core, or in the middle of the compositor's frame, and must not move
   the compositor's target from under it. */
uint32_t gfx_get_s(const struct gfx_surface *t, int x, int y);
void gfx_put_s(const struct gfx_surface *t, int x, int y, uint32_t c);
void gfx_blend_s(const struct gfx_surface *t, int x, int y, uint32_t c, int a);
void gfx_fill_s(const struct gfx_surface *t, int x, int y, int w, int h,
                uint32_t c, int a);
void gfx_fill_grad_s(const struct gfx_surface *t, int x, int y, int w, int h,
                     uint32_t top, uint32_t bottom, int a);
void gfx_rrect16_s(const struct gfx_surface *t, int x, int y, int w, int h,
                   int r, uint32_t c, int a);
void gfx_rring16_s(const struct gfx_surface *t, int x, int y, int w, int h,
                   int r, int thick, uint32_t c, int a);
void gfx_rrect_grad_s(const struct gfx_surface *t, int x, int y, int w, int h,
                      int r, uint32_t top, uint32_t bottom, int a);
void gfx_disc16_s(const struct gfx_surface *t, int cx, int cy, int rad,
                  uint32_t c, int a);
void gfx_line16_s(const struct gfx_surface *t, int x0, int y0, int x1, int y1,
                  int thick, uint32_t c, int a);

/* The 'shape' shell command: shapes printed as numbers, for the test */
void gfx_command(const char *arg);

#endif
