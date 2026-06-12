/* Wejście w tryb użytkownika (ring 3)
 *
 * "Powrót" na niższy poziom uprawnień przez iret ze spreparowaną
 * ramką: ss, esp, eflags (IF=1), cs, eip. Segmenty danych trzeba
 * załadować ręcznie - iret ich nie rusza.
 */

#include "include/usermode.h"
#include "include/gdt.h"

#define USER_CS (GDT_USER_CODE | 3)   /* selektory z RPL=3 */
#define USER_DS (GDT_USER_DATA | 3)

void enter_ring3(uint32_t entry, uint32_t user_stack_top)
{
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
