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
    int scale;                  /* the display size the command line
                                   asks for, per cent (0: the largest) */
};

/* Its screens' memory, before the programs start (their pages are
   counted from then on): 0, or -1 when there is none */
int mobile_init(const struct mobile_info *info);
/* The shell's thread: the lock screen, then whatever the finger says */
void mobile_main(void *arg);
/* A notification: kept for the shade, and a banner for a few seconds -
   any thread may post one, before the shell runs too */
void mobile_notify(const char *title, const char *body);

#endif
