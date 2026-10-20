#ifndef THREADOS_LOCALE_H
#define THREADOS_LOCALE_H

void locale_set_language(const char *language);
int locale_is_polish(void);
const char *locale_text(const char *english);

#endif
