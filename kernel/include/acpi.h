#ifndef PROTONOS_ACPI_H
#define PROTONOS_ACPI_H

#include <stdint.h>

/* ACPI - power management
 *
 * Without it the system cannot turn the computer off. On a virtual
 * machine that is an inconvenience; on a real one it is a missing
 * basic function: shutting the system down ends with the words "you
 * may now turn off the power" and holding the button.
 *
 * The ACPI tables are described by the firmware. We look for the RSDP
 * pointer in memory below one megabyte, through it we reach the table
 * listing (RSDT) and in it the FADT - the power table. The value that
 * has to be written to make the computer go dark sits deeper still: in
 * the AML block (DSDT), under the name _S5_.
 */

/* The table pointer handed over by the bootloader (Multiboot 2).
   Called before acpi_init; 0 = the loader did not supply it and we
   have to search ourselves. */
void acpi_set_rsdp(uint32_t addr);

/* Finds the ACPI tables. 1 = the computer can be turned off. */
int acpi_init(void);

/* Any ACPI table by signature ("APIC", "HPET", ...), glued onto the
   mapping. NULL when there is no such table. Called after acpi_init. */
const void *acpi_table(const char *signature);

/* The vendor name from the table (for the startup message); "" if none */
const char *acpi_oem(void);

/* Turns the computer off. Returns only when it failed. */
void acpi_poweroff(void);

/* Reboot through the ACPI reset register. 0 = there is no such register. */
int acpi_reboot(void);

#endif
