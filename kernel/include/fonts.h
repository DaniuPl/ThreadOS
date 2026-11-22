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

/* Inter, regular and bold: Weave's letters. While fonts_use_inter(1)
   is in force the desktop's sans and bold are these - kernel/uitext.c
   asks fonts_inter() once for each text it draws or measures. */
const struct ttf_font *font_inter(void);
const struct ttf_font *font_inter_bold(void);
/* JetBrains Mono: Weave's columns - code, the terminal - in place of
   mono.ttf while fonts_inter() says Weave */
const struct ttf_font *font_jbmono(void);
/* Source Serif 4's cut, regular and bold: Notes' letters, in any style */
const struct ttf_font *font_serif(void);
const struct ttf_font *font_serif_bold(void);
void fonts_use_inter(int on);
int fonts_inter(void);

/* The 'font' shell command: what the fonts are, and glyphs drawn as
   numbers - which is how the test compares them with FreeType. */
void fonts_command(const char *arg);

#endif
