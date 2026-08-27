/* Tablica deskryptorow przerwan w trybie dlugim
 *
 * Wpis ma szesnascie bajtow zamiast osmiu - adres procedury jest
 * 64-bitowy i musi sie w nim zmiescic w trzech kawalkach. Doszlo tez
 * pole IST: pozwala kazdemu wektorowi zazadac wlasnego, z gory
 * wybranego stosu. Nie korzystamy z niego jeszcze (zero = ten sam stos,
 * co przerwany kod), ale to wlasnie ono ratuje system przy bledzie
 * stosu, gdy dotychczasowemu nie mozna juz ufac.
 */

#include <stdint.h>
#include "../kernel/include/isr.h"
#include "idt64.h"

#define IDT_ENTRIES 256
#define KERNEL_CS   0x08
/* Obecny, ring 0, bramka przerwania (gasi przerwania na czas obslugi) */
#define GATE_INTERRUPT 0x8E

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;                /* 0 = zwykly stos */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

static void set_gate(int n, void (*handler)(void))
{
    uint64_t addr = (uint64_t)(uintptr_t)handler;
    idt[n].offset_low = (uint16_t)addr;
    idt[n].selector = KERNEL_CS;
    idt[n].ist = 0;
    idt[n].type_attr = GATE_INTERRUPT;
    idt[n].offset_mid = (uint16_t)(addr >> 16);
    idt[n].offset_high = (uint32_t)(addr >> 32);
    idt[n].zero = 0;
}

/* Stuby z boot64/isr64.S - wyjatki 0-31 i przerwania sprzetowe 32-47 */
#define DECL(n) extern void isr64_##n(void);
#define LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)

LIST(DECL)

#define ENTRY(n) set_gate(n, isr64_##n);

void idt64_init(void)
{
    LIST(ENTRY)

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idtp));
}
