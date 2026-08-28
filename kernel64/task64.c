/* Watki jadra w trybie dlugim
 *
 * Lista jest cykliczna, a watek glowny - ten, ktory przyszedl prosto
 * z rozruchu - nigdy nie spi i pelni role watku bezczynnosci. Dzieki
 * temu scheduler zawsze ma kogo wybrac i nie musi znac przypadku
 * "nie ma nikogo gotowego".
 *
 * Zakonczone watki zostaja na liscie jako zombie i znikaja przy
 * kolejnym przelaczeniu - nigdy nie sprzatamy tego, na ktorego stosie
 * wlasnie stoimy.
 */

#include <stddef.h>
#include <stdint.h>
#include "task64.h"
#include "paging64.h"
#include "syscall64.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/io.h"

#define STACK_SIZE 16384
#define NAME_MAX   16

enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_ZOMBIE };

struct task64 {
    uint64_t rsp;                /* wierzcholek stosu, gdy watek nie biegnie */
    void *stack;                 /* NULL dla watku glownego - ma stos z rozruchu */
    void (*entry)(void *);
    void *arg;
    enum task_state state;
    uint32_t wake_at;
    uint64_t space;              /* wlasna przestrzen adresowa (0 = jadra) */
    uint64_t kstack_top;         /* stos jadra dla wejsc z ring 3 */
    int id;
    char name[NAME_MAX];
    struct task64 *next;
};

extern void context_switch64(uint64_t *old_rsp, uint64_t new_rsp);

static struct task64 *current;
static struct task64 *idle_task;
static int next_id;

static void set_name(struct task64 *t, const char *name)
{
    int i = 0;
    for (; name[i] && i < NAME_MAX - 1; i++)
        t->name[i] = name[i];
    t->name[i] = '\0';
}

/* Pierwszy kod kazdego nowego watku - context_switch64 "wraca" tutaj */
static void trampoline(void)
{
    __asm__ volatile("sti");
    current->entry(current->arg);
    task64_exit();
}

/* Watek bezczynnosci: nie robi nic i nigdy nie spi.
 *
 * Jest po to, zeby scheduler zawsze mial kogo wybrac. Bez niego
 * musialby umiec przypadek "nikt nie jest gotowy" - a jedyne, co
 * moglby wtedy zrobic, to czekac na przerwanie w srodku wlasnego
 * wywolania. Gdyby to wywolanie przyszlo z obslugi przerwania timera,
 * kolejne tykniecie wchodziloby w nie samo, i tak bez konca. */
static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile("hlt");
}

void scheduler64_init(void)
{
    struct task64 *main_task = kmalloc(sizeof(*main_task));
    main_task->stack = NULL;
    main_task->state = TASK_RUNNING;
    main_task->id = next_id++;
    main_task->wake_at = 0;
    main_task->space = 0;
    main_task->kstack_top = 0;
    set_name(main_task, "main");
    main_task->next = main_task;
    current = main_task;

    idle_task = task64_create("bezczynny", idle_loop, 0);
}

struct task64 *task64_create(const char *name, void (*entry)(void *),
                             void *arg)
{
    struct task64 *t = kmalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->stack = kmalloc(STACK_SIZE);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }
    t->entry = entry;
    t->arg = arg;
    t->state = TASK_READY;
    t->wake_at = 0;
    t->space = 0;
    t->kstack_top = 0;
    t->id = next_id++;
    set_name(t, name);

    /* Stos ukladamy tak, by context_switch64 zdjal z niego szesc zer
       i "wrocil" do trampoliny. Jedno slowo zapasu na gorze pilnuje
       wyrownania stosu, ktorego wymaga konwencja wywolan. */
    uint64_t *sp = (uint64_t *)((uint8_t *)t->stack + STACK_SIZE);
    *--sp = 0;                            /* wyrownanie do 16 bajtow */
    *--sp = (uint64_t)(uintptr_t)trampoline;   /* adres powrotu */
    for (int i = 0; i < 6; i++)
        *--sp = 0;                        /* rbp, rbx, r12-r15 */
    t->rsp = (uint64_t)(uintptr_t)sp;

    irqflags_t flags = irq_save();
    t->next = current->next;
    current->next = t;
    irq_restore(flags);
    return t;
}

/* Sprzata zakonczone watki - nigdy tego, na ktorego stosie stoimy */
static void reap_zombies(void)
{
    struct task64 *t = current;
    while (t->next != current) {
        if (t->next->state == TASK_ZOMBIE) {
            struct task64 *z = t->next;
            t->next = z->next;
            kfree(z->stack);
            kfree(z);
        } else {
            t = t->next;
        }
    }
}

/* Obchodzi liste, budzac tych, ktorym minal czas snu, i zwraca
   pierwszego gotowego poza biezacym. Watek bezczynnosci bierzemy
   dopiero wtedy, gdy nie ma nikogo innego - inaczej zjadalby polowe
   czasu procesora, nic nie robiac. */
