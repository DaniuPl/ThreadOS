/* What ACPICA asks of the system (acpiosxf.h), from the ThreadOS kernel
 *
 * ACPICA (third_party/acpica-unix-*.tar.gz) runs the firmware's AML -
 * the bytecode in the DSDT and SSDTs that knows where a laptop's
 * battery is, what its power button does and how its lid is read. It
 * does not touch the machine itself: every memory mapping, port, PCI
 * register, lock, semaphore, interrupt and timer goes through the
 * functions here.
 *
 * Two things are worth knowing about how they are made:
 *
 *   ACPICA's SCI handler runs in the interrupt and may queue work
 *   (AcpiOsExecute) - a GPE's method, a Notify. The queue is taken by
 *   a thread of its own ("acpiwork"), never in the interrupt: a method
 *   may wait, and nothing in an interrupt may.
 *
 *   A semaphore waits by sleeping a tick at a time. AML waits on
 *   mutexes and events for milliseconds at the least; there is nothing
 *   to gain from waking a waiter the moment it is signalled.
 */

#include "acpi.h"                 /* ACPICA's, from its source/include */
#include "include/acpi.h"
#include "include/printk.h"
#include "include/io.h"
#include "include/pci.h"
#include "include/heap.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/pic.h"
#include "include/isr.h"
#include "include/spinlock.h"
#include "../kernel64/paging64.h"

/* --- the start, and the tables ----------------------------------------- */

ACPI_STATUS AcpiOsInitialize(void)
{
    return AE_OK;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    /* Where acpi_init() found it - from the loader under UEFI, where
       the BIOS areas ACPICA would search hold nothing */
    ACPI_PHYSICAL_ADDRESS at = acpi_rsdp();
    if (!at) {
        ACPI_PHYSICAL_ADDRESS found = 0;
        if (ACPI_SUCCESS(AcpiFindRootPointer(&found)))
            at = found;
    }
    return at;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *init,
                                     ACPI_STRING *value)
{
    (void)init;
    *value = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *existing,
                                ACPI_TABLE_HEADER **replacement)
{
    (void)existing;
    *replacement = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *existing,
                                        ACPI_PHYSICAL_ADDRESS *address,
                                        UINT32 *length)
{
    (void)existing;
    *address = 0;
    *length = 0;
    return AE_OK;
}

/* --- memory ------------------------------------------------------------ */

/* Physical memory is mapped one to one; what lies above the part the
   kernel maps at start (tables, device registers) is added to it */
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS where, ACPI_SIZE length)
{
    if (where + length > paging64_identity_top() &&
        paging64_map_phys(where, length) < 0)
        return NULL;
    return (void *)(uintptr_t)where;
}

void AcpiOsUnmapMemory(void *where, ACPI_SIZE length)
{
    (void)where;
    (void)length;
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *logical,
                                     ACPI_PHYSICAL_ADDRESS *physical)
{
    *physical = (ACPI_PHYSICAL_ADDRESS)(uintptr_t)logical;
    return AE_OK;
}

void *AcpiOsAllocate(ACPI_SIZE size)
{
    return kmalloc(size);
}

void AcpiOsFree(void *memory)
{
    kfree(memory);
}

BOOLEAN AcpiOsReadable(void *memory, ACPI_SIZE length)
{
    (void)memory;
    (void)length;
    return TRUE;
}

BOOLEAN AcpiOsWritable(void *memory, ACPI_SIZE length)
{
    (void)memory;
    (void)length;
    return TRUE;
}

/* --- locks and semaphores ---------------------------------------------- */

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *out)
{
    struct spinlock *l = kmalloc(sizeof(*l));
    if (!l)
        return AE_NO_MEMORY;
    l->taken = 0;
    *out = l;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK handle)
{
    kfree(handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK handle)
{
    return spin_lock_irq((struct spinlock *)handle);
}

void AcpiOsReleaseLock(ACPI_SPINLOCK handle, ACPI_CPU_FLAGS flags)
{
    spin_unlock_irq((struct spinlock *)handle, (irqflags_t)flags);
}

struct asem {
    struct spinlock lock;
    UINT32 units, max;
};

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 max, UINT32 initial,
                                  ACPI_SEMAPHORE *out)
{
    struct asem *s = kmalloc(sizeof(*s));
    if (!s)
        return AE_NO_MEMORY;
    s->lock.taken = 0;
    s->units = initial;
    s->max = max;
    *out = s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE handle)
{
    if (!handle)
        return AE_BAD_PARAMETER;
    kfree(handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE handle, UINT32 units,
                                UINT16 timeout)
{
    struct asem *s = handle;
    if (!s)
        return AE_BAD_PARAMETER;
    uint32_t start = pit_ticks();
    for (;;) {
        irqflags_t f = spin_lock_irq(&s->lock);
        if (s->units >= units) {
            s->units -= units;
            spin_unlock_irq(&s->lock, f);
            return AE_OK;
        }
        spin_unlock_irq(&s->lock, f);
        if (timeout == 0)
            return AE_TIME;
        if (timeout != ACPI_WAIT_FOREVER &&
            (pit_ticks() - start) * 10 >= timeout)
            return AE_TIME;
        task_sleep(10);
    }
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE handle, UINT32 units)
{
    struct asem *s = handle;
    if (!s)
        return AE_BAD_PARAMETER;
    irqflags_t f = spin_lock_irq(&s->lock);
    s->units += units;
    if (s->max && s->units > s->max)
        s->units = s->max;
    spin_unlock_irq(&s->lock, f);
    return AE_OK;
}

/* --- threads, work, time ----------------------------------------------- */

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    /* never 0, which ACPICA reads as "nobody" */
    return (ACPI_THREAD_ID)task_current_id() + 1;
}

