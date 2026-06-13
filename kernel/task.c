/* Wątki jądra i scheduler (round-robin z wywłaszczaniem)
 *
 * Każdy wątek ma własny stos 8 KiB. Przełączenie kontekstu (switch.S)
 * zapisuje rejestry callee-saved i ESP - resztę stanu trzyma stos:
 * wątek wywłaszczony przerwaniem ma na stosie pełną ramkę przerwania
 * i wznowi się przez iret, wątek po task_sleep/yield wróci ze
 * zwykłego wywołania schedule().
 *
 * Lista wątków jest cykliczna; "main" (dawne kmain) nigdy nie śpi
 * i pełni rolę wątku bezczynności (hlt), więc scheduler zawsze ma
 * kogo wybrać. Zakończone wątki (zombie) są sprzątane przy kolejnych
 * wywołaniach schedule() - nigdy nie sprzątamy bieżącego.
 */

#include <stddef.h>
#include "include/task.h"
#include "include/heap.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"
#include "include/gdt.h"
#include "include/paging.h"

#define STACK_SIZE 8192
#define NAME_MAX   16

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED,
                  TASK_ZOMBIE };

struct task {
    uint32_t esp;
    uint32_t kstack_top;        /* dla TSS.esp0 (przerwania z ring 3) */
    uint32_t cr3;               /* przestrzeń adresowa zadania */
    int owns_dir;               /* czy katalog stron zwolnić przy reap */
    uint32_t brk;               /* koniec sterty procesu (sbrk); 0 = wątek jądra */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;           /* tyknięcie, przy którym obudzić */
    int id;
    char name[NAME_MAX];
    void *stack;                /* NULL dla wątku main (stos z boot.S) */
    struct file_desc fds[TASK_MAX_FD];
    struct task *next;
};

/* Stos rozruchowy z boot.S - wątek main kontynuuje na nim */
extern char stack_top[];

extern void context_switch(uint32_t *old_esp_store, uint32_t new_esp);

static struct task *current;
static int next_id;

static void task_set_name(struct task *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* Standardowe strumienie: 0=klawiatura (read), 1/2=konsola (write) */
static void task_init_fds(struct task *t)
{
    memset(t->fds, 0, sizeof(t->fds));
    t->fds[0].used = 1; t->fds[0].readable = 1;   /* stdin  */
    t->fds[1].used = 1; t->fds[1].writable = 1;   /* stdout */
    t->fds[2].used = 1; t->fds[2].writable = 1;   /* stderr */
}

struct file_desc *task_fds(void)
{
    return current->fds;
}

/* Pierwszy kod każdego nowego wątku - context_switch "wraca" tutaj */
static void task_trampoline(void)
{
    __asm__ volatile("sti");
    current->entry(current->arg);
    task_exit();
}

void scheduler_init(void)
{
    struct task *main_task = kmalloc(sizeof(*main_task));
    main_task->id = next_id++;
    task_set_name(main_task, "main");
    main_task->state = TASK_RUNNING;
    main_task->stack = NULL;
    main_task->kstack_top = (uint32_t)stack_top;
    main_task->cr3 = paging_kernel_cr3();
    main_task->owns_dir = 0;
    main_task->brk = 0;
    task_init_fds(main_task);
    main_task->next = main_task;
    current = main_task;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct task *t = kmalloc(sizeof(*t));
    t->stack = kmalloc(STACK_SIZE);
    t->kstack_top = (uint32_t)t->stack + STACK_SIZE;
    t->cr3 = paging_kernel_cr3();
    t->owns_dir = 0;
    t->brk = 0;
    task_init_fds(t);
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->id = next_id++;
    task_set_name(t, name);

    /* Spreparuj stos tak, by context_switch zdjął zerowe rejestry
       i "wrócił" do trampoliny */
    uint32_t *sp = (uint32_t *)((uint32_t)t->stack + STACK_SIZE);
    *--sp = (uint32_t)task_trampoline;  /* adres powrotu z context_switch */
    *--sp = 0;                          /* ebp */
    *--sp = 0;                          /* ebx */
    *--sp = 0;                          /* esi */
    *--sp = 0;                          /* edi */
    t->esp = (uint32_t)sp;

    uint32_t flags = irq_save();
    t->next = current->next;
    current->next = t;
    irq_restore(flags);
    return t;
}

static void reap_zombies(void)
{
    struct task *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task *z = t->next;
            t->next = z->next;
            /* CR3 zombie nie jest juz zaladowany - bezpiecznie zwolnic */
            if (z->owns_dir)
                paging_destroy_address_space(z->cr3);
            kfree(z->stack);
            kfree(z);
        } else {
            t = t->next;
        }
    }
}