static struct task64 *pick_ready(void)
{
    uint32_t now = pit_ticks();
    struct task64 *t = current->next;
    struct task64 *found = NULL;

    while (t != current) {
        if (t->state == TASK_SLEEPING && now >= t->wake_at)
            t->state = TASK_READY;
        if (t->state == TASK_READY && !found && t != idle_task)
            found = t;
        t = t->next;
    }
    if (!found && idle_task && idle_task != current &&
        idle_task->state == TASK_READY)
        found = idle_task;
    return found;
}

/* Ustawia procesorowi to, co nalezy do wybranego zadania: jego
   przestrzen adresowa i stos, na ktory ma wejsc jadro przy przerwaniu
   albo wywolaniu systemowym z ring 3.
 *
 * CR3 przeladowujemy tylko wtedy, gdy naprawde sie zmienia - kazde
 * przeladowanie wyrzuca caly bufor tlumaczen adresow, wiec robienie
 * tego przy przelaczaniu miedzy watkami jadra bylo by czysta strata. */
static void apply_context(struct task64 *t)
{
    uint64_t want = t->space ? t->space : paging64_kernel_space();
    if (paging64_current_space() != want)
        paging64_switch(want);
    if (t->kstack_top)
        syscall64_set_kernel_stack(t->kstack_top);
}

/* Oddaje przestrzen zadania i wraca do przestrzeni jadra.
   Wolane, gdy zadanie umiera - a umiera, stojac jeszcze na wlasnym
   CR3, wiec najpierw trzeba z niego zejsc. */
static void drop_space(struct task64 *t)
{
    if (!t->space)
        return;
    uint64_t doomed = t->space;
    t->space = 0;
    t->kstack_top = 0;
    paging64_switch(paging64_kernel_space());
    paging64_free_space(doomed);
}

/* Bierze przestrzen na wlasnosc i od razu na nia przechodzi.
   Wolane przez watek, ktory wlasnie zaladowal program. */
void task64_enter_space(uint64_t space, uint64_t kstack_top)
{
    irqflags_t flags = irq_save();
    current->space = space;
    current->kstack_top = kstack_top;
    apply_context(current);
    irq_restore(flags);
}

/* Wybiera nastepny watek i przelacza kontekst. Wolac z IF=0. */
void schedule(void)
{
    if (!current)
        return;

    reap_zombies();

    struct task64 *next = pick_ready();

    /* Nie ma nikogo innego - zostajemy, o ile sami mozemy biec dalej.
       Watek bezczynnosci sprawia, ze przypadek "nie ma nikogo, a my nie
       mozemy" po prostu nie wystepuje. */
    if (!next)
        next = current;

    if (next == current) {
        current->state = TASK_RUNNING;
        return;
    }

    struct task64 *prev = current;
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current = next;

    apply_context(next);
    context_switch64(&prev->rsp, next->rsp);
}

void task64_yield(void)
{
    irqflags_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void task64_sleep(uint32_t ms)
{
    irqflags_t flags = irq_save();
    current->wake_at = pit_ticks() + (ms + 9) / 10;   /* tykniecie = 10 ms */
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(flags);
}

void task64_exit(void)
{
    irq_save();
    drop_space(current);
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;)                              /* zombie nie zostanie wybrany */
        __asm__ volatile("hlt");
}

/* Zabija biezace zadanie z podanym powodem. Nie wraca.
 *
 * Wolane z obslugi wyjatku, gdy zawinil program w ring 3. Roznica
 * wzgledem task64_exit jest tylko taka, ze tu jest co powiedziec:
 * zadanie nie skonczylo sie samo, tylko zostalo zatrzymane. */
void task64_kill_current(const char *why)
{
    irq_save();
    printk("[jadro] zadanie %d (%s) zabite: %s\n",
           current->id, current->name, why);
    drop_space(current);
    current->state = TASK_ZOMBIE;
    schedule();
    for (;;)
        __asm__ volatile("hlt");
}

int task64_id(const struct task64 *t)
{
    return t ? t->id : -1;
}

int task64_count(void)
{
    irqflags_t flags = irq_save();
    int n = 0;
    struct task64 *t = current;
    do {
        n++;
        t = t->next;
    } while (t != current);
    irq_restore(flags);
    return n;
}

void task64_dump(void)
{
    static const char *state_names[] = { "gotowy", "dziala", "spi", "zombie" };
    irqflags_t flags = irq_save();
    printk("  ID  STAN    NAZWA\n");
    struct task64 *t = current;
    do {
        printk("  %d   %s  %s%s\n", t->id, state_names[t->state], t->name,
               t == current ? " (biezacy)" : "");
        t = t->next;
    } while (t != current);
    irq_restore(flags);
}
