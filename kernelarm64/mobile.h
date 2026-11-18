#ifndef THREADOS_ARM64_MOBILE_H
#define THREADOS_ARM64_MOBILE_H

#include <stdint.h>

/* ThreadOS mobile: the phone's shell (mobile.c) */

/* What the About app says, as kmain found it */
struct mobile_info {
    const char *cpu;            /* "Arm Cortex-A72" */
    int cores;
    uint32_t mem_mib;
    int screen_w, screen_h;     /* the panel's pixels */
    const char *touch;          /* the touchscreen's name, or 0 */
};

/* Its screens' memory, before the programs start (their pages are
   counted from then on): 0, or -1 when there is none */
int mobile_init(const struct mobile_info *info);
/* The shell's thread: the lock screen, then whatever the finger says */
void mobile_main(void *arg);

#endif
