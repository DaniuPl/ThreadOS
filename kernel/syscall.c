/* Obsługa syscalli (int 0x80)
 *
 * Konwencja jak w starym Linuksie: numer w eax, argumenty w ebx/ecx/edx,
 * wynik w eax. Handler dostaje wskaźnik na zapisane rejestry użytkownika
 * i modyfikuje eax w ramce - stub przerwania odtworzy go przy iret.
 */

#include <stdint.h>
#include "include/syscall.h"
#include "include/isr.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/keyboard.h"
#include "include/exec.h"
#include "include/kfile.h"

/* Wskazniki uzytkownika sa zaufane naiwnie - bledny adres skonczy sie
   page faultem w ring 0 (ginie proces). Pelna walidacja przyjdzie z
   prawdziwym oddzieleniem przestrzeni jadra. Limit ucina absurdy. */
#define IO_MAX 4096

void syscall_handler(struct regs *r)
{
    switch (r->eax) {
    case SYS_WRITE:
        r->eax = (uint32_t)file_write((int)r->ebx, (const void *)r->ecx,
                                      r->edx > IO_MAX ? IO_MAX : r->edx);
        break;
    case SYS_EXIT:
        printk("[jadro] proces '%s' zakonczyl sie (kod %d)\n",
               task_current_name(), (int)r->ebx);
        task_exit();
        break;
    case SYS_SLEEP:
        task_sleep(r->ebx);
        r->eax = 0;
        break;
    case SYS_GETTICKS:
        r->eax = pit_ticks();
        break;
    case SYS_READ:
        r->eax = (uint32_t)file_read((int)r->ebx, (void *)r->ecx,
                                     r->edx > IO_MAX ? IO_MAX : r->edx);
        break;
    case SYS_SPAWN:
        r->eax = (uint32_t)exec_spawn((const char *)r->ebx);
        break;
    case SYS_WAIT:
        /* Najprostsze czekanie: drzemka, az proces zniknie z listy */
        while (task_alive((int)r->ebx))
            task_sleep(20);
        r->eax = 0;
        break;
    case SYS_GETPID:
        r->eax = (uint32_t)task_current_id();
        break;
    case SYS_SBRK:
        r->eax = process_sbrk((int32_t)r->ebx);
        break;
    case SYS_OPEN:
        r->eax = (uint32_t)file_open((const char *)r->ebx, (int)r->ecx);
        break;
    case SYS_CLOSE:
        r->eax = (uint32_t)file_close((int)r->ebx);
        break;
    case SYS_SEEK:
        r->eax = (uint32_t)file_seek((int)r->ebx, r->ecx);
        break;
    default:
        printk("[jadro] nieznany syscall %u od '%s'\n",
               r->eax, task_current_name());
        r->eax = (uint32_t)-1;
    }
}
