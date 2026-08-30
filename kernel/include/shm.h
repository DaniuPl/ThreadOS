#ifndef PROTONOS_SHM_H
#define PROTONOS_SHM_H

#include <stdint.h>

/* Memory shared between processes, identified by a key. The first use
   of a key allocates physical frames; later ones (from any process)
   point at the same frames. */

/* Returns the physical base address of the region for a key
   (allocating it on first use); 0 on error. */
uint32_t shm_get(int key, uint32_t size);

/* Does a given frame belong to a shared region (destroy does not free
   it while other processes may still be using it). */
int shm_is_shared(uint32_t frame);

#endif
