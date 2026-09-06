/* Duplicating a process (fork) in long mode
 *
 * The child differs from the parent in two things and nothing more:
 * it has its own copy of memory and zero instead of the child number
 * in RAX.
 *
 * The whole trick is that the kernel has the program's complete state
 * at hand. The syscall stub lays down exactly the frame an interrupt
 * lays down - so it is enough to copy it and tell the new task to
 * return to ring 3 through it. No registers have to be restored "by
 * hand" along the way: that is done by the same path that returns from
 * every other call.
 */

#include <stddef.h>
#include <stdint.h>
#include "proc64.h"
#include "paging64.h"
#include "task64.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/initrd.h"
#include "elf64.h"
#include "../kernel/include/prog.h"
#include "../kernel/include/shm.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/biglock.h"
#include "syscall64.h"

/* Return to ring 3 from a frame given by pointer (boot64/syscall64.S) */
extern void return_to_user_64(struct regs *frame);

/* How many frames the last fork cost. With shared pages these should
   be page tables only - the program's memory is not copied at all. */
static uint32_t last_cost;

uint32_t proc64_last_fork_cost(void)
{
    return last_cost;
}

/* --- Arguments on the program stack ------------------------------
 *
 * The layout is the same as everywhere: first the strings themselves,
 * above them an array of pointers terminated by zero, and the program
 * receives their count and the array address in registers. There is
 * nothing magic about it - this is ordinary program memory that the
 * kernel happened to fill in before its first instruction.
 *
 * We write straight to user addresses, because at this moment we are
 * already standing in its space. Returns the new stack top and writes
 * out the array address. */
static uint64_t build_args(uint64_t rsp, const struct args64 *args,
                           uint64_t *argv_va)
{
    uint64_t ptrs[ARG_MAX];
    int argc = args ? args->argc : 0;

    if (argc > ARG_MAX)
        argc = ARG_MAX;

    for (int i = 0; i < argc; i++) {
        uint32_t len = 0;
        while (args->v[i][len])
            len++;
        rsp -= len + 1;
        for (uint32_t j = 0; j <= len; j++)
            ((char *)(uintptr_t)rsp)[j] = args->v[i][j];
        ptrs[i] = rsp;
    }

    rsp &= ~0xFULL;
    rsp -= (uint64_t)(argc + 1) * 8;
    uint64_t *tab = (uint64_t *)(uintptr_t)rsp;
    for (int i = 0; i < argc; i++)
        tab[i] = ptrs[i];
    tab[argc] = 0;                   /* the array ends with a zero */

    *argv_va = rsp;

    /* Top aligned to sixteen bytes, minus eight.
       A program starts at a _start function written in C, and the
       compiler treats it like any other: it assumes the stack looks the
       way it does after a call instruction, that is, with a return
       address on top. It bases the alignment of its variables on that
       assumption - and when it does not hold, the first SSE instruction
       writing sixteen bytes (movaps) ends in a general protection
       fault. Signal delivery computes the stack the same way, and for
       the same reason. */
    return (rsp & ~0xFULL) - 8;
}

/* Copying a string from program memory into the kernel.
   We check page by page, because the string may end just before the
   boundary of what the program has mapped. */
static int copy_user_string(uint64_t ptr, char *dst, int max)
{
    for (int i = 0; i < max - 1; i++) {
        if ((ptr & 0xFFF) == 0 || i == 0) {
            if (!paging64_user_range(ptr, 1))
                return -1;
        }
        char c = *(const char *)(uintptr_t)ptr++;
        dst[i] = c;
        if (!c)
            return i;
    }
    dst[max - 1] = '\0';
    return max - 1;
}

/* Gathers the argv given by the program. Returns the argument count. */
int proc64_copy_args(uint64_t argv_ptr, struct args64 *out)
{
    out->argc = 0;
    if (!argv_ptr)
        return 0;
    if (!paging64_user_range(argv_ptr, (uint64_t)(ARG_MAX + 1) * 8))
        return 0;

    const uint64_t *tab = (const uint64_t *)(uintptr_t)argv_ptr;
    for (int i = 0; i < ARG_MAX && tab[i]; i++) {
        if (copy_user_string(tab[i], out->v[i], ARG_LEN) < 0)
            break;
        out->argc++;
    }
    return out->argc;
}

