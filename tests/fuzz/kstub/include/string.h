/* Zaslepka string.h jadra: te same funkcje, ale bierzemy je z libc
   hosta (deklaracje zamiast wciagania systemowego naglowka, zeby nie
   walczyc z kolejnoscia szukania plikow) */
#ifndef STUB_STRING_H
#define STUB_STRING_H

#include <stddef.h>

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#endif
