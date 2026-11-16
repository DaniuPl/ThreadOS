/* The initrd on the host: the one file the driver asks for,
   third_party/iwlwifi/iwlwifi-3160-17.ucode, mapped read-only */
#ifndef HW_IWL_INITRD_H
#define HW_IWL_INITRD_H
#include <stdint.h>
const void *initrd_find(const char *name, uint32_t *size);
#endif