struct fork_arg {
    struct regs frame;
    uint64_t space;
};

/* The child's first code. It still runs in the kernel, on its own
   stack - it takes ownership of the space and returns to ring 3 through
   a copy of the parent's frame. */
static void child_start(void *arg)
{
    struct fork_arg *fa = (struct fork_arg *)arg;
    struct regs frame = fa->frame;
    uint64_t space = fa->space;
    kfree(fa);

    /* The child's kernel stack - it starts from here on every subsequent
       entry from ring 3. We take it before the frame goes onto the
       stack, so that the frame lies below it and does not get
       overwritten. */
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(space, kstack & ~0xFULL);

    frame.rax = 0;                   /* the child is recognized by the zero */
    /* We enter user code - the big kernel lock stays here */
    biglock_leave();
    return_to_user_64(&frame);       /* does not return */
}

int64_t proc64_fork(struct regs *r)
{
    uint32_t before = pmm_used_frames();
    uint64_t space = paging64_clone_space(paging64_current_space());
    last_cost = pmm_used_frames() - before;
    if (!space) {
        printk("[kernel] fork: no memory to copy the address space\n");
        return -1;
    }

    struct fork_arg *fa = kmalloc(sizeof(*fa));
    if (!fa) {
        paging64_free_space(space);
        return -1;
    }
    fa->frame = *r;
    fa->space = space;

    struct task64 *child = task64_create("child", child_start, fa);
    if (child) {
        task64_inherit_fds(child);   /* open files go with the memory */
        task64_inherit_brk(child);
        task64_mmap_inherit(child);  /* and mapped files too */
    }
    if (!child) {
        kfree(fa);
        paging64_free_space(space);
        return -1;
    }
    return task64_id(child);
}

/* Replacing the process image (exec)
 *
 * The process stays the same - the same task number, the same parent,
 * the same kernel stack - and everything it has in memory changes.
 *
 * Returning to the new program needs no separate path of its own: we
 * are inside a system call handler, and that handler returns to ring 3
 * through the frame it was given. It is enough to rewrite it - a new
 * start address, a new stack, zeroed registers - and let it return.
 * The program on the other side has no way of noticing that it
 * returned somewhere other than where it called from.
 *
 * The order here is the only possible one: the name has to be copied
 * into the kernel BEFORE the memory it lies in disappears, and the old
 * space may be freed only once the processor has left it.
 */
int64_t proc64_exec(struct regs *r, const char *name, uint64_t len,
                    uint64_t argv_ptr)
{
    char kname[16];
    struct args64 args;

    if (len == 0 || len >= sizeof(kname) ||
        !paging64_user_range((uint64_t)(uintptr_t)name, len))
        return -1;
    for (uint64_t i = 0; i < len; i++)
        kname[i] = name[i];
    kname[len] = '\0';

    /* We copy the arguments into the kernel BEFORE the memory they lie
       in disappears - just like the program name */
    proc64_copy_args(argv_ptr, &args);
    if (args.argc == 0) {
        args.argc = 1;
        for (uint64_t i = 0; i <= len && i < ARG_LEN - 1; i++)
            args.v[0][i] = kname[i];
    }

    uint32_t size = 0;
    void *loaded = (void *)0;
    const void *image = prog_image(kname, &size, &loaded);
    if (!image) {
        printk("[kernel] exec: there is no program '%s'\n", kname);
        return -1;
    }

    uint64_t space = paging64_new_space();
    if (!space) {
        prog_release(loaded);
        return -1;
    }

    /* Writable mappings of the old image are flushed while its pages are
       still there - in a moment this space will be gone */
    proc64_msync();

    /* The kernel stack stays the same - we are standing on it right now */
    uint64_t old = task64_swap_space(space, 0);

    uint64_t entry, rsp, brk_base;
    if (elf64_load(image, size, &entry, &brk_base) < 0 ||
        !(rsp = elf64_make_stack())) {
        /* The old image no longer exists and the new one did not load -
           the process has nothing to return to */
        printk("[kernel] exec: '%s' cannot be loaded\n", kname);
        paging64_free_space(old);
        prog_release(loaded);
        task64_exit_code(-1);
    }
    paging64_free_space(old);
    /* The file mappings described the old space, which is gone - the new
       image starts with an empty table */
    task64_mmap_release();
    /* The image has already been copied into the program's pages - the
       copy in kernel memory is not needed for anything */
    prog_release(loaded);
    task64_set_brk_base(brk_base);   /* new image, new heap */
    task64_rename(kname);

    uint64_t argv_va = 0;
    rsp = build_args(rsp, &args, &argv_va);

    /* The return frame now describes the new program */
    r->rip = entry;
    r->rsp = rsp;
    r->rax = r->rbx = r->rcx = r->rdx = 0;
    r->rbp = 0;
    r->rdi = (uint64_t)args.argc;
    r->rsi = argv_va;
    r->r8 = r->r9 = r->r10 = r->r11 = 0;
    r->r12 = r->r13 = r->r14 = r->r15 = 0;
    return 0;
}

