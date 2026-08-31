/* exec - loading ELF programs from the initrd
 *
 * Every process gets its own page directory (kernel mappings shared as
 * supervisor, program segments and stack as USER). The loader runs in
 * the kernel thread of a freshly created task: it builds the address
 * space, copies PT_LOAD segments into new frames, maps the stack at
 * 0xC0000000 and jumps into ring 3 through iret.
 */

#include <stdint.h>
#include "include/exec.h"
#include "include/initrd.h"
#include "include/prog.h"
#include "include/paging.h"
#include "include/dynlink.h"
#include "include/biglock.h"
#include "include/smp.h"
#include "include/pmm.h"
#include "include/task.h"
#include "include/usermode.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/fs.h"
#include "include/blk.h"
#include "include/kfile.h"
#include "include/pipe.h"
#include "include/shm.h"
#include "include/printk.h"
#include "include/isr.h"

#define USER_STACK_TOP 0xC0000000u
/* The process stack: one page was enough until programs started doing
   crypto - RSA on 2048 bits needs a few kilobytes for the working
   numbers alone. Eight pages is 32 KiB. */
#define USER_STACK_PAGES 8
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* How far the stack may grow beyond those eight pages. The rest
   arrives on its own, under the program's fingers - exactly as the
   heap grows upward. Guessing up front how much stack is needed always
   ends either in wasted memory or in an exception in the middle of a
   deep call; and the limit is there so that a wild pointer does not
   quietly turn into a mapped page - below it a page fault stays a
   fault. */
#define USER_STACK_MAX   (256 * 1024u)
#define USER_STACK_BOTTOM   (USER_STACK_TOP - USER_STACK_MAX)
/* The shared library lies at a fixed address, the same in every
   process - only thanks to that can its code really be one. The place
   is just below the mapping area, and the heap ends at its doorstep;
   USER_LIB_BASE in kernel/include/exec.h and the address in the linker
   script user/libproton.ld must agree. */
#define USER_BRK_MAX   USER_LIB_BASE /* ceiling of the process heap (sbrk) */
#define MMAP_BASE      0xB0000000u   /* the mmap area (above the library) */
#define MMAP_MAX       0xBF000000u   /* mmap ceiling (below the space for the stack) */
#define ARG_MAX        16
#define CMD_MAX        128
#define MAX_STAGES     5     /* max number of pipeline stages a|b|c|... */

/* Arguments passed to a process thread: the command line plus
   independent stdin/stdout settings. This allows redirections to be
   combined, e.g. a pipe consumer with output to a file
   ('a | b > file'). */
struct exec_args {
    char cmd[CMD_MAX];
    int in_pipe;                  /* stdin from a pipe (idx) or -1 */
    int out_pipe;                 /* stdout to a pipe (idx) or -1 */
    char out_file[FS_NAME_MAX + 1]; /* stdout to a file ("" = none) */
    /* stdout inherited from whoever started us: the PFS inode of the
       file the parent writes to, or -1. A program started by a
       redirected program writes where its parent writes - which is
       what makes a shell inside a terminal show anything at all. */
    int out_inode;
    int out_append;               /* 1 = append (>>), 0 = truncate (>) */
    int anycore;                 /* 1 = any core may run it */
};

/* ELF32 headers (only the fields we use) */
struct elf_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

#define ET_EXEC  2
#define EM_386   3
#define PT_LOAD  1

static int elf_valid(const struct elf_ehdr *eh, uint32_t size)
{
    if (size < sizeof(*eh))
        return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 1)            /* ELFCLASS32 */
        return 0;
    return eh->e_type == ET_EXEC && eh->e_machine == EM_386 && eh->e_phnum;
}

/* A fresh frame for page vaddr, mapped READ-ONLY - for memory-mapped
   files */
