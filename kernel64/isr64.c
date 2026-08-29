/* Wspolna obsluga przerwan w trybie dlugim
 *
 * Rola ta sama, co w jadrze 32-bitowym: wyjatek procesora zatrzymuje
 * system z diagnostyka, a przerwanie sprzetowe trafia do handlera
 * zarejestrowanego przez sterownik. Rozni sie tylko ramka rejestrow
 * i to, ze adresy sa 64-bitowe.
 */

#include <stddef.h>
#include <stdint.h>
#include "../kernel/include/isr.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/vga.h"
#include "task64.h"
#include "elf64.h"
#include "isr64.h"
#include "paging64.h"
#include "signal64.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/blk.h"

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

/* Slad po ostatnim wyjatku z ring 3 - do sprawdzenia po tym, jak
   winny program juz nie zyje */
static int user_fault_seen;
static uint32_t user_fault_vec, user_fault_err;
static uint32_t heap_pages;        /* ile stron sterty naprawde powstalo */
static uint32_t stack_pages;       /* i ile stron doszlo stosowi */
static uint32_t mmap_pages;        /* i ile wczytano z odwzorowanych plikow */
static uint64_t user_fault_addr;

int isr64_user_fault(void)            { return user_fault_seen; }
uint32_t isr64_user_fault_vector(void) { return user_fault_vec; }
uint32_t isr64_user_fault_err(void)    { return user_fault_err; }
uint64_t isr64_user_fault_addr(void)   { return user_fault_addr; }
void isr64_forget_user_fault(void)     { user_fault_seen = 0; }
uint32_t isr64_heap_pages(void)        { return heap_pages; }
uint32_t isr64_stack_pages(void)       { return stack_pages; }
uint32_t isr64_mmap_pages(void)        { return mmap_pages; }

static uint64_t read_cr2(void)
{
    uint64_t cr2;
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));
    return cr2;
}

void irq_install_handler(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16)
        irq_handlers[irq] = handler;
}

