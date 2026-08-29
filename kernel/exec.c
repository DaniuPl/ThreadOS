/* exec - ładowanie programów ELF z initrd
 *
 * Każdy proces dostaje własny katalog stron (mapowania jądra
 * współdzielone jako supervisor, segmenty programu i stos jako USER).
 * Loader działa w wątku jądra świeżo utworzonego zadania: buduje
 * przestrzeń adresową, kopiuje segmenty PT_LOAD do nowych ramek,
 * mapuje stos pod 0xC0000000 i skacze do ring 3 przez iret.
 */

#include <stdint.h>
#include "include/exec.h"
#include "include/initrd.h"
#include "include/prog.h"
#include "include/paging.h"
#include "include/pmm.h"
#include "include/task.h"
#include "include/usermode.h"
#include "include/heap.h"
#include "include/string.h"
#include "include/fs.h"
#include "include/blk.h"
#include "include/kfile.h"
#include "include/pipe.h"
#include "include/shm.h"
#include "include/printk.h"
#include "include/isr.h"

#define USER_STACK_TOP 0xC0000000u
/* Stos procesu: jedna strona wystarczala, dopoki programy nie zaczely
   liczyc kryptografii - RSA na 2048 bitach potrzebuje kilku kilobajtow
   na same liczby robocze. Osiem stron to 32 KiB. */
#define USER_STACK_PAGES 8
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* Dokad stosowi wolno urosnac poza te osiem stron. Reszta dochodzi
   sama, pod palcami - tak samo jak sterta. Zgadywanie z gory, ile
   stosu potrzeba, zawsze konczy sie albo marnowaniem pamieci, albo
   wyjatkiem w srodku glebokiego wywolania; a granica jest po to, zeby
   dziki wskaznik nie zamienil sie po cichu w zmapowana strone - pod
   nia blad strony pozostaje bledem. */
#define USER_STACK_MAX   (256 * 1024u)
#define USER_STACK_DNO   (USER_STACK_TOP - USER_STACK_MAX)
#define USER_BRK_MAX   0xB0000000u   /* sufit sterty procesu (sbrk) */
#define MMAP_BASE      0xB0000000u   /* obszar mmap (powyzej sterty) */
#define MMAP_MAX       0xBF000000u   /* sufit mmap (ponizej miejsca na stos) */
#define ARG_MAX        16
#define CMD_MAX        128
#define MAX_STAGES     5     /* maks. liczba etapow potoku a|b|c|... */

/* Argumenty przekazywane do watka-procesu: linia polecen plus
   niezalezne ustawienie stdin/stdout. Pozwala laczyc przekierowania,
   np. konsument potoku z wyjsciem do pliku ('a | b > plik'). */
struct exec_args {
    char cmd[CMD_MAX];
    int in_pipe;                  /* stdin z potoku (idx) albo -1 */
    int out_pipe;                 /* stdout do potoku (idx) albo -1 */
    char out_file[FS_NAME_MAX + 1]; /* stdout do pliku ("" = brak) */
    int out_append;               /* 1 = dopisuj (>>), 0 = obcinaj (>) */
};

/* Nagłówki ELF32 (tylko pola, których używamy) */
struct elf_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} __attribute__((packed));

#define ET_EXEC  2
#define EM_386   3
#define PT_LOAD  1

static int elf_valid(const struct elf_ehdr *eh, uint32_t size)
{
    if (size < sizeof(*eh))
        return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 1)            /* ELFCLASS32 */
        return 0;
    return eh->e_type == ET_EXEC && eh->e_machine == EM_386 && eh->e_phnum;
}

/* Swieza ramka pod strone vaddr, zamapowana TYLKO DO ODCZYTU -
   dla plikow odwzorowanych w pamieci */
