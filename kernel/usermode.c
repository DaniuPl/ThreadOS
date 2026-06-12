/* Wejście w tryb użytkownika (ring 3)
 *
 * Program użytkownika jest wkompilowany w obraz jądra w osobnej,
 * wyrównanej do stron sekcji .user (zob. linker.ld i userprog.c).
 * Przed startem nadajemy jego stronom bit USER, przydzielamy ramkę
 * na stos użytkownika i "wracamy" do ring 3 przez iret ze spreparowaną
 * ramką. Cała reszta jądra zostaje niedostępna - próba dostępu
 * z ring 3 kończy się page faultem i ubiciem procesu.
 */

#include <stdint.h>
#include "include/usermode.h"
#include "include/task.h"
#include "include/paging.h"
#include "include/pmm.h"
#include "include/gdt.h"
#include "include/printk.h"

/* Granice sekcji .user z linker.ld i wejście programu z userprog.c */
extern char _user_start[], _user_end[];
extern void user_main(void);

#define USER_CS (GDT_USER_CODE | 3)   /* selektory z RPL=3 */
#define USER_DS (GDT_USER_DATA | 3)

static void enter_ring3(uint32_t entry, uint32_t user_stack_top)
{
    /* Ramka iret dla powrotu na nizszy poziom uprawnien:
       ss, esp, eflags (IF=1), cs, eip. Segmenty danych trzeba
       zaladowac recznie - iret ich nie ruszy. */
    __asm__ volatile(
        "cli\n\t"
        "movw %w0, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushl %0\n\t"
        "pushl %1\n\t"
        "pushl $0x202\n\t"
        "pushl %2\n\t"
        "pushl %3\n\t"
        "iret\n\t"
        :
        : "r"((uint32_t)USER_DS), "r"(user_stack_top),
          "r"((uint32_t)USER_CS), "r"(entry)
        : "eax", "memory");
    __builtin_unreachable();
}

/* Watek jadra, ktory zamienia sie w proces uzytkownika */
static void user_task_entry(void *arg)
{
    (void)arg;

    paging_make_user((uint32_t)_user_start,
                     (uint32_t)(_user_end - _user_start));

    uint32_t stack_frame = pmm_alloc_frame();
    if (!stack_frame) {
        printk("[jadro] brak ramki na stos uzytkownika\n");
        return;
    }
    paging_make_user(stack_frame, PAGE_SIZE);

    printk("[jadro] start procesu uzytkownika (kod %p-%p, stos %x)\n",
           (void *)_user_start, (void *)_user_end, stack_frame);

    enter_ring3((uint32_t)user_main, stack_frame + PAGE_SIZE);
}

void usermode_demo(void)
{
    task_create("user-demo", user_task_entry, (void *)0);
}
