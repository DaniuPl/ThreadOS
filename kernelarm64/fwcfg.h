#ifndef THREADOS_ARM64_FWCFG_H
#define THREADOS_ARM64_FWCFG_H

#include <stdint.h>

/* QEMU's fw_cfg: files the host hands the machine (fwcfg.c) */

/* Whether fw_cfg answers at this address, with DMA. 0 = it does. */
int fwcfg_init(uint64_t base);
/* A file by name: its item number and size. 0 = found. */
int fwcfg_find(const char *name, uint16_t *item, uint32_t *size);
/* Moves len bytes between memory and an item, by DMA. 0 = done. */
int fwcfg_read(uint16_t item, void *buf, uint32_t len);
int fwcfg_write(uint16_t item, const void *buf, uint32_t len);

#endif
