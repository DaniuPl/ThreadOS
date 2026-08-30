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
#include "../kernel/include/smp.h"
#include "../kernel/include/biglock.h"
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
#include "../kernel/include/net.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/shm.h"
#include "../kernel/include/rtc.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_KERNEL_GS 0xC0000102

#define EFER_SCE   (1u << 0)
#define RFLAGS_IF  0x200

extern void syscall64_entry(void);

/* Blok danych rdzenia widziany przez stub wejsciowy przez rejestr GS.
   Uklad musi zgadzac sie z boot64/syscall64.S. */
struct core_syscall {
    uint64_t kernel_rsp;     /* przesuniecie 0 */
    uint64_t scratch;        /* przesuniecie 8 */
};
extern struct core_syscall syscall64_bloki[];
_Static_assert(sizeof(struct core_syscall) == 16,
               "stub liczy po szesnascie bajtow na rdzen");

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

/* Wolane na KAZDYM rdzeniu: wszystkie te rejestry sa jego wlasne.
   Rdzen, ktory ich nie ustawi, nie przyjmie instrukcji syscall wcale. */
void syscall64_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* Wskaznik na blok tego rdzenia. Stub wejsciowy siega po niego
       przez swapgs - to jedyna droga bez wolnego rejestru. */
    wrmsr(MSR_KERNEL_GS,
          (uint64_t)(uintptr_t)&syscall64_bloki[smp_core()]);

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
    syscall64_bloki[smp_core()].kernel_rsp = rsp;
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
static int name_from_ring3(uint64_t ptr, uint64_t len, char *out)
{
    if (len == 0 || len > FS_NAME_MAX || !paging64_user_range(ptr, len))
        return -1;
    const char *src = (const char *)(uintptr_t)ptr;
    for (uint64_t i = 0; i < len; i++)
        out[i] = src[i];
    out[len] = '\0';
    return 0;
}

/* Napis zakonczony zerem z pamieci programu do jadra. Dlugosci nie
   znamy z gory, wiec kazdy bajt trzeba sprawdzic osobno: napis moze
   siegac ostatniego bajtu mapowania i ani jednego dalej. Zwraca 0
   albo -1, gdy napis wychodzi poza pamiec programu albo nie miesci
   sie w buforze. */
static int text_from_ring3(uint64_t ptr, char *out, int max)
{
    for (int i = 0; i < max - 1; i++) {
        if (!paging64_user_range(ptr + (uint64_t)i, 1))
            return -1;
        out[i] = *(const char *)(uintptr_t)(ptr + (uint64_t)i);
        if (!out[i])
            return 0;
    }
    out[max - 1] = '\0';
    return 0;
}

/* Polecenia rysowania przekazywane przez program. Uklad musi zgadzac
   sie z tym w user64/usys64.h - w trybie dlugim wskaznik ma osiem
   bajtow i wymaga wyrownania, stad dziura po trzech liczbach. */
struct win_rect_cmd64 {
    int32_t x, y, w, h, color;
};
struct win_text_cmd64 {
    int32_t x, y, color;
    uint64_t text;
};
struct win_event64 {
    int32_t typ;
    int32_t x, y;
    int32_t val;
};

/* Opis gniazda przekazywany przez program. Uklad musi zgadzac sie
   z tym w user64/usys64.h. */
struct udp_info {
    int sock;
    uint32_t ip;
    uint16_t port;
    uint16_t _padding;
    uint32_t timeout;
};

static void handle(struct regs *r);

/* Wejscie ze stubu. Sygnal dostarczamy tuz przed powrotem - wtedy
   i tylko wtedy ramka opisuje program, a nie jadro. */
/* --- ktore wywolania nie potrzebuja wielkiego zamka jadra ----------
 *
 * Zamek jest jeden na cale jadro i bierze go kazde wejscie z
 * pierscienia 3, bo sterowniki, system plikow i wymiana stron pisane
 * byly dla jednego rdzenia i cicho na to licza. Ale nie kazde
 * wywolanie tam siega. Te ponizej dotykaja WYLACZNIE stanu wolajacego
 * zadania oraz rzeczy, ktore maja wlasne zamki - dwa rdzenie moga byc
 * w nich naraz i nic z tego nie wyniknie.
 *
 * Lista jest krotka i jawna z rozmyslem: dopisanie do niej wywolania
 * jest obietnica, ze przeczytalo sie je do konca, razem z tym, co wola
 * dalej. Domyslna odpowiedz brzmi "z zamkiem" i tak ma zostac.
 */
