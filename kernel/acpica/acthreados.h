/* ACPICA's host configuration for the ThreadOS kernel
 *
 * ACPICA (third_party/acpica-unix-*.tar.gz) asks each system it runs in
 * for a header like this one: the machine's width, the integer types,
 * what its locks and semaphores are. Ours is the kernel's own - no C
 * library, x86-64, threads that sleep - and the patch in
 * kernel/acpica/patches/ is the one line in ACPICA's acenv.h that
 * names it.
 *
 * ACPICA's own string and memory functions are used (utclib.c), under
 * names of their own: the build maps memcpy to acpi_memcpy and so on,
 * so they do not meet the kernel's.
 */

#ifndef __ACTHREADOS_H__
#define __ACTHREADOS_H__

#include <stdint.h>
#include <stdarg.h>

#define ACPI_MACHINE_WIDTH          64
#define COMPILER_DEPENDENT_INT64    int64_t
#define COMPILER_DEPENDENT_UINT64   uint64_t

/* 64-bit division is one instruction here, and ACPICA's caches are
   as good as any we would write */
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
#define ACPI_USE_LOCAL_CACHE
#define ACPI_CACHE_T                ACPI_MEMORY_LIST
#define ACPI_USE_DO_WHILE_0

/* Locks spin with interrupts off; semaphores - and the mutexes made of
   them - make the waiting thread sleep (kernel/acpios.c) */
#define ACPI_SPINLOCK               void *
#define ACPI_CPU_FLAGS              unsigned long
#define ACPI_SEMAPHORE              void *
#define ACPI_MUTEX_TYPE             ACPI_BINARY_SEMAPHORE

#define ACPI_FLUSH_CPU_CACHE()      __asm__ volatile("wbinvd" ::: "memory")

#endif
