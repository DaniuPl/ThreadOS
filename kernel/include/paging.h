#ifndef PROTONOS_PAGING_H
#define PROTONOS_PAGING_H

/* Granica mapowania identycznościowego (musi zgadzać się z pmm.c) */
#define PAGING_IDENTITY_LIMIT (64u * 1024 * 1024)

#include <stdint.h>

/* Mapuje identycznościowo pierwsze 64 MiB i włącza stronicowanie */
void paging_init(void);

/* Nadaje stronom z podanego zakresu bit USER (dostęp z ring 3).
   Zakres musi leżeć w obszarze mapowanym identycznościowo. */
void paging_make_user(uint32_t addr, uint32_t size);

#endif
