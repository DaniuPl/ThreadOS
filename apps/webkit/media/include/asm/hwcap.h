/* <asm/hwcap.h> for GStreamer's core
 *
 * On ARM Linux, gst/gstcpuid.c asks getauxval(AT_HWCAP) whether the
 * processor has NEON, and includes the Linux kernel's <asm/hwcap.h>
 * for the names of the bits. That header comes with a kernel's, not
 * with musl; musl's <sys/auxv.h> names the same bits (bits/hwcap.h),
 * so this is it. Only the core's build looks in this directory
 * (Makefile, GST_A).
 */
#ifndef THREADOS_ASM_HWCAP_H
#define THREADOS_ASM_HWCAP_H
#include <sys/auxv.h>
#endif
