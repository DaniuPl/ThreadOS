#ifndef PROTONOS_PAGING_H
#define PROTONOS_PAGING_H

/* Granica mapowania identycznościowego (musi zgadzać się z pmm.c) */
#define PAGING_IDENTITY_LIMIT (64u * 1024 * 1024)

/* Mapuje identycznościowo pierwsze 64 MiB i włącza stronicowanie */
void paging_init(void);

#endif
