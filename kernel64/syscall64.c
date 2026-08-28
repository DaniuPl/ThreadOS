/* Obsluga wywolan systemowych w trybie dlugim
 *
 * Instrukcje syscall/sysret trzeba najpierw wlaczyc i powiedziec
 * procesorowi trzy rzeczy:
 *
 *   EFER.SCE - zgoda na uzywanie tych instrukcji w ogole,
 *   STAR     - z jakich selektorow skladac segmenty przy wejsciu
 *              i przy powrocie,
 *   LSTAR    - pod jaki adres skakac,
 *   SFMASK   - ktore flagi zgasic po wejsciu; gasimy IF, zeby jadro
 *              zaczynalo prace bez przerwan, tak jak przy bramce.
 */

#include <stdint.h>
#include "syscall64.h"
#include "gdt64.h"
#include "task64.h"
#include "paging64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pit.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

#define EFER_SCE   (1u << 0)
#define RFLAGS_IF  0x200

extern void syscall64_entry(void);
extern uint64_t syscall64_kernel_rsp;

static int user_ran;
static uint32_t user_cs;
static int user_exited;
static int user_status;

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr"
                     : : "c"(msr), "a"((uint32_t)value),
                         "d"((uint32_t)(value >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall64_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* Bity 47-32: selektor kodu jadra przy wejsciu.
       Bity 63-48: podstawa, z ktorej sysret sklada selektory programu -
       dane to podstawa + 8, kod to podstawa + 16. Dlatego wpisujemy
       tu selektor DANYCH jadra, a nie kodu uzytkownika. */
    wrmsr(MSR_STAR, ((uint64_t)GDT64_KERNEL_DATA << 48) |
                    ((uint64_t)GDT64_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall64_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF);
}

void syscall64_set_kernel_stack(uint64_t rsp)
{
    syscall64_kernel_rsp = rsp;
    gdt64_set_kernel_stack(rsp);   /* to samo dla przerwan z ring 3 */
}

int syscall64_user_ran(void)
{
    return user_ran;
}

uint32_t syscall64_user_cs(void)
{
    return user_cs;
}

int syscall64_user_exited(void)
{
    return user_exited;
}

int syscall64_user_status(void)
{
    return user_status;
}

void syscall64_forget_user(void)
{
    user_ran = 0;
    user_cs = 0;
    user_exited = 0;
    user_status = 0;
}

/* Wolane ze stubu w boot64/syscall64.S.
 *
 * Numer wywolania jest w RAX, argumenty w RDI i RSI, wynik wraca
 * w RAX. Wskaznikom z ring 3 nie wolno wierzyc na slowo: zanim jadro
 * cokolwiek z nich przeczyta, pyta tablice stron, czy program w ogole
 * ma prawo tam siegac. Bez tego wystarczyloby podac adres pamieci
 * jadra, zeby kazac mu ja sobie wypisac. */
uint64_t syscall64_handler(uint64_t nr, uint64_t a1, uint64_t a2)
{
    switch (nr) {
    case SYS64_WRITE: {
        const char *buf = (const char *)(uintptr_t)a1;
        if (a2 > 4096)
            a2 = 4096;               /* najpierw ograniczenie, potem
                                        sprawdzanie - inaczej dlugosc
                                        rzedu miliardow stron zajelaby
                                        jadro na dobre */
        if (!paging64_user_range(a1, a2)) {
            printk("[jadro] write: wskaznik %x nie nalezy do programu\n",
                   (uint32_t)a1);
            return (uint64_t)-1;
        }
        for (uint64_t i = 0; i < a2; i++)
            printk("%c", buf[i]);
        return a2;
    }
    case SYS64_EXIT:
        user_status = (int)a1;
        user_exited = 1;
        task64_exit();               /* nie wraca - scheduler idzie dalej */
        return 0;
    case SYS64_RING:                 /* program mowi, w ktorym pierscieniu jest */
        user_cs = (uint32_t)a1;
        user_ran = 1;
        return 0;
    case SYS64_SLEEP:
        task64_sleep((uint32_t)a1);
        return 0;
    case SYS64_TICKS:
        return pit_ticks();
    default:
        printk("[jadro] nieznane wywolanie systemowe %u\n", (uint32_t)nr);
        return (uint64_t)-1;
    }
}
