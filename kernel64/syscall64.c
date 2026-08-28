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
#include "../kernel/include/pmm.h"
#include "../kernel/include/string.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/isr.h"
#include "proc64.h"
#include "signal64.h"
#include "keyboard64.h"
#include "../kernel/include/fs.h"
#include "elf64.h"
#include "pipe64.h"

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
static int user_exits, user_failures;

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

int syscall64_user_exits(void)
{
    return user_exits;
}

int syscall64_user_failures(void)
{
    return user_failures;
}

void syscall64_forget_user(void)
{
    user_ran = 0;
    user_cs = 0;
    user_exited = 0;
    user_status = 0;
    user_exits = 0;
    user_failures = 0;
}

/* Wolane ze stubu w boot64/syscall64.S.
 *
 * Stub odklada dokladnie taka ramke, jaka odklada przerwanie, i podaje
 * na nia wskaznik. Numer wywolania jest w RAX, argumenty w RDI i RSI,
 * wynik wraca tam, gdzie program go szuka - w polu rax ramki, bo to
 * z niej procesor odtworzy rejestry przy powrocie.
 *
 * Wskaznikom z ring 3 nie wolno wierzyc na slowo: zanim jadro cokolwiek
 * z nich przeczyta, pyta tablice stron, czy program w ogole ma prawo
 * tam siegac. Bez tego wystarczyloby podac adres pamieci jadra, zeby
 * kazac mu ja sobie wypisac. */
/* Nazwa pliku z pamieci programu do jadra. Zwraca 0 albo -1. */
static int nazwa_z_ring3(uint64_t ptr, uint64_t len, char *out)
{
    if (len == 0 || len > FS_NAME_MAX || !paging64_user_range(ptr, len))
        return -1;
    const char *src = (const char *)(uintptr_t)ptr;
    for (uint64_t i = 0; i < len; i++)
        out[i] = src[i];
    out[len] = '\0';
    return 0;
}

static void handle(struct regs *r);

/* Wejscie ze stubu. Sygnal dostarczamy tuz przed powrotem - wtedy
   i tylko wtedy ramka opisuje program, a nie jadro. */
void syscall64_handler(struct regs *r)
{
    handle(r);
    if ((r->cs & 3) == 3)
        signal64_deliver(r);
}

