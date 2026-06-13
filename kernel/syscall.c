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
#include "include/fs.h"

#define WRITE_MAX 1024

static int32_t sys_write(const char *s, uint32_t len)
{
    if (len > WRITE_MAX)
        len = WRITE_MAX;
    /* Zaufanie do wskaźnika użytkownika jest naiwne, ale bezpieczne
       o tyle, że błędny adres skończy się page faultem w ring 0 -
       walidacja zakresów przyjdzie z prawdziwą przestrzenią adresową. */
    for (uint32_t i = 0; i < len; i++)
        printk("%c", s[i]);
    return (int32_t)len;
}

void syscall_handler(struct regs *r)
{
    switch (r->eax) {
    case SYS_WRITE:
        r->eax = (uint32_t)sys_write((const char *)r->ebx, r->ecx);
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
        r->eax = (uint32_t)keyboard_read((char *)r->ebx, r->ecx);
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
    case SYS_FWRITE:
        r->eax = (uint32_t)fs_write((const char *)r->ebx,
                                    (const void *)r->ecx, r->edx);
        break;
    case SYS_FREAD:
        r->eax = (uint32_t)fs_read((const char *)r->ebx,
                                   (void *)r->ecx, r->edx);
        break;
    default:
        printk("[jadro] nieznany syscall %u od '%s'\n",
               r->eax, task_current_name());
        r->eax = (uint32_t)-1;
    }
}
