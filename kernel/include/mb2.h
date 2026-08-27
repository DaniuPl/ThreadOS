#ifndef PROTONOS_MB2_H
#define PROTONOS_MB2_H

#include <stdint.h>
#include "multiboot.h"

/* Multiboot 2
 *
 * Obraz jadra nosi oba naglowki naraz: QEMU z opcja -kernel rozumie
 * tylko starszy, GRUB woli nowszy. Loader mowi w magicu, ktorej postaci
 * uzyl - i to jedyne miejsce w jadrze, ktore musi o tym wiedziec.
 *
 * Informacje z nowszej postaci przepisujemy do tej samej struktury,
 * ktorej reszta jadra uzywa od poczatku. Dzieki temu ani PMM, ani
 * initrd, ani obsluga linii polecen nie widza roznicy.
 */

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289

/* Rozpoznaje postac informacji od loadera i oddaje ja w jednolitym
   ksztalcie. NULL = magic nie nalezy do zadnej znanej postaci. */
const struct multiboot_info *mb2_info(uint32_t magic, uint32_t addr);

/* Bufor obrazu podany przez loader (tryb graficzny z kolorami wprost).
   NULL = loader zostawil tryb tekstowy albo nie umial inaczej. */
struct mb2_framebuffer {
    uint32_t addr;
    uint32_t pitch;              /* bajtow na wiersz */
    uint32_t width, height;
    uint32_t bpp;
};
const struct mb2_framebuffer *mb2_framebuffer(void);

/* Ktora postac zostala uzyta - do komunikatu startowego */
const char *mb2_boot_kind(void);

/* Wskaznik na tablice ACPI podany przez loader (0 = nie podal).
   Pod UEFI nie ma go gdzie szukac samemu. */
uint32_t mb2_rsdp(void);

#endif