/* --- Running a program from the initrd ----------------------------
 *
 * A new process means a new task, a new address space and a program
 * read from a file. The kernel does this at startup and the shell on
 * request - and it is exactly the same path.
 */
struct spawn_req {
    char name[16];
    struct args64 args;
    int quiet;
    int anycore;                /* 1 = any core is allowed to run it */
};

static void spawn_thread(void *arg)
{
    struct spawn_req *req = (struct spawn_req *)arg;
    char name[16];
    struct args64 args = req->args;
    int quiet = req->quiet;

    /* A process allowed to run on any core says so now - before it does
       anything. Its kernel side runs under the big lock anyway; what
       goes in parallel is the counting itself, in ring 3. */
    if (req->anycore)
        task_set_anycore();

    for (int i = 0; i < (int)sizeof(name); i++)
        name[i] = req->name[i];
    kfree(req);

    uint32_t size = 0;
    void *loaded = (void *)0;
    const void *image = prog_image(name, &size, &loaded);
    if (!image) {
        printk("[ !! ] there is no program '%s' in the initrd or on the disk\n",
               name);
        task64_exit();
    }
    const char *from_where = prog_source();

    /* Its own address space. From this moment the program's gigabyte
       belongs to it alone - and kernel memory is still visible, because
       it is the same in every space. */
    uint64_t space = paging64_new_space();
    if (!space) {
        printk("[ !! ] no memory for an address space\n");
        task64_exit();
    }

    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(space, kstack & ~0xFULL);

    uint64_t entry, brk_base;
    int loaded_flag = elf64_load(image, size, &entry, &brk_base) == 0;
    prog_release(loaded);        /* the image is already in the program's pages */
    if (!loaded_flag)
        task64_exit();
    task64_set_brk_base(brk_base);

    uint64_t rsp = elf64_make_stack();
    if (!rsp) {
        printk("[ !! ] elf64: no memory for the program's stack\n");
        task64_exit();
    }

    if (!quiet)
        printk("[ OK ] Program '%s' from %s: %u B, entry %x\n",
               name, from_where, size, (uint32_t)entry);

    uint64_t argv_va = 0;
    rsp = build_args(rsp, &args, &argv_va);

    /* We enter user code - the big kernel lock stays here */
    biglock_leave();
    enter_ring3_64(entry, rsp, (uint64_t)args.argc, argv_va);
}

/* --- Shared memory ------------------------------------------------
 *
 * A region is common to everyone who gives the same key: the same
 * physical frames enter several address spaces at once. Every process
 * sees them at ITS OWN address - the next free one in its mapping area
 * - because a virtual address is a matter of the space, not of the
 * memory.
 *
 * The pages do not get the no-execute bit: a shared buffer is as often
 * a place for code as for data, and what lies in it is decided by
 * whoever sets it up.
 */
/* --- Anonymous memory ---------------------------------------------
 *
 * Fresh, zeroed memory in the process's mapping area. Nothing is
 * allocated here: we only reserve the addresses, and the pages appear
 * on the first touch - exactly like the heap. A program that asks for
 * a megabyte and uses one page will also occupy one.
 *
 * That is what a thread's stack comes from, and it is the reason this
 * exists at all: a thread needs a piece of the process's space that is
 * nobody else's, and 'nobody else's' is decided by the one counter
 * every thread of the process shares.
 */