static void handle(struct regs *r)
{
    uint64_t nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx;

    switch (nr) {
    case SYS64_WRITE: {
        const char *buf = (const char *)(uintptr_t)a2;
        if (a3 > 4096)
            a3 = 4096;               /* najpierw ograniczenie, potem
                                        sprawdzanie - inaczej dlugosc
                                        rzedu miliardow stron zajelaby
                                        jadro na dobre */
        if (!paging64_user_range(a2, a3)) {
            printk("[jadro] write: wskaznik %x nie nalezy do programu\n",
                   (uint32_t)a2);
            r->rax = (uint64_t)-1;
            return;
        }
        /* O tym, dokad idzie zapis, decyduje RODZAJ deskryptora,
           a nie jego numer. Jedynka bez przekierowania to konsola -
           ale po dup2 moze byc koncem potoku i program tego nie widzi. */
        int rodzaj = task64_fd_kind((int)a1);
        if (rodzaj == FDK_WOLNY) {
            if (a1 != FD_STDOUT) {
                r->rax = (uint64_t)-1;
                return;
            }
            for (uint64_t i = 0; i < a3; i++)
                printk("%c", buf[i]);
            r->rax = a3;
            return;
        }
        if (rodzaj == FDK_POTOK_W) {
            int n = pipe64_write(task64_fd_slot((int)a1), buf, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (rodzaj != FDK_PLIK) {
            r->rax = (uint64_t)-1;   /* zapis do konca do czytania */
            return;
        }
        int slot = task64_fd_slot((int)a1);
        int n = fs_write_at(slot, task64_fd_pos((int)a1), buf, (uint32_t)a3);
        if (n > 0)
            task64_fd_advance((int)a1, (uint32_t)n);
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_EXIT:
        user_status = (int)a1;
        user_exited = 1;
        user_exits++;
        if (a1)
            user_failures++;
        task64_exit_code((int)a1);   /* nie wraca - scheduler idzie dalej */
        return;
    case SYS64_RING:                 /* program mowi, w ktorym pierscieniu jest */
        user_cs = (uint32_t)a1;
        user_ran = 1;
        r->rax = 0;
        return;
    case SYS64_SLEEP:
        task64_sleep((uint32_t)a1);
        r->rax = 0;
        return;
    case SYS64_TICKS:
        r->rax = pit_ticks();
        return;
    case SYS64_FORK:
        r->rax = (uint64_t)proc64_fork(r);
        return;
    case SYS64_EXEC:
        /* Przy powodzeniu nie wraca tam, skad wolano: ramka opisuje
           juz nowy program, a stary nie istnieje */
        r->rax = (uint64_t)proc64_exec(r, (const char *)(uintptr_t)a1,
                                       a2, a3);
        return;
    case SYS64_WAIT: {
        int code = 0;
        int id = task64_wait(&code);
        if (id >= 0 && a1) {
            if (!paging64_user_range(a1, sizeof(int)))
                return;              /* zly wskaznik - kod przepada */
            *(int *)(uintptr_t)a1 = code;
        }
        r->rax = (uint64_t)(int64_t)id;
        return;
    }
    case SYS64_READ: {
        if (a3 > 4096)
            a3 = 4096;
        if (!paging64_user_range(a2, a3)) {
            printk("[jadro] read: wskaznik %x nie nalezy do programu\n",
                   (uint32_t)a2);
            r->rax = (uint64_t)-1;
            return;
        }
        int rodzaj = task64_fd_kind((int)a1);
        if (rodzaj == FDK_WOLNY) {
            if (a1 != FD_STDIN) {
                r->rax = (uint64_t)-1;
                return;
            }
            int n = keyboard64_read((char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (rodzaj == FDK_POTOK_R) {
            int n = pipe64_read(task64_fd_slot((int)a1),
                                (char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (rodzaj != FDK_PLIK) {
            r->rax = (uint64_t)-1;   /* odczyt z konca do pisania */
            return;
        }
        int slot = task64_fd_slot((int)a1);
        int n = fs_read_at(slot, task64_fd_pos((int)a1),
                           (void *)(uintptr_t)a2, (uint32_t)a3);
        if (n > 0)
            task64_fd_advance((int)a1, (uint32_t)n);
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_OPEN: {
        char nazwa[FS_NAME_MAX + 1];
        if (a2 == 0 || a2 > FS_NAME_MAX ||
            !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        const char *src = (const char *)(uintptr_t)a1;
        for (uint64_t i = 0; i < a2; i++)
            nazwa[i] = src[i];
        nazwa[a2] = '\0';

        int slot = fs_open(task64_cwd(), nazwa, (int)a3);
        if (slot < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)task64_fd_open(slot);
        return;
    }
    case SYS64_SBRK: {
        /* Zwracamy stara granice - program dostaje adres, od ktorego
           moze pisac. Przesuniecie o zero to pytanie "gdzie jestem". */
        int64_t delta = (int64_t)a1;
        uint64_t stara = task64_brk();
        if (!stara) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint64_t nowa = stara + (uint64_t)delta;
        if (nowa < task64_brk_base() || nowa >= USER64_TOP) {
            r->rax = (uint64_t)-1;   /* ponizej wlasnej pamieci albo w stos */
            return;
        }
        /* Zwezenie oddaje strony od razu - inaczej pamiec wracalaby
           do puli dopiero razem z calym procesem */
        for (uint64_t va = nowa & ~0xFFFULL; va + 4096 <= stara;
             va += 4096) {
            if (va < nowa)
                continue;
            uint64_t f = paging64_unmap(va);
            if (f)
                pmm_free_frame((uint32_t)f);
        }
        task64_set_brk(nowa);
        r->rax = stara;
        return;
    }
    case SYS64_MKDIR: {
        char nazwa[FS_NAME_MAX + 1];
        if (nazwa_z_ring3(a1, a2, nazwa) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_mkdir(task64_cwd(), nazwa);
        return;
    }
    case SYS64_CHDIR: {
        char nazwa[FS_NAME_MAX + 1];
        int nowy = FS_ROOT;
        if (nazwa_z_ring3(a1, a2, nazwa) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (fs_chdir(task64_cwd(), nazwa, &nowy) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        task64_set_cwd(nowy);
        r->rax = 0;
        return;
    }
    case SYS64_GETCWD: {
        char sciezka[FS_NAME_MAX * 4];
        if (a2 == 0 || !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        fs_getpath(task64_cwd(), sciezka);
        char *dst = (char *)(uintptr_t)a1;
        uint64_t i = 0;
        while (i + 1 < a2 && sciezka[i]) {
            dst[i] = sciezka[i];
            i++;
        }
        dst[i] = '\0';
        r->rax = i;
        return;
    }
    case SYS64_READDIR: {
        /* Jeden wpis na wywolanie - program pyta o kolejne numery,
           dopoki dostaje odpowiedz. Bufor opisuje struct dirent64
           z usys64.h i jest po stronie programu. */
        struct {
            char name[FS_NAME_MAX + 1];
            uint32_t size;
            int is_dir;
        } wpis;

        if (!paging64_user_range(a2, sizeof(wpis))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t rozmiar = 0;
        int katalog = 0;
        /* fs_readdir oddaje 1 przy trafieniu i 0 na koncu katalogu -
           nie ujemna wartosc. Sprawdzanie "mniej niz zero" konczy sie
           tu petla bez konca, bo koniec katalogu wyglada jak sukces. */
        if (fs_readdir(task64_cwd(), (int)a1, wpis.name,
                       &rozmiar, &katalog) != 1) {
            r->rax = (uint64_t)-1;
            return;
        }
        wpis.size = rozmiar;
        wpis.is_dir = katalog;
        memcpy((void *)(uintptr_t)a2, &wpis, sizeof(wpis));
        r->rax = 0;
        return;
    }
    case SYS64_UNLINK: {
        char nazwa[FS_NAME_MAX + 1];
        if (nazwa_z_ring3(a1, a2, nazwa) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_unlink(task64_cwd(), nazwa);
        return;
    }
    case SYS64_PIPE: {
        /* Program dostaje dwa numery: koniec do czytania i koniec do
           pisania. Dopoki trzyma oba, potok nie skonczy sie nigdy -
           dlatego shell zamyka u siebie oba zaraz po rozdaniu ich
           dzieciom. */
        if (!paging64_user_range(a1, 2 * sizeof(int))) {
            r->rax = (uint64_t)-1;
            return;
        }
        int idx = pipe64_new();
        if (idx < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        int rfd = task64_fd_pipe(idx, 1);
        int wfd = task64_fd_pipe(idx, 0);
        if (rfd < 0 || wfd < 0) {
            if (rfd >= 0)
                task64_fd_close(rfd);
            if (wfd >= 0)
                task64_fd_close(wfd);
            else
                pipe64_unref(idx, 0);
            r->rax = (uint64_t)-1;
            return;
        }
        int *out = (int *)(uintptr_t)a1;
        out[0] = rfd;
        out[1] = wfd;
        r->rax = 0;
        return;
    }
    case SYS64_DUP2:
        r->rax = (uint64_t)(int64_t)task64_fd_dup2((int)a1, (int)a2);
        return;
    case SYS64_CLOSE:
        r->rax = (uint64_t)(int64_t)task64_fd_close((int)a1);
        return;
    case SYS64_SIGNAL:
        /* Program podaje swoja procedure obslugi i adres, pod ktory ma
           wrocic po jej zakonczeniu. Ten drugi musi przyjsc od niego:
           kod wykonywalny jest tylko w jego wlasnych stronach. */
        task64_set_sighandler(a2, a3);
        r->rax = 0;
        return;
    case SYS64_SIGRETURN:
        if (signal64_return(r) < 0) {
            printk("[jadro] sigreturn: ramka poza pamiecia programu\n");
            task64_kill_current("zepsuty sigreturn");
        }
        return;                      /* ramka opisuje juz przerwane miejsce */
    case SYS64_KILL:
        r->rax = task64_signal((int)a1, (int)a2) ? 0 : (uint64_t)-1;
        return;
    default:
        printk("[jadro] nieznane wywolanie systemowe %u\n", (uint32_t)nr);
        r->rax = (uint64_t)-1;
        return;
    }
}
