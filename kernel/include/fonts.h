#ifndef THREADOS_FONTS_H
#define THREADOS_FONTS_H

#include "ttf.h"

/* The fonts the system carries in its initrd: a proportional one for
   the desktop's own text, its bold, and a monospaced one for terminals and the
   editor. Opened once; NULL when the initrd does not have the file or
   it does not read as a font. */
void fonts_init(void);
const struct ttf_font *font_sans(void);
const struct ttf_font *font_bold(void);
const struct ttf_font *font_mono(void);

/* The 'font' shell command: what the fonts are, and glyphs drawn as
   numbers - which is how the test compares them with FreeType. */
void fonts_command(const char *arg);

#endif