uint64_t proc64_mmap(uint64_t size)
{
    if (!size)
        return 0;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t base = task64_mmap_next();
    uint64_t end = base + pages * PAGE_SIZE;
    if (!base || end > USER64_MMAP_MAX || end < base)
        return 0;

    task64_set_mmap_next(end);
    return base;
}

/* --- A thread of the process ---------------------------------------
 *
 * The same address space, its own stack. Everything hard about this
 * lies elsewhere: the space has to outlive the first thread to end
 * (task64.c), the next free address has to be one counter for all of
 * them and not a copy each (task64.c), and a mapping changed by one of
 * them has to reach the cores running the others (kernel/tlb.c).
 * What is left here is handing out a stack and going down to ring 3.
 */
#define THREAD_STACK (8 * PAGE_SIZE)

struct thread64_start {
    uint64_t entry;
    uint64_t arg;
    uint64_t ustack_top;
};

static void thread64_trampoline(void *a)
{
    struct thread64_start ts = *(struct thread64_start *)a;
    kfree(a);

    /* The kernel stack of THIS thread - the processor has to be told
       where to come back to on an interrupt from ring 3, and on a
       syscall, and each thread has its own */
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(paging64_current_space(), kstack & ~0xFULL);

    /* We enter user code - the big kernel lock stays here */
    biglock_leave();
    enter_ring3_64(ts.entry, ts.ustack_top, ts.arg, 0);
}

int proc64_thread(uint64_t entry, uint64_t arg)
{
    uint64_t space = paging64_current_space();
    if (!task64_mmap_next() || !space)      /* from a ring 3 process only */
        return -1;

    uint64_t stack = proc64_mmap(THREAD_STACK);
    if (!stack)
        return -1;

    struct thread64_start *ts = kmalloc(sizeof(*ts));
    if (!ts)
        return -1;
    ts->entry = entry;
    ts->arg = arg;
    /* Aligned to sixteen bytes, minus eight - and the minus eight is
       the whole point. A thread starts at an ordinary C function, and
       the compiler treats it like any other: it assumes the stack looks
       the way it does after a call instruction, with a return address
       on top. It bases the alignment of its own variables on that, and
       when it does not hold, the first movaps writing sixteen bytes
       ends in a general protection fault. The program's own stack is
       laid out the same way and for the same reason. */
    ts->ustack_top = ((stack + THREAD_STACK - 16) & ~0xFULL) - 8;

    struct task64 *t = task64_create_thread("thread", thread64_trampoline,
                                            ts, space);
    if (!t) {
        kfree(ts);
        return -1;
    }
    return task64_id(t);
}

uint64_t proc64_shm(int key, uint64_t size)
{
    if (!size)
        size = PAGE_SIZE;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t base = task64_mmap_next();
    uint64_t end = base + pages * PAGE_SIZE;
    if (!base || end > USER64_MMAP_MAX || end < base)
        return 0;

    uint32_t phys = shm_get(key, (uint32_t)(pages * PAGE_SIZE));
    if (!phys)
        return 0;

    for (uint64_t i = 0; i < pages; i++)
        if (paging64_map(base + i * PAGE_SIZE,
                         (uint64_t)phys + i * PAGE_SIZE,
                         PG_USER | PG_WRITE) < 0)
            return 0;

    task64_set_mmap_next(end);
    return base;
}

/* --- A file mapped into memory ------------------------------------
 *
 * The program is given an address at which it sees the file's
 * contents. No page is created right away: we remember the block
 * numbers, and the bytes themselves come from disk only on the first
 * touch. Asking to map a hundred-megabyte file therefore costs as much
 * as a list of its blocks - not a hundred megabytes.
 *
 * 'write' decides what touching it with a write means. Without it the
 * page has no write permission and the attempt ends in a protection
 * fault - better that than a change quietly dying. With it the page is
 * writable and its contents go back to the file at proc64_msync
 * (explicitly or when the process exits). When - that is the program's
 * decision, not the kernel's: merely holding a dirty page costs
 * nothing, while flushing it after every byte would kill the whole
 * point of a mapping.
 */
