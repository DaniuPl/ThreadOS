/* Obsługa syscalli (int 0x80)
 *
 * Konwencja jak w starym Linuksie: numer w eax, argumenty w ebx/ecx/edx,
 * wynik w eax. Handler dostaje wskaźnik na zapisane rejestry użytkownika
 * i modyfikuje eax w ramce - stub przerwania odtworzy go przy iret.
 *
 * Granica jądro/użytkownik: proces w ring 3 nie może sięgnąć po pamięć
 * jądra sam (pilnuje tego stronicowanie), ale mógłby to zrobić cudzymi
 * rękami - podając w syscallu wskaźnik na pamięć jądra. Jądro pisałoby
 * wtedy pod ten adres w ring 0, z pełnymi prawami. Dlatego każdy adres
 * przychodzący z ring 3 przechodzi przez user_ok()/user_str(): musi
 * leżeć w przestrzeni użytkownika i być realnie zamapowany w katalogu
 * stron procesu. Zły wskaźnik = błąd -1 (EFAULT), a nie page fault
 * w jądrze albo cicha kradzież pamięci.
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
#include "include/pmm.h"
#include "include/rtc.h"
#include "include/desktop.h"
#include "include/paging.h"
#include "include/net.h"
#include "include/tcp.h"
#include "include/string.h"

/* Polecenia rysowania przekazywane przez wskaznik (uklad zgodny z usys.h) */
struct win_rect_cmd {
    int32_t x, y, w, h, color;
};
struct win_text_cmd {
    int32_t x, y, color;
    uint32_t s;                 /* wskaznik na lancuch (char *) */
};
struct sysinfo_buf {
    uint32_t ram_kib;
    uint32_t used_kib;
    uint32_t uptime_s;
};
struct win_event_buf {
    int32_t type, x, y, val;
};
struct udp_send_cmd {
    uint32_t ip, port, buf, len;
};
struct udp_recv_cmd {
    uint32_t buf, max, timeout_ms;
    uint32_t from_ip, from_port;    /* wypelniane przez jadro */
};
struct netinfo_buf {
    uint32_t ip, mask, gw, dns;
};
struct tcp_io_cmd {
    uint32_t buf, len, timeout_ms;
};

#define IO_MAX     4096          /* limit na jeden read/write */
#define PATH_MAX   128           /* najdluzsza sciezka/linia polecen */
#define USER_TOP   0xC0000000u   /* szczyt przestrzeni uzytkownika (stos) */
#define EFAULT     ((uint32_t)-1)

/* --- walidacja wskaznikow z ring 3 ------------------------------------ */

/* Czy [addr, addr+len) w calosci lezy w pamieci uzytkownika biezacego
   procesu? paging_get_mapping() zwraca ramke tylko dla stron z wlasnych
   tablic procesu - strony jadra (wspoldzielone PDE) daja 0, wiec adres
   z obszaru jadra odpada automatycznie. */
