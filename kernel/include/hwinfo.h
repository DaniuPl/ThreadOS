#ifndef PROTONOS_HWINFO_H
#define PROTONOS_HWINFO_H

/* What this machine actually is
 *
 * Written for a first boot on hardware nobody has tried yet, and it
 * lives here rather than in a shell because both shells need it and
 * the machine it describes is the same machine either way. On a
 * laptop this is the first thing one types, and it is the difference
 * between "it does not work" and knowing which of five things is
 * missing.
 */

/* Screen, PS/2, USB, the disk and its partitions, and everything on
   the PCI bus. */
void hwinfo_print(void);

/* The disks the machine has, and which of them the system is using.
   Reading only - nothing here writes a sector. */
void hwinfo_disks(void);

/* How loud, said and set. An empty argument only says; a number sets
   it; the word 'cisza' silences without forgetting the number. Both
   shells have it, so it lives once. */
void hwinfo_volume(const char *arg);

#endif
