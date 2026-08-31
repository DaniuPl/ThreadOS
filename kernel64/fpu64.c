/* The state of the floating-point and SSE unit
 *
 * The XMM registers are as much a task's state as RAX or RSP - with
 * the difference that there are a lot of them and the processor saves
 * them with a single instruction into a 512-byte area. Without that
 * save, two programs computing at the same time would overwrite each
 * other's results without knowing it.
 *
 * Alignment to sixteen bytes is not a courtesy here: fxsave and
 * fxrstor simply refuse to work at a badly aligned address.
 */

#include "fpu64.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/string.h"

#define FPU_AREA 512

/* The state of a freshly initialized unit - every new task gets a copy
   of it instead of starting from random bits */
static uint8_t clean[FPU_AREA] __attribute__((aligned(16)));
static int enabled;

/* The control registers are the CORE's, not the system's.
 *
 * Every core has its own CR0 and CR4, so every one has to be told
 * separately that these instructions are allowed. A core that is not
 * told answers an ordinary movdqa with an invalid opcode - and on
 * x86-64 the compiler reaches for XMM without asking, because the
 * calling convention says it may. So a program running on a secondary
 * core would die of an instruction that is perfectly correct. */
void fpu64_enable_core(void)
{
    uint64_t cr0, cr4;

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(uint64_t)(1 << 2);      /* EM: no more coprocessor emulation */
    cr0 |= (1 << 1);                 /* MP: coprocessor monitoring */
    cr0 |= (1 << 5);                 /* NE: errors through an exception, not an IRQ */
    __asm__ volatile("movq %0, %%cr0" : : "r"(cr0));

    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 9);                 /* OSFXSR: we know how to fxsave/fxrstor */
    cr4 |= (1 << 10);                /* OSXMMEXCPT: and SIMD errors */
    __asm__ volatile("movq %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
}

int fpu64_init(void)
{
    fpu64_enable_core();
    /* The pattern for a new task is taken once, from the boot core -
       a freshly initialized unit looks the same on every one of them */
    __asm__ volatile("fxsave (%0)" : : "r"(clean) : "memory");

    enabled = 1;
    return 1;
}

int fpu64_enabled(void)
{
    return enabled;
}

void *fpu64_alloc(void)
{
    /* The slack is 32 bytes, not 16, and not out of caution. We hide
       the raw address just BEFORE the aligned area, because otherwise
       there would be no way to free it - and if the heap handed back an
       already aligned address, the area would start exactly on it and
       that write would land outside the allocated memory. The shift by
       16 makes the room for the pointer always our own. */
    uint8_t *raw = kmalloc(FPU_AREA + 32);
    if (!raw)
        return (void *)0;
    uint8_t *area = (uint8_t *)(((uintptr_t)raw + 16 + 15) & ~(uintptr_t)15);
    memcpy(area, clean, FPU_AREA);

    ((uint8_t **)area)[-1] = raw;
    return area;
}

void fpu64_free(void *area)
{
    if (area)
        kfree(((uint8_t **)area)[-1]);
}

void fpu64_save(void *area)
{
    if (enabled && area)
        __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu64_load(const void *area)
{
    if (enabled && area)
        __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}