static int lockless(uint64_t nr)
{
    switch (nr) {
    case SYS64_GETPID:   /* wlasny numer, jedno pole zadania */
    case SYS64_TICKS:    /* licznik przerwan zegara, jedno slowo */
    case SYS64_RING:     /* wlasny selektor kodu z ramki przerwania */
    case SYS64_CORE:    /* pytanie do wlasnego lokalnego APIC */
    case SYS64_SLEEP:    /* wlasne "obudz mnie" i scheduler, ktory ma
                            swoj zamek - a przez przelaczenie kontekstu
                            wielkiego zamka i tak sie nie trzyma */
        return 1;
    default:
        return 0;
    }
}

/* Wywolanie systemowe przychodzi z pierscienia 3, wiec bierze wielki
   zamek jadra: program moze biec na kazdym rdzeniu, ale w jadrze ma
   byc naraz jeden. Instrukcja syscall nie idzie przez tablice
   przerwan, wiec to jedyne miejsce, w ktorym da sie to dla niej
   zrobic. */
void syscall64_handler(struct regs *r)
{
    if (lockless(r->rax)) {
        handle(r);
        /* Sygnal przestawia programowi stos i adres powrotu - to juz
           nie jest praca "tylko na swoim". Zamek bierzemy jednak
           dopiero wtedy, gdy naprawde jest co doreczyc. */
        if ((r->cs & 3) == 3 && task64_signal_pending()) {
            biglock_enter();
            signal64_deliver(r);
            biglock_leave();
        }
        return;
    }

    biglock_enter();
    handle(r);
    if ((r->cs & 3) == 3)
        signal64_deliver(r);
    biglock_leave();
}