void isr64_handler(struct regs *r)
{
    if (r->int_no < 32) {
        /* Breakpoint to wyjatek-pulapka: zglos i jedz dalej */
        if (r->int_no == 3) {
            printk("[INT3] breakpoint pod rip=%x\n", (uint32_t)r->rip);
            return;
        }

        /* Zapis do strony wspoldzielonej po fork-u to nie blad, tylko
         * rachunek do zaplacenia: strone trzeba rozdzielic i pozwolic
         * programowi powtorzyc instrukcje.
         *
         * Sprawdzamy to przed wszystkim innym i niezaleznie od
         * pierscienia - z wlaczona ochrona zapisu takze jadro dostaje
         * tu blad, gdy pisze po pamieci procesu. */
        if (r->int_no == 14 && (r->err_code & 0x02) &&
            paging64_handle_cow(read_cr2()))
            return;

        /* Dotkniecie strony sterty, o ktora program poprosil, ale
         * ktorej jeszcze nie ma. To nie blad, tylko chwila, w ktorej
         * pamiec robi sie prawdziwa: prosba o megabajt kosztowala
         * zero, a placi sie dopiero za strony naprawde uzyte.
         *
         * Zapisywalna i bez prawa wykonywania - w danych programu nie
         * ma kodu, a gdyby byl, procesor go nie wykona. */
        /* Strona, ktora poszla na dysk. Sciagamy ja z powrotem i
         * pozwalamy powtorzyc instrukcje - dla programu nie zdarzylo
         * sie nic poza chwila zwloki. */
        if (r->int_no == 14 && !(r->err_code & 0x01) &&
            paging64_swap_in(paging64_current_space(), read_cr2()))
            return;

        /* Strona pliku odwzorowanego w pamieci.
         *
         * Bajty przychodza z dysku dopiero teraz - warstwa blokowa,
         * po numerach ustalonych przy zakladaniu odwzorowania. Do kodu
         * systemu plikow tu nie wchodzimy: ma on jeden wspolny bufor
         * na blok, a blad strony potrafi trafic w srodku dowolnej
         * operacji na plikach.
         *
         * Ogon ostatniej strony zostaje wyzerowany - plik konczy sie
         * tam, gdzie sie konczy.
         *
         * Zapisywalne odwzorowanie dostaje prawo zapisu i znacznik
         * PG_PLIK; bez niego strona jest tylko do odczytu i proba
         * zapisu ma sie skonczyc bledem, a nie cichym zgubieniem
         * danych. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t adres = read_cr2();
            uint32_t pierwszy = 0, zostalo = 0, zapis = 0;
            if (task64_mmap_znajdz(adres, &pierwszy, &zostalo, &zapis) &&
                paging64_lookup(adres & ~0xFFFULL) == 0) {
                uint32_t ramka = pmm_alloc_frame();
                if (ramka) {
                    uint8_t *cel = (uint8_t *)(uintptr_t)ramka;
                    memset(cel, 0, PAGE_SIZE);
                    for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK &&
                                         zostalo > b * FS_BLOCK; b++) {
                        uint32_t blok = task64_mmap_blok(adres, pierwszy + b);
                        if (!blok)
                            break;
                        blk_read(fs_block_lba(blok), FS_BLOCK / 512,
                                 cel + b * FS_BLOCK);
                    }
                    uint64_t prawa = PG_USER | PG_NX;
                    if (zapis)
                        prawa |= PG_WRITE | PG_PLIK;
                    if (paging64_map(adres & ~0xFFFULL, ramka, prawa) == 0) {
                        mmap_pages++;
                        return;
                    }
                    pmm_free_frame(ramka);
                }
            }
        }

        /* Stos, ktory wlasnie uroslo o strone w dol.
         *
         * Program dostaje na start cztery strony, a reszte dokladamy
         * pod palcami - dokladnie tak, jak sterte. Granica jest po to,
         * zeby dziki wskaznik nie zamienil sie po cichu w zmapowana
         * strone: pod nia blad strony pozostaje bledem. */
        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t adres = read_cr2();
            if (adres < USER64_STACK_TOP &&
                adres >= USER64_STACK_TOP - USER64_STACK_MAX &&
                paging64_lookup(adres & ~0xFFFULL) == 0) {
                uint32_t ramka = pmm_alloc_frame();
                if (ramka) {
                    memset((void *)(uintptr_t)ramka, 0, 4096);
                    if (paging64_map(adres & ~0xFFFULL, ramka,
                                     PG_USER | PG_WRITE | PG_NX) == 0) {
                        stack_pages++;
                        return;
                    }
                    pmm_free_frame(ramka);
                }
            }
        }

        if (r->int_no == 14 && (r->cs & 3) == 3) {
            uint64_t adres = read_cr2();
            if (task64_in_heap(adres) &&
                paging64_lookup(adres & ~0xFFFULL) == 0) {
                uint32_t ramka = pmm_alloc_frame();
                if (ramka) {
                    memset((void *)(uintptr_t)ramka, 0, 4096);
                    if (paging64_map(adres & ~0xFFFULL, ramka,
                                     PG_USER | PG_WRITE | PG_NX) == 0) {
                        heap_pages++;
                        return;
                    }
                    pmm_free_frame(ramka);
                }
            }
        }

        /* Wyjatek z pierscienia 3 to blad PROGRAMU, nie systemu.
         *
         * Dwa najnizsze bity selektora kodu mowia, skad przyszlo
         * przerwanie - a odklada go tam procesor, wiec program nie ma
         * jak sklamac. Jesli to on zawinil, ginie tylko on: jadro
         * wypisuje, co sie stalo, i wraca do pracy. Zatrzymywanie
         * calego systemu dlatego, ze jeden program siegnal nie tam,
         * gdzie wolno, bylo dobre w czasach bez ochrony pamieci.
         *
         * Handler juz z tego wyjatku nie wraca: schedule() przelacza
         * sie na inny watek i ramka przerwania zostaje porzucona razem
         * ze stosem martwego zadania. */
        if ((r->cs & 3) == 3) {
            user_fault_seen = 1;
            user_fault_vec = (uint32_t)r->int_no;
            user_fault_err = (uint32_t)r->err_code;
            user_fault_addr = r->int_no == 14 ? read_cr2() : r->rip;

            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            printk("[jadro] wyjatek %u (%s) w programie: err=%x, "
                   "rip=%x, adres=%x\n",
                   (uint32_t)r->int_no, exception_names[r->int_no],
                   (uint32_t)r->err_code, (uint32_t)r->rip,
                   (uint32_t)user_fault_addr);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            task64_kill_current(exception_names[r->int_no]);
        }

        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("\n!!! WYJATEK CPU %u: %s (err=%x)\n",
               (uint32_t)r->int_no, exception_names[r->int_no],
               (uint32_t)r->err_code);
        printk("rip=%x:%x rsp=%x:%x\n",
               (uint32_t)(r->rip >> 32), (uint32_t)r->rip,
               (uint32_t)(r->rsp >> 32), (uint32_t)r->rsp);
        if (r->int_no == 14) {
            uint64_t cr2 = read_cr2();
            printk("Page fault pod adresem (CR2): %x:%x\n",
                   (uint32_t)(cr2 >> 32), (uint32_t)cr2);
        }
        printk("System zatrzymany.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= IRQ_BASE && r->int_no < IRQ_BASE + 16) {
        int irq = (int)(r->int_no - IRQ_BASE);
        /* EOI przed handlerem - tak samo jak w jadrze 32-bitowym */
        pic_send_eoi(irq);
        if (irq_handlers[irq])
            irq_handlers[irq](r);
    }

    /* Program przerwany w pol kroku to najczestszy moment, w ktorym da
       sie dostarczyc sygnal - i jedyny, ktory dziala takze wtedy, gdy
       program o nic jadra nie prosi */
    if ((r->cs & 3) == 3)
        signal64_deliver(r);
}
