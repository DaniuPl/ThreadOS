#ifndef PROTONOS_INSTALL_H
#define PROTONOS_INSTALL_H

#include <stdint.h>

/* Installing ProtonOS onto the machine's own disk
 *
 * What installing amounts to on a UEFI machine is smaller than the
 * word suggests: three files - the loader, the kernel and the initrd -
 * written onto the EFI partition that is already there. No partition
 * is created, no table is touched, nothing that belongs to the system
 * already living on this disk is moved or removed. The firmware finds
 * a loader on the EFI partition and starts it; that is the whole
 * mechanism.
 *
 * The three files come from the loader that started this machine: it
 * hands them over as Multiboot 2 modules, because the kernel in memory
 * is loaded segments and not the file it came from. A system started
 * some other way says so and offers nothing.
 *
 * Split in two on purpose. install_plan() reads and prints and changes
 * nothing; install_do() writes. Between the two sits a person saying
 * yes - which the shells ask for, because reading the keyboard is
 * theirs and this is not.
 */

/* Reads the disk and says what would be written where. 1 = it can be
   done. Changes nothing. */
int install_plan(void);

/* Writes. Only call after install_plan() has said yes and somebody has
   agreed. 0 = done, -1 = it did not work. */
int install_do(void);

#endif