static uint32_t page_frame_for_ro(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    if (!paging_map_user_ro_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

/* A fresh frame for a page of a writable file mapping: writable, but
   marked so that fork shares it and swapping passes it by */
static uint32_t page_frame_for_file(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    if (!paging_map_user_file_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

/* Maps (or finds) the frame for the process's page vaddr */
static uint32_t page_frame_for(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = paging_get_mapping(dir, vaddr);
    if (frame)
        return frame;
    frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    memset((void *)frame, 0, PAGE_SIZE);
    if (!paging_map_user_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

static int load_segment(uint32_t dir, const uint8_t *file,
                        const struct elf_phdr *ph)
{
    if (ph->p_vaddr < PAGING_USER_BASE ||
        ph->p_vaddr + ph->p_memsz > USER_STACK_BASE) {
        printk("exec: segment poza dozwolonym zakresem (%x)\n", ph->p_vaddr);
        return 0;
    }

    uint32_t file_end = ph->p_vaddr + ph->p_filesz;

    for (uint32_t va = ph->p_vaddr & ~0xFFFu;
         va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
        uint32_t frame = page_frame_for(dir, va);
        if (!frame)
            return 0;

        /* Copy the part of the file that falls on this page
           (the rest stays zeroed - that is .bss) */
        uint32_t copy_from = va > ph->p_vaddr ? va : ph->p_vaddr;
        if (copy_from < file_end) {
            uint32_t copy_to = va + PAGE_SIZE < file_end
                                   ? va + PAGE_SIZE : file_end;
            memcpy((void *)(frame + (copy_from - va)),
                   file + ph->p_offset + (copy_from - ph->p_vaddr),
                   copy_to - copy_from);
        }
    }
    return 1;
}

/* --- the shared library in a process's address space ------------------
 *
 * Segments without write permission (code and constants) get SHARED
 * FRAMES: the very ones every other process sees. That is the whole
 * saving - crypto and TLS lie in memory once, not once per process.
 *
 * The data segment is different, and has to be: the library keeps its
 * own state there (the random generator seed, TLS settings), and one
 * process's state has no business being visible in another. Each one
 * therefore gets a fresh copy - exactly as every other system does it.
 */
static int map_library(uint32_t dir)
{
    dyn_addr vaddr;
    uint32_t memsz, filesz;
    int right;
    const uint8_t *data;

    for (int i = 0; dyn_segment(i, &vaddr, &memsz, &filesz, &right, &data);
         i++) {
        uint32_t start = vaddr & ~0xFFFu;
        uint32_t end = vaddr + memsz;
        for (uint32_t va = start; va < end; va += PAGE_SIZE) {
            if (!(right & DYN_WRITE)) {
                uint32_t frame = dyn_frame(va);
                if (!frame)
                    return 0;
                if (!paging_map_user_ro_in(dir, va, frame))
                    return 0;
                /* The frame has to outlive the process - it belongs to the
                   system, not to it */
                pmm_frame_ref(frame);
                continue;
            }

            uint32_t frame = page_frame_for(dir, va);
            if (!frame)
                return 0;
            uint32_t od = va > vaddr ? va : vaddr;
            uint32_t file_to = vaddr + filesz;
            if (od < file_to) {
                uint32_t do_ = va + PAGE_SIZE < file_to ? va + PAGE_SIZE
                                                       : file_to;
                memcpy((void *)(frame + (od - va)), data + (od - vaddr),
                       do_ - od);
            }
        }
    }
    return 1;
}

/* Stores a resolved address in the program's jump table. The process's
   address space is not active yet, so we go through the frame. */
static int write_addr(void *ctx, dyn_addr addr, dyn_addr value)
{
    uint32_t dir = *(uint32_t *)ctx;
    uint32_t frame = paging_get_mapping(dir, addr & ~0xFFFu);
    if (!frame)
        return -1;
    *(uint32_t *)(frame + (addr & 0xFFFu)) = value;
    return 0;
}

/* Writing a 32-bit value onto the user stack - the kernel writes
   through the frame's physical address, because the process's address
   space is not active yet */
static void stack_write32(uint32_t sframe, uint32_t sp, uint32_t val)
{
    *(uint32_t *)(sframe + (sp - (USER_STACK_TOP - PAGE_SIZE))) = val;
}

/* Lays the arguments out on the process stack (the cdecl convention
   for _start(int argc, char **argv)) and returns the initial ESP */
static uint32_t build_argv_stack(uint32_t sframe, char **tok, int argc)
{
    uint32_t stack_base = USER_STACK_TOP - PAGE_SIZE;
    uint32_t sp = USER_STACK_TOP;
    uint32_t uargv[ARG_MAX];

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = strlen(tok[i]) + 1;
        sp -= len;
        memcpy((void *)(sframe + (sp - stack_base)), tok[i], len);
        uargv[i] = sp;
    }

    sp &= ~3u;                          /* align to 4 B */
    sp -= 4 * (argc + 1);               /* argv[] plus a NULL at the end */
    uint32_t argv_base = sp;
    for (int i = 0; i < argc; i++)
        stack_write32(sframe, argv_base + 4 * i, uargv[i]);
    stack_write32(sframe, argv_base + 4 * argc, 0);

    sp -= 4;
    stack_write32(sframe, sp, argv_base);   /* argv */
    sp -= 4;
    stack_write32(sframe, sp, (uint32_t)argc);
    sp -= 4;
    stack_write32(sframe, sp, 0);           /* a dummy return address */
    return sp;
}

/* Cleaning up after a failed start: if the process was to take over
   pipe ends, they have to be released, otherwise the other side hangs */
static void exec_fail(struct exec_args *a)
{
    if (a->out_pipe >= 0)
        pipe_close_writer(a->out_pipe);
    if (a->in_pipe >= 0)
        pipe_close_reader(a->in_pipe);
    kfree(a);
}

/* Splits a command line into words in place; returns argc */
static int tokenize(char *cmd, char **tok)
{
    int argc = 0;
    char *p = cmd;
    while (*p && argc < ARG_MAX) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        tok[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    return argc;
}

/* Builds a fresh process image: a new address space, the ELF segments
   from the initrd, the stack and the arguments on it. Returns CR3
   (0 on error) and fills in entry/esp/brk. Two paths use this - the
   start of a new process (exec_task) and replacing the current image
   (process_exec). */
static uint32_t build_image(char **tok, int argc, uint32_t *entry_out,
                            uint32_t *esp_out, uint32_t *brk_out)
{
    uint32_t size;
    void *loaded = (void *)0;
    const uint8_t *file = prog_image(tok[0], &size, &loaded);
    if (!file) {
        printk("exec: nie ma '%s' ani w initrd, ani na dysku "
               "(sprawdz 'ls' i 'fls')\n", tok[0]);
        return 0;
    }

    const struct elf_ehdr *eh = (const struct elf_ehdr *)file;
    if (!elf_valid(eh, size)) {
        printk("exec: '%s' nie jest programem ELF32 i386\n", tok[0]);
        prog_release(loaded);
        return 0;
    }

    uint32_t dir = paging_new_address_space();
    if (!dir) {
        printk("exec: brak pamieci na katalog stron\n");
        prog_release(loaded);
        return 0;
    }

    /* Load the segments, tracking the end of the image - that is where
       the process heap (sbrk) will begin */
    uint32_t brk = 0;
    const struct elf_phdr *ph =
        (const struct elf_phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (!load_segment(dir, file, &ph[i])) {
            paging_destroy_address_space(dir);
            prog_release(loaded);
            return 0;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz > brk)
            brk = ph[i].p_vaddr + ph[i].p_memsz;
    }

    /* The program may carry not the code it uses but only empty slots
       for the addresses of library functions. Those slots then have to
       be filled in - and NOW, before the first instruction, because a
       jump through an unfilled entry has nowhere to lead. */
    if (dyn_is_dynamic(file, size)) {
        if (!dyn_present()) {
            printk("exec: '%s' potrzebuje biblioteki %s, ktorej nie ma\n",
                   tok[0], dyn_name());
            paging_destroy_address_space(dir);
            prog_release(loaded);
            return 0;
        }
        if (!map_library(dir) ||
            dyn_relocate(file, size, &dir, write_addr) < 0) {
            printk("exec: '%s' - nie da sie zwiazac z %s\n",
                   tok[0], dyn_name());
            paging_destroy_address_space(dir);
            prog_release(loaded);
            return 0;
        }
    }

    /* The topmost stack page (where the arguments go), and the rest below it */
    uint32_t sframe = 0;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va = USER_STACK_TOP - (uint32_t)(i + 1) * PAGE_SIZE;
        uint32_t frame = page_frame_for(dir, va);
        if (!frame) {
            printk("exec: brak pamieci na stos\n");
            paging_destroy_address_space(dir);
            prog_release(loaded);
            return 0;
        }
        if (i == 0)
            sframe = frame;
    }

    *esp_out = build_argv_stack(sframe, tok, argc);
    *entry_out = eh->e_entry;
    *brk_out = brk;
    printk("[jadro] exec '%s' z %s: ELF %u B, entry %x, argc=%d\n",
           tok[0], prog_source(), size, eh->e_entry, argc);
    /* The segments are already in the process's pages - the copy in
       kernel memory is of no further use */
    prog_release(loaded);
    return dir;
}

/* Kernel thread of a new process: build the address space and enter ring 3 */
static void exec_task(void *arg)
{
    struct exec_args *a = arg;

    /* A process that may run on any core says so now - before it does
       anything. Its kernel side runs under the big lock anyway; what
       goes in parallel is the computing it does in ring 3. */
    if (a->anycore)
        task_set_anycore();

    /* Tokenising in place: a->cmd is our own copy */
    char *tok[ARG_MAX];
    int argc = tokenize(a->cmd, tok);
    if (!argc) {
        exec_fail(a);
        return;
    }

    uint32_t entry, esp, brk;
    uint32_t dir = build_image(tok, argc, &entry, &esp, &brk);
    if (!dir) {
        exec_fail(a);
        return;
    }

    /* Redirecting the streams before entering ring 3 (independently) */
    if (a->in_pipe >= 0)
        file_set_pipe(0, a->in_pipe, 0);    /* stdin <- the read end */
    if (a->out_pipe >= 0)
        file_set_pipe(1, a->out_pipe, 1);   /* stdout -> the write end */
    else if (a->out_file[0]) {
        if (file_redirect_stdout(a->out_file, a->out_append) < 0)
            printk("exec: nie moge przekierowac stdout do '%s'\n",
                   a->out_file);
    } else if (a->out_inode >= 0) {
        /* Nobody said where to write, but whoever started us writes to
           a file - so we write there too, after what is in it */
        file_inherit_stdout(a->out_inode);
    }
    kfree(a);

    /* From now on this task has an address space of its own; its frames
       will be released when the zombie is reaped */
    task_set_brk(brk);
    task_set_brk_base(brk);
    task_set_mmap_next(MMAP_BASE);
    task_use_address_space(dir);
    /* We are entering user code - the big kernel lock stays here. The
       program may run in parallel with others; it will come back into
       the kernel through an interrupt or a system call and take the
       lock again there. */
    biglock_leave();
    enter_ring3(entry, esp);
}

/* --- demand paging --------------------------------------------------
 *
 * The heap (sbrk) and the mmap area are promised to the process but
 * empty: none of those addresses has a frame yet. The first access
 * ends in a page fault, and only then does the kernel hand out a
 * zeroed frame and resume the instruction.
 *
 * The same point handles pages that are not so much yet to be created
 * as evicted to disk - then, instead of a fresh zero, their former
 * content comes back.
 *
 * This is called both from the exception handler (an access from
 * ring 3) and from pointer validation in syscalls: a kernel about to
 * fill a process's buffer has to make it real first - otherwise a
 * freshly allocated, untouched buffer would look like a bad pointer.
 *
 * Returns 1 when the page appeared (execution can resume), 0 when the
 * address belongs to no promised area or memory ran out.
 */
/* How many pages have been read in from memory-mapped files - for the
   'mem' command and for the tests: mapping a large file is to cost as
   much as was actually taken from it, not as much as it has pages */
static uint32_t mmap_pages;

uint32_t process_mmap_pages(void)
{
    return mmap_pages;
}

/* --- the fault causes that need no big kernel lock -------------------
 *
 * A page fault is the hottest way into the kernel there is: every
 * program touching memory goes through it thousands of times. Most of
 * those faults are not errors and not even work worth the name - a
 * frame from the allocator, four kilobytes of zeros and one entry in a
 * table, and all three have locks of their own.
 *
 * So the list below, exactly like the list of system calls that need
 * no lock: heap, stack, and anonymous memory in the mapping area.
 * Everything else - a page from the swap file, a page of a mapped
 * file, and every real error - goes in as before, because it reaches
 * for the disk or for the task list.
 *
 * 'Anonymous' is the awkward one: file mappings live in the same area,
 * so it is not the address that decides but the absence of a mapping
 * covering it. And the entry has to be EMPTY, not merely absent: a
 * page evicted to disk is absent too, and handing it a fresh page of
 * zeros would lose what is on the disk.
 *
 * Returns 1 when it dealt with the fault.
 */
int process_demand_page_nolock(uint32_t addr)
{
    uint32_t brk = task_brk();
    if (!brk)                       /* a kernel thread - it has no heap */
        return 0;

    uint32_t va = addr & ~(PAGE_SIZE - 1);
    int in_heap = addr >= task_brk_base() && addr < brk;
    int in_mmap = addr >= MMAP_BASE && addr < task_mmap_next();
    int in_stack = addr >= USER_STACK_BOTTOM && addr < USER_STACK_TOP;
    if (!in_heap && !in_mmap && !in_stack)
        return 0;

    uint32_t first = 0, remaining = 0, write = 0;
    if (in_mmap && task_mmap_find(addr, &first, &remaining, &write))
        return 0;                   /* a file - that one reaches for the disk */

    uint32_t dir = task_current_cr3();
    if (!paging_entry_empty(dir, va))
        return 0;                   /* present, or on the disk - not ours */

    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;                   /* out of memory - let the locked path try */
    memset((void *)frame, 0, PAGE_SIZE);
    if (!paging_map_user_if_absent(dir, va, frame)) {
        /* Another core got here first with its own frame. That is not a
           failure: the page is there, and the instruction may be
           repeated. Ours is the one that turns out to be spare. */
        pmm_free_frame(frame);
    }
    return 1;
}

int process_demand_page(uint32_t addr)
{
    uint32_t brk = task_brk();
    if (!brk)                       /* a kernel thread - it has no heap */
        return 0;

    uint32_t dir = task_current_cr3();
    uint32_t va = addr & ~(PAGE_SIZE - 1);
    if (paging_get_mapping(dir, va))
        return 0;                   /* the page is already there - the fault has another cause */

    /* Disk first, and for EVERY user address: swapping also takes code
       and stack pages, which belong to no promised area. The range
       check below concerns only pages that are yet to be created. */
    if (paging_swap_in(dir, va))
        return 1;

    /* A page of a memory-mapped file: the bytes come from disk only
       now, through the block layer, by the numbers fixed when the
       mapping was created. We do not step into the filesystem code
       here: it has one shared block buffer. */
    uint32_t first = 0, remaining = 0, write = 0;
    if (task_mmap_find(addr, &first, &remaining, &write)) {
        uint32_t frame = write ? page_frame_for_file(dir, va)
                               : page_frame_for_ro(dir, va);
        if (!frame)
            return 0;
        uint8_t *dst = (uint8_t *)frame;
        memset(dst, 0, PAGE_SIZE);
        for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK &&
                             remaining > b * FS_BLOCK; b++) {
            uint32_t blok = task_mmap_block(addr, first + b);
            if (!blok)
                break;
            blk_read(fs_block_lba(blok), FS_BLOCK / 512, dst + b * FS_BLOCK);
        }
        mmap_pages++;
        return 1;
    }

    int in_heap = addr >= task_brk_base() && addr < brk;
    int in_mmap = addr >= MMAP_BASE && addr < task_mmap_next();
    /* The stack grows down under the program's fingers - exactly as the
       heap grows up. Eight pages the program gets at the start, the
       rest arrives when it really touches them. */
    int in_stack = addr >= USER_STACK_BOTTOM && addr < USER_STACK_TOP;
    if (!in_heap && !in_mmap && !in_stack)
        return 0;

    return page_frame_for(dir, va) != 0;
}


/* --- fork: forking a process ----------------------------------------
 *
 * The child is not loaded afresh - it gets a clone of the parent's
 * address space (copy-on-write, see paging_clone_address_space) and
 * starts as a kernel thread that immediately returns to ring 3 with
 * the register frame remembered at the moment of the syscall. It
 * therefore returns at the same point of the program as the parent -
 * all that differs is the contents of eax: the parent gets the child's
 * pid, the child gets zero.
 */
static void fork_child(void *arg)
{
    struct regs *saved = arg;
    struct regs frame = *saved;       /* onto its own kernel stack */
    kfree(saved);
    /* We are entering user code - the big kernel lock stays here. The
       program may run in parallel with others; it will come back into
       the kernel through an interrupt or a system call and take the
       lock again there. */
    biglock_leave();
    enter_ring3_regs(&frame);
}

int process_fork(struct regs *r)
{
    if (!task_brk())                  /* a kernel thread has nothing to fork */
        return -1;
    if ((r->cs & 3) != 3)             /* fork called from outside ring 3 */
        return -1;

    struct regs *saved = kmalloc(sizeof(*saved));
    if (!saved)
        return -1;
    *saved = *r;
    saved->eax = 0;                   /* fork() returns zero in the child */

    uint32_t dir = paging_clone_address_space(task_current_cr3());
    if (!dir) {
        kfree(saved);
        return -1;
    }

    struct task *t = task_fork(fork_child, saved, dir);
    if (!t) {
        paging_destroy_address_space(dir);
        kfree(saved);
        return -1;
    }
    return task_id_of(t);
}

/* --- exec: replacing the current process image ----------------------
 *
 * The equivalent of Unix execv: the pid, the descriptors and the
 * working directory stay, only the program and the whole address space
 * are exchanged. Together with fork() this gives the classic pair -
 * fork, and in the child jump into a new program. On success it does
 * not return.
 */
int process_exec(const char *cmdline)
{
    if (!task_brk())                  /* a kernel thread has nothing to replace */
        return -1;

    char buf[CMD_MAX];
    uint32_t i = 0;
    while (cmdline[i] && i < sizeof(buf) - 1) {
        buf[i] = cmdline[i];
        i++;
    }
    buf[i] = '\0';

    char *tok[ARG_MAX];
    int argc = tokenize(buf, tok);
    if (!argc)
        return -1;

    uint32_t entry, esp, brk;
    uint32_t dir = build_image(tok, argc, &entry, &esp, &brk);
    if (!dir)
        return -1;                    /* the old image is still running */

    /* From here on there is no way back */
    /* We write back the old image's writable mappings while its pages
       are still there - in a moment the address space will be gone */
    process_msync();

    uint32_t old = task_current_cr3();
    task_rename(tok[0]);              /* 'ps' is to show the new program */
    task_set_brk(brk);
    task_set_brk_base(brk);
    /* The mapping descriptions spoke of an address space that will no
       longer exist, and their address ranges lie where the new image
       is about to set up its own - they have to go with the old image */
    task_mmap_release();
    task_set_mmap_next(MMAP_BASE);
    task_use_address_space(dir);

    /* The old space disappears, unless some thread of this process is
       still moving around in it (sys_thread) */
    if (!task_cr3_shared(old))
        paging_destroy_address_space(old);

    /* We are entering user code - the big kernel lock stays here. The
       program may run in parallel with others; it will come back into
       the kernel through an interrupt or a system call and take the
       lock again there. */
    biglock_leave();
    enter_ring3(entry, esp);       /* does not return */
    return 0;
}

/* sbrk: moves the end of the process heap, mapping new pages.
   Returns the old brk or -1. */
uint32_t process_sbrk(int32_t increment)
{
    uint32_t brk = task_brk();
    if (!brk)                       /* not a process (a kernel thread) */
        return (uint32_t)-1;
    if (increment == 0)
        return brk;
    if (increment < 0)              /* shrinking: not supported */
        return (uint32_t)-1;

    uint32_t new_brk = brk + (uint32_t)increment;
    if (new_brk < brk || new_brk > USER_BRK_MAX)
        return (uint32_t)-1;

    /* We allocate no frame here: the heap grows on paper, and the
       pages appear only on first touch (process_demand_page). A
       program that asks for 200 MiB and uses one will also occupy
       one. */
    task_set_brk(new_brk);
    return brk;
}

/* mmap: maps 'size' bytes of fresh (zeroed) process memory in the mmap
   area and returns its address or 0. It is released together with the
   address space at the end of the process. */
uint32_t process_mmap(uint32_t size)
{
    uint32_t base = task_mmap_next();
    if (!base || !size)             /* not a process, or a zero size */
        return 0;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t end = base + pages * PAGE_SIZE;
    if (end > MMAP_MAX || end < base)
        return 0;

    task_set_mmap_next(end);        /* pages as with sbrk - on demand */
    return base;
}

/* --- A memory-mapped file ------------------------------------------
 *
 * The program gets an address at which it sees the contents of a file.
 * No page appears at once: we remember the block numbers, and the
 * bytes themselves come from disk only on first touch.
 *
 * 'writable' decides what touching by writing means. Without it the
 * page has no write permission and the attempt ends in a protection
 * fault - better that than a change dying quietly. With it the page is
 * writable and its content goes back to the file at process_msync
 * (explicitly or when the process exits). When - is the program's
 * decision, not the kernel's: merely holding a dirty page costs
 * nothing, and writing it back after every byte would kill the whole
 * point of a mapping.
 */
uint32_t process_mmap_file(const char *name, uint32_t *size_out,
                           uint32_t write)
{
    if (!fs_ready())
        return 0;

    int f = fs_open(task_cwd(), name, 0);
    if (f < 0)
        return 0;
    int len = fs_size(f);
    if (len <= 0)
        return 0;

    uint32_t nblocks = ((uint32_t)len + FS_BLOCK - 1) / FS_BLOCK;
    uint32_t pages = ((uint32_t)len + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t base = task_mmap_next();
    uint32_t end = base + pages * PAGE_SIZE;
    if (!base || end > MMAP_MAX || end < base)
        return 0;

    uint32_t *blocks = kmalloc(nblocks * sizeof(uint32_t));
    if (!blocks)
        return 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        /* We fix the numbers NOW, at leisure - inside the page fault
           handler we may no longer step into the filesystem code */
        blocks[i] = fs_file_block(f, i, 0);
        if (!blocks[i]) {
            kfree(blocks);
            return 0;
        }
    }

    if (task_mmap_add(base, end, (uint32_t)len, blocks, nblocks, write) < 0) {
        kfree(blocks);
        return 0;
    }
    task_set_mmap_next(end);
    *size_out = (uint32_t)len;
    return base;
}

/* Writes back to their files the pages of the current process's
 * writable mappings.
 *
 * We walk pages, not blocks: the page is the unit the processor calls
 * "dirty", and only such pages are copied here. The rest of the walk
 * mirrors the filling done on a page fault - the same block numbers,
 * fixed when the mapping was created, only with blk_write instead of
 * blk_read.
 *
 * Returns the number of pages written back.
 */
uint32_t process_msync(void)
{
    if (!task_mmap_next())
        return 0;
    uint32_t dir = task_current_cr3();
    uint32_t sent_back_n = 0;

    for (int i = 0; i < MMAP_FILES; i++) {
        uint32_t start, end, size, write;
        if (!task_mmap_info(i, &start, &end, &size, &write) || !write)
            continue;
        for (uint32_t va = start; va < end; va += PAGE_SIZE) {
            if (!paging_file_dirty(dir, va))
                continue;
            uint32_t frame = paging_get_mapping(dir, va);
            if (!frame)
                continue;
            uint32_t offset = va - start;
            const uint8_t *src = (const uint8_t *)frame;
            for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK; b++) {
                /* Beyond the end of the file there is nothing to write back - the
                   block either does not exist or already belongs to
                   somebody else */
                if (offset + b * FS_BLOCK >= size)
                    break;
                uint32_t blok = task_mmap_block(va, offset / FS_BLOCK + b);
                if (!blok)
                    break;
                blk_write(fs_block_lba(blok), FS_BLOCK / 512,
                          src + b * FS_BLOCK);
            }
            paging_file_clean(dir, va);
            sent_back_n++;
        }
    }
    return sent_back_n;
}

/* Threads within a process: a new task sharing the address space,
   entering ring 3 at 'entry' with a stack of its own. */
struct thread_start {
    uint32_t entry;
    uint32_t arg;
    uint32_t ustack_top;
};

static void thread_trampoline(void *a)
{
    /* We are already running in the process's address space (the
       scheduler loaded its CR3). Let us lay out the user stack and
       enter ring 3. */
    struct thread_start ts = *(struct thread_start *)a;
    kfree(a);
    uint32_t *sp = (uint32_t *)ts.ustack_top;
    *--sp = ts.arg;                  /* the thread function's argument */
    *--sp = 0;                       /* a dummy return address */
    /* We are entering user code - the big kernel lock stays here. The
       program may run in parallel with others; it will come back into
       the kernel through an interrupt or a system call and take the
       lock again there. */
    biglock_leave();
    enter_ring3(ts.entry, (uint32_t)sp);
}

/* Creates a thread running in the current process's address space.
   Returns a tid or -1. */
int process_thread(uint32_t entry, uint32_t arg)
{
    if (!task_mmap_next())                  /* from a ring 3 process only */
        return -1;
    uint32_t stack = process_mmap(8192);    /* the thread stack in the process's space */
    if (!stack)
        return -1;

    struct thread_start *ts = kmalloc(sizeof(*ts));
    ts->entry = entry;
    ts->arg = arg;
    ts->ustack_top = stack + 8192;
    return task_id_of(task_create_thread("thread", thread_trampoline, ts,
                                         task_current_cr3()));
}

/* Maps a shared memory region (by key) into the current process's
   address space and returns its virtual address or 0. Different
   processes point at the same physical frames -> they see the same
   memory. */
uint32_t process_shm(int key, uint32_t size)
{
    uint32_t base = task_mmap_next();
    if (!base)
        return 0;
    uint32_t phys = shm_get(key, size);
    if (!phys)
        return 0;

    uint32_t pages = (size ? size : 4096);
    pages = (pages + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t end = base + pages * PAGE_SIZE;
    if (end > MMAP_MAX || end < base)
        return 0;

    uint32_t dir = task_current_cr3();
    for (uint32_t i = 0; i < pages; i++)
        if (!paging_map_user_in(dir, base + i * PAGE_SIZE,
                                phys + i * PAGE_SIZE))
            return 0;
    task_set_mmap_next(end);
    return base;
}

/* Builds exec_args for a given command line, copying the program name
   into prog[16]. Returns NULL when the program does not exist in the
   initrd. */
static struct exec_args *make_args(const char *cmdline, char *prog)
{
    uint32_t i = 0;
    while (*cmdline == ' ')
        cmdline++;
    while (cmdline[i] && cmdline[i] != ' ' && i < 15) {
        prog[i] = cmdline[i];
        i++;
    }
    prog[i] = '\0';

    if (!prog[0])
        return (void *)0;
    if (!prog_exists(prog)) {
        printk("run: nie ma '%s' ani w initrd, ani na dysku "
               "(sprawdz 'ls' i 'fls')\n", prog);
        return (void *)0;
    }

    struct exec_args *a = kmalloc(sizeof(*a));
    uint32_t n = 0;
    while (cmdline[n] && n < CMD_MAX - 1) {
        a->cmd[n] = cmdline[n];
        n++;
    }
    a->cmd[n] = '\0';
    a->in_pipe = -1;
    a->out_pipe = -1;
    a->out_file[0] = '\0';
    a->out_append = 0;
    a->anycore = 0;
    /* Whatever our own stdout is, the child gets the same - unless the
       caller says otherwise a line further on */
    a->out_inode = file_stdout_inode();
    return a;
}

static void set_outfile(struct exec_args *a, const char *outfile)
{
    if (!outfile || !outfile[0])
        return;
    uint32_t m = 0;
    while (outfile[m] && m < FS_NAME_MAX) {
        a->out_file[m] = outfile[m];
        m++;
    }
    a->out_file[m] = '\0';
}

int exec_spawn(const char *cmdline)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;
    return task_id_of(task_create(prog, exec_task, a));
}

/* The same, but EVERY core may run the process.
 *
 * Its kernel side stays ordinary kernel code under the big lock -
 * otherwise this would be a promise nobody could keep. What goes in
 * parallel is the computing in ring 3, and that is where the program
 * spends most of its time anyway. */
int exec_spawn_anycore(const char *cmdline)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;
    a->anycore = 1;
    return task_id_of(task_create(prog, exec_task, a));
}

int exec_spawn_redirect(const char *cmdline, const char *outfile, int append)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;
    set_outfile(a, outfile);
    a->out_append = append;
    return task_id_of(task_create(prog, exec_task, a));
}

/* Starts a program whose stdin is a pipe held by the caller.
 *
 * Everything about the program is ordinary: it reads descriptor zero
 * and knows nothing about where the bytes come from. What is new is
 * that the other end belongs to a PROGRAM rather than to another stage
 * of a pipeline - so a terminal can hand a running program what
 * somebody is typing, letter by letter or line by line.
 *
 * The caller becomes the pipe's writer. Closing it is how the program
 * is told there will be no more input; forgetting to close it leaves
 * the program waiting for a line that will never come. */
int exec_spawn_talk(const char *cmdline, const char *outfile, int *in_pipe)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;

    int idx = pipe_alloc();
    if (idx < 0) {
        kfree(a);
        return -1;
    }
    a->in_pipe = idx;
    set_outfile(a, outfile);

    int pid = task_id_of(task_create(prog, exec_task, a));
    if (pid < 0) {
        /* Nobody was created, so nobody will free the arguments or
           close the pipe - both are ours to undo */
        pipe_close_reader(idx);
        pipe_close_writer(idx);
        kfree(a);
        return -1;
    }
    if (in_pipe)
        *in_pipe = idx;
    return pid;
}

