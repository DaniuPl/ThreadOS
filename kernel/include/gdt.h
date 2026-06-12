#ifndef PROTONOS_GDT_H
#define PROTONOS_GDT_H

/* Selektory segmentów (indeks w GDT * 8) */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20

void gdt_init(void);

#endif