#define WORK_MAX 64
static struct {
    ACPI_OSD_EXEC_CALLBACK fn;
    void *context;
} work[WORK_MAX];
static unsigned work_head, work_tail;
static volatile int work_busy;
static struct spinlock work_lock = SPINLOCK_FREE;

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE type,
                          ACPI_OSD_EXEC_CALLBACK fn, void *context)
{
    (void)type;
    irqflags_t f = spin_lock_irq(&work_lock);
    if (work_head - work_tail >= WORK_MAX) {
        spin_unlock_irq(&work_lock, f);
        return AE_NO_MEMORY;
    }
    work[work_head % WORK_MAX].fn = fn;
    work[work_head % WORK_MAX].context = context;
    work_head++;
    spin_unlock_irq(&work_lock, f);
    return AE_OK;
}

/* The thread that does the queued work, for as long as the system runs */
void acpios_worker(void *arg)
{
    (void)arg;
    for (;;) {
        irqflags_t f = spin_lock_irq(&work_lock);
        if (work_tail == work_head) {
            spin_unlock_irq(&work_lock, f);
            task_sleep(10);
            continue;
        }
        ACPI_OSD_EXEC_CALLBACK fn = work[work_tail % WORK_MAX].fn;
        void *context = work[work_tail % WORK_MAX].context;
        work_tail++;
        work_busy = 1;
        spin_unlock_irq(&work_lock, f);
        fn(context);
        work_busy = 0;
    }
}