static int user_ok(uint32_t addr, uint32_t len)
{
    if (!len)
        return 1;
    if (!addr || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;

    uint32_t cr3 = task_current_cr3();
    for (uint32_t page = addr & ~0xFFFu; page < addr + len; page += 0x1000) {
        if (!paging_get_mapping(cr3, page))
            return 0;
    }
    return 1;
}

/* Kopiuje lancuch z przestrzeni uzytkownika do bufora jadra.
   0 = OK, -1 = zly wskaznik albo lancuch dluzszy niz max-1. */
static int user_str(char *dst, uint32_t uaddr, uint32_t max)
{
    uint32_t cr3 = task_current_cr3();

    for (uint32_t i = 0; i < max; i++) {
        uint32_t va = uaddr + i;
        if (!va || va >= USER_TOP)
            return -1;
        /* Sprawdzaj mapowanie na starcie i przy kazdym przejsciu strony */
        if (i == 0 || (va & 0xFFF) == 0) {
            if (!paging_get_mapping(cr3, va & ~0xFFFu))
                return -1;
        }
        dst[i] = *(const volatile char *)va;
        if (!dst[i])
            return 0;
    }
    return -1;                   /* brak zera w limicie - odrzuc */
}

/* Zapis do pamieci uzytkownika po sprawdzeniu adresu. 0 = OK, -1 = blad. */
static int user_put(uint32_t uaddr, const void *src, uint32_t len)
{
    if (!user_ok(uaddr, len))
        return -1;
    memcpy((void *)uaddr, src, len);
    return 0;
}

/* Odczyt z pamieci uzytkownika po sprawdzeniu adresu. 0 = OK, -1 = blad. */
static int user_get(void *dst, uint32_t uaddr, uint32_t len)
{
    if (!user_ok(uaddr, len))
        return -1;
    memcpy(dst, (const void *)uaddr, len);
    return 0;
}

void syscall_handler(struct regs *r)
{
    switch (r->eax) {
    case SYS_WRITE: {
        uint32_t len = r->edx > IO_MAX ? IO_MAX : r->edx;
        if (!user_ok(r->ecx, len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)file_write((int)r->ebx, (const void *)r->ecx, len);
        break;
    }
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
    case SYS_READ: {
        uint32_t len = r->edx > IO_MAX ? IO_MAX : r->edx;
        if (!user_ok(r->ecx, len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)file_read((int)r->ebx, (void *)r->ecx, len);
        break;
    }
    case SYS_SPAWN: {
        /* Linia polecen (i nazwa pliku na stdout) ida do buforow jadra -
           exec zaklada wlasna przestrzen adresowa, wiec pamiec procesu
           wolajacego nie moze byc pod nia potrzebna. */
        char cmd[PATH_MAX], out[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (r->ecx) {
            if (user_str(out, r->ecx, sizeof(out)) < 0) {
                r->eax = EFAULT;
                break;
            }
            r->eax = (uint32_t)exec_spawn_redirect(cmd, out, (int)r->edx);
        } else {
            r->eax = (uint32_t)exec_spawn(cmd);
        }
        break;
    }
    case SYS_WAIT:
        r->eax = (uint32_t)task_wait((int)r->ebx);  /* -> kod wyjscia */
        break;
    case SYS_GETPID:
        r->eax = (uint32_t)task_current_id();
        break;
    case SYS_SBRK:
        r->eax = process_sbrk((int32_t)r->ebx);
        break;
    case SYS_OPEN: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)file_open(path, (int)r->ecx);
        break;
    }
    case SYS_CLOSE:
        r->eax = (uint32_t)file_close((int)r->ebx);
        break;
    case SYS_SEEK:
        r->eax = (uint32_t)file_seek((int)r->ebx, r->ecx);
        break;
    case SYS_PIPELINE: {
        char cmd[PATH_MAX], out[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (r->ecx && user_str(out, r->ecx, sizeof(out)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)exec_spawn_pipeline(cmd, r->ecx ? out : 0);
        break;
    }
    case SYS_READDIR: {
        char nb[FS_NAME_MAX + 1];
        uint32_t size;
        int is_d;
        if (!fs_readdir(task_cwd(), (int)r->ebx, nb, &size, &is_d)) {
            r->eax = (uint32_t)-1;       /* brak kolejnego wpisu */
            break;
        }
        if (user_put(r->ecx, nb, strlen(nb) + 1) < 0)
            r->eax = EFAULT;
        else
            r->eax = is_d ? (uint32_t)-2 : size;  /* -2 = katalog */
        break;
    }
    case SYS_UNLINK: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)fs_unlink(task_cwd(), path);
        break;
    }
    case SYS_TIME: {
        struct rtc_time t;
        rtc_read(&t);
        r->eax = user_put(r->ebx, &t, sizeof(t)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_CHDIR: {
        char path[PATH_MAX];
        int nc;
        if (user_str(path, r->ebx, sizeof(path)) < 0) {
            r->eax = EFAULT;
            break;
        }
        if (fs_chdir(task_cwd(), path, &nc) == 0) {
            task_set_cwd(nc);
            r->eax = 0;
        } else {
            r->eax = (uint32_t)-1;
        }
        break;
    }
    case SYS_GETCWD: {
        char path[PATH_MAX];
        fs_getpath(task_cwd(), path);
        r->eax = user_put(r->ebx, path, strlen(path) + 1) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_MKDIR: {
        char path[PATH_MAX];
        if (user_str(path, r->ebx, sizeof(path)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)fs_mkdir(task_cwd(), path);
        break;
    }
    case SYS_KILL:
        r->eax = (uint32_t)task_kill((int)r->ebx);
        break;
    case SYS_WIN_CREATE: {
        char title[64];
        if (user_str(title, r->edx, sizeof(title)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)desktop_win_create(task_current_id(),
                                                  (int)r->ebx, (int)r->ecx,
                                                  title);
        break;
    }
    case SYS_WIN_RECT: {
        struct win_rect_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)desktop_win_rect((int)r->ebx,
                                                task_current_id(),
                                                c.x, c.y, c.w, c.h,
                                                (uint8_t)c.color);
        break;
    }
    case SYS_WIN_TEXT: {
        struct win_text_cmd c;
        char s[128];
        if (user_get(&c, r->ecx, sizeof(c)) < 0 ||
            user_str(s, c.s, sizeof(s)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)desktop_win_text((int)r->ebx, task_current_id(),
                                            c.x, c.y, s, (uint8_t)c.color);
        break;
    }
    case SYS_WIN_POLL: {
        struct win_event_buf ev;
        int got = desktop_win_poll((int)r->ebx, task_current_id(), &ev);
        if (got == 1 && user_put(r->ecx, &ev, sizeof(ev)) < 0)
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)got;
        break;
    }
    case SYS_MMAP:
        r->eax = process_mmap(r->ebx);
        break;
    case SYS_SHM:
        r->eax = process_shm((int)r->ebx, r->ecx);
        break;
    case SYS_THREAD:
        /* Watek startuje w ring 3 - punkt wejscia musi byc kodem procesu */
        if (!user_ok(r->ebx, 1))
            r->eax = EFAULT;
        else
            r->eax = (uint32_t)process_thread(r->ebx, r->ecx);
        break;
    case SYS_SETPRIO:
        task_set_priority((int)r->ebx);
        r->eax = 0;
        break;
    case SYS_SYSINFO: {
        struct sysinfo_buf si;
        si.ram_kib = pmm_ram_kib();
        si.used_kib = pmm_used_frames() * 4;
        si.uptime_s = pit_ticks() / 100;
        r->eax = user_put(r->ebx, &si, sizeof(si)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_UDP_OPEN:
        r->eax = (uint32_t)net_udp_open((uint16_t)r->ebx, task_current_id());
        break;
    case SYS_UDP_SEND: {
        struct udp_send_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)net_udp_send((int)r->ebx, task_current_id(), c.ip,
                                        (uint16_t)c.port, (const void *)c.buf,
                                        (uint16_t)c.len);
        break;
    }
    case SYS_UDP_RECV: {
        struct udp_recv_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.max > IO_MAX ||
            !user_ok(c.buf, c.max)) {
            r->eax = EFAULT;
            break;
        }
        /* Odbior czeka (task_sleep), ale pisze do bufora dopiero po
           obudzeniu - a wtedy znowu aktywna jest przestrzen adresowa
           tego procesu. Wskaznik uzytkownika jest juz sprawdzony, wiec
           dane moga isc prosto do niego, bez posrednika w jadrze. */
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = net_udp_recv((int)r->ebx, task_current_id(), &from_ip,
                             &from_port, (void *)c.buf, (uint16_t)c.max,
                             c.timeout_ms);
        if (n < 0) {
            r->eax = (uint32_t)-1;
            break;
        }
        c.from_ip = from_ip;
        c.from_port = from_port;
        r->eax = user_put(r->ecx, &c, sizeof(c)) < 0 ? EFAULT : (uint32_t)n;
        break;
    }
    case SYS_UDP_CLOSE:
        net_udp_close((int)r->ebx, task_current_id());
        r->eax = 0;
        break;
    case SYS_NETINFO: {
        struct netinfo_buf ni;
        if (!net_configured()) {
            r->eax = (uint32_t)-1;      /* siec jeszcze bez adresu */
            break;
        }
        ni.ip = net_ip();
        ni.mask = net_mask();
        ni.gw = net_gw();
        ni.dns = net_dns();
        r->eax = user_put(r->ebx, &ni, sizeof(ni)) < 0 ? EFAULT : 0;
        break;
    }
    case SYS_TCP_CONNECT:
        r->eax = (uint32_t)tcp_connect(r->ebx, (uint16_t)r->ecx,
                                       task_current_id(), r->edx);
        break;
    case SYS_TCP_LISTEN:
        r->eax = (uint32_t)tcp_listen((uint16_t)r->ebx, task_current_id());
        break;
    case SYS_TCP_ACCEPT:
        r->eax = (uint32_t)tcp_accept((int)r->ebx, task_current_id(), r->ecx);
        break;
    case SYS_TCP_SEND: {
        struct tcp_io_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)tcp_send((int)r->ebx, task_current_id(),
                                    (const void *)c.buf, (uint16_t)c.len,
                                    c.timeout_ms);
        break;
    }
    case SYS_TCP_RECV: {
        struct tcp_io_cmd c;
        if (user_get(&c, r->ecx, sizeof(c)) < 0 || c.len > IO_MAX ||
            !user_ok(c.buf, c.len)) {
            r->eax = EFAULT;
            break;
        }
        /* Jak przy UDP: po obudzeniu znowu aktywna jest przestrzen tego
           procesu, wiec dane moga isc prosto do sprawdzonego bufora. */
        r->eax = (uint32_t)tcp_recv((int)r->ebx, task_current_id(),
                                    (void *)c.buf, (uint16_t)c.len,
                                    c.timeout_ms);
        break;
    }
    case SYS_TCP_CLOSE:
        tcp_close((int)r->ebx, task_current_id());
        r->eax = 0;
        break;
    case SYS_WAITANY: {
        int code = 0;
        int pid = task_wait_any(&code);
        if (pid >= 0 && r->ebx && user_put(r->ebx, &code, sizeof(code)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)pid;      /* -1 = nie ma juz dzieci */
        break;
    }
    case SYS_EXEC: {
        /* Linia polecen do bufora jadra: pamiec procesu zaraz zniknie
           razem z cala jego przestrzenia adresowa */
        char cmd[PATH_MAX];
        if (user_str(cmd, r->ebx, sizeof(cmd)) < 0) {
            r->eax = EFAULT;
            break;
        }
        r->eax = (uint32_t)process_exec(cmd);   /* przy powodzeniu nie wraca */
        break;
    }
    case SYS_FORK:
        /* Jedyny syscall, ktoremu potrzebna jest cala ramka rejestrow:
           z niej dziecko wraca do ring 3 tam, gdzie rodzic */
        r->eax = (uint32_t)process_fork(r);
        break;
    default:
        printk("[jadro] nieznany syscall %u od '%s'\n",
               r->eax, task_current_name());
        r->eax = (uint32_t)-1;
    }
}
