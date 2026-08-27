#ifndef PROTONOS_GDT64_H
#define PROTONOS_GDT64_H

#include <stdint.h>

/* Selektory. Kolejnosc nie jest dowolna: instrukcja sysret sklada
   selektory uzytkownika z jednej wartosci w rejestrze STAR - dane biora
   sie z niej plus osiem, a kod plus szesnascie. Segment danych
   uzytkownika musi wiec lezec PRZED jego segmentem kodu. */
#define GDT64_KERNEL_CODE 0x08
#define GDT64_KERNEL_DATA 0x10
#define GDT64_USER_DATA   0x18
#define GDT64_USER_CODE   0x20
#define GDT64_TSS         0x28

/* Laduje tablice deskryptorow z segmentami ring 3 i zadaniem TSS */
void gdt64_init(void);

/* Stos jadra, na ktory procesor przejdzie przy przerwaniu z ring 3 */
void gdt64_set_kernel_stack(uint64_t rsp);

#endif
