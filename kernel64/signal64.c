/* Sygnaly w trybie dlugim
 *
 * Sygnal nie jest wiadomoscia do przeczytania, tylko przerwaniem toku
 * programu. Jadro nie moze go dostarczyc w chwili wyslania: adresat
 * jest wtedy gdzie indziej - moze spac, moze siedziec w srodku
 * wywolania systemowego, moze w ogole nie biec. Sygnal czeka wiec przy
 * zadaniu do NAJBLIZSZEGO powrotu do pierscienia 3.
 *
 * Sam powrot znowu sprowadza sie do ramki. Dostarczenie sygnalu to:
 * odlozyc ramke programu na jego wlasnym stosie, przestawic ja na
 * procedure obslugi i wsunac pod nia adres powrotu, ktory wywola
 * jadro z powrotem. Program po drugiej stronie nie ma jak zauwazyc,
 * ze cokolwiek go przerwalo - a po zakonczeniu procedury wraca
 * dokladnie tam, gdzie mu przerwano, co do rejestru.
 */

#include <stdint.h>
#include "signal64.h"
#include "task64.h"
#include "paging64.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"

/* Flagi, ktore program moze sobie sam ustawic. Reszty - w tym zgody na
   przerwania i poziomu dostepu do portow - nie oddajemy nigdy: ramka
   wraca ze stosu uzytkownika, wiec program moglby ja podmienic. */
#define RFLAGS_USER_MASK 0x0CD5
#define RFLAGS_ALWAYS    0x0202          /* bit staly + przerwania wlaczone */

void signal64_deliver(struct regs *r)
{
    int sig = task64_take_signal();
    if (!sig)
        return;

    uint64_t handler = task64_sighandler();
    uint64_t restorer = task64_sigrestorer();

    /* Bez wlasnej procedury zostaje reakcja domyslna. SIGKILL nie ma
       innej z zalozenia - dlatego jest nie do przechwycenia. */
    if (!handler || sig == SIGKILL) {
        printk("[jadro] zadanie %d dostalo sygnal %d\n",
               task64_current_id(), sig);
        task64_kill_current("sygnal");     /* nie wraca */
    }

    /* Miejsce na ramke i adres powrotu na stosie programu. Wierzcholek
       wyrownujemy do szesnastu bajtow, a potem odejmujemy osiem na
       adres powrotu - procedura ma zastac stos dokladnie taki, jaki
       zastaje kazda funkcja wolana instrukcja call. */
    uint64_t frame = (r->rsp - sizeof(struct regs)) & ~0xFULL;
    uint64_t sp = frame - 8;

    if (!paging64_user_range(sp, sizeof(struct regs) + 8)) {
        printk("[jadro] sygnal %d: brak miejsca na stosie programu\n", sig);
        task64_kill_current("brak stosu na sygnal");
    }

    memcpy((void *)(uintptr_t)frame, r, sizeof(*r));
    *(uint64_t *)(uintptr_t)sp = restorer;

    r->rsp = sp;
    r->rip = handler;
    r->rdi = (uint64_t)sig;              /* numer sygnalu dla procedury */
}

int signal64_return(struct regs *r)
{
    /* Procedura skonczyla sie zwyklym powrotem, wiec stos wskazuje
       teraz dokladnie na odlozona ramke */
    uint64_t src = r->rsp;
    if (!paging64_user_range(src, sizeof(struct regs)))
        return -1;

    struct regs saved;
    memcpy(&saved, (const void *)(uintptr_t)src, sizeof(saved));

    /* Ramka lezala na stosie uzytkownika, wiec program mogl ja
       podmienic. Rejestry i adres powrotu sa jego sprawa - segmenty
       i flagi nie, bo tedy wchodzi sie w jadro. */
    saved.cs = 0x23;
    saved.ss = 0x1B;
    saved.rflags = (saved.rflags & RFLAGS_USER_MASK) | RFLAGS_ALWAYS;
    saved.int_no = 0;
    saved.err_code = 0;

    *r = saved;
    return 0;
}
