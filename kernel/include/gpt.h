#ifndef THREADOS_GPT_H
#define THREADOS_GPT_H

#include <stdint.h>

/* Writing a partition table
 *
 * kernel/part.c reads tables and deliberately writes nothing: knowing
 * what is on a disk is one thing and changing it is another. This is
 * the other thing, and it exists for exactly one reason - a machine on
 * which ThreadOS is to be the only system has to have a table made for
 * it, and nobody else is going to make one.
 *
 * What is written is a GPT, because that is what a UEFI machine boots
 * from, with a protective MBR in front of it so that a program which
 * knows only the old format sees a disk that is full rather than a
 * disk that is empty. Two copies, as the format requires: the header
 * and the entries at the front and the same again at the very end, so
 * that a torn front is a disk somebody can still repair.
 *
 * Nothing here is subtle and everything here is final. This call
 * destroys every partition on the disk.
 */

/* Makes a fresh table: an EFI partition of 'esp_mib' megabytes and
   one partition for ThreadOS taking the rest. 0 = done, -1 = it did
   not work (no disk, or the disk is too small). */
int gpt_make(uint32_t esp_mib);

/* The smallest disk this makes sense on, in MiB - the EFI partition
   has to hold a FAT32, and FAT32 has a floor of its own. */
#define GPT_MIN_MIB 256

/* CRC-32 (IEEE), the same as zlib.crc32 */
uint32_t crc32(const void *data, uint32_t len);

#endif