/* Runs a multi-stage pipeline 'a | b | c | ...': every stage's stdout
   flows into the next one's stdin. All stages run in parallel.
   'pipeline' is the whole line with '|' separators. When outfile is
   not NULL, the last stage's stdout also goes to the file. Returns the
   pid of the last stage or -1.

   We first prepare the arguments of ALL stages (validating that the
   programs exist) and only then create the tasks - so that an error in
   one stage does not leave half a pipeline running. */
int exec_spawn_pipeline(const char *pipeline, const char *outfile)
{
    char buf[CMD_MAX * MAX_STAGES];
    uint32_t bl = 0;
    while (pipeline[bl] && bl < sizeof(buf) - 1) {
        buf[bl] = pipeline[bl];
        bl++;
    }
    buf[bl] = '\0';

    /* Split into stages at '|' (trimming spaces) */
    char *seg[MAX_STAGES];
    int nseg = 0;
    char *start = buf;
    for (char *p = buf;; p++) {
        if (*p == '|' || *p == '\0') {
            int end = (*p == '\0');
            *p = '\0';
            while (*start == ' ')
                start++;
            char *e = start + strlen(start);
            while (e > start && e[-1] == ' ')
                *--e = '\0';
            if (nseg < MAX_STAGES)
                seg[nseg++] = start;
            start = p + 1;
            if (end)
                break;
        }
    }
    if (nseg < 2)
        return -1;

    /* Prepare the arguments of all stages (validating existence) */
    struct exec_args *args[MAX_STAGES];
    char prog[MAX_STAGES][16];
    for (int i = 0; i < nseg; i++) {
        args[i] = make_args(seg[i], prog[i]);
        if (!args[i]) {
            for (int j = 0; j < i; j++)
                kfree(args[j]);
            return -1;
        }
    }

    /* Pipes between neighbouring stages: pipe[i] joins stage i to i+1 */
    for (int i = 0; i + 1 < nseg; i++) {
        int pidx = pipe_alloc();
        if (pidx < 0) {
            printk("run: brak wolnego potoku\n");
            for (int j = 0; j < nseg; j++)
                kfree(args[j]);
            /* release the pipes already allocated by closing both ends */
            for (int j = 0; j < i; j++) {
                pipe_close_reader(args[j + 1]->in_pipe);
                pipe_close_writer(args[j]->out_pipe);
            }
            return -1;
        }
        args[i]->out_pipe = pidx;
        args[i + 1]->in_pipe = pidx;
    }

    set_outfile(args[nseg - 1], outfile);  /* the last one's stdout -> a file */

    int last_pid = -1;
    for (int i = 0; i < nseg; i++)
        last_pid = task_id_of(task_create(prog[i], exec_task, args[i]));
    return last_pid;
}
