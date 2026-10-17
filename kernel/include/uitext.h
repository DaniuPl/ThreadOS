#ifndef THREADOS_UITEXT_H
#define THREADOS_UITEXT_H

#include <stdint.h>

/* The desktop's own lettering - window titles, the taskbar, the menu -
 * in the TrueType sans, anti-aliased against whatever it is drawn on.
 *
 * The text is UTF-8. Positions are whole pixels; the pen between the
 * glyphs moves in 1/64 of a pixel, as the font says, and each glyph is
 * put at the nearest whole pixel. When the initrd has no font, the 8x8
 * bitmap is used instead, so the desktop never goes without letters.
 */

#define UI_PX 11                 /* the size of the desktop's text */

/* The three faces there are */
#define UI_SANS 0
#define UI_BOLD 1
#define UI_MONO 2

/* How far above and below the baseline the text reaches, whole pixels */
int ui_ascent(void);
int ui_descent(void);

/* The width the text will take */
int ui_text_width(const char *s);

/* Draws the text with its baseline at 'baseline', in palette colour
   'colour'. Returns where the pen ended, which is where the next piece
   of text would go. */
int ui_text(int x, int baseline, const char *s, uint8_t colour);

/* The baseline that centres capital letters in a band of the screen -
   which is how a title or a button label sits in its bar */
int ui_baseline_in(int top, int height);

/* The same in any face and size, in a colour of 0x00RRGGBB, and onto
   whatever kernel/gfx.c is drawing on - the screen or a picture */
int ui_font_text(int font, int px, int x, int baseline, const char *s,
                 uint32_t rgb);
int ui_font_width(int font, int px, const char *s);
int ui_font_baseline_in(int px, int top, int height);

#endif
