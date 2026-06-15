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
#include "include/fs.h"
#include "include/rtc.h"
#include "include/desktop.h"

/* Polecenia rysowania przekazywane przez wskaznik (uklad zgodny z usys.h) */
struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_text_cmd {
    int32_t x, y, color;
    uint32_t s;                 /* wskaznik na lancuch (char *) */
};

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
        task_exit((int)r->ebx);
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
        if (r->ecx)
            r->eax = (uint32_t)exec_spawn_redirect((const char *)r->ebx,
                                                   (const char *)r->ecx,
                                                   (int)r->edx);
        else
            r->eax = (uint32_t)exec_spawn((const char *)r->ebx);
        break;
    case SYS_WAIT:
        r->eax = (uint32_t)task_wait((int)r->ebx);  /* -> kod wyjscia */
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
    case SYS_PIPELINE:
        r->eax = (uint32_t)exec_spawn_pipeline((const char *)r->ebx,
                                               (const char *)r->ecx);
        break;
    case SYS_READDIR: {
        char *nb = (char *)r->ecx;
        uint32_t size;
        int is_d;
        if (!fs_readdir(task_cwd(), (int)r->ebx, nb, &size, &is_d))
            r->eax = (uint32_t)-1;       /* brak kolejnego wpisu */
        else
            r->eax = is_d ? (uint32_t)-2 : size;  /* -2 = katalog */
        break;
    }
    case SYS_UNLINK:
        r->eax = (uint32_t)fs_unlink(task_cwd(), (const char *)r->ebx);
        break;
    case SYS_TIME:
        rtc_read((struct rtc_time *)r->ebx);
        r->eax = 0;
        break;
    case SYS_CHDIR: {
        int nc;
        if (fs_chdir(task_cwd(), (const char *)r->ebx, &nc) == 0) {
            task_set_cwd(nc);
            r->eax = 0;
        } else {
            r->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_GETCWD:
        fs_getpath(task_cwd(), (char *)r->ebx);
        r->eax = 0;
        break;
    case SYS_MKDIR:
        r->eax = (uint32_t)fs_mkdir(task_cwd(), (const char *)r->ebx);
        break;
    case SYS_KILL:
        r->eax = (uint32_t)task_kill((int)r->ebx);
        break;
    case SYS_WIN_CREATE:
        r->eax = (uint32_t)desktop_win_create(task_current_id(), (int)r->ebx,
                                              (int)r->ecx, (const char *)r->edx);
        break;
    case SYS_WIN_RECT: {
        struct win_rect_cmd *c = (struct win_rect_cmd *)r->ecx;
        r->eax = (uint32_t)desktop_win_rect((int)r->ebx, task_current_id(),
                                            c->x, c->y, c->w, c->h,
                                            (uint8_t)c->color);
        break;
    }
    case SYS_WIN_TEXT: {
        struct win_text_cmd *c = (struct win_text_cmd *)r->ecx;
        r->eax = (uint32_t)desktop_win_text((int)r->ebx, task_current_id(),
                                            c->x, c->y, (const char *)c->s,
                                            (uint8_t)c->color);
        break;
    }
    case SYS_WIN_POLL:
        r->eax = (uint32_t)desktop_win_poll((int)r->ebx, task_current_id(),
                                            (void *)r->ecx);
        break;
    case SYS_MMAP:
        r->eax = process_mmap(r->ebx);
        break;
    default:
        printk("[jadro] nieznany syscall %u od '%s'\n",
               r->eax, task_current_name());
        r->eax = (uint32_t)-1;
    }
}
