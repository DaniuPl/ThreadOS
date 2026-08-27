/* Sygnaly
 *
 * Sygnal to najprostsza forma zdarzenia asynchronicznego: jeden bit
 * w masce oczekujacych. Doreczamy go zawsze w tym samym, bezpiecznym
 * miejscu - przy powrocie do ring 3, gdy na stosie jadra lezy pelna
 * ramka rejestrow procesu.
 *
 * Gdy proces ma wlasna procedure obslugi, jadro odklada te ramke na
 * jego wlasny stos, dokladajac numer sygnalu jako argument i adres
 * trampoliny jako adres powrotu. Handler wykonuje sie wiec w ring 3,
 * na wlasnym stosie procesu. Kiedy wraca, trampolina wola sigreturn,
 * a jadro odtwarza z tej ramki przerwany stan - proces wraca dokladnie
 * tam, gdzie sygnal go zastal.
 *
 * Do procesu nie wraca nic, czego nie zapisalismy sami: przy odtwarzaniu
 * bierzemy z ramki wylacznie rejestry ogolne, eip, esp i flagi
 * arytmetyczne. Selektory segmentow i IOPL zostaja jadrowe, inaczej
 * proces podstawilby sobie w sigreturn wlasny stan uprzywilejowany.
 */

#include <stdint.h>
#include "include/signal.h"
#include "include/isr.h"
#include "include/task.h"
#include "include/paging.h"
#include "include/printk.h"
#include "include/string.h"

#define USER_TOP 0xC0000000u

/* Flagi eflags, ktore proces moze sobie przywrocic: same arytmetyczne
   plus kierunek. Bez TF (pulapka krokowa), bez IOPL, bez NT. */
#define EFLAGS_USER_MASK 0xCD5u

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGINT:  return "SIGINT";
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGCHLD: return "SIGCHLD";
    default:      return "sygnal";
    }
}

/* Czy caly zakres lezy w zamapowanej pamieci uzytkownika? */
static int user_range_ok(uint32_t addr, uint32_t len)
{
    if (!addr || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;
    uint32_t cr3 = task_current_cr3();
    for (uint32_t page = addr & ~0xFFFu; page < addr + len; page += 0x1000)
        if (!paging_get_mapping(cr3, page))
            return 0;
    return 1;
}

/* Domyslna reakcja jadra na sygnal bez procedury obslugi */
static void default_action(int sig)
{
    if (sig == SIGCHLD)
        return;                     /* domyslnie nikogo nie obchodzi */
    printk("[jadro] proces '%s' (pid %d) zakonczony przez %s\n",
           task_current_name(), task_current_id(), signal_name(sig));
    task_exit(-sig);
}

void signal_deliver(struct regs *r)
{
    if ((r->cs & 3) != 3)               /* tylko przy powrocie do ring 3 */
        return;
    if (task_signal_in_handler())       /* nie zagniezdzamy procedur obslugi */
        return;

    int sig = task_signal_take();
    if (!sig)
        return;

    uint32_t handler = task_signal_handler(sig);
    if (handler == SIG_IGN)
        return;
    if (handler == SIG_DFL) {
        default_action(sig);
        return;
    }

    /* Ramka przerwanego stanu i dwa slowa wywolania handlera */
    uint32_t frame = (r->useresp - sizeof(struct regs)) & ~3u;
    uint32_t sp = frame - 8;
    if (!user_range_ok(sp, sizeof(struct regs) + 8)) {
        printk("[jadro] proces '%s': brak miejsca na stosie na %s\n",
               task_current_name(), signal_name(sig));
        task_exit(-sig);
        return;
    }

    memcpy((void *)frame, r, sizeof(struct regs));
    ((uint32_t *)sp)[1] = (uint32_t)sig;          /* argument handlera */
    ((uint32_t *)sp)[0] = task_signal_tramp();    /* adres powrotu */

    r->useresp = sp;
    r->eip = handler;
    r->eflags |= 0x200;                           /* handler chodzi z IF=1 */
    task_signal_enter(frame);
}

void signal_return(struct regs *r)
{
    uint32_t frame = task_signal_leave();
    if (!frame || !user_range_ok(frame, sizeof(struct regs))) {
        printk("[jadro] proces '%s': sigreturn bez wlasnej ramki\n",
               task_current_name());
        task_exit(-1);
        return;
    }

    struct regs saved;
    memcpy(&saved, (const void *)frame, sizeof(saved));

    r->edi = saved.edi;
    r->esi = saved.esi;
    r->ebp = saved.ebp;
    r->ebx = saved.ebx;
    r->edx = saved.edx;
    r->ecx = saved.ecx;
    r->eax = saved.eax;
    r->eip = saved.eip;
    r->useresp = saved.useresp;
    r->eflags = (saved.eflags & EFLAGS_USER_MASK) | 0x202;
}
