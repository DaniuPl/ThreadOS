/* Common interrupt handling in long mode
 *
 * The role is the same as in the 32-bit kernel: a processor exception
 * stops the system with a diagnostic, and a hardware interrupt goes to
 * the handler registered by a driver. The only differences are the
 * register frame and the fact that addresses are 64-bit.
 */

#include <stddef.h>
#include <stdint.h>
#include "../kernel/include/isr.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/tlb.h"
#include "../kernel/include/biglock.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/vga.h"
#include "task64.h"
#include "elf64.h"
#include "isr64.h"
#include "paging64.h"
#include "signal64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/blk.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "(reserved)",
    "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD FP Exception", "Virtualization Exception", "Control Protection",
    "(reserved)", "(reserved)", "(reserved)",
    "(reserved)", "(reserved)", "(reserved)",
    "(reserved)", "(reserved)", "(reserved)",
    "(reserved)",
};

static irq_handler_t irq_handlers[16];

/* The trace left by the last exception from ring 3 - to be inspected
   after the guilty program is no longer alive */
static int user_fault_seen;
static uint32_t user_fault_vec, user_fault_err;
static uint32_t heap_pages;        /* how many heap pages really came into being */
static uint32_t stack_pages;       /* and how many pages the stack gained */
static uint32_t mmap_pages;        /* and how many were read from mapped files */
static uint64_t user_fault_addr;

int isr64_user_fault(void)            { return user_fault_seen; }
uint32_t isr64_user_fault_vector(void) { return user_fault_vec; }
uint32_t isr64_user_fault_err(void)    { return user_fault_err; }
uint64_t isr64_user_fault_addr(void)   { return user_fault_addr; }
void isr64_forget_user_fault(void)     { user_fault_seen = 0; }
uint32_t isr64_heap_pages(void)        { return heap_pages; }
uint32_t isr64_stack_pages(void)       { return stack_pages; }
uint32_t isr64_mmap_pages(void)        { return mmap_pages; }

