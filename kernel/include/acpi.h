#ifndef PROTONOS_ACPI_H
#define PROTONOS_ACPI_H

#include <stdint.h>

/* ACPI - zarzadzanie zasilaniem
 *
 * Bez tego system nie potrafi wylaczyc komputera. Na maszynie
 * wirtualnej to niewygoda, na prawdziwej - brak podstawowej funkcji:
 * zamkniecie systemu konczy sie napisem "mozna wylaczyc zasilanie"
 * i trzymaniem przycisku.
 *
 * Tablice ACPI opisuje firmware. Szukamy wskaznika RSDP w pamieci
 * ponizej megabajta, przez niego trafiamy do spisu tablic (RSDT),
 * a w nim do FADT - tablicy zasilania. Sama wartosc, ktora trzeba
 * wpisac, zeby komputer zgasl, siedzi jeszcze glebiej: w bloku AML
 * (DSDT), pod nazwa _S5_.
 */

/* Wskaznik na tablice podany przez bootloader (Multiboot 2). Wolane
   przed acpi_init; 0 = loader go nie podal i trzeba poszukac samemu. */
void acpi_set_rsdp(uint32_t addr);

/* Znajduje tablice ACPI. 1 = mozna wylaczyc komputer. */
int acpi_init(void);

/* Dowolna tablica ACPI po sygnaturze ("APIC", "HPET", ...), doklejona
   do mapowania. NULL, gdy takiej nie ma. Wolane po acpi_init. */
const void *acpi_table(const char *sygnatura);

/* Nazwa producenta z tablicy (do komunikatu startowego); "" gdy brak */
const char *acpi_oem(void);

/* Wylacza komputer. Wraca tylko wtedy, gdy sie nie udalo. */
void acpi_poweroff(void);

/* Restart przez rejestr resetu ACPI. 0 = nie ma takiego rejestru. */
int acpi_reboot(void);

#endif