/* Budzi zaspane wątki i zwraca pierwszy gotowy po current (lub NULL) */
static struct task *pick_next(void)
{
    uint32_t now = pit_ticks();
    struct task *found = NULL;

    struct task *t = current->next;
    do {
        if (t->state == TASK_SLEEPING && (int32_t)(now - t->wake_at) >= 0)
            t->state = TASK_READY;
        if (!found && t->state == TASK_READY && t != current)
            found = t;
        t = t->next;
    } while (t != current->next);

    return found;
}

void schedule(void)
{
    if (!current)   /* timer tyka, zanim scheduler wystartuje */
        return;

    reap_zombies();

    struct task *next = pick_next();
    if (!next)      /* nikt inny nie jest gotowy - bież dalej */
        return;

    struct task *prev = current;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current = next;
    /* Przerwanie z ring 3 tego wątku ma wylądować na jego stosie jądra */
    tss_set_esp0(next->kstack_top);
    /* Zmiana przestrzeni adresowej tylko, gdy trzeba (przeladowanie
       CR3 oproznia TLB - szkoda cykli miedzy watkami jadra) */
    if (next->cr3 != prev->cr3)
        __asm__ volatile("movl %0, %%cr3" : : "r"(next->cr3) : "memory");
    context_switch(&prev->esp, next->esp);
}

void task_yield(void)
{
    uint32_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task_sleep(uint32_t ms)
{
    uint32_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;  /* tyknięcie = 10 ms */
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(flags);
}

/* Blokowanie bez terminu - ktoś musi obudzić przez task_unblock */
void task_block(void)
{
    uint32_t flags = irq_save();
    current->state = TASK_BLOCKED;
    schedule();
    irq_restore(flags);
}

/* Wołane z IF=0 (kontekst IRQ albo sekcja krytyczna) */
void task_unblock(struct task *t)
{
    if (t->state == TASK_BLOCKED)
        t->state = TASK_READY;
}

struct task *task_current(void)
{
    return current;
}

int task_id_of(struct task *t)
{
    return t->id;
}

int task_current_id(void)
{
    return current->id;
}

int task_alive(int id)
{
    uint32_t flags = irq_save();
    int alive = 0;
    struct task *t = current;
    do {
        if (t->id == id && t->state != TASK_ZOMBIE) {
            alive = 1;
            break;
        }
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return alive;
}

void task_exit(void)
{
    irq_save();
    current->state = TASK_ZOMBIE;
    schedule();
    /* zombie nigdy nie zostanie wybrany ponownie */
    for (;;)
        __asm__ volatile("hlt");
}

void task_use_address_space(uint32_t dir_phys)
{
    uint32_t flags = irq_save();
    current->cr3 = dir_phys;
    current->owns_dir = 1;
    __asm__ volatile("movl %0, %%cr3" : : "r"(dir_phys) : "memory");
    irq_restore(flags);
}

uint32_t task_current_cr3(void)
{
    return current->cr3;
}

uint32_t task_brk(void)
{
    return current->brk;
}

void task_set_brk(uint32_t brk)
{
    current->brk = brk;
}

const char *task_current_name(void)
{
    return current ? current->name : "(brak)";
}

void task_dump(void)
{
    static const char *state_names[] = { "gotowy", "dziala", "spi",
                                         "czeka", "zombie" };

    uint32_t flags = irq_save();
    printk("  ID  STAN    NAZWA\n");
    struct task *t = current;
    do {
        printk("  %d   %s", t->id, state_names[t->state]);
        for (int pad = 8 - (int)strlen(state_names[t->state]); pad > 0; pad--)
            printk(" ");
        printk("%s%s\n", t->name, t == current ? " (biezacy)" : "");
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}