static void handle(struct regs *r)
{
    /* Czwarty argument jedzie w R10, a nie w RCX: instrukcja syscall
       wpisuje do RCX adres powrotu, wiec RCX na argument nie ma szans.
       Tak samo robi Linux i z tego samego powodu. */
    uint64_t nr = r->rax, a1 = r->rdi, a2 = r->rsi, a3 = r->rdx,
             a4 = r->r10;

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
        int kind = task64_fd_kind((int)a1);
        if (kind == FDK_WOLNY) {
            if (a1 != FD_STDOUT) {
                r->rax = (uint64_t)-1;
                return;
            }
            for (uint64_t i = 0; i < a3; i++)
                printk("%c", buf[i]);
            r->rax = a3;
            return;
        }
        if (kind == FDK_POTOK_W) {
            int n = pipe64_write(task64_fd_slot((int)a1), buf, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind != FDK_FILE) {
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
        int kind = task64_fd_kind((int)a1);
        if (kind == FDK_WOLNY) {
            if (a1 != FD_STDIN) {
                r->rax = (uint64_t)-1;
                return;
            }
            int n = keyboard64_read((char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind == FDK_POTOK_R) {
            int n = pipe64_read(task64_fd_slot((int)a1),
                                (char *)(uintptr_t)a2, (uint32_t)a3);
            r->rax = (uint64_t)(int64_t)n;
            return;
        }
        if (kind != FDK_FILE) {
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
        char name[FS_NAME_MAX + 1];
        if (a2 == 0 || a2 > FS_NAME_MAX ||
            !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        const char *src = (const char *)(uintptr_t)a1;
        for (uint64_t i = 0; i < a2; i++)
            name[i] = src[i];
        name[a2] = '\0';

        int slot = fs_open(task64_cwd(), name, (int)a3);
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
        uint64_t old_one = task64_brk();
        if (!old_one) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint64_t fresh = old_one + (uint64_t)delta;
        if (fresh < task64_brk_base() || fresh >= USER64_TOP) {
            r->rax = (uint64_t)-1;   /* ponizej wlasnej pamieci albo w stos */
            return;
        }
        /* Zwezenie oddaje strony od razu - inaczej pamiec wracalaby
           do puli dopiero razem z calym procesem */
        for (uint64_t va = fresh & ~0xFFFULL; va + 4096 <= old_one;
             va += 4096) {
            if (va < fresh)
                continue;
            uint64_t f = paging64_unmap(va);
            if (f)
                pmm_free_frame((uint32_t)f);
        }
        task64_set_brk(fresh);
        r->rax = old_one;
        return;
    }
    case SYS64_MKDIR: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_mkdir(task64_cwd(), name);
        return;
    }
    case SYS64_CHDIR: {
        char name[FS_NAME_MAX + 1];
        int fresh_one = FS_ROOT;
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        if (fs_chdir(task64_cwd(), name, &fresh_one) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        task64_set_cwd(fresh_one);
        r->rax = 0;
        return;
    }
    case SYS64_GETCWD: {
        char path[FS_NAME_MAX * 4];
        if (a2 == 0 || !paging64_user_range(a1, a2)) {
            r->rax = (uint64_t)-1;
            return;
        }
        fs_getpath(task64_cwd(), path);
        char *dst = (char *)(uintptr_t)a1;
        uint64_t i = 0;
        while (i + 1 < a2 && path[i]) {
            dst[i] = path[i];
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
        } entry;

        if (!paging64_user_range(a2, sizeof(entry))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t size = 0;
        int dir = 0;
        /* fs_readdir oddaje 1 przy trafieniu i 0 na koncu katalogu -
           nie ujemna wartosc. Sprawdzanie "mniej niz zero" konczy sie
           tu petla bez konca, bo koniec katalogu wyglada jak sukces. */
        if (fs_readdir(task64_cwd(), (int)a1, entry.name,
                       &size, &dir) != 1) {
            r->rax = (uint64_t)-1;
            return;
        }
        entry.size = size;
        entry.is_dir = dir;
        memcpy((void *)(uintptr_t)a2, &entry, sizeof(entry));
        r->rax = 0;
        return;
    }
    case SYS64_UNLINK: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)fs_unlink(task64_cwd(), name);
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
    /* --- Siec ---------------------------------------------------
     *
     * Gniazdo nalezy do zadania, ktore je otworzylo - jadro trzyma
     * jego numer i sprzata po jego smierci. Dzieki temu program nie
     * moze siegnac po cudze gniazdo, podajac jego numer. */
    case SYS64_NETINFO: {
        /* Cztery adresy: wlasny, maska, brama, DNS */
        if (!paging64_user_range(a1, 4 * sizeof(uint32_t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        uint32_t *out = (uint32_t *)(uintptr_t)a1;
        out[0] = net_ip();
        out[1] = net_mask();
        out[2] = net_gw();
        out[3] = net_dns();
        r->rax = (uint64_t)net_configured();
        return;
    }
    case SYS64_UDP_OPEN:
        r->rax = (uint64_t)(int64_t)net_udp_open((uint16_t)a1,
                                                 task64_current_id());
        return;
    case SYS64_UDP_CLOSE:
        net_udp_close((int)a1, task64_current_id());
        r->rax = 0;
        return;
    case SYS64_UDP_SEND: {
        /* Wszystko poza samymi danymi idzie w jednym opisie. Trzy
           rejestry na argumenty to za malo na gniazdo, adres, port,
           dlugosc i limit czasu - a upychanie po dwie rzeczy w jeden
           rejestr czyta sie fatalnie i psuje przy pierwszej zmianie. */
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)net_udp_send(
            info.sock, task64_current_id(), info.ip, info.port,
            (const void *)(uintptr_t)a2, (uint16_t)a3);
        return;
    }
    case SYS64_UDP_RECV: {
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }

        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = net_udp_recv(info.sock, task64_current_id(),
                             &from_ip, &from_port,
                             (void *)(uintptr_t)a2, (uint16_t)a3,
                             info.timeout);
        if (n > 0) {
            /* Kto przyslal - program pyta o to przez ten sam opis */
            info.ip = from_ip;
            info.port = from_port;
            memcpy((void *)(uintptr_t)a1, &info, sizeof(info));
        }
        r->rax = (uint64_t)(int64_t)n;
        return;
    }
    case SYS64_TCP_CONNECT: {
        /* Ten sam opis, co przy UDP - port i limit czasu maja tam
           swoje miejsce, a adres jest tam, gdzie go widac */
        struct udp_info info;
        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        memcpy(&info, (const void *)(uintptr_t)a1, sizeof(info));
        r->rax = (uint64_t)(int64_t)tcp_connect(info.ip, info.port,
                                                task64_current_id(),
                                                info.timeout);
        return;
    }
    case SYS64_TCP_SEND:
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)tcp_send(
            (int)a1, task64_current_id(), (const void *)(uintptr_t)a2,
            (uint16_t)a3, 5000);
        return;
    case SYS64_TCP_RECV:
        if (a3 > 1400 || !paging64_user_range(a2, a3)) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)tcp_recv(
            (int)a1, task64_current_id(), (void *)(uintptr_t)a2,
            (uint16_t)a3, 5000);
        return;
    case SYS64_TCP_CLOSE:
        tcp_close((int)a1, task64_current_id());
        r->rax = 0;
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
    /* Okna. Wlascicielem jest proces, a nie ten, kto poda numer okna:
       kompozytor sprawdza go przy kazdym rysowaniu, wiec program nie
       narysuje niczego w cudzym oknie. */
    case SYS64_WIN_CREATE: {
        char title[64];
        if (text_from_ring3(a3, title, sizeof(title)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_create(
            task64_current_id(), (int)a1, (int)a2, title);
        return;
    }
    case SYS64_WIN_RECT: {
        struct win_rect_cmd64 c;
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_rect_cmd64 *)(uintptr_t)a2;
        r->rax = (uint64_t)(int64_t)desktop_win_rect(
            (int)a1, task64_current_id(), c.x, c.y, c.w, c.h,
            (uint8_t)c.color);
        return;
    }
    case SYS64_WIN_TEXT: {
        struct win_text_cmd64 c;
        char s[128];
        if (!paging64_user_range(a2, sizeof(c))) {
            r->rax = (uint64_t)-1;
            return;
        }
        c = *(const struct win_text_cmd64 *)(uintptr_t)a2;
        if (text_from_ring3(c.text, s, sizeof(s)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }
        r->rax = (uint64_t)(int64_t)desktop_win_text(
            (int)a1, task64_current_id(), c.x, c.y, s, (uint8_t)c.color);
        return;
    }
    /* Pamiec wspoldzielona. Region zyje w jadrze i jest wspolny dla
       wszystkich, kto poda ten sam klucz; proces dostaje go pod
       kolejnym wolnym adresem swojego obszaru mapowan. */
    case SYS64_SHM:
        r->rax = proc64_shm((int)a1, a2);
        return;
    case SYS64_SYSINFO: {
        /* To samo, co pokazuje 'mem' i 'uptime' - tyle ze programowi.
           Nic tu nie jest tajemnica: liczby opisuja maszyne, a nie
           cudze procesy. */
        struct {
            uint32_t ram_kib;
            uint32_t used_kib;
            uint32_t uptime_s;
        } info;

        if (!paging64_user_range(a1, sizeof(info))) {
            r->rax = (uint64_t)-1;
            return;
        }
        info.ram_kib = pmm_ram_kib();
        info.used_kib = pmm_used_frames() * 4;
        info.uptime_s = pit_ticks() / 100;
        *(typeof(info) *)(uintptr_t)a1 = info;
        r->rax = 0;
        return;
    }
    case SYS64_MMAP_FILE: {
        char name[FS_NAME_MAX + 1];
        if (name_from_ring3(a1, a2, name) < 0 ||
            !paging64_user_range(a3, sizeof(uint64_t))) {
            r->rax = 0;
            return;
        }
        uint64_t size = 0;
        uint64_t addr = proc64_mmap_file(name, &size, a4);
        if (addr)
            *(uint64_t *)(uintptr_t)a3 = size;
        r->rax = addr;
        return;
    }
    case SYS64_MSYNC:
        r->rax = proc64_msync();
        return;
    /* Numer rdzenia. Program nie ma jak przeczytac go sam - lokalny
       APIC jest poza jego przestrzenia. */
    case SYS64_CORE:
        r->rax = (uint64_t)(uint32_t)smp_core();
        return;
    case SYS64_GETPID:
        r->rax = (uint64_t)(int64_t)task64_current_id();
        return;
    case SYS64_TIME: {
        struct rtc_time t;
        if (!paging64_user_range(a1, sizeof(t))) {
            r->rax = (uint64_t)-1;
            return;
        }
        rtc_read(&t);
        *(struct rtc_time *)(uintptr_t)a1 = t;
        r->rax = 0;
        return;
    }
    case SYS64_WIN_POLL: {
        struct win_event64 ev;
        int jest = desktop_win_poll((int)a1, task64_current_id(), &ev);
        if (jest == 1) {
            if (!paging64_user_range(a2, sizeof(ev))) {
                r->rax = (uint64_t)-1;
                return;
            }
            *(struct win_event64 *)(uintptr_t)a2 = ev;
        }
        r->rax = (uint64_t)(int64_t)jest;
        return;
    }
    default:
        printk("[jadro] nieznane wywolanie systemowe %u\n", (uint32_t)nr);
        r->rax = (uint64_t)-1;
        return;
    }
}
