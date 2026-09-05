#ifndef PROTONOS_MB2_H
#define PROTONOS_MB2_H

#include <stdint.h>
#include "multiboot.h"

/* Multiboot 2
 *
 * The kernel image carries both headers at once: QEMU with -kernel
 * understands only the older one, GRUB prefers the newer. The loader
 * says in the magic which form it used - and this is the only place in
 * the kernel that has to know about it.
 *
 * Information from the newer form is copied into the same structure
 * the rest of the kernel has used from the start. That way neither the
 * PMM, nor the initrd, nor the command line handling sees a difference.
 */

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

/* Recognises the form of the loader's information and hands it back in
   one uniform shape. NULL = the magic belongs to no known form. */
const struct multiboot_info *mb2_info(uint32_t magic, uint32_t addr);

/* The framebuffer supplied by the loader (a graphics mode with direct
   colours). NULL = the loader left text mode alone or could not do
   otherwise. */
struct mb2_framebuffer {
    uint32_t addr;
    uint32_t pitch;              /* bytes per row */
    uint32_t width, height;
    uint32_t bpp;
};
const struct mb2_framebuffer *mb2_framebuffer(void);

/* Which form was used - for the startup message */
const char *mb2_boot_kind(void);

/* Pointer to the ACPI tables supplied by the loader (0 = not supplied).
   Under UEFI there is nowhere to look for it ourselves. */
uint32_t mb2_rsdp(void);

/* A module the loader handed over, found by the name it gave it.
 * 1 = there is one.
 *
 * The initrd is taken by position and always has been, because it was
 * the only module there was. Anything beyond it is taken by name: the
 * alternative is the loader and the kernel agreeing on an order, which
 * is the sort of agreement that goes wrong without saying so.
 */
int mb2_module(const char *name, uint32_t *start, uint32_t *size);

#endif
