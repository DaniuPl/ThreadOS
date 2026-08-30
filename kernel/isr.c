/* Wspólna obsługa przerwań - wołana ze stubów w kernel/isr.S */

#include <stddef.h>
#include "include/isr.h"
#include "include/paging.h"
#include "include/signal.h"
#include "include/exec.h"
#include "include/pic.h"
#include "include/printk.h"
#include "include/smp.h"
#include "include/syscall.h"
#include "include/task.h"
#include "include/zamekjadra.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint",
    "Overflow", "BOUND Range Exceeded", "Invalid Opcode",
    "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
    "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
    "General Protection Fault", "Page Fault", "(zarezerwowany)",
    "x87 FPU Error", "Alignment Check", "Machine Check",
    "SIMD FP Exception", "Virtualization Exception", "Control Protection",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)", "(zarezerwowany)", "(zarezerwowany)",
    "(zarezerwowany)",
};

static irq_handler_t irq_handlers[16];

void irq_install_handler(int irq, irq_handler_t handler)
{
    irq_handlers[irq] = handler;
}

static void isr_obsluz(struct regs *r);

/* Wejscie do jadra z pierscienia 3 bierze wielki zamek jadra.
 *
 * Program moze biec na kazdym rdzeniu, ale w jadrze ma byc naraz
 * jeden: sterowniki, system plikow i wymiana stron pisane byly dla
 * jednego rdzenia i cicho na to licza. Przerwanie, ktore zastalo
 * procesor juz w jadrze, nie bierze niczego nowego - zamek jest
 * zagniezdzalny i rdzen, ktory go ma, przechodzi dalej bez czekania.
 */
void isr_handler(struct regs *r)
{
    if ((r->cs & 3) != 3) {         /* jadro przerwane w jadrze */
        isr_obsluz(r);
        return;
    }

    /* Wywolanie systemowe, ktore siega tylko po stan wolajacego
       zadania, zamka nie potrzebuje - i dopiero to sprawia, ze dwa
       procesy moga byc w jadrze naraz. Reszta wchodzi po staremu. */
    if (r->int_no == SYSCALL_VECTOR && syscall_bez_zamka(r->eax)) {
        syscall_handler(r);
        /* Zabicie i sygnal to juz nie jest praca "tylko na swoim":
           jedno konczy proces, drugie przestawia mu stos. Zamek
           bierzemy jednak dopiero wtedy, gdy naprawde jest co
           doreczyc - inaczej ta sciezka nie roznilaby sie od zwyklej
           niczym poza nazwa. */
        if (task_killed() || task_signal_pending()) {
            jadro_wejdz();
            task_kill_pending();
            signal_deliver(r);
            jadro_wyjdz();
        }
        return;
    }

    jadro_wejdz();
    isr_obsluz(r);
    jadro_wyjdz();
}

static void isr_obsluz(struct regs *r)
{
    if (r->int_no == SYSCALL_VECTOR) {
        syscall_handler(r);
        task_kill_pending();    /* dorecz zabicie takze przy powrocie z syscalla */
        signal_deliver(r);      /* ...a zaraz po nim oczekujace sygnaly */
        return;
    }

    if (r->int_no < 32) {
        /* Page fault na stronie oddanej na kopiowanie przy zapisie
           (fork) nie jest bledem - zrob prywatna kopie i wznow
           instrukcje. Dotyczy tez zapisow jadra (CR0.WP), np. gdy
           syscall wypelnia bufor swiezo rozwidlonego procesu. */
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            if (paging_handle_cow(cr2, r->err_code))
                return;
            /* Strona ze sterty albo mmap, ktorej proces jeszcze nie
               dotknal - przydziel ja teraz i wznow instrukcje */
            if (!(r->err_code & 0x1) && process_demand_page(cr2))
                return;
        }

        /* Breakpoint (int3) to wyjątek-pułapka: zgłoś i jedź dalej */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod eip=%x\n", r->eip);
            return;
        }

        /* Wyjątek w ring 3: ginie tylko proces użytkownika, nie system */
        if ((r->cs & 3) == 3) {
            printk("\n[jadro] proces '%s' ubity na rdzeniu %d: "
                   "%s (err=%x, eip=%x)\n",
                   task_current_name(), smp_rdzen(),
                   exception_names[r->int_no], r->err_code, r->eip);
            if (r->int_no == 14) {
                uint32_t cr2;
                __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
                printk("[jadro] proces siegnal po adres %x\n", cr2);
            }
            task_exit(-1);          /* ubity przez wyjatek CPU */
        }

        printk("\n!!! WYJATEK CPU %u: %s (err=%x) pod eip=%x\n",
               r->int_no, exception_names[r->int_no], r->err_code, r->eip);
        if (r->int_no == 14) {
            uint32_t cr2;
            __asm__ volatile("movl %%cr2, %0" : "=r"(cr2));
            printk("Page fault pod adresem (CR2): %x\n", cr2);
        }
        printk("System zatrzymany.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* Zegar lokalnego APIC. Nie przeszedl przez kontroler PIC, wiec nie
       ma tam czego zamykac - potwierdzenie idzie do wlasnego APIC tego
       rdzenia. Na rdzeniu wtornym to jedyne przerwanie, jakie w ogole
       przychodzi. */
    if (r->int_no == 48) {
        smp_zegar_tyk();
        return;
    }

    /* Pobudka od innego rdzenia. Nie niesie zadnej tresci: chodzilo
       tylko o to, zeby wyrwac ten procesor z instrukcji hlt. */
    if (r->int_no == 49) {
        smp_pobudka_odbierz();
        return;
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = r->int_no - IRQ_BASE;
        /* EOI przed handlerem: handler timera przelacza konteksty,
           wiec do "konca" tego przerwania moze dojsc dopiero, gdy
           watek znow dostanie procesor - PIC nie moze na to czekac.
           Bramki przerwan trzymaja IF=0, wiec zagniezdzenie nie grozi. */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }

    /* Przy powrocie do ring 3 dorecz oczekujace zabicie (kill) -
       proces ginie tu, w punkcie powrotu do trybu uzytkownika */
    if ((r->cs & 3) == 3) {
        task_kill_pending();
        signal_deliver(r);
    }
}
