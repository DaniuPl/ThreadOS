/* Wejście w tryb użytkownika (ring 3)
 *
 * "Powrót" na niższy poziom uprawnień przez iret ze spreparowaną
 * ramką: ss, esp, eflags (IF=1), cs, eip. Segmenty danych trzeba
 * załadować ręcznie - iret ich nie rusza.
 */

#include "include/usermode.h"
#include "include/gdt.h"
#include "include/isr.h"

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

/* Powrót do ring 3 z zapisanej ramki rejestrów.
 *
 * Używa tego fork(): dziecko jest osobnym wątkiem jądra, który od razu
 * "wraca z syscalla" w tym samym miejscu programu co rodzic. Ramkę
 * układamy w kolejności, jakiej oczekuje popal, i dokładamy na koniec
 * to, co zdejmuje iret (eip, cs, eflags, esp, ss).
 */
void enter_ring3_regs(const struct regs *r)
{
    volatile uint32_t f[13];
    f[0] = r->edi;  f[1] = r->esi;  f[2] = r->ebp;  f[3] = 0; /* esp - popal je pomija */
    f[4] = r->ebx;  f[5] = r->edx;  f[6] = r->ecx;  f[7] = r->eax;
    f[8] = r->eip;  f[9] = r->cs;   f[10] = r->eflags | 0x200; /* IF=1 */
    f[11] = r->useresp;
    f[12] = r->ss;

    __asm__ volatile(
        "cli\n\t"
        "movw %w1, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movl %0, %%esp\n\t"
        "popal\n\t"
        "iret\n\t"
        :
        : "r"(&f[0]), "r"((uint32_t)USER_DS)
        : "eax", "memory");
    __builtin_unreachable();
}
