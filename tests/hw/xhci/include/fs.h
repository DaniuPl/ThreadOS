/* The file system, for the model: there is no disk (keys stay in memory) */
#ifndef HW_XHCI_FS_H
#define HW_XHCI_FS_H
#include <stdint.h>
int fs_ready(void);
int fs_read(const char *name, void *buf, uint32_t max);
int fs_write(const char *name, const void *data, uint32_t len);
#endif