static uint32_t page_frame_for_ro(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    if (!paging_map_user_ro_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

/* Mapuje (lub odnajduje) ramkę dla strony vaddr procesu */
static uint32_t page_frame_for(uint32_t dir, uint32_t vaddr)
{
    uint32_t frame = paging_get_mapping(dir, vaddr);
    if (frame)
        return frame;
    frame = pmm_alloc_frame();
    if (!frame)
        return 0;
    memset((void *)frame, 0, PAGE_SIZE);
    if (!paging_map_user_in(dir, vaddr, frame)) {
        pmm_free_frame(frame);
        return 0;
    }
    return frame;
}

static int load_segment(uint32_t dir, const uint8_t *file,
                        const struct elf_phdr *ph)
{
    if (ph->p_vaddr < PAGING_USER_BASE ||
        ph->p_vaddr + ph->p_memsz > USER_STACK_BASE) {
        printk("exec: segment poza dozwolonym zakresem (%x)\n", ph->p_vaddr);
        return 0;
    }

    uint32_t file_end = ph->p_vaddr + ph->p_filesz;

    for (uint32_t va = ph->p_vaddr & ~0xFFFu;
         va < ph->p_vaddr + ph->p_memsz; va += PAGE_SIZE) {
        uint32_t frame = page_frame_for(dir, va);
        if (!frame)
            return 0;

        /* Skopiuj fragment pliku przypadający na tę stronę
           (reszta zostaje wyzerowana - to .bss) */
        uint32_t copy_from = va > ph->p_vaddr ? va : ph->p_vaddr;
        if (copy_from < file_end) {
            uint32_t copy_to = va + PAGE_SIZE < file_end
                                   ? va + PAGE_SIZE : file_end;
            memcpy((void *)(frame + (copy_from - va)),
                   file + ph->p_offset + (copy_from - ph->p_vaddr),
                   copy_to - copy_from);
        }
    }
    return 1;
}

/* Zapis 32-bitowej wartości na stos użytkownika - jądro pisze przez
   adres fizyczny ramki, bo przestrzeń procesu nie jest jeszcze aktywna */
static void stack_write32(uint32_t sframe, uint32_t sp, uint32_t val)
{
    *(uint32_t *)(sframe + (sp - (USER_STACK_TOP - PAGE_SIZE))) = val;
}

/* Układa argumenty na stosie procesu (konwencja cdecl dla
   _start(int argc, char **argv)) i zwraca początkowy ESP */
static uint32_t build_argv_stack(uint32_t sframe, char **tok, int argc)
{
    uint32_t stack_base = USER_STACK_TOP - PAGE_SIZE;
    uint32_t sp = USER_STACK_TOP;
    uint32_t uargv[ARG_MAX];

    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = strlen(tok[i]) + 1;
        sp -= len;
        memcpy((void *)(sframe + (sp - stack_base)), tok[i], len);
        uargv[i] = sp;
    }

    sp &= ~3u;                          /* wyrównanie do 4 B */
    sp -= 4 * (argc + 1);               /* argv[] + NULL na końcu */
    uint32_t argv_base = sp;
    for (int i = 0; i < argc; i++)
        stack_write32(sframe, argv_base + 4 * i, uargv[i]);
    stack_write32(sframe, argv_base + 4 * argc, 0);

    sp -= 4;
    stack_write32(sframe, sp, argv_base);   /* argv */
    sp -= 4;
    stack_write32(sframe, sp, (uint32_t)argc);
    sp -= 4;
    stack_write32(sframe, sp, 0);           /* atrapa adresu powrotu */
    return sp;
}

/* Sprzatniecie po nieudanym starcie: jesli proces mial przejac konce
   potoku, trzeba je zwolnic, inaczej druga strona zawisnie */
static void exec_fail(struct exec_args *a)
{
    if (a->out_pipe >= 0)
        pipe_close_writer(a->out_pipe);
    if (a->in_pipe >= 0)
        pipe_close_reader(a->in_pipe);
    kfree(a);
}

/* Dzieli linie polecen na slowa w miejscu; zwraca argc */
static int tokenize(char *cmd, char **tok)
{
    int argc = 0;
    char *p = cmd;
    while (*p && argc < ARG_MAX) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        tok[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    return argc;
}

/* Buduje swiezy obraz procesu: nowa przestrzen adresowa, segmenty ELF
   z initrd, stos i argumenty na nim. Zwraca CR3 (0 przy bledzie)
   i wypelnia entry/esp/brk. Uzywaja tego dwie drogi - start nowego
   procesu (exec_task) i podmiana obrazu biezacego (process_exec). */
static uint32_t build_image(char **tok, int argc, uint32_t *entry_out,
                            uint32_t *esp_out, uint32_t *brk_out)
{
    uint32_t size;
    void *wczytany = (void *)0;
    const uint8_t *file = prog_obraz(tok[0], &size, &wczytany);
    if (!file) {
        printk("exec: nie ma '%s' ani w initrd, ani na dysku "
               "(sprawdz 'ls' i 'fls')\n", tok[0]);
        return 0;
    }

    const struct elf_ehdr *eh = (const struct elf_ehdr *)file;
    if (!elf_valid(eh, size)) {
        printk("exec: '%s' nie jest programem ELF32 i386\n", tok[0]);
        prog_zwolnij(wczytany);
        return 0;
    }

    uint32_t dir = paging_new_address_space();
    if (!dir) {
        printk("exec: brak pamieci na katalog stron\n");
        prog_zwolnij(wczytany);
        return 0;
    }

    /* Załaduj segmenty, śledząc koniec obrazu - tam zacznie się
       sterta procesu (sbrk) */
    uint32_t brk = 0;
    const struct elf_phdr *ph =
        (const struct elf_phdr *)(file + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        if (!load_segment(dir, file, &ph[i])) {
            paging_destroy_address_space(dir);
            prog_zwolnij(wczytany);
            return 0;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz > brk)
            brk = ph[i].p_vaddr + ph[i].p_memsz;
    }

    /* Najwyzsza strona stosu (tam trafiaja argumenty), a pod nia reszta */
    uint32_t sframe = 0;
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va = USER_STACK_TOP - (uint32_t)(i + 1) * PAGE_SIZE;
        uint32_t frame = page_frame_for(dir, va);
        if (!frame) {
            printk("exec: brak pamieci na stos\n");
            paging_destroy_address_space(dir);
            prog_zwolnij(wczytany);
            return 0;
        }
        if (i == 0)
            sframe = frame;
    }

    *esp_out = build_argv_stack(sframe, tok, argc);
    *entry_out = eh->e_entry;
    *brk_out = brk;
    printk("[jadro] exec '%s' z %s: ELF %u B, entry %x, argc=%d\n",
           tok[0], prog_skad(), size, eh->e_entry, argc);
    /* Segmenty sa juz w stronach procesu - kopia w pamieci jadra
       nie jest do niczego potrzebna */
    prog_zwolnij(wczytany);
    return dir;
}

/* Wątek jądra nowego procesu: zbuduj przestrzeń adresową i wejdź w ring 3 */
static void exec_task(void *arg)
{
    struct exec_args *a = arg;

    /* Tokenizacja w miejscu: a->cmd to nasza kopia */
    char *tok[ARG_MAX];
    int argc = tokenize(a->cmd, tok);
    if (!argc) {
        exec_fail(a);
        return;
    }

    uint32_t entry, esp, brk;
    uint32_t dir = build_image(tok, argc, &entry, &esp, &brk);
    if (!dir) {
        exec_fail(a);
        return;
    }

    /* Przekierowanie strumieni przed wejsciem w ring 3 (niezaleznie) */
    if (a->in_pipe >= 0)
        file_set_pipe(0, a->in_pipe, 0);    /* stdin <- koniec odczytu */
    if (a->out_pipe >= 0)
        file_set_pipe(1, a->out_pipe, 1);   /* stdout -> koniec zapisu */
    else if (a->out_file[0] &&
             file_redirect_stdout(a->out_file, a->out_append) < 0)
        printk("exec: nie moge przekierowac stdout do '%s'\n", a->out_file);
    kfree(a);

    /* Od teraz to zadanie ma wlasna przestrzen adresowa; jej ramki
       zostana zwolnione przy sprzataniu zombie */
    task_set_brk(brk);
    task_set_brk_base(brk);
    task_set_mmap_next(MMAP_BASE);
    task_use_address_space(dir);
    enter_ring3(entry, esp);
}

/* --- stronicowanie na zadanie --------------------------------------
 *
 * Sterta (sbrk) i obszar mmap sa procesowi obiecane, ale puste: zaden
 * z tych adresow nie ma jeszcze ramki. Pierwszy dostep konczy sie
 * bledem strony, a jadro dopiero wtedy przydziela wyzerowana ramke
 * i wznawia instrukcje.
 *
 * Ten sam punkt obsluguje strony, ktore nie tyle jeszcze nie powstaly,
 * co zostaly wyrzucone na dysk - wtedy zamiast swiezego zera wraca ich
 * dawna zawartosc.
 *
 * Wola to zarowno obsluga wyjatku (dostep z ring 3), jak i walidacja
 * wskaznikow w syscallach: jadro, ktore ma zaraz wypelnic bufor
 * procesu, musi go najpierw urzeczywistnic - inaczej swiezo
 * przydzielony, nietkniety bufor wygladalby jak zly wskaznik.
 *
 * Zwraca 1, gdy strona powstala (mozna wznowic), 0 gdy adres nie nalezy
 * do zadnego obiecanego obszaru albo zabraklo pamieci.
 */
/* Ile stron wczytano z plikow odwzorowanych w pamieci - do komendy
   'mem' i do testow: odwzorowanie duzego pliku ma kosztowac tyle, ile
   sie z niego wzielo, a nie tyle, ile ma stron */
static uint32_t mmap_pages;

uint32_t process_mmap_pages(void)
{
    return mmap_pages;
}

int process_demand_page(uint32_t addr)
{
    uint32_t brk = task_brk();
    if (!brk)                       /* watek jadra - nie ma sterty */
        return 0;

    uint32_t dir = task_current_cr3();
    uint32_t va = addr & ~(PAGE_SIZE - 1);
    if (paging_get_mapping(dir, va))
        return 0;                   /* strona juz jest - blad z innego powodu */

    /* Najpierw dysk, i to dla KAZDEGO adresu uzytkownika: wymiana
       zabiera tez strony kodu i stosu, ktore do zadnego obiecanego
       obszaru nie naleza. Sprawdzenie zakresu ponizej dotyczy tylko
       stron, ktore maja dopiero powstac. */
    if (paging_swap_in(dir, va))
        return 1;

    /* Strona pliku odwzorowanego w pamieci: bajty przychodza z dysku
       dopiero teraz, warstwa blokowa, po numerach ustalonych przy
       zakladaniu odwzorowania. Do kodu systemu plikow tu nie wchodzimy:
       ma on jeden wspolny bufor na blok. */
    uint32_t pierwszy = 0, zostalo = 0;
    if (task_mmap_znajdz(addr, &pierwszy, &zostalo)) {
        uint32_t ramka = page_frame_for_ro(dir, va);
        if (!ramka)
            return 0;
        uint8_t *cel = (uint8_t *)ramka;
        memset(cel, 0, PAGE_SIZE);
        for (uint32_t b = 0; b < PAGE_SIZE / FS_BLOCK &&
                             zostalo > b * FS_BLOCK; b++) {
            uint32_t blok = task_mmap_blok(addr, pierwszy + b);
            if (!blok)
                break;
            blk_read(fs_block_lba(blok), FS_BLOCK / 512, cel + b * FS_BLOCK);
        }
        mmap_pages++;
        return 1;
    }

    int in_heap = addr >= task_brk_base() && addr < brk;
    int in_mmap = addr >= MMAP_BASE && addr < task_mmap_next();
    /* Stos rosnie w dol pod palcami - dokladnie tak, jak sterta rosnie
       w gore. Osiem stron dostaje program na start, reszta dochodzi
       wtedy, gdy naprawde jej dotknie. */
    int in_stack = addr >= USER_STACK_DNO && addr < USER_STACK_TOP;
    if (!in_heap && !in_mmap && !in_stack)
        return 0;

    return page_frame_for(dir, va) != 0;
}

/* --- fork: rozwidlenie procesu --------------------------------------
 *
 * Dziecko nie jest ladowane od nowa - dostaje klon przestrzeni adresowej
 * rodzica (kopiowanie przy zapisie, zob. paging_clone_address_space) i
 * startuje jako watek jadra, ktory natychmiast wraca do ring 3 z ramka
 * rejestrow zapamietana w chwili syscalla. Wraca wiec w tym samym
 * miejscu programu co rodzic - rozni ich tylko zawartosc eax: rodzic
 * dostaje pid dziecka, dziecko zero.
 */
static void fork_child(void *arg)
{
    struct regs *saved = arg;
    struct regs frame = *saved;       /* na wlasny stos jadra */
    kfree(saved);
    enter_ring3_regs(&frame);
}

int process_fork(struct regs *r)
{
    if (!task_brk())                  /* watek jadra nie ma czego rozwidlac */
        return -1;
    if ((r->cs & 3) != 3)             /* fork wolany spoza ring 3 */
        return -1;

    struct regs *saved = kmalloc(sizeof(*saved));
    if (!saved)
        return -1;
    *saved = *r;
    saved->eax = 0;                   /* fork() zwraca w dziecku zero */

    uint32_t dir = paging_clone_address_space(task_current_cr3());
    if (!dir) {
        kfree(saved);
        return -1;
    }

    struct task *t = task_fork(fork_child, saved, dir);
    if (!t) {
        paging_destroy_address_space(dir);
        kfree(saved);
        return -1;
    }
    return task_id_of(t);
}

/* --- exec: podmiana obrazu biezacego procesu ------------------------
 *
 * Odpowiednik uniksowego execv: pid, deskryptory i katalog roboczy
 * zostaja, wymienia sie tylko program i cala przestrzen adresowa.
 * Razem z fork() daje to klasyczna pare - rozwidl sie, a w dziecku
 * wskocz w nowy program. Przy powodzeniu nie wraca.
 */
int process_exec(const char *cmdline)
{
    if (!task_brk())                  /* watek jadra nie ma czego podmieniac */
        return -1;

    char buf[CMD_MAX];
    uint32_t i = 0;
    while (cmdline[i] && i < sizeof(buf) - 1) {
        buf[i] = cmdline[i];
        i++;
    }
    buf[i] = '\0';

    char *tok[ARG_MAX];
    int argc = tokenize(buf, tok);
    if (!argc)
        return -1;

    uint32_t entry, esp, brk;
    uint32_t dir = build_image(tok, argc, &entry, &esp, &brk);
    if (!dir)
        return -1;                    /* stary obraz caly czas dziala */

    /* Od tego miejsca nie ma juz odwrotu */
    uint32_t old = task_current_cr3();
    task_rename(tok[0]);              /* 'ps' ma pokazywac nowy program */
    task_set_brk(brk);
    task_set_brk_base(brk);
    task_set_mmap_next(MMAP_BASE);
    task_use_address_space(dir);

    /* Stara przestrzen znika, o ile nie kreci sie w niej jeszcze jakis
       watek tego procesu (sys_thread) */
    if (!task_cr3_shared(old))
        paging_destroy_address_space(old);

    enter_ring3(entry, esp);       /* nie wraca */
    return 0;
}

/* sbrk: przesuwa koniec sterty procesu, mapując nowe strony.
   Zwraca stary brk albo -1. */
uint32_t process_sbrk(int32_t increment)
{
    uint32_t brk = task_brk();
    if (!brk)                       /* nie-proces (watek jadra) */
        return (uint32_t)-1;
    if (increment == 0)
        return brk;
    if (increment < 0)              /* zmniejszanie: nieobslugiwane */
        return (uint32_t)-1;

    uint32_t new_brk = brk + (uint32_t)increment;
    if (new_brk < brk || new_brk > USER_BRK_MAX)
        return (uint32_t)-1;

    /* Zadnej ramki tu nie przydzielamy: sterta rosnie na papierze,
       a strony pojawiaja sie dopiero przy pierwszym dotknieciu
       (process_demand_page). Program, ktory prosi o 200 MiB i uzywa
       jednego, zajmie tez jeden. */
    task_set_brk(new_brk);
    return brk;
}

/* mmap: mapuje 'size' bajtow swiezej (wyzerowanej) pamieci procesu w
   obszarze mmap i zwraca jej adres albo 0. Zwolnienie nastepuje wraz
   z przestrzenia adresowa przy koncu procesu. */
uint32_t process_mmap(uint32_t size)
{
    uint32_t base = task_mmap_next();
    if (!base || !size)             /* nie-proces albo zerowy rozmiar */
        return 0;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t end = base + pages * PAGE_SIZE;
    if (end > MMAP_MAX || end < base)
        return 0;

    task_set_mmap_next(end);        /* strony jak przy sbrk - na zadanie */
    return base;
}

/* --- Plik odwzorowany w pamieci ------------------------------------
 *
 * Program dostaje adres, pod ktorym widzi zawartosc pliku. Zadna
 * strona nie powstaje od razu: numery blokow zapamietujemy, a same
 * bajty przychodza z dysku dopiero przy pierwszym dotknieciu.
 *
 * Odwzorowanie jest tylko do odczytu. Zapis wymagalby odsylania stron
 * z powrotem na dysk i rozstrzygania, kiedy; dopoki tego nie ma,
 * lepiej niech proba zapisu konczy sie bledem, niz cicho ginie.
 */
uint32_t process_mmap_file(const char *nazwa, uint32_t *rozmiar_out)
{
    if (!fs_ready())
        return 0;

    int f = fs_open(task_cwd(), nazwa, 0);
    if (f < 0)
        return 0;
    int len = fs_size(f);
    if (len <= 0)
        return 0;

    uint32_t blokow = ((uint32_t)len + FS_BLOCK - 1) / FS_BLOCK;
    uint32_t stron = ((uint32_t)len + PAGE_SIZE - 1) / PAGE_SIZE;

    uint32_t baza = task_mmap_next();
    uint32_t koniec = baza + stron * PAGE_SIZE;
    if (!baza || koniec > MMAP_MAX || koniec < baza)
        return 0;

    uint32_t *bloki = kmalloc(blokow * sizeof(uint32_t));
    if (!bloki)
        return 0;
    for (uint32_t i = 0; i < blokow; i++) {
        /* Numery ustalamy TERAZ, na spokojnie - w obsludze bledu
           strony nie wolno juz wchodzic w kod systemu plikow */
        bloki[i] = fs_file_block(f, i, 0);
        if (!bloki[i]) {
            kfree(bloki);
            return 0;
        }
    }

    if (task_mmap_dodaj(baza, koniec, (uint32_t)len, bloki, blokow) < 0) {
        kfree(bloki);
        return 0;
    }
    task_set_mmap_next(koniec);
    *rozmiar_out = (uint32_t)len;
    return baza;
}

/* Watki w procesie: nowe zadanie dzielace przestrzen adresowa, wchodzace
   w ring 3 pod 'entry' z wlasnym stosem. */
struct thread_start {
    uint32_t entry;
    uint32_t arg;
    uint32_t ustack_top;
};

static void thread_trampoline(void *a)
{
    /* Biegniemy juz w przestrzeni adresowej procesu (scheduler zaladowal
       jej CR3). Ulozmy stos uzytkownika i wejdzmy w ring 3. */
    struct thread_start ts = *(struct thread_start *)a;
    kfree(a);
    uint32_t *sp = (uint32_t *)ts.ustack_top;
    *--sp = ts.arg;                  /* argument funkcji watku */
    *--sp = 0;                       /* atrapa adresu powrotu */
    enter_ring3(ts.entry, (uint32_t)sp);
}

/* Tworzy watek biegnacy w przestrzeni biezacego procesu. Zwraca tid albo -1. */
int process_thread(uint32_t entry, uint32_t arg)
{
    if (!task_mmap_next())                  /* tylko z procesu ring 3 */
        return -1;
    uint32_t stack = process_mmap(8192);    /* stos watku w przestrzeni procesu */
    if (!stack)
        return -1;

    struct thread_start *ts = kmalloc(sizeof(*ts));
    ts->entry = entry;
    ts->arg = arg;
    ts->ustack_top = stack + 8192;
    return task_id_of(task_create_thread("thread", thread_trampoline, ts,
                                         task_current_cr3()));
}

/* Mapuje region pamieci wspoldzielonej (po kluczu) do przestrzeni
   biezacego procesu i zwraca jego adres wirtualny albo 0. Rozne procesy
   wskazuja te same ramki fizyczne -> widza te sama pamiec. */
uint32_t process_shm(int key, uint32_t size)
{
    uint32_t base = task_mmap_next();
    if (!base)
        return 0;
    uint32_t phys = shm_get(key, size);
    if (!phys)
        return 0;

    uint32_t pages = (size ? size : 4096);
    pages = (pages + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t end = base + pages * PAGE_SIZE;
    if (end > MMAP_MAX || end < base)
        return 0;

    uint32_t dir = task_current_cr3();
    for (uint32_t i = 0; i < pages; i++)
        if (!paging_map_user_in(dir, base + i * PAGE_SIZE,
                                phys + i * PAGE_SIZE))
            return 0;
    task_set_mmap_next(end);
    return base;
}

/* Buduje exec_args dla zadanej linii polecen, kopiujac nazwe programu
   do prog[16]. Zwraca NULL, gdy program nie istnieje w initrd. */
static struct exec_args *make_args(const char *cmdline, char *prog)
{
    uint32_t i = 0;
    while (*cmdline == ' ')
        cmdline++;
    while (cmdline[i] && cmdline[i] != ' ' && i < 15) {
        prog[i] = cmdline[i];
        i++;
    }
    prog[i] = '\0';

    if (!prog[0])
        return (void *)0;
    if (!prog_istnieje(prog)) {
        printk("run: nie ma '%s' ani w initrd, ani na dysku "
               "(sprawdz 'ls' i 'fls')\n", prog);
        return (void *)0;
    }

    struct exec_args *a = kmalloc(sizeof(*a));
    uint32_t n = 0;
    while (cmdline[n] && n < CMD_MAX - 1) {
        a->cmd[n] = cmdline[n];
        n++;
    }
    a->cmd[n] = '\0';
    a->in_pipe = -1;
    a->out_pipe = -1;
    a->out_file[0] = '\0';
    a->out_append = 0;
    return a;
}

static void set_outfile(struct exec_args *a, const char *outfile)
{
    if (!outfile || !outfile[0])
        return;
    uint32_t m = 0;
    while (outfile[m] && m < FS_NAME_MAX) {
        a->out_file[m] = outfile[m];
        m++;
    }
    a->out_file[m] = '\0';
}

int exec_spawn(const char *cmdline)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;
    return task_id_of(task_create(prog, exec_task, a));
}

int exec_spawn_redirect(const char *cmdline, const char *outfile, int append)
{
    char prog[16];
    struct exec_args *a = make_args(cmdline, prog);
    if (!a)
        return -1;
    set_outfile(a, outfile);
    a->out_append = append;
    return task_id_of(task_create(prog, exec_task, a));
}

/* Uruchamia wielostopniowy potok 'a | b | c | ...': stdout kazdego
   etapu plynie do stdin nastepnego. Wszystkie etapy dzialaja rownolegle.
   'pipeline' to cala linia z separatorami '|'. Gdy outfile != NULL,
   stdout ostatniego etapu idzie dodatkowo do pliku. Zwraca pid
   ostatniego etapu albo -1.

   Najpierw przygotowujemy argumenty WSZYSTKICH etapow (walidujac, ze
   programy istnieja) i dopiero potem tworzymy zadania - dzieki temu
   blad w ktoryms etapie nie zostawia polowy uruchomionego potoku. */
int exec_spawn_pipeline(const char *pipeline, const char *outfile)
{
    char buf[CMD_MAX * MAX_STAGES];
    uint32_t bl = 0;
    while (pipeline[bl] && bl < sizeof(buf) - 1) {
        buf[bl] = pipeline[bl];
        bl++;
    }
    buf[bl] = '\0';

    /* Podziel na etapy po '|' (z przycieciem spacji) */
    char *seg[MAX_STAGES];
    int nseg = 0;
    char *start = buf;
    for (char *p = buf;; p++) {
        if (*p == '|' || *p == '\0') {
            int end = (*p == '\0');
            *p = '\0';
            while (*start == ' ')
                start++;
            char *e = start + strlen(start);
            while (e > start && e[-1] == ' ')
                *--e = '\0';
            if (nseg < MAX_STAGES)
                seg[nseg++] = start;
            start = p + 1;
            if (end)
                break;
        }
    }
    if (nseg < 2)
        return -1;

    /* Przygotuj argumenty wszystkich etapow (walidacja istnienia) */
    struct exec_args *args[MAX_STAGES];
    char prog[MAX_STAGES][16];
    for (int i = 0; i < nseg; i++) {
        args[i] = make_args(seg[i], prog[i]);
        if (!args[i]) {
            for (int j = 0; j < i; j++)
                kfree(args[j]);
            return -1;
        }
    }

    /* Potoki miedzy sasiednimi etapami: pipe[i] laczy etap i z i+1 */
    for (int i = 0; i + 1 < nseg; i++) {
        int pidx = pipe_alloc();
        if (pidx < 0) {
            printk("run: brak wolnego potoku\n");
            for (int j = 0; j < nseg; j++)
                kfree(args[j]);
            /* zwolnij juz przydzielone potoki przez zamkniecie obu koncow */
            for (int j = 0; j < i; j++) {
                pipe_close_reader(args[j + 1]->in_pipe);
                pipe_close_writer(args[j]->out_pipe);
            }
            return -1;
        }
        args[i]->out_pipe = pidx;
        args[i + 1]->in_pipe = pidx;
    }

    set_outfile(args[nseg - 1], outfile);  /* stdout ostatniego -> plik */

    int last_pid = -1;
    for (int i = 0; i < nseg; i++)
        last_pid = task_id_of(task_create(prog[i], exec_task, args[i]));
    return last_pid;
}