uint64_t proc64_mmap_file(const char *name, uint64_t *size_out,
                           uint64_t write)
{
    if (!fs_ready())
        return 0;

    int f = fs_open(task64_cwd(), name, 0);
    if (f < 0)
        return 0;
    int len = fs_size(f);
    if (len <= 0)
        return 0;

    uint32_t nblocks = ((uint32_t)len + FS_BLOCK - 1) / FS_BLOCK;
    uint64_t pages = ((uint64_t)len + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t base = task64_mmap_next();
    uint64_t end = base + pages * PAGE_SIZE;
    if (!base || end > USER64_MMAP_MAX || end < base)
        return 0;

    uint32_t *blocks = kmalloc(nblocks * sizeof(uint32_t));
    if (!blocks)
        return 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        /* We work the numbers out NOW, at leisure - inside a page fault
           handler we may no longer step into file system code */
        blocks[i] = fs_file_block(f, i, 0);
        if (!blocks[i]) {
            kfree(blocks);
            return 0;
        }
    }

    if (task64_mmap_add(base, end, (uint32_t)len, blocks, nblocks,
                          write ? 1u : 0u) < 0) {
        kfree(blocks);
        return 0;
    }
    task64_set_mmap_next(end);
    *size_out = (uint64_t)len;
    return base;
}

/* Flushes the pages of the current process's writable mappings back to
 * the file. We walk pages, not blocks: the page is the unit the
 * processor calls "dirty" for us, and only such pages are copied here.
 * The rest of the walk mirrors the fill on a page fault - the same
 * block numbers, worked out when the mapping was set up, only with
 * blk_write instead of blk_read.
 *
 * Returns the number of pages flushed.
 */
uint64_t proc64_msync(void)
{
    if (!task64_mmap_next())
        return 0;
    uint64_t sent_back_n = 0;

    for (int i = 0; i < MMAP_FILES; i++) {
        uint64_t start, end;
        uint32_t size, write;
        if (!task64_mmap_info(i, &start, &end, &size, &write) || !write)
            continue;
        for (uint64_t va = start; va < end; va += PAGE_SIZE) {
            if (!paging64_file_dirty(va))
                continue;
            uint64_t frame = paging64_lookup(va) & ~0xFFFULL;
            if (!frame)
                continue;
            uint64_t offset = va - start;
            const uint8_t *src = (const uint8_t *)(uintptr_t)frame;
            for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK; b++) {
                /* Past the end of the file there is nothing to flush - the block
                   either does not exist or already belongs to somebody
                   else */
                if (offset + b * FS_BLOCK >= size)
                    break;
                uint32_t blok = task64_mmap_block(va,
                                    (uint32_t)(offset / FS_BLOCK) + b);
                if (!blok)
                    break;
                blk_write(fs_block_lba(blok), FS_BLOCK / 512,
                          src + b * FS_BLOCK);
            }
            paging64_file_clean(va);
            sent_back_n++;
        }
    }
    return sent_back_n;
}

static int spawn_inner(const char *name, const struct args64 *args,
                      int quiet, int anycore)
{
    struct spawn_req *req;
    int i;

    if (!prog_exists(name))
        return -1;

    req = kmalloc(sizeof(*req));
    if (!req)
        return -1;
    for (i = 0; i < (int)sizeof(req->name) - 1 && name[i]; i++)
        req->name[i] = name[i];
    req->name[i] = '\0';
    req->quiet = quiet;
    req->anycore = anycore;
    if (args) {
        req->args = *args;
    } else {
        /* With no arguments a program still gets its own name - argv[0]
           is what it is everywhere */
        req->args.argc = 1;
        for (int k = 0; k < ARG_LEN - 1 && req->name[k]; k++)
            req->args.v[0][k] = req->name[k];
        req->args.v[0][ARG_LEN - 1] = '\0';
        for (int k = 0; k < ARG_LEN; k++)
            if (!req->name[k]) { req->args.v[0][k] = '\0'; break; }
    }

    struct task64 *t = task64_create(req->name, spawn_thread, req);
    if (!t) {
        kfree(req);
        return -1;
    }
    return task64_id(t);
}

int proc64_spawn(const char *name, const struct args64 *args, int quiet)
{
    return spawn_inner(name, args, quiet, 0);
}

/* The same, but the process may be run by ANY core */
int proc64_spawn_anycore(const char *name, const struct args64 *args,
                          int quiet)
{
    return spawn_inner(name, args, quiet, 1);
}
