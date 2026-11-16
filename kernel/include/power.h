#ifndef THREADOS_POWER_H
#define THREADOS_POWER_H

#include <stdint.h>

/* Power through the firmware's ACPI (kernel/power.c, on ACPICA): the
   power button, and the battery of a laptop */

/* Starts ACPICA in a thread of its own; the rest of the start does not
   wait for it. (kmain64.c leaves it off for 'noacpica' on the command
   line - a way round firmware it cannot get through.) */
void power_start(void);

/* 1 once ACPICA runs the firmware's AML, -1 when it failed, 0 before */
int power_ready(void);

struct power_battery {
    int present;
    int percent;               /* of the last full charge, 0..100 */
    int charging, discharging, critical;
    uint32_t remaining, full;  /* in units of 'unit' */
    const char *unit;          /* "mWh" or "mAh" */
};

/* The first battery's state, as last read (every few seconds, and when
   the firmware says it changed). 0 = there is no battery. */
int power_battery(struct power_battery *out);

/* For the shell's 'acpi': how far ACPICA got, and what it found */
void power_report(void);

#endif