static uint64_t read_cr2(void)
{
    uint64_t cr2;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void irq_install_handler(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

static void isr64_dispatch(struct regs *r);

/* Entry into the kernel from ring 3 takes the big kernel lock.
 *
 * A program may run on any core, but inside the kernel there is to be
 * one at a time. An interrupt that caught the processor already in the
 * kernel takes nothing new: the lock is recursive. */
/* --- the fault causes that need no big kernel lock -------------------
 *
 * The same list as in the 32-bit kernel (kernel/exec.c): heap, stack
 * and anonymous memory in the mapping area. A frame, four kilobytes of
 * zeros and one entry in a table - and all three have locks of their
 * own. Everything else reaches for the disk or for the task list and
 * goes in as before.
 *
 * Returns 1 when it dealt with the fault.
 */
static int demand_page_nolock(uint64_t addr)
{
    uint64_t page = addr & ~0xFFFULL;
    uint32_t first = 0, remaining = 0, write = 0;

    int in_heap = task64_in_heap(addr);
    int in_mmap = task64_in_mmap(addr);
    int in_stack = addr < USER64_STACK_TOP &&
                   addr >= USER64_STACK_TOP - USER64_STACK_MAX;
    if (!in_heap && !in_mmap && !in_stack)
        return 0;
    if (in_mmap && task64_mmap_find(addr, &first, &remaining, &write))
        return 0;                   /* a file - that one reaches for the disk */
    if (!paging64_entry_empty(page))
        return 0;                   /* present, or on the disk - not ours */

    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                   /* out of memory - let the locked path try */
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
    if (paging64_map_if_absent(page, frame, PG_USER | PG_WRITE | PG_NX)) {
        if (in_stack)
            stack_pages++;
        else if (in_mmap)
            mmap_pages++;
        else
            heap_pages++;
    } else {
        /* Another core got here first with its own frame. The page is
           there, the instruction may be repeated - ours is spare. */
        pmm_free_frame(frame);
    }
    return 1;
}

/* How many page faults were served without the big kernel lock, and
   how many had to take it. Nothing depends on these; they are here so
   that the question "is the lock really not being taken" has an answer
   that is not a stopwatch. */
static uint32_t faults_nolock;
static uint32_t faults_locked;

uint32_t isr_faults_nolock(void) { return faults_nolock; }
uint32_t isr_faults_locked(void) { return faults_locked; }

void isr64_handler(struct regs *r)
{
    int z_ring3 = (r->cs & 3) == 3;

    /* A page fault is the commonest way into the kernel there is, and
       most of them are not errors. Those causes are served before the
       lock is taken - the tables, the frame allocator and the
       translation buffers have locks of their own. Copy-on-write goes
       the same way, and it is the sharper case: there the page is
       present and the frame under it changes. */
    if (r->int_no == 14) {
        uint64_t cr2 = read_cr2();
        if ((r->err_code & 0x02) && paging64_handle_cow(cr2)) {
            faults_nolock++;
            return;
        }
        if (z_ring3 && !(r->err_code & 0x01) && demand_page_nolock(cr2)) {
            faults_nolock++;
            return;
        }
        faults_locked++;
    }

    if (z_ring3)
        biglock_enter();
    isr64_dispatch(r);
    if (z_ring3)
        biglock_leave();
}


static void isr64_dispatch(struct regs *r)
{
    if (r->int_no < 32) {
        /* A breakpoint is a trap exception: report it and carry on */
        if (r->int_no == 3) {
            printk("[INT3] a breakpoint at rip=%x\n", (uint32_t)r->rip);
            return;
        }

        /* A write to a page shared after a fork is not an error but a bill
         * to be paid: the page has to be split and the program allowed
         * to repeat the instruction.
         *
         * We check this before anything else and regardless of the
         * ring - with write protection enabled the kernel gets a fault
         * here too when it writes to a process's memory. */
        if (r->int_no == 14 && (r->err_code & 0x02) &&
            paging64_handle_cow(read_cr2()))
            return;

        /* A page that went out to disk. We fetch it back and let the
         * instruction be repeated - as far as the program is concerned
         * nothing happened beyond a moment's delay. */
        if (r->int_no == 14 && !(r->err_code & 0x01) &&
            paging64_swap_in(paging64_current_space(), read_cr2()))
            return;

        /* A page of a file mapped into memory.
         *
         * The bytes come from disk only now - through the block layer,
         * by the numbers worked out when the mapping was set up. We do
         * not step into file system code here: it has one shared buffer
         * for a block, and a page fault can land in the middle of any
         * file operation.
         *
         * The tail of the last page is zeroed out - a file ends where
         * it ends.
         *
         * A writable mapping gets write permission and the PG_FILE
         * flag; without it the page is read-only and an attempted write
         * has to end in a fault rather than in data quietly getting
         * lost. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t addr = read_cr2();
            uint32_t first = 0, remaining = 0, write = 0;
            if (task64_mmap_find(addr, &first, &remaining, &write) &&
                paging64_lookup(addr & ~0xFFFULL) == 0) {
                uint32_t frame = pmm_alloc_frame();
                if (frame) {
                    uint8_t *dst = (uint8_t *)(uintptr_t)frame;
                    memset(dst, 0, PAGE_SIZE);
                    for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK &&
                                         remaining > b * FS_BLOCK; b++) {
                        uint32_t block = task64_mmap_block(addr, first + b);
                        if (!block)
                            break;
                        blk_read(fs_block_lba(block), FS_BLOCK / 512,
                                 dst + b * FS_BLOCK);
                    }
                    uint64_t right = PG_USER | PG_NX;
                    if (write)
                        right |= PG_WRITE | PG_FILE;
                    if (paging64_map(addr & ~0xFFFULL, frame, right) == 0) {
                        mmap_pages++;
                        return;
                    }
                    pmm_free_frame(frame);
                }
            }
        }

        /* Anonymous memory the process asked for in the mapping area.
         *
         * The same bargain as the heap: asking reserved addresses and
         * nothing more, and the page appears the moment somebody
         * touches it. This is what a thread's stack is made of, so it
         * has to come AFTER the mapped-file case - both live in the
         * same area, and a file has contents while this has zeros. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t addr = read_cr2();
            if (task64_in_mmap(addr) &&
                paging64_lookup(addr & ~0xFFFULL) == 0) {
                uint32_t frame = pmm_alloc_frame();
                if (frame) {
                    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
                    if (paging64_map(addr & ~0xFFFULL, frame,
                                     PG_USER | PG_WRITE | PG_NX) == 0) {
                        mmap_pages++;
                        return;
                    }
                    pmm_free_frame(frame);
                }
            }
        }

        /* The stack, which has just grown by one page downwards.
         *
         * A program gets four pages to start with and we add the rest
         * under its fingers - exactly like the heap. The limit is there
         * so that a wild pointer does not quietly turn into a mapped
         * page: below it a page fault stays a fault. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t addr = read_cr2();
            if (addr < USER64_STACK_TOP &&
                addr >= USER64_STACK_TOP - USER64_STACK_MAX &&
                paging64_lookup(addr & ~0xFFFULL) == 0) {
                uint32_t frame = pmm_alloc_frame();
                if (frame) {
                    memset((void *)(uintptr_t)frame, 0, 4096);
                    if (paging64_map(addr & ~0xFFFULL, frame,
                                     PG_USER | PG_WRITE | PG_NX) == 0) {
                        stack_pages++;
                        return;
                    }
                    pmm_free_frame(frame);
                }
            }
        }

        /* A touch of a heap page the program asked for but which does
         * not exist yet. This is not an error but the moment memory
         * becomes real: asking for a megabyte cost zero, and payment is
         * due only for the pages actually used.
         *
         * Writable and with no execute permission - there is no code in
         * a program's data, and if there were, the processor would not
         * run it. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t addr = read_cr2();
            if (task64_in_heap(addr) &&
                paging64_lookup(addr & ~0xFFFULL) == 0) {
                uint32_t frame = pmm_alloc_frame();
                if (frame) {
                    memset((void *)(uintptr_t)frame, 0, 4096);
                    if (paging64_map(addr & ~0xFFFULL, frame,
                                     PG_USER | PG_WRITE | PG_NX) == 0) {
                        heap_pages++;
                        return;
                    }
                    pmm_free_frame(frame);
                }
            }
        }

        /* An exception from ring 3 is the PROGRAM's error, not the
         * system's.
         *
         * The two lowest bits of the code selector say where the
         * interrupt came from - and it is the processor that puts them
         * there, so the program has no way of lying. If it was at
         * fault, only it dies: the kernel prints what happened and
         * goes back to work. Stopping the whole system because one
         * program reached where it must not was fine in the days
         * before memory protection.
         *
         * The handler does not return from this exception: schedule()
         * switches to another thread and the interrupt frame is
         * abandoned together with the dead task's stack. */
        if ((r->cs & 3) == 3) {
            user_fault_seen = 1;
            user_fault_vec = (uint32_t)r->int_no;
            user_fault_err = (uint32_t)r->err_code;
            user_fault_addr = r->int_no == 14 ? read_cr2() : r->rip;

            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            printk("[kernel] exception %u (%s) in a program: err=%x, "
                   "rip=%x, address=%x\n",
                   (uint32_t)r->int_no, exception_names[r->int_no],
                   (uint32_t)r->err_code, (uint32_t)r->rip,
                   (uint32_t)user_fault_addr);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            task64_kill_current(exception_names[r->int_no]);
        }

        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("\n!!! CPU EXCEPTION %u: %s (err=%x)\n",
               (uint32_t)r->int_no, exception_names[r->int_no],
               (uint32_t)r->err_code);
        printk("rip=%x:%x rsp=%x:%x\n",
               (uint32_t)(r->rip >> 32), (uint32_t)r->rip,
               (uint32_t)(r->rsp >> 32), (uint32_t)r->rsp);
        if (r->int_no == 14) {
            uint64_t cr2 = read_cr2();
            printk("Page fault at the address (CR2): %x:%x\n",
                   (uint32_t)(cr2 >> 32), (uint32_t)cr2);
        }
        printk("The system has stopped.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* The local APIC timer. It did not come through the PIC, so there is
       nothing to close there - the acknowledgement goes to this core's
       own APIC. On a secondary core this is the only interrupt that
       arrives at all. */
    if (r->int_no == 48) {
        smp_timer_tick();
        return;
    }

    /* A wake-up from another core. It carries no content: the point was
       only to pull this processor out of a hlt instruction. */
    if (r->int_no == 49) {
        smp_wakeup_recv();
        return;
    }

    /* A mapping has changed on another core and it is waiting for our
       answer. Nothing here may take a lock: the core on the other side
       is holding one and standing still until we reply. */
    if (r->int_no == 50) {
        tlb_ipi_recv();
        return;
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = (int)(r->int_no - IRQ_BASE);
        /* EOI before the handler - just as in the 32-bit kernel */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }

    /* A program interrupted mid-step is the most frequent moment at
       which a signal can be delivered - and the only one that works
       even when the program asks the kernel for nothing */
    if ((r->cs & 3) == 3)
        signal64_deliver(r);
}