void AcpiOsWaitEventsComplete(void)
{
    while (work_tail != work_head || work_busy)
        task_sleep(10);
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Cycles of the time stamp counter in a microsecond, measured against
   the timer once: a stall is microseconds, and the timer ticks every
   ten thousand of them */
static uint64_t tsc_per_us;

void acpios_calibrate(void)
{
    uint32_t t = pit_ticks();
    while (pit_ticks() == t)
        __asm__ volatile("pause");
    uint64_t c0 = rdtsc();
    t = pit_ticks();
    while (pit_ticks() - t < 5)
        __asm__ volatile("pause");
    tsc_per_us = (rdtsc() - c0) / 50000;
    if (!tsc_per_us)
        tsc_per_us = 1;
}

void AcpiOsSleep(UINT64 ms)
{
    task_sleep((uint32_t)ms);
}

void AcpiOsStall(UINT32 us)
{
    uint64_t end = rdtsc() + (uint64_t)us * (tsc_per_us ? tsc_per_us : 1000);
    while (rdtsc() < end)
        __asm__ volatile("pause");
}

/* In hundreds of nanoseconds, as ACPI's timer counts */
UINT64 AcpiOsGetTimer(void)
{
    if (!tsc_per_us)
        return (UINT64)pit_ticks() * 100000;
    return rdtsc() / tsc_per_us * 10;
}

/* --- ports, memory, PCI ------------------------------------------------ */

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS address, UINT32 *value,
                           UINT32 width)
{
    switch (width) {
    case 8:  *value = inb((uint16_t)address); break;
    case 16: *value = inw((uint16_t)address); break;
    case 32: *value = inl((uint16_t)address); break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS address, UINT32 value,
                            UINT32 width)
{
    switch (width) {
    case 8:  outb((uint16_t)address, (uint8_t)value); break;
    case 16: outw((uint16_t)address, (uint16_t)value); break;
    case 32: outl((uint16_t)address, value); break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 *value,
                             UINT32 width)
{
    void *p = AcpiOsMapMemory(address, width / 8);
    if (!p)
        return AE_NO_MEMORY;
    switch (width) {
    case 8:  *value = *(volatile uint8_t *)p; break;
    case 16: *value = *(volatile uint16_t *)p; break;
    case 32: *value = *(volatile uint32_t *)p; break;
    case 64: *value = *(volatile uint64_t *)p; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS address, UINT64 value,
                              UINT32 width)
{
    void *p = AcpiOsMapMemory(address, width / 8);
    if (!p)
        return AE_NO_MEMORY;
    switch (width) {
    case 8:  *(volatile uint8_t *)p = (uint8_t)value; break;
    case 16: *(volatile uint16_t *)p = (uint16_t)value; break;
    case 32: *(volatile uint32_t *)p = (uint32_t)value; break;
    case 64: *(volatile uint64_t *)p = value; break;
    default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

/* Configuration space a double word at a time, as the PCI layer reads
   it; narrower widths are the part of the word asked for */
ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *id, UINT32 reg,
                                       UINT64 *value, UINT32 width)
{
    if (id->Segment || reg > 0xFF)
        return AE_BAD_PARAMETER;
    if (width == 64) {
        uint64_t lo = pci_config_read(id->Bus, id->Device, id->Function,
                                      (int)(reg & ~3u));
        uint64_t hi = pci_config_read(id->Bus, id->Device, id->Function,
                                      (int)((reg & ~3u) + 4));
        *value = lo | hi << 32;
        return AE_OK;
    }
    uint32_t dw = pci_config_read(id->Bus, id->Device, id->Function,
                                  (int)(reg & ~3u));
    dw >>= (reg & 3) * 8;
    *value = width == 8 ? (dw & 0xFF) : width == 16 ? (dw & 0xFFFF) : dw;
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *id, UINT32 reg,
                                        UINT64 value, UINT32 width)
{
    if (id->Segment || reg > 0xFF)
        return AE_BAD_PARAMETER;
    if (width == 32 || width == 64) {
        pci_config_write(id->Bus, id->Device, id->Function, (int)reg,
                         (uint32_t)value);
        if (width == 64)
            pci_config_write(id->Bus, id->Device, id->Function,
                             (int)reg + 4, (uint32_t)(value >> 32));
        return AE_OK;
    }
    int at = (int)(reg & ~3u), shift = (int)(reg & 3) * 8;
    uint32_t mask = (width == 8 ? 0xFFu : 0xFFFFu) << shift;
    uint32_t dw = pci_config_read(id->Bus, id->Device, id->Function, at);
    dw = (dw & ~mask) | (((uint32_t)value << shift) & mask);
    pci_config_write(id->Bus, id->Device, id->Function, at, dw);
    return AE_OK;
}

/* --- the SCI ------------------------------------------------------------ */

static ACPI_OSD_HANDLER sci_handler;
static void *sci_context;
static int sci_irq = -1;
static volatile uint32_t sci_count;

static void sci_entry(struct regs *r)
{
    (void)r;
    sci_count++;
    if (sci_handler)
        sci_handler(sci_context);
}

uint32_t acpios_sci_count(void)
{
    return sci_count;
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER fn,
                                          void *context)
{
    if (irq >= 16 || !fn)
        return AE_BAD_PARAMETER;
    if (sci_handler)
        return AE_ALREADY_EXISTS;
    sci_handler = fn;
    sci_context = context;
    sci_irq = (int)irq;
    irq_install_handler((int)irq, sci_entry);
    pic_unmask((uint8_t)irq);
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 irq, ACPI_OSD_HANDLER fn)
{
    if ((int)irq != sci_irq || fn != sci_handler)
        return AE_NOT_EXIST;
    pic_mask((uint8_t)irq);
    irq_install_handler((int)irq, (irq_handler_t)0);
    sci_handler = NULL;
    sci_irq = -1;
    return AE_OK;
}

/* --- what it says ------------------------------------------------------- */

/* ACPICA's own (utprint.c), under the name the build gives it: the
   kernel has printk, not a formatter into a buffer */
int acpi_vsnprintf(char *s, ACPI_SIZE size, const char *format, va_list args);

void AcpiOsVprintf(const char *format, va_list args)
{
    char line[256];
    acpi_vsnprintf(line, sizeof(line), format, args);
    printk("%s", line);
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    AcpiOsVprintf(format, args);
    va_end(args);
}

ACPI_STATUS AcpiOsSignal(UINT32 function, void *info)
{
    (void)info;
    if (function == ACPI_SIGNAL_FATAL)
        printk("[ !! ] ACPI: the firmware's AML signalled a fatal error\n");
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 state, UINT32 a, UINT32 b)
{
    (void)state;
    (void)a;
    (void)b;
    return AE_OK;
}

void AcpiOsTracePoint(ACPI_TRACE_EVENT_TYPE type, BOOLEAN begin,
                      UINT8 *aml, char *pathname)
{
    (void)type;
    (void)begin;
    (void)aml;
    (void)pathname;
}
